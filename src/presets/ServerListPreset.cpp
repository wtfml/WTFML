/**
 * @file ServerListPreset.cpp
 * @brief 实现 ServerListPreset.hpp（判据 + 落到实例上）。理由和取舍都写在那边文件头。
 *
 * 【这一层一个 `QFile` 都不开】
 *
 * 读一份 `servers.dat`（认 gzip、认 NBT、看不懂就拒绝）和写回去（`QSaveFile` 原子替换）
 * 各只有一处实现 —— 在 `ServerListFile` 里，而那正是用户点"编辑 / 加 / 删"走的那一份代码
 * （见那个文件头的「为什么新开一个文件」）。这一层从它那儿拿那几口
 * （`loadDocument()` / `serversListOf()` / `storeDocument()` / `emptyDocument()` /
 * `entryNode()` / `kFreshFileGzipped`），自己再写一遍 gzip 检测或者落盘，就等于多开了一个
 * 能把用户的服务器列表写坏的入口 —— 而 `NbtUtil.hpp` 文件头那条设计决定防的正是这件事。
 *
 * `applyServerListToInstance()` 落盘时"要改哪几条"用的是 `planServerList()` 算出来的那个
 * 计划（连「覆盖」档"哪条对得上哪条"都是同一个 `matchForReplace()`）。原版设置那边
 * （`VanillaOptions`）同一条规矩：计划说添 3 条、实际就得添 3 条，做不到说明是 bug，
 * 不是"以实际为准"。
 */

#include "ServerListPreset.hpp"

#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonValue>

#include "manage/ServerListFile.hpp"
#include "util/NbtTree.hpp"
#include "util/ServerPing.hpp"

namespace Presets {

namespace {

/// 取一格字符串（那一格不在 / 不是字符串 → 空串）。
///
/// NOTE: 这几行和 `ServerListFile.cpp` 匿名命名空间里那一份**意思一样**（那边出不来）。
/// 规矩只有一条：取不到就是空串，别替它猜。
QString stringFieldOf(const NbtTree::Node &compound, const QString &name) {
    const NbtTree::Node *node = NbtTree::field(compound, name);
    if (node == nullptr || node->type != NbtTree::Type::String) {
        return QString();
    }
    return node->text;
}

/**
 * @brief 这个元素算不算一条服务器。
 *
 * 判据和读侧**必须一直是同一条**：`NbtUtil::readServerList()` 跳的正是"不是复合标签的、
 * 或者 `ip` 那一格不是字符串 / 是空串的"这些元素。不这么对齐的话，界面上第 N 行和树里
 * 第 N 个元素就是两回事 —— `ServerListFile.hpp` 文件头第 3 条纪律防的就是这件事。
 */
bool isServerEntry(const NbtTree::Node &item, NbtUtil::ServerEntry *out) {
    if (item.type != NbtTree::Type::Compound) {
        return false;
    }
    const NbtTree::Node *address = NbtTree::field(item, QStringLiteral("ip"));
    if (address == nullptr || address->type != NbtTree::Type::String || address->text.isEmpty()) {
        return false;
    }
    if (out != nullptr) {
        out->address = address->text;
        out->name = stringFieldOf(item, QStringLiteral("name"));
    }
    return true;
}

/// 目标上有没有同一个地址的那一台。
bool targetHasAddress(const QList<NbtUtil::ServerEntry> &target, const QString &address) {
    for (int i = 0; i < target.size(); ++i) {
        if (sameServerAddress(target.at(i).address, address)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 「覆盖」档里，目标上那几条各自会怎么样。
 *
 * `pickedForTarget` 和 `targetEntries` 一一对应：`>= 0` 是"认领了 `accepted` 里的这一个
 * 下标"（= 就地改名），`-1` 是"对不上，要拿掉"。`claimed` 和 `accepted` 一一对应：写的时候
 * 认领过的**就地改**、没认领的**接在表尾**。
 *
 * 计划和执行**共用这一趟**（`planServerListApply()` 数 `removed`、`applyServerListToInstance()`
 * 真的挑哪一条改，走的都是它）—— 各算一遍的话，"计划说拿掉 2 条、实际拿了 1 条"就会变成
 * 一个只有用户能发现的 bug。认过的不会第二条再认（一个地址在目标上出现两次是合法的，
 * 那一档只认头一条、第二条进 `-1`）。
 */
struct ReplaceMatch {
    QList<int> pickedForTarget;
    QList<bool> claimed;
};

ReplaceMatch matchForReplace(const QList<NbtUtil::ServerEntry> &targetEntries,
                             const QList<NbtUtil::ServerEntry> &accepted) {
    ReplaceMatch match;
    for (int i = 0; i < accepted.size(); ++i) {
        match.claimed.append(false);
    }
    for (int i = 0; i < targetEntries.size(); ++i) {
        int pick = -1;
        for (int j = 0; j < accepted.size(); ++j) {
            if (!match.claimed.at(j) && sameServerAddress(targetEntries.at(i).address,
                                                          accepted.at(j).address)) {
                pick = j;
                break;
            }
        }
        if (pick >= 0) {
            match.claimed[pick] = true;
        }
        match.pickedForTarget.append(pick);
    }
    return match;
}

}  // namespace

// ---------------------------------------------------------------------------
// 判"是不是同一台"
// ---------------------------------------------------------------------------

QString normalizeServerAddress(const QString &address) {
    const QString trimmed = address.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    const ServerPing::Address parsed = ServerPing::splitAddress(trimmed);
    if (!parsed.valid) {
        // 认不出来的**原样回去**（只去两头空白）：那多半是模组加的地址形状、或者用户手打
        // 错了一个字。替它补上 25565 换来的是一句假的"这两条是同一台"
        // （`ServerPing::splitAddress()` 那段 ★ 是同一条取舍）。
        return trimmed;
    }
    // IPv6 字面量把方括号加回去：`[::1]` 和 `[::1]:25565` 得归一成同一个串，而没加方括号的
    // 裸 `::1` 走的是上面那一档（看不懂 → 原样）—— 不把方括号加回来的话这两者会撞在一起。
    const QString host = parsed.host.contains(QLatin1Char(':'))
                             ? QStringLiteral("[") + parsed.host + QStringLiteral("]")
                             : parsed.host.toLower();
    return host + QLatin1Char(':') + QString::number(parsed.port);
}

bool sameServerAddress(const QString &left, const QString &right) {
    return normalizeServerAddress(left) == normalizeServerAddress(right);
}

// ---------------------------------------------------------------------------
// 一棵 `servers.dat` 树 → 条目
// ---------------------------------------------------------------------------

bool serverEntriesOf(NbtTree::Document *document, QList<NbtUtil::ServerEntry> *out,
                     int *untouchableOut, QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    NbtTree::Node *servers = nullptr;
    if (!ServerListFile::serversListOf(document, &servers, problemOut)) {
        return false;
    }
    int untouchable = 0;
    for (int i = 0; i < servers->items.size(); ++i) {
        NbtUtil::ServerEntry entry;
        if (isServerEntry(servers->items.at(i), &entry)) {
            if (out != nullptr) {
                out->append(entry);
            }
        } else {
            ++untouchable;
        }
    }
    if (untouchableOut != nullptr) {
        *untouchableOut = untouchable;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 计划
// ---------------------------------------------------------------------------

ServerListPlan planServerList(const QList<NbtUtil::ServerEntry> &target, int targetUntouchable,
                              const QList<NbtUtil::ServerEntry> &source, ApplyMode mode) {
    ServerListPlan plan;
    plan.targetUntouchable = targetUntouchable;
    plan.targetEntryCount = static_cast<int>(target.size());

    // ---------- 先校验源：有一条没有地址就整份不认 ----------
    for (int i = 0; i < source.size(); ++i) {
        if (source.at(i).address.trimmed().isEmpty()) {
            plan.problem = QCoreApplication::translate(
                "Presets",
                "预设中的服务器地址为空，服务器列表未修改。");
            return plan;
        }
    }

    if (mode == ApplyMode::Skip) {
        for (int i = 0; i < source.size(); ++i) {
            plan.skippedAddresses.append(source.at(i).address);
        }
        plan.ok = true;
        return plan;
    }

    if (mode == ApplyMode::Replace) {
        // 「覆盖」= 源里那些就是这张表该有的样子（目标上对得上的就地改名，见 .hpp）。
        for (int i = 0; i < source.size(); ++i) {
            plan.accepted.append(source.at(i));
            if (!targetHasAddress(target, source.at(i).address)) {
                ++plan.addedCount;
            }
        }
        plan.ok = true;
        return plan;
    }

    // ---------- 其余三档（Merge / Append / CopyIfMissing）----------
    //
    // 对一张**列表**来说这三档是同一件事（见 .hpp）：添目标上没有的、保序、按归一化去重。
    // 源里自己重复的那两条也只留头一条 —— 那一条之外的第二条在游戏里和第一条长得
    // 一模一样（同地址同名字），添进去只是让用户多一行要自己删的东西。
    for (int i = 0; i < source.size(); ++i) {
        if (targetHasAddress(target, source.at(i).address)) {
            plan.skippedAddresses.append(source.at(i).address);
            continue;
        }
        bool alreadyTaken = false;
        for (int j = 0; j < plan.accepted.size(); ++j) {
            if (sameServerAddress(plan.accepted.at(j).address, source.at(i).address)) {
                alreadyTaken = true;
                break;
            }
        }
        if (alreadyTaken) {
            plan.skippedAddresses.append(source.at(i).address);
            continue;
        }
        plan.accepted.append(source.at(i));
        ++plan.addedCount;
    }
    plan.ok = true;
    return plan;
}

ServerListReport planServerListApply(ApplyMode mode, const QList<NbtUtil::ServerEntry> &target,
                                     int targetUntouchable,
                                     const QList<NbtUtil::ServerEntry> &source,
                                     bool replaceConfirmed) {
    ServerListReport report;
    const ServerListPlan plan = planServerList(target, targetUntouchable, source, mode);
    report.skippedAddresses = plan.skippedAddresses;
    report.skipped = plan.skippedCount();
    if (!plan.ok) {
        report.problem = plan.problem;
        return report;
    }

    report.written = plan.acceptedCount();
    report.added = plan.addedCount;
    report.untouchable = plan.targetUntouchable;

    if (mode == ApplyMode::Replace) {
        // 「覆盖」真的拿掉的是"目标上有、而预设里没有同一个地址"的那几条 —— 对得上的那些是
        // **就地改名**（在 `written` 里算了），不是"拿掉再添一条"（见 .hpp 里 Replace 那条）。
        const ReplaceMatch match = matchForReplace(target, plan.accepted);
        for (int i = 0; i < match.pickedForTarget.size(); ++i) {
            if (match.pickedForTarget.at(i) < 0) {
                ++report.removed;
            }
        }
        if (!replaceConfirmed) {
            report.needsConfirm = true;
            report.problem = QCoreApplication::translate(
                "Presets",
                "覆盖将使用预设列表替换目标服务器列表，并移除只存在于目标中的条目。需要确认。");
            return report;
        }
    }

    report.ok = true;
    return report;
}

// ---------------------------------------------------------------------------
// 落到那个实例上
// ---------------------------------------------------------------------------

ServerListReport applyServerListToInstance(const QString &datPath, ApplyMode mode,
                                           const QList<NbtUtil::ServerEntry> &source,
                                           bool replaceConfirmed) {
    ServerListReport report;
    if (datPath.trimmed().isEmpty()) {
        report.problem =
            QCoreApplication::translate("Presets", "未指定要写入的 servers.dat。");
        return report;
    }

    // ---------- 读（和用户点"编辑 / 加 / 删"那三个入口走的是同一份代码） ----------
    NbtTree::Document document;
    bool gzipped = false;
    QString why;
    const ServerListFile::FileState state =
        ServerListFile::loadDocument(datPath, &document, &gzipped, &why);
    if (state == ServerListFile::FileState::Unreadable) {
        report.problem = why;
        return report;
    }

    QList<NbtUtil::ServerEntry> target;
    int untouchable = 0;
    if (state == ServerListFile::FileState::Ok) {
        if (!serverEntriesOf(&document, &target, &untouchable, &why)) {
            report.problem = why;
            return report;
        }
    }
    // `Missing` 不是失败（见 .hpp）：目标那边就是一张空表，真要写的时候才建文件。

    report = planServerListApply(mode, target, untouchable, source, replaceConfirmed);
    if (!report.ok) {
        return report;  // 含「覆盖没确认」——盘的形状一个字节都没变
    }

    // ---------- 一条都不用动就别碰盘 ----------
    //
    // （全部重复、或者 Skip、或者"目标上本来是空的而预设里也没有"都在这一档。）把一个
    // 一个字节没变的文件重写一遍，只会在用户的实例目录里留下一个变了 mtime 的文件。
    if (report.written == 0 && report.removed == 0) {
        return report;
    }

    if (state == ServerListFile::FileState::Missing) {
        document = ServerListFile::emptyDocument();
        gzipped = ServerListFile::kFreshFileGzipped;
    }
    NbtTree::Node *servers = nullptr;
    if (!ServerListFile::serversListOf(&document, &servers, &why)) {
        report.ok = false;
        report.problem = why;
        return report;
    }

    const ServerListPlan plan = planServerList(target, untouchable, source, mode);
    QList<bool> claimed;
    for (int i = 0; i < plan.accepted.size(); ++i) {
        claimed.append(false);
    }

    if (mode == ApplyMode::Replace) {
        // ---------- 就地改名 / 拿掉 ----------
        //
        // 不是"清空整张表再照预设重建"：重建会把每一条都换成只有 `ip` + `name` 两格的新
        // 节点，顺手抹掉盘上原来那些 `icon` / `hidden`（`ServerListFile.hpp` 那条"只动
        // name 和 ip 两格"的纪律）。对得上的那几条**整颗节点带着走，只改两格**。
        const ReplaceMatch match = matchForReplace(target, plan.accepted);
        claimed = match.claimed;

        NbtTree::NodeList kept;
        kept.reserve(servers->items.size());
        int cursor = 0;
        for (int i = 0; i < servers->items.size(); ++i) {
            NbtUtil::ServerEntry one;
            if (!isServerEntry(servers->items.at(i), &one)) {
                kept.append(servers->items.at(i));  // 认不出的一个都不动（**位置也照旧**）
                continue;
            }
            const int pick = cursor < match.pickedForTarget.size()
                                 ? match.pickedForTarget.at(cursor)
                                 : -1;
            ++cursor;
            if (pick < 0) {
                continue;  // 预设里没有这一台 ⇒ 拿掉
            }
            NbtTree::Node entry = servers->items.at(i);
            NbtTree::Node *addressField = NbtTree::field(&entry, QStringLiteral("ip"));
            NbtTree::Node *nameField = NbtTree::field(&entry, QStringLiteral("name"));
            if (addressField == nullptr || addressField->type != NbtTree::Type::String ||
                (nameField != nullptr && nameField->type != NbtTree::Type::String)) {
                // 形状不对（`ip` 那一格的类型 `isServerEntry()` 判过了，所以这一条只可能是
                // `name` 不是字符串）：**整份不写**。只把名字留着没改、却报"成了"，比一个
                // 明确的失败难查得多（同 `ServerListFile::updateEntry()` 那一档）。
                report.ok = false;
                report.problem = QCoreApplication::translate(
                                     "Presets",
                                     "servers.dat 中「%1」的 ip 或 name 字段不是字符串，已保留原条目。")
                                     .arg(one.address);
                return report;
            }
            addressField->text = plan.accepted.at(pick).address;
            if (nameField != nullptr) {
                nameField->text = plan.accepted.at(pick).name;  // ← 必须在 append 之前用掉
            } else {
                // 盘上可以没有 `name` 那一格（1.12.2 那种老夹具），缺了就补建一格，位置排在
                // `ip` 后面 —— 和 `ServerListFile::entryNode()` 那个形状一致。
                NbtTree::Field fresh;
                fresh.first = QStringLiteral("name");
                fresh.second.type = NbtTree::Type::String;
                fresh.second.text = plan.accepted.at(pick).name;
                entry.fields.append(fresh);
            }
            kept.append(entry);
        }
        servers->items = kept;
    }

    // ---------- 没认领的那些接在表尾 ----------
    //
    // `Append` 档全部走这儿（那儿一条也认领不到）；「覆盖」档是"目标上原来没有"的那些。
    // 接在尾巴上而不是插进中间：原版在游戏里点"添加服务器"也是接在表尾
    // （`ServerListFile::addEntry()`），而且已有的顺序是用户自己排过的。
    for (int i = 0; i < plan.accepted.size(); ++i) {
        if (claimed.at(i)) {
            continue;
        }
        servers->items.append(
            ServerListFile::entryNode(plan.accepted.at(i).name, plan.accepted.at(i).address));
    }

    if (!ServerListFile::storeDocument(datPath, document, gzipped, &why)) {
        report.ok = false;
        report.problem = why;
        return report;
    }
    return report;
}

bool serverListHandlesSection(SectionKind kind) {
    return kind == SectionKind::ServerList;
}

// ---------------------------------------------------------------------------
// 预设段 ⇄ 这一份列表
// ---------------------------------------------------------------------------

QList<PresetSection> sectionsFromServerList(const QList<NbtUtil::ServerEntry> &entries,
                                            ApplyMode mode) {
    PresetSection section;
    section.id = sectionIdFor(SectionKind::ServerList);
    section.kind = SectionKind::ServerList;
    section.mode = mode;
    for (int i = 0; i < entries.size(); ++i) {
        QJsonObject item;
        // 名字允许是空串（盘上的 `name` 真能是空串），所以这一格**照样写**、不省掉
        // （见 .hpp 那条 NOTE）——"省掉的 `name`"和"名字是空的"在读回来那一头是两句话。
        item.insert(QStringLiteral("name"), entries.at(i).name);
        item.insert(QStringLiteral("address"), entries.at(i).address);
        section.items.append(item);
    }
    QList<PresetSection> sections;
    sections.append(section);
    return sections;
}

bool serverListFromSection(const PresetSection &section, QList<NbtUtil::ServerEntry> *out,
                           QString *problemOut) {
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (!serverListHandlesSection(section.kind)) {
        if (problemOut != nullptr) {
            *problemOut =
                QCoreApplication::translate("Presets", "此段不属于服务器列表层管理范围。");
        }
        return false;
    }

    QList<NbtUtil::ServerEntry> entries;
    for (int i = 0; i < section.items.size(); ++i) {
        const QJsonValue value = section.items.at(i);
        if (!value.isObject()) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate(
                                  "Presets", "服务器列表的第 %1 项应为对象。")
                                  .arg(i + 1);
            }
            return false;
        }
        const QJsonObject item = value.toObject();
        const QJsonValue name = item.value(QStringLiteral("name"));
        const QJsonValue address = item.value(QStringLiteral("address"));
        if (!name.isString() || !address.isString()) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate(
                                  "Presets",
                                  "服务器列表第 %1 项缺少有效的 name 或 address 字段。")
                                  .arg(i + 1);
            }
            return false;
        }
        if (address.toString().trimmed().isEmpty()) {
            if (problemOut != nullptr) {
                *problemOut = QCoreApplication::translate(
                                  "Presets", "服务器列表第 %1 项的地址为空。")
                                  .arg(i + 1);
            }
            return false;
        }
        NbtUtil::ServerEntry entry;
        entry.name = name.toString();
        entry.address = address.toString();
        entries.append(entry);
    }
    if (out != nullptr) {
        *out = entries;
    }
    return true;
}

}  // namespace Presets
