/**
 * @file MalilibKeys.cpp
 * @brief 实现 MalilibKeys.hpp（认这一族 / 读一格一格 / 算这一趟 / 落盘）。理由和取舍都写在
 * 那边文件头，这一份只说三件实现上的事。
 *
 * 【一、"一格一格地写"长什么样】
 *
 * `applyCell()` 是**唯一**一处改用户配置的地方，它对一格做四件事，顺序不许颠倒：
 *
 * 1. **先判类型**（这一格是对象吗？`hotkey` 是对象吗？`enabled` 是布尔吗？）—— 判完才动手。
 * 边判边改的话，一个走到一半被判死的格子已经在盘上留了半截（"不许带半截"）。
 * **值那一档是这条规矩的例外**：它的格子天生是个标量（`"RenderLines": true`），
 * "整格必须是对象"只对热键那两档成立 —— 见 `applyCell()` 里那一处注释（套上去了的话
 * 每一个**已经存在**的值都会被判成"类型对不上"，永远搬不动）。
 * 2. **「这一格什么都不说」就整格不动**（没绑、没开关、没别的字段）：真数据里 `keys: ""`
 * 那种空格子满地都是，照着写一圈等于把目标上**所有**绑好的键解绑一遍 —— 「合并」这一档
 * 会当场变成「覆盖」。
 * 3. 写热键那一半（`keys`）、`enabled`、`extra`（**只补缺，绝不覆盖**）。
 * 4. 放回根上。
 *
 * `extra` 的方向只有一条：**源上有、目标上没有的才补**。反过来的（目标上有、源上没有）
 * 一个字不动 —— 那些是用户在这台电脑上调出来的东西，不是这一份预设能代表的。
 *
 * 【二、写回去的文件长什么样（别指望和原来逐字节一样）】
 *
 * 一份真被改了的配置文件是**整个重写**的：Qt 那把 `QJsonDocument` 的缩进和键序（`QJsonObject`
 * 自己的次序，和 MaLiLib 那一族写出来的字典序一样）落地，源文件里的注释、空行、缩进风格
 * 一概不留。这一取舍是故意的：这一族读配置是按**键名**读的，缩进和次序对它没有意义，而模组
 * 自己**下一次退出游戏时会把整份文件按它自己的样子重写一遍**，这点差别留不住。
 * 而"一个字都没改"的那些文件**根本不写**（见 `MalilibFileResult::changed`）：重写会动它的
 * mtime，而这一族有按 mtime 判"要不要重读"的用法。
 *
 * 【三、`QJsonObject` 迭代时不许插东西】
 *
 * 覆盖档那趟清扫会一边走一边改，所以它是"先把要改的收集起来、走完再一次性插回去"——
 * Qt 那些容器（`QJsonObject` / `QHash` / `QSet`）在迭代中插入是未定义行为，不是"可能没效果"。
 */

#include "MalilibKeys.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPair>
#include <QSaveFile>
#include <QSet>

#include "manage/InstanceFiles.hpp"

namespace Presets {

namespace {

/// 这一层的提示语都在同一个上下文里（和 `VanillaOptions` / `ServerListPreset` / `GenericFiles`
/// 同一个名字：`Presets`）。
[[maybe_unused]] QString presetText(const char *source) { return QCoreApplication::translate("Presets", source); }

void failWith(QString *problemOut, const QString &why) {
    if (problemOut != nullptr) {
        *problemOut = why;
    }
}

QString jsonTypeName(const QJsonValue &value) {
    switch (value.type()) {
    case QJsonValue::Null:
        return QCoreApplication::translate("Presets", "空值");
    case QJsonValue::Bool:
        return QCoreApplication::translate("Presets", "开关");
    case QJsonValue::Double:
        return QCoreApplication::translate("Presets", "数字");
    case QJsonValue::String:
        return QCoreApplication::translate("Presets", "字符串");
    case QJsonValue::Array:
        return QCoreApplication::translate("Presets", "数组");
    case QJsonValue::Object:
        return QCoreApplication::translate("Presets", "对象");
    case QJsonValue::Undefined:
        break;
    }
    return QCoreApplication::translate("Presets", "缺少此项");
}

/// `"LEFT_ALT,Z"` → `{"LEFT_ALT", "Z"}`。空串、只有逗号/空白 ⇒ 空绑定。
QStringList splitKeys(const QString &text) {
    QStringList out;
    const QStringList raw = text.split(QLatin1Char(','));
    for (const QString &one : raw) {
        const QString trimmed = one.trimmed();
        if (!trimmed.isEmpty()) {
            out.append(trimmed);
        }
    }
    return out;
}

/// 一格在盘上算哪一档（**这一层唯一的分类处**，`malilibInspect()` 和读格子都走它）。
MalilibEntryKind classifyCell(const QJsonValue &value) {
    if (!value.isObject()) {
        return MalilibEntryKind::Value;
    }
    const QJsonObject object = value.toObject();
    if (object.value(QStringLiteral("hotkey")).isObject()) {
        return MalilibEntryKind::Hotkeyed;
    }
    if (object.contains(QStringLiteral("keys"))) {
        return MalilibEntryKind::Hotkey;
    }
    return MalilibEntryKind::Value;
}

/// 顶层那一大格（`Generic` / `TweakHotkeys` ……）里的一格。
bool isCategoryValue(const QJsonValue &value) { return value.isObject(); }

/// 一份文件整读进来。
bool readWholeFile(const QString &path, QByteArray *out, QString *problemOut) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        failWith(problemOut, QCoreApplication::translate("Presets", "无法读取「%1」：%2").arg(path, file.errorString()));
        return false;
    }
    if (out != nullptr) {
        *out = file.readAll();
    }
    return true;
}

/// 一份根对象 → 一格一格。`malilibEntriesFromFile()` 和冲突分析都走这一处。
///
/// "什么都不说的格子"**不收**（见文件头第 2 条）：`{"keys": ""}` 是"这一格没绑"，把它当成
/// 一份预设的内容，等于宣布"目标上这一格也得解绑"。
QList<MalilibEntry> entriesFromRoot(const QString &modId, const QJsonObject &root,
                                    bool includeValues) {
    QList<MalilibEntry> entries;
    for (auto groupIt = root.constBegin(); groupIt != root.constEnd(); ++groupIt) {
        if (!isCategoryValue(groupIt.value())) {
            continue;  // `config_version` 那种顶层标量：不算一格（但原样留着，见 apply 那一条）
        }
        const QJsonObject group = groupIt.value().toObject();
        for (auto cellIt = group.constBegin(); cellIt != group.constEnd(); ++cellIt) {
            MalilibEntry entry;
            entry.modId = modId;
            entry.groupId = groupIt.key();
            entry.optionName = cellIt.key();

            const MalilibEntryKind kind = classifyCell(cellIt.value());
            if (kind == MalilibEntryKind::Value) {
                if (!includeValues) {
                    continue;
                }
                entry.kind = MalilibEntryKind::Value;
                entry.value = cellIt.value();
            } else {
                const QJsonObject cell = cellIt.value().toObject();
                entry.kind = kind;
                if (kind == MalilibEntryKind::Hotkeyed) {
                    entry.gesture = keyGestureFromHotkeyObject(
                        cell.value(QStringLiteral("hotkey")).toObject());
                    // 开关那一半**只有用户要"连开关一起搬"时才收**（见文件头那条：这一趟要
                    // 搬哪几样这件事只写在载荷里）。
                    if (includeValues && cell.value(QStringLiteral("enabled")).isBool()) {
                        entry.hasEnabled = true;
                        entry.enabled = cell.value(QStringLiteral("enabled")).toBool();
                    }
                    entry.extra = cell;
                    entry.extra.remove(QStringLiteral("hotkey"));
                    entry.extra.remove(QStringLiteral("enabled"));
                } else {
                    entry.gesture = keyGestureFromHotkeyObject(cell);
                    entry.extra = cell;
                    entry.extra.remove(QStringLiteral("keys"));
                }
                if (entry.gesture.isEmpty() && !entry.hasEnabled && entry.extra.isEmpty()) {
                    continue;  // 什么都不说
                }
            }
            entries.append(entry);
        }
    }
    return entries;
}

/// 一格 → 载荷里的一项（形状写在 `MalilibKeys.hpp` 那段）。
QJsonObject entryToJson(const MalilibEntry &entry) {
    QJsonObject object;
    object.insert(QStringLiteral("mod"), entry.modId);
    object.insert(QStringLiteral("group"), entry.groupId);
    object.insert(QStringLiteral("option"), entry.optionName);
    object.insert(QStringLiteral("type"), malilibEntryKindKey(entry.kind));
    if (entry.kind != MalilibEntryKind::Value) {
        // 空绑定**不写**（见文件头第 2 条）：`keys` 缺了就是"这一格的热键那一半不动"
        // （「覆盖」档例外，那儿的空 = 解绑）。
        if (!entry.gesture.keys.isEmpty()) {
            object.insert(QStringLiteral("keys"), QJsonArray::fromStringList(entry.gesture.keys));
        }
        if (!entry.gesture.trigger.isEmpty()) {
            object.insert(QStringLiteral("trigger"), entry.gesture.trigger);
        }
        if (!entry.gesture.context.isEmpty()) {
            object.insert(QStringLiteral("context"), entry.gesture.context);
        }
        if (entry.gesture.orderSensitive) {
            object.insert(QStringLiteral("orderSensitive"), true);
        }
        if (entry.gesture.allowExtraKeys) {
            object.insert(QStringLiteral("allowExtraKeys"), true);
        }
        if (entry.gesture.exclusive) {
            object.insert(QStringLiteral("exclusive"), true);
        }
    }
    if (entry.hasEnabled) {
        object.insert(QStringLiteral("enabled"), entry.enabled);
    }
    if (entry.kind == MalilibEntryKind::Value) {
        object.insert(QStringLiteral("value"), entry.value);
    }
    if (!entry.extra.isEmpty()) {
        object.insert(QStringLiteral("extra"), entry.extra);
    }
    const QJsonObject source = entry.source.toJson();
    if (!source.isEmpty()) {
        object.insert(QStringLiteral("source"), source);
    }
    return object;
}

/// 一格的结果（`applyCell()` 的产物）。
struct CellApply {
    bool written = false;      ///< 这一格放进了要写的那一份（**含"本来就是那样"的**）
    bool added = false;        ///< 其中目标上原来没有这一格
    bool unchanged = false;    ///< 其中内容本来就一模一样（这一趟没改它）
    bool skipped = false;      ///< 这一趟按某一条规矩不动它
    bool typeMismatch = false; ///< 类型对不上，**拒绝转换**
    bool unbound = false;      ///< 「覆盖」档解绑了目标上本来绑着的这一格
    bool modified = false;     ///< 根上真改了字节（决定这一份要不要落盘）
};

/**
 * @brief 把一格写进那份根上（这一层**唯一**一处在用户的配置上动手的地方）。
 *
 * 分支顺序见文件头第 1 条：所有"拒绝"都在第一次改动**之前**。
 */
CellApply applyCell(QJsonObject &root, const MalilibEntry &entry, ApplyMode mode,
                    QStringList *details) {
    CellApply result;

    // ---- ① 目标上这一格现在是什么样 ----
    const QString optionKey = entry.optionName;
    const QJsonValue groupValue = root.value(entry.groupId);
    if (!groupValue.isUndefined() && !groupValue.isObject()) {
        details->append(QCoreApplication::translate("Presets", "「%1」中，目标「%2」的类型为%3，已保留原值。")
                            .arg(entry.label(), entry.groupId, jsonTypeName(groupValue)));
        result.typeMismatch = true;
        return result;
    }
    QJsonObject group = groupValue.toObject();
    const bool cellPresent = group.contains(optionKey);
    const QJsonValue cellValue = group.value(optionKey);

    // 「缺了才写」这一档按**格**算（和原版设置那一层同一把尺子：目标上已经有这一格就一个字
    // 不动）—— 要"整份配置在目标上还没有的时候照搬一份"是通用文件组的活，不是这一层。
    if (mode == ApplyMode::CopyIfMissing && cellPresent) {
        result.skipped = true;
        return result;
    }

    // ---- ② 值那一格（没有热键那一半，单独一条路）----
    //
    // **它排在"整格必须是个对象"那条规矩前面**（下面那一处判的是热键那两档的事）：
    // 这一档的格子天生就是个标量（`"RenderLines": true`），套上"整格必须是对象"的话
    // 每一个**已经存在**的值都会被判成"类型对不上" —— 用户勾了「连设置一起搬」也永远
    // 搬不动（目标上没有的那几格倒进得去，改已有的进不去）。类型对不对这一档自己判。
    if (entry.kind == MalilibEntryKind::Value) {
        if (entry.value.isUndefined()) {
            details->append(QCoreApplication::translate("Presets", "「%1」：此项载荷无值，已跳过。").arg(entry.label()));
            result.skipped = true;
            return result;
        }
        const bool absent = !cellPresent;
        if (absent && mode != ApplyMode::CopyIfMissing) {
            details->append(QCoreApplication::translate("Presets", "目标中缺少「%1」，已跳过。请检查模组版本是否一致。")
                                .arg(entry.label()));
            result.skipped = true;
            return result;
        }
        if (!absent && mode == ApplyMode::Append) {
            result.skipped = true;  // 「别抢我已经调好的」
            return result;
        }
        if (!absent && cellValue.type() != entry.value.type()) {
            details->append(QCoreApplication::translate("Presets", "「%1」的类型不匹配。目标为%2，预设为%3，已跳过。")
                                .arg(entry.label(), jsonTypeName(cellValue),
                                     jsonTypeName(entry.value)));
            result.typeMismatch = true;
            return result;
        }
        if (!absent && cellValue == entry.value) {
            result.written = true;
            result.unchanged = true;
            return result;
        }
        group.insert(optionKey, entry.value);
        root.insert(entry.groupId, group);
        result.written = true;
        result.added = absent;
        result.modified = true;
        return result;
    }

    // ---- ①-b 热键那两档：目标上这一格**得是个对象**（`{"keys": …}` 或者 `{"enabled": …,
    // "hotkey": …}`）----
    if (cellPresent && !cellValue.isObject()) {
        details->append(presetText("「%1」：目标上那一格是个%2（不是一格对象），换成我们的样子"
                                   "就是改类型，没动它。")
                            .arg(entry.label(), jsonTypeName(cellValue)));
        result.typeMismatch = true;
        return result;
    }
    QJsonObject cell = cellValue.toObject();

    // ---- ③ 热键那一半装在哪儿（`Hotkeyed` 装在这一格的 `hotkey` 里）----
    const bool hotkeyed = entry.kind == MalilibEntryKind::Hotkeyed;
    QJsonObject holder = cell;
    if (hotkeyed) {
        const QJsonValue hotkeyValue = cell.value(QStringLiteral("hotkey"));
        if (cellPresent && !hotkeyValue.isUndefined() && !hotkeyValue.isObject()) {
            details->append(presetText("「%1」：目标上这一格的 hotkey 是个%2，不是一格热键，"
                                       "没动它。")
                                .arg(entry.label(), jsonTypeName(hotkeyValue)));
            result.typeMismatch = true;
            return result;
        }
        holder = hotkeyValue.toObject();
    }

    const QString keysString = entry.gesture.toKeysString();
    const QJsonValue targetKeysValue = holder.value(QStringLiteral("keys"));
    const bool targetHasKeys = targetKeysValue.isString();
    const QString targetKeys = targetKeysValue.toString();
    const bool targetBound = targetHasKeys && !targetKeys.trimmed().isEmpty();
    const bool wantBind = !keysString.isEmpty();

    if (mode == ApplyMode::Append && targetBound) {
        result.skipped = true;  // 「别抢我已经绑好的」
        return result;
    }
    // 「覆盖」档：预设里这一格没绑 ⇒ 目标上得解绑（完整替换）。别的档：没绑就是"不动"。
    const bool wantUnbind = !wantBind && mode == ApplyMode::Replace && targetBound;

    // ---- ④ 拒绝那些（都在改动之前）----
    if (entry.hasEnabled) {
        const QJsonValue enabledValue = cell.value(QStringLiteral("enabled"));
        if (!enabledValue.isUndefined() && !enabledValue.isBool()) {
            details->append(presetText("「%1」：目标上那一格的 enabled 是个%2，不是开关，"
                                       "没动它。")
                                .arg(entry.label(), jsonTypeName(enabledValue)));
            result.typeMismatch = true;
            return result;
        }
        if (cellPresent && enabledValue.isUndefined()) {
            // 这一档要的是"连开关一起搬"：搬不了开关就整格不动（半搬比不搬更难解释，
            // 用户看到"热键搬过去了、开关没搬"只会以为我们搞坏了）。
            details->append(presetText("「%1」：目标那一格没有 enabled 这一项，这一格整格跳过"
                                       "（要的是连开关一起搬，搬不了开关就不搬这一格）。")
                                .arg(entry.label()));
            result.skipped = true;
            return result;
        }
    }

    if (!wantBind && !wantUnbind && !entry.hasEnabled && entry.extra.isEmpty()) {
        result.skipped = true;  // 这一格什么都不说
        return result;
    }

    // ---- ⑤ 写 ----
    bool modified = false;
    bool holderModified = false;
    if (wantBind) {
        if (!targetHasKeys || targetKeys != keysString) {
            holder.insert(QStringLiteral("keys"), keysString);
            modified = true;
            holderModified = true;
        }
    } else if (wantUnbind) {
        holder.insert(QStringLiteral("keys"), QString());
        modified = true;
        holderModified = true;
        result.unbound = true;
    }
    if (entry.hasEnabled) {
        const QJsonValue enabledValue = cell.value(QStringLiteral("enabled"));
        if (!enabledValue.isBool() || enabledValue.toBool() != entry.enabled) {
            cell.insert(QStringLiteral("enabled"), entry.enabled);
            modified = true;
        }
    }
    // `extra`：**只补缺**（见文件头第 1 条）。`Hotkeyed` 的那几样兄弟落在这**一格**上，
    // `Hotkey` 的那几样本事就长在 holder（= 这一格）上 —— 两条路在这儿合上。
    QJsonObject &extraTarget = hotkeyed ? cell : holder;
    for (auto it = entry.extra.constBegin(); it != entry.extra.constEnd(); ++it) {
        if (!extraTarget.contains(it.key())) {
            extraTarget.insert(it.key(), it.value());
            modified = true;
            if (!hotkeyed) {
                holderModified = true;
            }
        }
    }
    if (hotkeyed) {
        if (holderModified) {
            cell.insert(QStringLiteral("hotkey"), holder);
        }
        group.insert(optionKey, cell);
    } else {
        group.insert(optionKey, holder);
    }
    root.insert(entry.groupId, group);

    result.written = true;
    result.added = !cellPresent;
    result.unchanged = !modified;
    result.modified = modified;
    return result;
}

/// 「覆盖」档那一趟清扫的结果。
struct ReplaceSweep {
    int removed = 0;   ///< 解绑了几格
    int untouched = 0; ///< 认不出来 / 是值、所以没动的有几格
};

/// 「覆盖」档：目标上**没被预设点名**的热键全解绑。
///
/// 只解绑、**不删格子**（见 `MalilibKeys.hpp` 文件头）：删掉那一格，模组会按它代码里的
/// 默认值重建，用户看到的就不是"这个键被解绑了"。
/// 值那一格和认不出来是什么的那些格子**一个都不动**（`untouched` 数着）。
ReplaceSweep sweepUnboundHotkeys(QJsonObject &root, const QSet<QPair<QString, QString>> &named) {
    ReplaceSweep sweep;
    QList<QPair<QString, QJsonObject>> touched;
    for (auto groupIt = root.constBegin(); groupIt != root.constEnd(); ++groupIt) {
        if (!isCategoryValue(groupIt.value())) {
            continue;
        }
        const QJsonObject group = groupIt.value().toObject();
        QList<QPair<QString, QJsonObject>> changes;
        for (auto cellIt = group.constBegin(); cellIt != group.constEnd(); ++cellIt) {
            if (named.contains(qMakePair(groupIt.key(), cellIt.key()))) {
                continue;  // 预设点名的那几格：已经写过了，不再碰
            }
            if (!cellIt.value().isObject()) {
                ++sweep.untouched;
                continue;
            }
            QJsonObject cell = cellIt.value().toObject();
            const bool hotkeyed = cell.value(QStringLiteral("hotkey")).isObject();
            QJsonObject holder = hotkeyed ? cell.value(QStringLiteral("hotkey")).toObject() : cell;
            const QJsonValue keysValue = holder.value(QStringLiteral("keys"));
            if (!keysValue.isString()) {
                ++sweep.untouched;  // 认不出来是什么：不动
                continue;
            }
            if (keysValue.toString().trimmed().isEmpty()) {
                continue;  // 本来就没绑
            }
            holder.insert(QStringLiteral("keys"), QString());
            QJsonObject updated = holder;
            if (hotkeyed) {
                updated = cell;
                updated.insert(QStringLiteral("hotkey"), holder);
            }
            changes.append(qMakePair(cellIt.key(), updated));
            ++sweep.removed;
        }
        if (changes.isEmpty()) {
            continue;
        }
        QJsonObject edited = group;
        for (const QPair<QString, QJsonObject> &change : changes) {
            edited.insert(change.first, change.second);
        }
        touched.append(qMakePair(groupIt.key(), edited));
    }
    for (const QPair<QString, QJsonObject> &change : touched) {
        root.insert(change.first, change.second);
    }
    return sweep;
}

}  // namespace

// ---------------------------------------------------------------------------
// 认这一族：看结构，不看名字
// ---------------------------------------------------------------------------

MalilibFileFacts malilibInspect(const QByteArray &bytes) {
    MalilibFileFacts facts;

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        facts.problem = QCoreApplication::translate("Presets", "JSON 格式无效（%1，字节偏移 %2）。")
                            .arg(error.errorString())
                            .arg(error.offset);
        return facts;
    }
    if (!doc.isObject()) {
        facts.problem = QCoreApplication::translate("Presets", "MaLiLib 配置的顶层必须是 JSON 对象。");
        return facts;
    }

    const QJsonObject root = doc.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        if (!isCategoryValue(it.value())) {
            continue;  // `config_version` 那种标量不算一格（但原样留着）
        }
        ++facts.categories;
        const QJsonObject group = it.value().toObject();
        for (auto cell = group.constBegin(); cell != group.constEnd(); ++cell) {
            switch (classifyCell(cell.value())) {
            case MalilibEntryKind::Hotkey:
                ++facts.hotkeys;
                break;
            case MalilibEntryKind::Hotkeyed:
                ++facts.hotkeyedValues;
                break;
            case MalilibEntryKind::Value:
                ++facts.values;
                break;
            }
        }
    }

    // 判据：顶层是"一大格一大格"的、而且**至少有一格是热键**（整格是热键，或者一格装
    // 开关 + 热键）。光有顶层对象不够 —— 那会把一大堆别的模组的 json 都圈进来。
    facts.recognized = facts.categories > 0 && (facts.hotkeys + facts.hotkeyedValues) > 0;
    if (!facts.recognized) {
        facts.problem = QCoreApplication::translate("Presets", "未找到快捷键字段，无法识别为 MaLiLib 配置。");
    }
    return facts;
}

bool malilibLooksLikeConfig(const QByteArray &bytes) { return malilibInspect(bytes).recognized; }

// ---------------------------------------------------------------------------
// 键位那一张（DTO）
// ---------------------------------------------------------------------------

QString KeyGesture::toKeysString() const {
    // 拼法只有这一处：逗号分隔、不加空格（真数据就是这么写的）。
    return keys.join(QLatin1Char(','));
}

KeyGesture KeyGesture::fromKeysString(const QString &text) {
    KeyGesture gesture;
    gesture.keys = splitKeys(text);
    return gesture;
}

KeyGesture keyGestureFromHotkeyObject(const QJsonObject &hotkey) {
    KeyGesture gesture;
    if (hotkey.value(QStringLiteral("keys")).isString()) {
        gesture.keys = splitKeys(hotkey.value(QStringLiteral("keys")).toString());
    }
    gesture.trigger = hotkey.value(QStringLiteral("trigger")).toString();
    gesture.context = hotkey.value(QStringLiteral("context")).toString();
    gesture.orderSensitive = hotkey.value(QStringLiteral("orderSensitive")).toBool();
    gesture.allowExtraKeys = hotkey.value(QStringLiteral("allowExtraKeys")).toBool();
    gesture.exclusive = hotkey.value(QStringLiteral("exclusive")).toBool();
    return gesture;
}

bool keyGesturesEqual(const KeyGesture &left, const KeyGesture &right) {
    // 空绑定之间不算"相同"（两个都没绑不是"抢同一个键位"）。
    if (left.isEmpty() || right.isEmpty()) {
        return false;
    }
    return left.keys == right.keys && left.trigger == right.trigger &&
           left.context == right.context && left.orderSensitive == right.orderSensitive &&
           left.allowExtraKeys == right.allowExtraKeys && left.exclusive == right.exclusive;
}

// ---------------------------------------------------------------------------
// 那一格的种类 / 来源版本
// ---------------------------------------------------------------------------

QString malilibEntryKindKey(MalilibEntryKind kind) {
    // 这是**盘上的词**（改一个词就是改格式），和 `sectionKindKey()` 那条同一个规矩：不翻译。
    switch (kind) {
    case MalilibEntryKind::Hotkey:
        return QStringLiteral("hotkey");
    case MalilibEntryKind::Hotkeyed:
        return QStringLiteral("hotkeyed");
    case MalilibEntryKind::Value:
        break;
    }
    return QStringLiteral("value");
}

bool malilibEntryKindFromKey(const QString &key, MalilibEntryKind *out) {
    if (key == QStringLiteral("hotkey")) {
        if (out != nullptr) {
            *out = MalilibEntryKind::Hotkey;
        }
        return true;
    }
    if (key == QStringLiteral("hotkeyed")) {
        if (out != nullptr) {
            *out = MalilibEntryKind::Hotkeyed;
        }
        return true;
    }
    if (key == QStringLiteral("value")) {
        if (out != nullptr) {
            *out = MalilibEntryKind::Value;
        }
        return true;
    }
    return false;
}

QJsonObject MalilibSourceVersions::toJson() const {
    QJsonObject object;
    // 空的那些**根本不写**（见头文件那条：不知道的字段写个空串，读的人会以为那是答案）。
    if (!minecraft.isEmpty()) {
        object.insert(QStringLiteral("minecraft"), minecraft);
    }
    if (!loader.isEmpty()) {
        object.insert(QStringLiteral("loader"), loader);
    }
    if (!mod.isEmpty()) {
        object.insert(QStringLiteral("mod"), mod);
    }
    if (!malilib.isEmpty()) {
        object.insert(QStringLiteral("malilib"), malilib);
    }
    return object;
}

MalilibSourceVersions MalilibSourceVersions::fromJson(const QJsonObject &object) {
    MalilibSourceVersions versions;
    versions.minecraft = object.value(QStringLiteral("minecraft")).toString();
    versions.loader = object.value(QStringLiteral("loader")).toString();
    versions.mod = object.value(QStringLiteral("mod")).toString();
    versions.malilib = object.value(QStringLiteral("malilib")).toString();
    return versions;
}

QString MalilibEntry::label() const {
    // 身份那三样的拼法只有这一处（不用翻译名、不用行号）。
    return modId + QLatin1Char('/') + groupId + QLatin1Char('/') + optionName;
}

// ---------------------------------------------------------------------------
// 文件 / 段 ⇄ 一格一格
// ---------------------------------------------------------------------------

QString malilibConfigRelativePath(const QString &modId) {
    return QStringLiteral("config/") + modId + QStringLiteral(".json");
}

QString malilibModIdProblem(const QString &modId) {
    if (modId.isEmpty()) {
        return QCoreApplication::translate("Presets", "模组名称为空。");
    }
    // 用的是实例名那一套规矩（Windows 的禁区、`.` / `..`、保留设备名、结尾的点或空格）——
    // 那一条只有一处实现，不另立一套。
    const QString why = InstanceFiles::validatePathSegment(modId);
    if (!why.isEmpty()) {
        return QCoreApplication::translate("Presets", "此模组名称不能作为文件名：%1").arg(why);
    }
    return QString();
}

bool malilibEntriesFromFile(const QString &modId, const QByteArray &bytes,
                            QList<MalilibEntry> *out, QString *problemOut, bool includeValues) {
    const QString modProblem = malilibModIdProblem(modId);
    if (!modProblem.isEmpty()) {
        failWith(problemOut, QCoreApplication::translate("Presets", "「%1」：%2").arg(modId, modProblem));
        return false;
    }
    const MalilibFileFacts facts = malilibInspect(bytes);
    if (!facts.recognized) {
        // 认不出来 = "我们不知道拿它怎么办"，**不是**"这一份是空的"：两种情况的后果差得远
        // （照着写下去就是拿一份半懂的文件去改用户的实例）。
        failWith(problemOut, QCoreApplication::translate("Presets", "「%1」：%2").arg(malilibConfigRelativePath(modId),
                                                        facts.problem));
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(bytes).object();
    if (out != nullptr) {
        *out = entriesFromRoot(modId, root, includeValues);
    }
    return true;
}

QString malilibSectionId(const QString &modId) {
    // `sectionIdFor()` 是 UUID v5（**单向**：算得出来、反推不回去），所以段的身份是"按模组名
    // 算出来的"，不是"从 id 里读出来的" —— 读一份预设的时候要按载荷里那个模组名再算一遍，
    // 对不上就是这一份被谁动过（`malilibEntriesFromSections()` 里那一条）。
    return sectionIdFor(SectionKind::MalilibKeys, modId);
}

bool malilibHandlesSection(SectionKind kind) { return kind == SectionKind::MalilibKeys; }

QList<PresetSection> sectionsFromMalilibEntries(const QList<MalilibEntry> &entries,
                                                ApplyMode mode) {
    QList<PresetSection> sections;
    QStringList modOrder;            // 一段一个模组，和 sections 一一对应（保序）
    QHash<QString, int> indexOfMod;  // 模组名 → sections 里那一段的下标

    for (const MalilibEntry &entry : entries) {
        int at = indexOfMod.value(entry.modId, -1);
        if (at < 0) {
            PresetSection section;
            section.id = malilibSectionId(entry.modId);
            section.kind = SectionKind::MalilibKeys;
            section.mode = mode;
            sections.append(section);
            modOrder.append(entry.modId);
            at = sections.size() - 1;
            indexOfMod.insert(entry.modId, at);
        }
        sections[at].items.append(entryToJson(entry));
        // 标题按**装了什么**定（一个模组一段，一段要么是热键、要么连设置一起）。
        if (entry.kind == MalilibEntryKind::Value) {
            sections[at].title = QCoreApplication::translate("Presets", "%1 的配置").arg(entry.modId);
        } else if (sections[at].title.isEmpty()) {
            sections[at].title = QCoreApplication::translate("Presets", "%1 的热键").arg(entry.modId);
        }
    }
    return sections;
}

bool malilibEntriesFromSections(const QList<PresetSection> &sections, QList<MalilibEntry> *out,
                                QString *problemOut) {
    QList<MalilibEntry> entries;
    for (const PresetSection &section : sections) {
        if (!malilibHandlesSection(section.kind)) {
            failWith(problemOut, QCoreApplication::translate("Presets", "此段（%1）不属于 MaLiLib 层管理范围。")
                                     .arg(sectionKindKey(section.kind)));
            return false;
        }
        QString modId;
        for (int i = 0; i < section.items.size(); ++i) {
            const QJsonValue value = section.items.at(i);
            if (!value.isObject()) {
                failWith(problemOut, QCoreApplication::translate("Presets", "MaLiLib 配置的第 %1 项应为对象。")
                                         .arg(i + 1));
                return false;
            }
            const QJsonObject item = value.toObject();

            const QJsonValue modValue = item.value(QStringLiteral("mod"));
            if (!modValue.isString() || modValue.toString().isEmpty()) {
                failWith(problemOut, presetText("MaLiLib 那一段里第 %1 项缺了 mod（或者它不是"
                                                "一个字符串）。")
                                         .arg(i + 1));
                return false;
            }
            const QString itemMod = modValue.toString();
            const QString modProblem = malilibModIdProblem(itemMod);
            if (!modProblem.isEmpty()) {
                failWith(problemOut, QCoreApplication::translate("Presets", "MaLiLib 配置第 %1 项的模组名称无效：%2")
                                         .arg(i + 1)
                                         .arg(modProblem));
                return false;
            }
            if (modId.isEmpty()) {
                modId = itemMod;
            } else if (modId != itemMod) {
                // 一段 = 一份文件（`config/<modId>.json`）：一段里两个模组，那这一段就没有
                // "落到哪儿"这回事了。
                failWith(problemOut, presetText("MaLiLib 那一段里第 %1 项是「%2」，而前面那些是"
                                                "「%3」—— 一段只能装一个模组。")
                                         .arg(i + 1)
                                         .arg(itemMod)
                                         .arg(modId));
                return false;
            }

            MalilibEntryKind kind = MalilibEntryKind::Value;
            if (!malilibEntryKindFromKey(item.value(QStringLiteral("type")).toString(), &kind)) {
                failWith(problemOut, QCoreApplication::translate("Presets", "MaLiLib 配置第 %1 项的 type 无法识别（%2）。")
                                         .arg(i + 1)
                                         .arg(item.value(QStringLiteral("type")).toString()));
                return false;
            }

            const QJsonValue groupValue = item.value(QStringLiteral("group"));
            const QJsonValue optionValue = item.value(QStringLiteral("option"));
            if (!groupValue.isString() || groupValue.toString().isEmpty() ||
                !optionValue.isString() || optionValue.toString().isEmpty()) {
                failWith(problemOut, presetText("MaLiLib 那一段里第 %1 项缺了 group 或者 option"
                                                "（或者它们不是字符串）。")
                                         .arg(i + 1));
                return false;
            }

            MalilibEntry entry;
            entry.modId = itemMod;
            entry.groupId = groupValue.toString();
            entry.optionName = optionValue.toString();
            entry.kind = kind;

            if (kind != MalilibEntryKind::Value) {
                if (item.contains(QStringLiteral("keys"))) {
                    const QJsonValue keysValue = item.value(QStringLiteral("keys"));
                    if (!keysValue.isArray()) {
                        failWith(problemOut,
                                 QCoreApplication::translate("Presets", "MaLiLib 配置第 %1 项的 keys 字段类型无效。")
                                     .arg(i + 1));
                        return false;
                    }
                    for (const QJsonValue &one : keysValue.toArray()) {
                        if (!one.isString()) {
                            failWith(problemOut,
                                     presetText("MaLiLib 那一段里第 %1 项的 keys 里有一项不是"
                                                "字符串。")
                                         .arg(i + 1));
                            return false;
                        }
                        entry.gesture.keys.append(one.toString());
                    }
                }
                entry.gesture.trigger = item.value(QStringLiteral("trigger")).toString();
                entry.gesture.context = item.value(QStringLiteral("context")).toString();
                entry.gesture.orderSensitive =
                    item.value(QStringLiteral("orderSensitive")).toBool();
                entry.gesture.allowExtraKeys =
                    item.value(QStringLiteral("allowExtraKeys")).toBool();
                entry.gesture.exclusive = item.value(QStringLiteral("exclusive")).toBool();
            }

            if (item.contains(QStringLiteral("enabled"))) {
                const QJsonValue enabledValue = item.value(QStringLiteral("enabled"));
                if (!enabledValue.isBool()) {
                    failWith(problemOut, presetText("MaLiLib 那一段里第 %1 项的 enabled 不是"
                                                    "开关。")
                                             .arg(i + 1));
                    return false;
                }
                entry.hasEnabled = true;
                entry.enabled = enabledValue.toBool();
            }

            if (kind == MalilibEntryKind::Value) {
                const QJsonValue valueOfItem = item.value(QStringLiteral("value"));
                if (valueOfItem.isUndefined()) {
                    failWith(problemOut, presetText("MaLiLib 那一段里第 %1 项是值那一档，却没有"
                                                    "value。")
                                             .arg(i + 1));
                    return false;
                }
                entry.value = valueOfItem;
            }

            if (item.contains(QStringLiteral("extra"))) {
                const QJsonValue extraValue = item.value(QStringLiteral("extra"));
                if (!extraValue.isObject()) {
                    failWith(problemOut,
                             QCoreApplication::translate("Presets", "MaLiLib 配置第 %1 项的 extra 字段应为对象。")
                                 .arg(i + 1));
                    return false;
                }
                entry.extra = extraValue.toObject();
            }

            entry.source =
                MalilibSourceVersions::fromJson(item.value(QStringLiteral("source")).toObject());
            entries.append(entry);
        }

        // 段的身份对一次（见 `malilibSectionId()`）：写的时候是按模组名算的，读回来再算一遍，
        // 对不上说明这一份被谁动过。
        if (!section.id.isEmpty() && !modId.isEmpty() &&
            section.id != malilibSectionId(modId)) {
            failWith(problemOut, presetText("MaLiLib 那一段的身份对不上（段 id 和里面的模组名"
                                            "「%1」不是一对），不敢照着它改实例。")
                                     .arg(modId));
            return false;
        }
    }
    if (out != nullptr) {
        *out = entries;
    }
    return true;
}

QStringList malilibModIdsInInstance(const QString &instanceRoot, QStringList *skippedOut) {
    QStringList modIds;
    QDir dir(QDir(instanceRoot).filePath(QStringLiteral("config")));
    if (!dir.exists()) {
        return modIds;
    }
    const QFileInfoList files = dir.entryInfoList(QStringList{QStringLiteral("*.json")},
                                                  QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &info : files) {
        const QString modId = info.completeBaseName();
        const QString modProblem = malilibModIdProblem(modId);
        if (!modProblem.isEmpty()) {
            if (skippedOut != nullptr) {
                *skippedOut << QCoreApplication::translate("Presets", "「%1」：%2").arg(info.fileName(), modProblem);
            }
            continue;
        }
        QByteArray bytes;
        QString why;
        if (!readWholeFile(info.absoluteFilePath(), &bytes, &why)) {
            if (skippedOut != nullptr) {
                *skippedOut << QCoreApplication::translate("Presets", "「%1」：%2").arg(info.fileName(), why);
            }
            continue;
        }
        // 认不出来的**不算问题**：`config/` 底下大多数模组的 json 都不是这一族（它们各有
        // 各的格式），一家一家报出来只会把真正读不动的那几份淹掉。
        if (!malilibLooksLikeConfig(bytes)) {
            continue;
        }
        modIds.append(modId);
    }
    return modIds;
}

// ---------------------------------------------------------------------------
// 冲突
// ---------------------------------------------------------------------------

QList<MalilibConflict> malilibKeyConflicts(const QList<MalilibEntry> &entries) {
    /// 一根已经开着的"这一组键位"。
    struct Group {
        KeyGesture gesture;
        QString keys;
        QStringList claims;
    };
    QList<Group> groups;
    QHash<QString, QList<int>> byKeys;  // 键位串 → groups 里的下标

    for (const MalilibEntry &entry : entries) {
        if (entry.kind == MalilibEntryKind::Value || entry.gesture.isEmpty()) {
            continue;  // 没绑的不参与（"两个都没绑"不是抢）
        }
        const QString key = entry.gesture.toKeysString();
        int found = -1;
        const QList<int> candidates = byKeys.value(key);
        for (const int candidate : candidates) {
            if (keyGesturesEqual(groups.at(candidate).gesture, entry.gesture)) {
                found = candidate;
                break;
            }
        }
        if (found < 0) {
            Group group;
            group.gesture = entry.gesture;
            group.keys = key;
            group.claims.append(entry.label());
            groups.append(group);
            byKeys[key].append(groups.size() - 1);
        } else {
            groups[found].claims.append(entry.label());
        }
    }

    QList<MalilibConflict> conflicts;
    for (const Group &group : groups) {
        if (group.claims.size() < 2) {
            continue;  // 只有一条：那不是冲突
        }
        MalilibConflict conflict;
        conflict.keys = group.keys;
        conflict.claims = group.claims;
        conflicts.append(conflict);
    }
    return conflicts;
}

// ---------------------------------------------------------------------------
// 计划与执行
// ---------------------------------------------------------------------------

const MalilibTargetFile *MalilibTarget::find(const QString &modId) const {
    for (const MalilibTargetFile &file : files) {
        if (file.modId == modId) {
            return &file;
        }
    }
    return nullptr;
}

MalilibTarget malilibReadTarget(const QString &instanceRoot, const QStringList &modIds,
                                QString *problemOut) {
    MalilibTarget target;
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    if (instanceRoot.trimmed().isEmpty()) {
        // 没给实例目录就往下走的话，`config/x.json` 会变成一个**相对路径** —— 读的是当前
        // 目录、写的也是当前目录。这一条必须是"整趟不办"，不是"那几份当作不存在"。
        failWith(problemOut, QCoreApplication::translate("Presets", "未指定实例目录，无法读取目标配置。"));
        return target;
    }
    for (const QString &modId : modIds) {
        MalilibTargetFile file;
        file.modId = modId;

        const QString modProblem = malilibModIdProblem(modId);
        if (!modProblem.isEmpty()) {
            file.problem = modProblem;
            target.files.append(file);
            continue;
        }
        const QString relativePath = malilibConfigRelativePath(modId);
        if (InstanceFiles::pathHasLinkComponent(instanceRoot, relativePath)) {
            file.problem =
                QCoreApplication::translate("Presets", "「%1」路径包含链接目录（junction / 符号链接），未处理。")
                    .arg(relativePath);
            target.files.append(file);
            continue;
        }
        const QString path = QDir(instanceRoot).filePath(relativePath);
        const QFileInfo info(path);
        if (!info.exists()) {
            // 没有这一份**不是问题**（`CopyIfMissing` 就靠这个）：`present = false`。
            target.files.append(file);
            continue;
        }
        QByteArray bytes;
        QString why;
        if (!readWholeFile(path, &bytes, &why)) {
            // 读不动 ⇒ `problem` 非空、`present` 留 false：这一份这一趟整个跳过。
            // "读不出来"和"没有这一份"是两件事，混成一件会让「缺了才写」照着一个读不动的
            // 文件往下写。
            file.problem = why;
            target.files.append(file);
            continue;
        }
        file.present = true;
        file.bytes = bytes;
        const MalilibFileFacts facts = malilibInspect(bytes);
        if (!facts.recognized) {
            file.problem = QCoreApplication::translate("Presets", "「%1」：%2").arg(malilibConfigRelativePath(modId),
                                                      facts.problem);
        }
        target.files.append(file);
    }
    return target;
}

MalilibApplyResult applyMalilibEntries(const QList<MalilibEntry> &entries,
                                       const MalilibTarget &target, ApplyMode mode,
                                       bool replaceConfirmed) {
    MalilibApplyResult result;
    MalilibApplyReport &report = result.report;

    // ---- ① 「覆盖」要用户明确确认过才动手（和原版设置那一层同一个形状）----
    if (mode == ApplyMode::Replace && !replaceConfirmed) {
        report.needsConfirm = true;
        report.problem = QCoreApplication::translate("Presets", "覆盖会解除目标配置中其余快捷键的绑定，需要确认。");
        return result;
    }

    // ---- ② 按模组分趟（一份文件一趟）----
    QList<QString> order;
    QHash<QString, QList<const MalilibEntry *>> byMod;
    for (const MalilibEntry &entry : entries) {
        if (!byMod.contains(entry.modId)) {
            order.append(entry.modId);
            byMod.insert(entry.modId, {});
        }
        byMod[entry.modId].append(&entry);
    }

    QList<MalilibEntry> afterAll;  // 这一趟之后各份文件里绑着的那些（用来算冲突）
    for (const QString &modId : order) {
        const QList<const MalilibEntry *> group = byMod.value(modId);

        if (mode == ApplyMode::Skip) {
            report.skipped += static_cast<int>(group.size());
            continue;
        }
        if (modId.isEmpty()) {
            report.skipped += static_cast<int>(group.size());
            report.details << QCoreApplication::translate("Presets", "有一条缺少模组名称，整段已跳过。");
            continue;
        }

        const MalilibTargetFile *file = target.find(modId);
        if (file != nullptr && !file->problem.isEmpty()) {
            report.skipped += static_cast<int>(group.size());
            report.details << QCoreApplication::translate("Presets", "已跳过「%1」：%2").arg(modId, file->problem);
            continue;
        }
        const bool present = file != nullptr && file->present;

        // ---- 这一份要写成什么样 ----
        QJsonObject root;
        if (present) {
            root = QJsonDocument::fromJson(file->bytes).object();
        }
        const QJsonObject before = root;

        QSet<QPair<QString, QString>> named;
        if (mode == ApplyMode::Replace) {
            for (const MalilibEntry *entry : group) {
                if (entry->kind != MalilibEntryKind::Value) {
                    named.insert(qMakePair(entry->groupId, entry->optionName));
                }
            }
        }

        for (const MalilibEntry *entry : group) {
            const CellApply applied = applyCell(root, *entry, mode, &report.details);
            if (applied.typeMismatch) {
                ++report.typeMismatch;
            } else if (applied.skipped) {
                ++report.skipped;
            } else if (applied.written) {
                ++report.written;
                if (applied.added) {
                    ++report.added;
                }
                if (applied.unchanged) {
                    ++report.unchanged;
                }
                if (applied.unbound) {
                    // 「覆盖」档里预设点名、而且是**没绑**的那一格：目标上那一格也是解绑。
                    ++report.removed;
                }
            } else {
                // 走到这儿说明 `applyCell()` 有一档没归上号 —— 算"没动"，但报出来。
                ++report.skipped;
                report.details << QCoreApplication::translate("Presets", "「%1」未匹配到配置项，已保留原值。")
                                      .arg(entry->label());
            }
        }

        if (mode == ApplyMode::Replace) {
            const ReplaceSweep sweep = sweepUnboundHotkeys(root, named);
            report.removed += sweep.removed;
            report.untouchable += sweep.untouched;
            if (sweep.untouched > 0) {
                report.details << presetText("「%1」：「覆盖」档解绑了 %2 格热键，另外 %3 格"
                                             "（值那些和认不出来是什么的）一个都没动。")
                                      .arg(modId)
                                      .arg(sweep.removed)
                                      .arg(sweep.untouched);
            }
        }

        // 只有**真改了字节**的那一份才落盘（`before` 是这一份原来的样子；目标上没有这一份
        // 时 `before` 是空的，什么都没写进去的话它俩还是一样 ⇒ 不写一个空文件出来）。
        const bool changed = root != before;
        MalilibFileResult outFile;
        outFile.modId = modId;
        outFile.bytes = changed ? QJsonDocument(root).toJson(QJsonDocument::Indented)
                                : (present ? file->bytes : QByteArray());
        outFile.changed = changed;
        result.files.append(outFile);

        // 这一份**这一趟之后**长什么样（冲突分析看的是这个，不是预设里那半截）。
        afterAll.append(entriesFromRoot(modId, root, false));
    }

    report.conflicts = malilibKeyConflicts(afterAll);
    result.after = afterAll;
    report.ok = true;
    return result;
}

MalilibApplyReport planMalilibApply(const QList<MalilibEntry> &entries,
                                    const MalilibTarget &target, ApplyMode mode,
                                    bool replaceConfirmed) {
    return applyMalilibEntries(entries, target, mode, replaceConfirmed).report;
}

MalilibApplyReport applyMalilibToInstance(const QString &instanceRoot,
                                          const QList<MalilibEntry> &entries, ApplyMode mode,
                                          bool replaceConfirmed) {
    QStringList modIds;
    for (const MalilibEntry &entry : entries) {
        if (!modIds.contains(entry.modId)) {
            modIds.append(entry.modId);
        }
    }
    const MalilibTarget target = malilibReadTarget(instanceRoot, modIds);
    const MalilibApplyResult planned =
        applyMalilibEntries(entries, target, mode, replaceConfirmed);
    MalilibApplyReport report = planned.report;
    if (!report.ok) {
        return report;  // 「覆盖」没确认：盘上什么都没动
    }

    for (const MalilibFileResult &file : planned.files) {
        if (!file.changed) {
            continue;  // 一个字都没改的那几份不写（见文件头第 2 条）
        }
        const QString relativePath = malilibConfigRelativePath(file.modId);
        // 读那一侧已经拦过（见 malilibReadTarget()），这里再判一次：这一层是唯一真写盘的
        // 地方，路径上任何一段是链接都不写 —— 不然 `mkpath` / `QSaveFile` 会穿过它。
        if (InstanceFiles::pathHasLinkComponent(instanceRoot, relativePath)) {
            report.ok = false;
            report.problem =
                QCoreApplication::translate("Presets", "「%1」的路径包含目录联接或符号链接，已跳过写入。")
                    .arg(relativePath);
            return report;
        }
        const QString path = QDir(instanceRoot).filePath(relativePath);
        const QString dirPath = QFileInfo(path).absolutePath();
        if (!QDir().mkpath(dirPath)) {
            report.ok = false;
            report.problem = QCoreApplication::translate("Presets", "无法创建目录「%1」，写入已停止。").arg(dirPath);
            return report;
        }
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly)) {
            report.ok = false;
            report.problem = QCoreApplication::translate("Presets", "无法写入「%1」：%2").arg(path, out.errorString());
            return report;
        }
        if (out.write(file.bytes) != file.bytes.size()) {
            out.cancelWriting();
            report.ok = false;
            report.problem = QCoreApplication::translate("Presets", "写入「%1」中断：%2").arg(path, out.errorString());
            return report;
        }
        if (!out.commit()) {
            report.ok = false;
            report.problem = QCoreApplication::translate("Presets", "替换「%1」失败：%2").arg(path, out.errorString());
            return report;
        }
    }
    return report;
}

// ---------------------------------------------------------------------------
// 提取：实例 → 预设
// ---------------------------------------------------------------------------

MalilibExtractReport extractMalilibSections(const MalilibExtractRequest &request) {
    MalilibExtractReport report;
    if (request.instanceRoot.trimmed().isEmpty()) {
        report.problem = QCoreApplication::translate("Presets", "未指定实例目录，无法提取 MaLiLib 配置。");
        return report;
    }

    // 点名的那些按点名收；没点名就扫 `config/` 底下所有认得出来的（结构识别，见文件头）。
    QStringList modIds = request.modIds;
    QList<QString> order;
    for (const QString &modId : modIds) {
        if (!order.contains(modId)) {
            order.append(modId);
        }
    }
    if (order.isEmpty()) {
        order = malilibModIdsInInstance(request.instanceRoot, &report.skipped);
    }

    QList<MalilibEntry> all;
    for (const QString &modId : order) {
        const QString modProblem = malilibModIdProblem(modId);
        if (!modProblem.isEmpty()) {
            report.skipped << QCoreApplication::translate("Presets", "「%1」：%2").arg(modId, modProblem);
            continue;
        }
        const QString path = QDir(request.instanceRoot).filePath(malilibConfigRelativePath(modId));
        if (!QFileInfo::exists(path)) {
            report.skipped << QCoreApplication::translate("Presets", "「%1」不在当前实例中。")
                                  .arg(malilibConfigRelativePath(modId));
            continue;
        }
        QByteArray bytes;
        QString why;
        if (!readWholeFile(path, &bytes, &why)) {
            report.skipped << QCoreApplication::translate("Presets", "「%1」：%2").arg(malilibConfigRelativePath(modId), why);
            continue;
        }
        QList<MalilibEntry> entries;
        QString problem;
        if (!malilibEntriesFromFile(modId, bytes, &entries, &problem, request.includeValues)) {
            report.skipped << problem;
            continue;
        }
        if (entries.isEmpty()) {
            report.skipped << QCoreApplication::translate("Presets", "「%1」中无内容可收集，已跳过。")
                                  .arg(malilibConfigRelativePath(modId));
            continue;
        }
        for (MalilibEntry &entry : entries) {
            entry.source = request.versions;
            switch (entry.kind) {
            case MalilibEntryKind::Hotkey:
                ++report.hotkeys;
                break;
            case MalilibEntryKind::Hotkeyed:
                ++report.hotkeyedValues;
                break;
            case MalilibEntryKind::Value:
                ++report.values;
                break;
            }
        }
        ++report.files;
        all.append(entries);
    }

    report.sections = sectionsFromMalilibEntries(all, request.mode);
    report.ok = true;
    return report;
}

}  // namespace Presets
