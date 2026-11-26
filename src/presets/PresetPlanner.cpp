/**
 * @file PresetPlanner.cpp
 * @brief 实现 PresetPlanner.hpp（一段一行地算这一趟会怎么样）。取舍都写在那边文件头。
 *
 * 【这一层里那些"数"是从哪儿来的（一段一段地对）】
 *
 * · `VanillaOptions` / `VanillaKeys` —— `planOptionsApply()` 的 `OptionsApplyReport`
 * （`written` / `added` / `removed` / `skipped` / `conflicts`）。
 * · `ServerList` —— `planServerListApply()` 的 `ServerListReport`
 * （`written` / `added` / `removed` / `skipped` / `untouchable`）。
 * · `GenericFiles` —— `planFileGroup()` 的 `GenericPlan`
 * （`replaced + copied` / `skipped` / `needsChoice`）+ 逐项的理由。
 * · `MalilibKeys` —— `planMalilibApply()` 的 `MalilibApplyReport`
 * （`written` / `removed` / `skipped` / `conflicts`）。
 *
 * MaLiLib 那一段的"冲突"和原版按键那一档**是同一件事**（两个东西抢同一组键位），
 * 所以它报在同一个格子上（`conflictCount`）。它和"待选择"（通用文件组那一档）不是
 * 一回事，别混着看。
 *
 * 【"还没确认"的那一趟为什么照样算】
 *
 * 两个适配器的计划函数在 `replaceConfirmed = false` 时都会**拒算**（回 `ok = false`、
 * `needsConfirm = true`），因为它们的调用方是"真的要写"那一路。可这一层是**预览**：
 * 用户正要拿这张表去决定要不要点头，跟他说"这一行是覆盖，几项不知道"等于没给他任何
 * 信息。所以这一层一律按 `replaceConfirmed = true` 去算，再把"还没点头"**自己**标在
 * 行上（`mode == Replace && !request.replaceConfirmed`）。
 */

#include "PresetPlanner.hpp"

#include <QCoreApplication>
#include <QSet>

#include "DataPackPreset.hpp"

namespace Presets {

namespace {

/// 这一层的提示语都在同一个上下文里（和 `VanillaOptions` / `ServerListPreset` /
/// `GenericFiles` 同一个名字）。
[[maybe_unused]] QString plannerText(const char *source) { return QCoreApplication::translate("Presets", source); }

/// 一段「覆盖」还没被点头时那一行下面那句话。
QString confirmReason(SectionKind kind) {
    switch (kind) {
    case SectionKind::VanillaOptions:
    case SectionKind::VanillaKeys:
        return QCoreApplication::translate("Presets", "覆盖会用预设替换目标中的同类设置，需要确认。");
    case SectionKind::ServerList:
        return QCoreApplication::translate("Presets", "覆盖会移除只存在于目标中的服务器条目，需要确认。");
    case SectionKind::GenericFiles:
        return plannerText("「覆盖」会把目标上同名的那几份换成预设里的（多出来的文件不动），"
                           "需要先确认。");
    case SectionKind::MalilibKeys:
        // 这一档的「覆盖」= 预设里没点名的那几格热键**整个解绑**（格子留着，见
        // `MalilibKeys.hpp`）—— 用户会看见"我原来绑的那些全没了"，所以那句话得说在这儿。
        return plannerText("「覆盖」会把目标上没被这一份点名的那几格热键整个解绑（格子留着不删），"
                           "需要先确认。");
    case SectionKind::Custom:
        break;
    }
    return QCoreApplication::translate("Presets", "「覆盖」会修改目标已有内容，需先确认。");
}

/// 这一段的种类这一版还认不认识（`Custom` 那一档是用户自己塞进来的、没有任何适配器接手
/// —— 落 `blocked` 是**对的**，悄悄跳过的话用户会以为"那一段也跟着过来了"）。
bool plannerKnowsKind(SectionKind kind) {
    return kind == SectionKind::VanillaOptions || kind == SectionKind::VanillaKeys ||
           kind == SectionKind::ServerList || kind == SectionKind::GenericFiles ||
           kind == SectionKind::MalilibKeys;
}

QString unknownKindReason(SectionKind kind) {
    // 把**盘上那个词**写进理由里（`sectionKindKey()` 那一套，不翻译）：用户看见"custom"
    // 才知道去预设里删哪一段，跟他说"不认识的种类"他只能猜。
    return QCoreApplication::translate("Presets", "当前版本不支持类别 %1，已跳过此部分。")
        .arg(sectionKindKey(kind));
}

/// 「覆盖」那一行：点头这件事是**这一层**判的（见文件头那段）。
void markConfirm(ApplyPlanRow *row, const PlanRequest &request, const PresetSection &section) {
    if (row->blocked || row->mode != ApplyMode::Replace || request.replaceConfirmed) {
        return;
    }
    row->needsConfirm = true;
    // 数据包落在**存档里**：那句话得说清楚覆盖的是哪个位置的东西（`Custom` 那个种类名
    // 说不出来这个）。
    row->problem = dataPackHandlesSection(section)
                       ? plannerText("「覆盖」会把目标存档里同名的数据包整个换成预设里的，"
                                     "需要先确认。")
                       : confirmReason(row->kind);
}

/// 落一段 `VanillaOptions` / `VanillaKeys` 的计划。
void planOptionsRow(const PlanRequest &request, const PresetSection &section, ApplyPlanRow *row) {
    bool readable = false;
    const OptionsFile source = optionsFromSections({section}, &readable);
    if (!readable) {
        row->blocked = true;
        row->problem = QCoreApplication::translate("Presets", "预设内容格式无效，已跳过此部分。");
        return;
    }

    // 「勾了哪些键」= **这一段里有的那些键**。段里的内容本来就是用户勾过的那一套
    // （`sectionsFromOptions()` 只写勾中的），所以这一趟不该拿"默认该勾什么"再猜一遍 ——
    // 猜的后果是用户特意没勾的硬件项被我们偷偷写进去（要求的「显示器、全屏分辨率等
    // 硬件相关项默认不选」）。
    QSet<QString> selected;
    for (const QString &key : source.keys()) {
        selected.insert(key);
    }

    // 一律按"已确认"算（见文件头），"还没点头"由 `markConfirm()` 标在行上。
    const OptionsApplyReport report =
        planOptionsApply(section.kind, source, request.target.options, selected, section.mode, true);
    if (!report.ok) {
        row->blocked = true;
        row->problem = report.problem;
        return;
    }

    row->count = report.written;
    row->removed = report.removed;
    row->skipped = report.skipped;
    row->conflictCount = report.conflicts.size();
}

/// 落一段 `ServerList` 的计划。
void planServerRow(const PlanRequest &request, const PresetSection &section, ApplyPlanRow *row) {
    if (!request.target.serversProblem.isEmpty()) {
        row->blocked = true;
        row->problem = request.target.serversProblem;
        return;
    }

    QList<NbtUtil::ServerEntry> source;
    QString why;
    if (!serverListFromSection(section, &source, &why)) {
        row->blocked = true;
        row->problem = why;
        return;
    }

    const ServerListReport report =
        planServerListApply(section.mode, request.target.servers, request.target.serversUntouchable,
                            source, true);
    if (!report.ok) {
        row->blocked = true;
        row->problem = report.problem;
        return;
    }

    row->count = report.written;
    row->removed = report.removed;
    row->skipped = report.skipped;
}

/// 落一段 `GenericFiles` 的计划。**这一档是四段里唯一会读盘的**（干跑数目标上缺哪几份）。
void planGenericFilesRow(const PlanRequest &request, const PresetSection &section,
                         ApplyPlanRow *row) {
    QList<GenericItem> items;
    QString why;
    if (!fileGroupFromSection(section, &items, &why)) {
        row->blocked = true;
        row->problem = why;
        return;
    }

    // 这一组是哪一组：**先看段的身份（id）**，空段（一条都没有）只有它说得出来。
    FileGroup group = FileGroup::Config;
    const bool idSays = fileGroupFromSectionId(section.id, &group);
    if (!idSays && items.isEmpty()) {
        row->blocked = true;
        row->problem =
            QCoreApplication::translate("Presets", "文件组 ID 无法识别且内容为空，已跳过此部分。");
        return;
    }
    if (idSays && !items.isEmpty()) {
        // 两份说法对了一下：id 说是 A 组、载荷里每一条路径却在 B 组底下 —— 这种段**不认**。
        // （`fileGroupFromSection()` 只保证载荷内部自洽；和 id 是否一致只有这儿判得了。）
        FileGroup fromPaths = FileGroup::Config;
        if (fileGroupOfPath(items.at(0).relativePath, &fromPaths) && fromPaths != group) {
            row->blocked = true;
            row->problem = QCoreApplication::translate("Presets", "此部分的标识（%1）与内容（%2）不匹配，已跳过。")
                               .arg(fileGroupDirName(group), fileGroupDirName(fromPaths));
            return;
        }
    }
    if (request.presetFilesRoot.trimmed().isEmpty() && !items.isEmpty()) {
        // 没有源根就数不出"目标上缺哪几份"。★ 这一档**不许**当成"没有东西要拷"（那会让
        // 用户以为预设里那几份配置是空的）。
        row->blocked = true;
        row->problem = QCoreApplication::translate("Presets", "未指定预设的 files/ 根目录，无法核对缺失文件。");
        return;
    }

    const GenericPlan plan =
        planFileGroup(request.presetFilesRoot, items, request.target.instanceRoot, section.mode);
    if (!plan.ok) {
        row->blocked = true;
        row->problem = plan.problem;
        return;
    }

    row->count = plan.replaced + plan.copied;
    row->skipped = plan.skipped;
    row->needsChoice = plan.needsChoice;
}

/// 落一段 `MalilibKeys` 的计划。
///
/// 这一段也要**读盘**（`config/<modId>.json`），和通用文件组那一档同一个形状：不读目标
/// 上那一份，就说不出"这一趟会解绑几格"。读的是**那一段点名的模组**那几份（不是整片扫
/// `config/`）—— 段里装着模组名，所以这一层知道该读谁。
void planMalilibRow(const PlanRequest &request, const PresetSection &section, ApplyPlanRow *row,
                    QList<MalilibEntry> *afterOut) {
    QList<MalilibEntry> entries;
    QString why;
    if (!malilibEntriesFromSections({section}, &entries, &why)) {
        row->blocked = true;
        row->problem = why;
        return;
    }

    QStringList modIds;
    for (const MalilibEntry &entry : entries) {
        if (!modIds.contains(entry.modId)) {
            modIds.append(entry.modId);
        }
    }

    // 空段（一格都没有）到这儿是 `modIds` 空 —— `malilibReadTarget()` 一份都不读，
    // 下面自然算出"不动"。那不是错误（用户可能就是把这一段的勾全去掉了）。
    const MalilibTarget target = malilibReadTarget(request.target.instanceRoot, modIds);
    // 一律按"已确认"算（见文件头）：这时候"还没点头"由 `markConfirm()` 标在行上。
    const MalilibApplyResult applied = applyMalilibEntries(entries, target, section.mode, true);
    const MalilibApplyReport &report = applied.report;
    if (!report.ok) {
        // 今天**到不了这儿**（上面那句"按已确认算"关掉了适配器里唯一一条 `ok = false` 的
        // 路）。留着是防着哪天那条路变成两条 —— 那一段该在表上落"做不了"，而不是悄悄报 0。
        row->blocked = true;
        row->problem = report.problem;
        return;
    }

    row->count = report.written;
    row->removed = report.removed;
    row->skipped = report.skipped;
    row->conflictCount = report.conflicts.size();

    // 这一份**这一趟之后**绑着的那几格交给**整份**那一张单子（跨模组的冲突只有攒到一处
    // 才看得出来 —— 见 `ApplyPlan::keyConflicts`）。被跳过的段（`blocked`）不交：那一段
    // 这一趟压根不动，拿它的格子去说"整份有冲突"是假的。
    if (afterOut != nullptr) {
        *afterOut += applied.after;
    }
}

/// 落一段数据包的计划（世界在不在那边说的是两件事）。
void planDataPackRow(const PlanRequest &request, const PresetSection &section, ApplyPlanRow *row) {
    if (row->title.isEmpty()) {
        row->title = QCoreApplication::translate("Presets", "数据包");
    }
    if (!request.target.supportsDataPacks) {
        row->blocked = true;
        row->problem = plannerText("这个实例的游戏版本还没有数据包（17w43a 之前），"
                                   "这一段这一趟不动。");
        return;
    }
    QList<DataPackItem> items;
    QString why;
    if (!dataPackItemsFromSection(section, &items, &why)) {
        row->blocked = true;
        row->problem = why;
        return;
    }
    if (request.presetFilesRoot.trimmed().isEmpty() && !items.isEmpty()) {
        row->blocked = true;
        row->problem =
            QCoreApplication::translate("Presets", "未指定预设的 files/ 根目录，无法定位数据包。");
        return;
    }
    const DataPackPlan plan =
        planDataPacks(request.target.instanceRoot, items, section.mode);
    if (!plan.ok) {
        row->blocked = true;
        row->problem = plan.problem;
        return;
    }
    row->count = plan.deploy;
    row->deferred = plan.deferred;
    row->skipped = plan.skipped;
    row->needsChoice = plan.needsChoice;
}

}  // namespace

// ---------------------------------------------------------------------------
// 那几口"字"的唯一出处
// ---------------------------------------------------------------------------

QString applyPlanVerb(ApplyMode mode) {
    switch (mode) {
    case ApplyMode::Merge:
        return QCoreApplication::translate("Presets", "合并");
    case ApplyMode::Replace:
        return QCoreApplication::translate("Presets", "覆盖");
    case ApplyMode::Append:
        return QCoreApplication::translate("Presets", "追加");
    case ApplyMode::CopyIfMissing:
        return QCoreApplication::translate("Presets", "仅在缺失时复制");
    case ApplyMode::Skip:
        return QCoreApplication::translate("Presets", "跳过");
    }
    return QString();
}

QString applyPlanUnit(SectionKind kind) {
    switch (kind) {
    // `options.txt` 那两段动的是**键**，说「项」（「游戏设置 合并 18 项」）。
    // MaLiLib 那一段动的也是**格**（一格热键 / 一格设置），还是「项」。
    case SectionKind::VanillaOptions:
    case SectionKind::VanillaKeys:
    case SectionKind::MalilibKeys:
        return QCoreApplication::translate("Presets", "项");
    // 服务器条目、配置文件、资源包都是"一个东西"，说「个」（「服务器列表 追加 3 个」）。
    case SectionKind::ServerList:
    case SectionKind::GenericFiles:
    case SectionKind::Custom:
        // `Custom` 这一档唯一有适配器接手的是数据包（`dataPackHandlesSection()`）；
        // 别的 `Custom` 段落 `blocked`，量词用不上。
        return QCoreApplication::translate("Presets", "个");
    }
    return QCoreApplication::translate("Presets", "项");
}

namespace {

/// 计划行里的"东西"按段分组：英文要选对名词，中文都还是「项」或「个」。
enum class PlanUnit { Items, Servers, Files, DataPacks };

PlanUnit planUnitFor(SectionKind kind) {
    switch (kind) {
    case SectionKind::ServerList:
        return PlanUnit::Servers;
    case SectionKind::GenericFiles:
        return PlanUnit::Files;
    case SectionKind::Custom:
        return PlanUnit::DataPacks;
    case SectionKind::VanillaOptions:
    case SectionKind::VanillaKeys:
    case SectionKind::MalilibKeys:
        break;
    }
    return PlanUnit::Items;
}

enum class PlanPart { Removed, Deferred, Skipped, NeedsChoice, Conflict };

/// 计划行的一段：模板自带量词和标点，跟在逗号后的那一档自带前导逗号。
QString countPart(PlanUnit unit, PlanPart part, int count, bool leading) {
    switch (part) {
    case PlanPart::Removed:
        switch (unit) {
        case PlanUnit::Items:
            return leading ? QCoreApplication::translate("Presets", "删除 %1 项").arg(count)
                           : QCoreApplication::translate("Presets", "，删除 %1 项").arg(count);
        case PlanUnit::Servers:
            return leading ? QCoreApplication::translate("Presets", "删除 %1 个", "服务器条目").arg(count)
                           : QCoreApplication::translate("Presets", "，删除 %1 个", "服务器条目").arg(count);
        case PlanUnit::Files:
            return leading ? QCoreApplication::translate("Presets", "删除 %1 个", "文件").arg(count)
                           : QCoreApplication::translate("Presets", "，删除 %1 个", "文件").arg(count);
        case PlanUnit::DataPacks:
            return leading ? QCoreApplication::translate("Presets", "删除 %1 个", "数据包").arg(count)
                           : QCoreApplication::translate("Presets", "，删除 %1 个", "数据包").arg(count);
        }
        break;
    case PlanPart::Deferred:
        switch (unit) {
        case PlanUnit::Items:
            return leading ? QCoreApplication::translate("Presets", "待应用 %1 项").arg(count)
                           : QCoreApplication::translate("Presets", "，待应用 %1 项").arg(count);
        case PlanUnit::Servers:
            return leading ? QCoreApplication::translate("Presets", "待应用 %1 个", "服务器条目").arg(count)
                           : QCoreApplication::translate("Presets", "，待应用 %1 个", "服务器条目").arg(count);
        case PlanUnit::Files:
            return leading ? QCoreApplication::translate("Presets", "待应用 %1 个", "文件").arg(count)
                           : QCoreApplication::translate("Presets", "，待应用 %1 个", "文件").arg(count);
        case PlanUnit::DataPacks:
            return leading ? QCoreApplication::translate("Presets", "待应用 %1 个", "数据包").arg(count)
                           : QCoreApplication::translate("Presets", "，待应用 %1 个", "数据包").arg(count);
        }
        break;
    case PlanPart::Skipped:
        switch (unit) {
        case PlanUnit::Items:
            return leading ? QCoreApplication::translate("Presets", "跳过 %1 项").arg(count)
                           : QCoreApplication::translate("Presets", "，跳过 %1 项").arg(count);
        case PlanUnit::Servers:
            return leading ? QCoreApplication::translate("Presets", "跳过 %1 个", "服务器条目").arg(count)
                           : QCoreApplication::translate("Presets", "，跳过 %1 个", "服务器条目").arg(count);
        case PlanUnit::Files:
            return leading ? QCoreApplication::translate("Presets", "跳过 %1 个", "文件").arg(count)
                           : QCoreApplication::translate("Presets", "，跳过 %1 个", "文件").arg(count);
        case PlanUnit::DataPacks:
            return leading ? QCoreApplication::translate("Presets", "跳过 %1 个", "数据包").arg(count)
                           : QCoreApplication::translate("Presets", "，跳过 %1 个", "数据包").arg(count);
        }
        break;
    case PlanPart::NeedsChoice:
        switch (unit) {
        case PlanUnit::Items:
            return QCoreApplication::translate("Presets", "待选择 %1 项").arg(count);
        case PlanUnit::Servers:
            return QCoreApplication::translate("Presets", "待选择 %1 个", "服务器条目").arg(count);
        case PlanUnit::Files:
            return QCoreApplication::translate("Presets", "待选择 %1 个", "文件").arg(count);
        case PlanUnit::DataPacks:
            return QCoreApplication::translate("Presets", "待选择 %1 个", "数据包").arg(count);
        }
        break;
    case PlanPart::Conflict:
        return leading ? QCoreApplication::translate("Presets", "冲突 %1 项").arg(count)
                       : QCoreApplication::translate("Presets", "，冲突 %1 项").arg(count);
    }
    return QString();
}

/// 「动词 + 数量 + 量词」的完整模板（单独显示的那一档）。
QString primaryCountText(PlanUnit unit, ApplyMode mode, int count) {
    const QString verb = applyPlanVerb(mode);
    switch (unit) {
    case PlanUnit::Items:
        return QCoreApplication::translate("Presets", "%1 %2 项").arg(verb).arg(count);
    case PlanUnit::Servers:
        return QCoreApplication::translate("Presets", "%1 %2 个", "服务器条目").arg(verb).arg(count);
    case PlanUnit::Files:
        return QCoreApplication::translate("Presets", "%1 %2 个", "文件").arg(verb).arg(count);
    case PlanUnit::DataPacks:
        return QCoreApplication::translate("Presets", "%1 %2 个", "数据包").arg(verb).arg(count);
    }
    return QString();
}

}  // namespace

QString applyPlanRowText(const ApplyPlanRow &row) {
    if (row.blocked) {
        return QCoreApplication::translate("Presets", "无法完成");
    }

    const PlanUnit unit = planUnitFor(row.kind);

    // 这一档我们做不了主 ⇒ 这一行只说得出口这一件事（`count` 那一趟必然是 0）。
    if (row.needsChoice > 0) {
        return countPart(unit, PlanPart::NeedsChoice, row.needsChoice, true);
    }

    QStringList parts;

    if (row.count > 0) {
        parts.append(primaryCountText(unit, row.mode, row.count));
    } else if (row.removed > 0) {
        // 会动的数是 0、却要删东西 —— 这就是"空段铺上去 = 把那一片清空"那一档。
        // 只写「覆盖 0 项」等于把最要紧的那半截藏起来了。
        parts.append(applyPlanVerb(row.mode));
    } else if (row.deferred > 0) {
        // 全是"目标上没有那个世界"的：这一行只说得出口"它们先记着"。
        parts.append(countPart(unit, PlanPart::Deferred, row.deferred, true));
    }

    if (!parts.isEmpty()) {
        if (row.removed > 0) {
            parts.append(countPart(unit, PlanPart::Removed, row.removed, false));
        }
        if (row.conflictCount > 0) {
            parts.append(countPart(unit, PlanPart::Conflict, row.conflictCount, false));
        }
        if (row.deferred > 0 && row.count > 0) {
            // 只在这一行已经有"部署"那半截时补一句；全待应用的那种上面已经说过了。
            parts.append(countPart(unit, PlanPart::Deferred, row.deferred, false));
        }
        if (row.skipped > 0) {
            parts.append(countPart(unit, PlanPart::Skipped, row.skipped, false));
        }
        return parts.join(QString());
    }

    // 会动的、要删的、有冲突的都没有 ⇒ 这一行要么是"跳过"，要么是"没什么可动的"。
    if (row.skipped > 0) {
        return countPart(unit, PlanPart::Skipped, row.skipped, true);
    }
    if (row.mode == ApplyMode::Skip) {
        return applyPlanVerb(ApplyMode::Skip);
    }
    return QCoreApplication::translate("Presets", "无变化");
}

// ---------------------------------------------------------------------------
// 整份计划
// ---------------------------------------------------------------------------

int ApplyPlan::totalCount() const {
    int total = 0;
    for (const ApplyPlanRow &row : rows) {
        total += row.count;
    }
    return total;
}

int ApplyPlan::deferredCount() const {
    int total = 0;
    for (const ApplyPlanRow &row : rows) {
        total += row.deferred;
    }
    return total;
}

int ApplyPlan::riskyRowCount() const {
    int total = 0;
    for (const ApplyPlanRow &row : rows) {
        if (row.risky()) {
            ++total;
        }
    }
    return total;
}

int ApplyPlan::blockedRowCount() const {
    int total = 0;
    for (const ApplyPlanRow &row : rows) {
        if (row.blocked) {
            ++total;
        }
    }
    return total;
}

bool ApplyPlan::needsConfirm() const {
    for (const ApplyPlanRow &row : rows) {
        if (row.needsConfirm) {
            return true;
        }
    }
    return false;
}

ApplyPlan planPresetApplication(const PlanRequest &request) {
    ApplyPlan plan;

    if (request.target.instanceRoot.trimmed().isEmpty()) {
        plan.problem = QCoreApplication::translate("Presets", "未指定目标实例，无法制定应用计划。");
        return plan;
    }

    // 游戏在跑这一条是**整趟**的闸（不是某一段的事），理由见头文件那一位。
    if (request.gameRunning) {
        plan.blockedReason = plannerText("这个实例现在正在跑。游戏退出时会把它自己的那一份"
                                        "整片写回去，我们这一趟写进去的东西会被盖掉 —— "
                                        "先把它关掉再应用预设。");
    }

    QList<MalilibEntry> malilibAfter;  // 各段这一趟**之后**绑着的那些格子（见 `keyConflicts`）
    for (const PresetSection &section : request.sections) {
        ApplyPlanRow row;
        row.sectionId = section.id;
        row.kind = section.kind;
        row.title = section.title;
        row.mode = section.mode;

        if (dataPackHandlesSection(section)) {
            // 排在 `plannerKnowsKind()` 之前：数据包挂在 `Custom` 底下（见
            // `DataPackPreset.hpp`），而 `Custom` 在那个函数里是"没人接手"的意思。
            planDataPackRow(request, section, &row);
        } else if (!plannerKnowsKind(section.kind)) {
            row.blocked = true;
            row.problem = unknownKindReason(section.kind);
        } else if (optionsHandlesSection(section.kind)) {
            planOptionsRow(request, section, &row);
        } else if (serverListHandlesSection(section.kind)) {
            planServerRow(request, section, &row);
        } else if (genericFilesHandlesSection(section.kind)) {
            planGenericFilesRow(request, section, &row);
        } else if (malilibHandlesSection(section.kind)) {
            planMalilibRow(request, section, &row, &malilibAfter);
        } else {
            // 四张分派表都不认 —— 那说明"认识的种类"和"有人接手的种类"对不上了（加适配器
            // 时漏改一处）。落 `blocked` 而不是崩：这一趟少一段，用户看得见。
            row.blocked = true;
            row.problem = QCoreApplication::translate("Presets", "此部分的格式不受支持，已跳过。");
        }

        if (!row.blocked && row.mode == ApplyMode::Skip) {
            // 「跳过」那一段：适配器照样算（数出来给用户看"你这一段跳过了多少东西"），
            // 但一个字节都不会动 —— 所以"会动"的那个数清零，免得计划表上虚报。
            row.count = 0;
            row.removed = 0;
        }

        markConfirm(&row, request, section);
        plan.rows.append(row);
    }

    // 整份键位里互相抢的那些（见 `ApplyPlan::keyConflicts`：**这一段是跨模组那一种的唯一出处**）。
    plan.keyConflicts = malilibKeyConflicts(malilibAfter);

    plan.ok = true;
    return plan;
}

}  // namespace Presets
