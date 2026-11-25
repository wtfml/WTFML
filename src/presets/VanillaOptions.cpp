/**
 * @file VanillaOptions.cpp
 * @brief `VanillaOptions.hpp` 的实现：`options.txt` 的保序读写、认得的键那张表、
 * 按键冲突、以及"这一段怎么落到目标上"的计划与执行。
 *
 * 三条贯穿全篇的取舍（理由都在 `VanillaOptions.hpp` 的文件头里）：
 * ① 别人的行一个字节都不动 —— 没被改过的行直接写回原字节（`OptionsLine::raw`）；
 * ② 不认识的键**不删、不选**，只是留着（`Unknown` 那一档）；
 * ③ 计划（`planOptionsApply`）和执行（`applyOptions`）走**同一段代码** ——「先说清楚要干
 * 什么、再动手」这件事如果分成两套实现，迟早会出现"计划说 18 项、实际动了 17 项"。
 */

#include "VanillaOptions.hpp"

#include <QCoreApplication>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QUuid>

#include <algorithm>

namespace Presets {

namespace {

/// 把一个键拆成 `key` / `value`：按**第一个**冒号切。
///
/// 没有冒号、或者冒号就在第一位（`:x` 那种空键的行）都算"没有键"—— 那一行原样留着。
/// 空键的行走"改一个空键"这条路只会在用户文件里凭空多出一行，没有任何正当用途。
void splitLine(const QByteArray &body, QString *key, QString *value) {
    const int colon = body.indexOf(':');
    if (colon <= 0) {
        key->clear();
        value->clear();
        return;
    }
    *key = QString::fromUtf8(body.left(colon));
    *value = QString::fromUtf8(body.mid(colon + 1));
}

/// 追加新行时用的换行：文件里最后一个**非空**的换行，没有就 `\n`。
///
/// 不逐行统一成一个值：手改过的文件常常是 CRLF，而游戏自己写的是 LF；把新行按"文件
/// 自己的样子"补上，比按"我们喜欢的样子"补少一层意外。
QString defaultEolFor(const OptionsFile &file) {
    for (int i = file.lines.size() - 1; i >= 0; --i) {
        if (!file.lines.at(i).eol.isEmpty()) {
            return file.lines.at(i).eol;
        }
    }
    return QStringLiteral("\n");
}

// ---------------------------------------------------------------------------
// 认得的键
// ---------------------------------------------------------------------------

/// 「显示器、全屏分辨率等硬件相关项」那一档（**默认不选**）。
///
/// 判据是"这个值对不对要对着**这台机器**才说得清"：分辨率、窗口模式、GUI 缩放（跟屏幕
/// 物理尺寸走）、有没有触摸屏。性能那几项（`renderDistance`、`maxFps`）**故意不进**：
/// 它们是用户的偏好，换台机器多半照样想用。以后要收紧只改这一张表。
const QSet<QString> &hardwareKeys() {
    static const QSet<QString> keys = {
        QStringLiteral("fullscreen"),            // 窗口模式（F11 那一下记的就是它）
        QStringLiteral("fullscreenResolution"),  // 全屏分辨率（值里自己还带冒号）
        QStringLiteral("exclusiveFullscreen"),  // 独占模式跟随显示环境
        QStringLiteral("preferredGraphicsBackend"),  // 图形 API 取决于驱动支持
        QStringLiteral("guiScale"),              // 界面缩放
        QStringLiteral("touchscreen"),           // 有没有触摸屏
    };
    return keys;
}

/// 「基础设置」那一档：对着**游戏**说得清的那些。
///
/// 这是一张**允许表**（不在表里的落 `Unknown`），不是"除了按键都是基础设置"那种排他
/// 判断。反过来写的后果是：任何我们没听说过的键（别的版本新增的、模组写的）都会被当成
/// "基础设置"默认勾上，一应用就是拿我们看不懂的值去改用户的实例。
const QSet<QString> &gameKeys() {
    static const QSet<QString> keys = {
        // 画面
        QStringLiteral("lang"), QStringLiteral("fov"), QStringLiteral("gamma"),
        QStringLiteral("renderDistance"), QStringLiteral("simulationDistance"),
        QStringLiteral("maxFps"), QStringLiteral("graphicsMode"), QStringLiteral("ao"),
        QStringLiteral("particles"), QStringLiteral("renderClouds"),
        QStringLiteral("entityShadows"), QStringLiteral("entityDistanceScaling"), QStringLiteral("bobView"),
        QStringLiteral("menuBackgroundBlurriness"), QStringLiteral("biomeBlendRadius"),
        QStringLiteral("enableVsync"), QStringLiteral("soundDevice"),
        QStringLiteral("showSubtitles"), QStringLiteral("forceUnicodeFont"),
        QStringLiteral("narrator"), QStringLiteral("narratorHotkey"),
        QStringLiteral("notificationDisplayTime"), QStringLiteral("hideLightningFlash"),
        QStringLiteral("highContrast"), QStringLiteral("highContrastBlockOutline"),
        QStringLiteral("darkMojangStudiosBackground"), QStringLiteral("screenEffectScale"),
        QStringLiteral("fovEffectScale"), QStringLiteral("darknessEffectScale"),
        QStringLiteral("glintSpeed"), QStringLiteral("glintStrength"),
        QStringLiteral("damageTiltStrength"), QStringLiteral("panoramaScrollSpeed"),
        QStringLiteral("rotateWithMinecart"), QStringLiteral("operatorItemsTab"),
        // 聊天
        QStringLiteral("chatVisibility"), QStringLiteral("chatOpacity"),
        QStringLiteral("chatScale"), QStringLiteral("chatWidth"),
        QStringLiteral("chatHeightFocused"), QStringLiteral("chatHeightUnfocused"),
        QStringLiteral("chatLineSpacing"), QStringLiteral("chatDelay"),
        QStringLiteral("chatColors"), QStringLiteral("chatLinks"),
        QStringLiteral("chatLinksPrompt"), QStringLiteral("textBackgroundOpacity"),
        QStringLiteral("textBackground"), QStringLiteral("backgroundForChatOnly"),
        QStringLiteral("autoSuggestions"), QStringLiteral("reducedDebugInfo"),
        // 操作
        QStringLiteral("mainHand"), QStringLiteral("attackIndicator"),
        QStringLiteral("autoJump"), QStringLiteral("sprintToggle"),
        QStringLiteral("toggleSprint"), QStringLiteral("toggleCrouch"),
        QStringLiteral("mouseSensitivity"), QStringLiteral("mouseWheelSensitivity"),
        QStringLiteral("invertYMouse"), QStringLiteral("discrete_mouse_scroll"),
        QStringLiteral("rawMouseInput"), QStringLiteral("pauseOnLostFocus"),
        // 杂项开关
        QStringLiteral("allowServerListing"), QStringLiteral("syncChunkWrites"),
        QStringLiteral("useNativeTransport"), QStringLiteral("skipMultiplayerWarning"),
        QStringLiteral("skipRealms32bitWarning"), QStringLiteral("hideBundleTutorial"),
        QStringLiteral("realmsNotifications"),
        // 新版视频、按键控制、鼠标、聊天、辅助功能和声音选项。
        "prioritizeChunkUpdates", "mipmapLevels", "cloudRange", "cutoutLeaves", "improvedTransparency",
        "textureFiltering", "maxAnisotropyBit", "weatherRadius", "inactivityFpsLimit", "showAutosaveIndicator",
        "vignette", "chunkSectionFadeInTime", "toggleAttack", "toggleUse", "sprintWindow", "invertXMouse",
        "allowCursorChanges", "hideMatchedNames", "onlyShowSecureChat", "saveChatDrafts", "hideSplashTexts",
        "directionalAudio", "musicFrequency", "musicToast", "modelPart_cape", "modelPart_jacket",
        "modelPart_left_sleeve", "modelPart_right_sleeve", "modelPart_left_pants_leg", "modelPart_right_pants_leg",
        "modelPart_hat",
    };
    return keys;
}

/// 这一段的**填充域**：`VanillaKeys` 只管 `key_*`，`VanillaOptions` 管其余全部
/// （含我们认不出来的那些 —— 用户真要搬一条我们看不懂的设置，拦的应该是界面，不是这一层）。
bool inFillDomain(SectionKind kind, const QString &key) {
    const bool isHotkey = key.startsWith(QLatin1String("key_"));
    return kind == SectionKind::VanillaKeys ? isHotkey : !isHotkey;
}

/// 「覆盖」档**删得掉**的那些键：这一段域里、我们**认得出**的几档。
///
/// `Unknown` 一个都不删（`inFillDomain` 为真但这里为假）：那是别人写的东西，不是
/// "多余的东西"。把不认识的键当成"该被覆盖掉的"，正是要避开的那个坑。
bool removableInReplace(SectionKind kind, const QString &key) {
    if (!inFillDomain(kind, key)) {
        return false;
    }
    const OptionsField field = optionsFieldOf(key);
    return field == OptionsField::Game || field == OptionsField::Hardware ||
           field == OptionsField::Hotkey;
}

/// 界面要分的那几组，固定顺序。
const QList<OptionsField> &allFields() {
    static const QList<OptionsField> fields = {OptionsField::Game, OptionsField::Hotkey,
                                               OptionsField::Hardware, OptionsField::Unknown};
    return fields;
}

// ---------------------------------------------------------------------------
// 计划 / 执行共用的那一段
// ---------------------------------------------------------------------------

/// 这一趟**要写进去**的那些键值（保源文件的顺序，同一个键只算最后一次的值）。
struct PlannedChange {
    QString key;
    QString value;
};

/// 「覆盖」档要删的目标键（去重后的）。
QSet<QString> removedKeysFor(SectionKind kind, const OptionsFile &target,
                             const QList<PlannedChange> &applied) {
    QSet<QString> appliedKeys;
    for (const PlannedChange &change : applied) {
        appliedKeys.insert(change.key);
    }
    QSet<QString> removed;
    for (const OptionsLine &line : target.lines) {
        if (line.hasKey() && removableInReplace(kind, line.key) &&
            !appliedKeys.contains(line.key)) {
            removed.insert(line.key);
        }
    }
    return removed;
}

/// 目标上"我们认不出来、任何一档都不动"的行数（空行也算 —— 它同样是我们没看懂的东西）。
int untouchableLineCount(const OptionsFile &target) {
    int count = 0;
    for (const OptionsLine &line : target.lines) {
        if (!line.hasKey() || optionsFieldOf(line.key) == OptionsField::Unknown) {
            ++count;
        }
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// 文件 → 行
// ---------------------------------------------------------------------------

OptionsFile parseOptions(const QByteArray &bytes) {
    OptionsFile file;
    int pos = 0;
    while (pos < bytes.size()) {
        const int nl = bytes.indexOf('\n', pos);
        const bool hasNl = nl >= 0;
        const int end = hasNl ? nl : bytes.size();
        QByteArray body = bytes.mid(pos, end - pos);

        QString eol;
        if (hasNl) {
            eol = QStringLiteral("\n");
            if (body.endsWith('\r')) {
                body.chop(1);
                eol = QStringLiteral("\r\n");
            }
        }

        OptionsLine line;
        line.raw = body;
        line.eol = eol;
        splitLine(body, &line.key, &line.value);
        if (!line.hasKey()) {
            // 认不出键的行：整行（解出来的）留着给界面看，写回时用的是 `raw`。
            line.text = QString::fromUtf8(body);
        }
        file.lines.append(line);
        pos = hasNl ? nl + 1 : bytes.size();
    }
    return file;
}

QByteArray OptionsFile::toBytes() const {
    QByteArray out;
    for (const OptionsLine &line : lines) {
        if (line.edited) {
            out += line.key.toUtf8() + ':' + line.value.toUtf8();
        } else {
            out += line.raw;
        }
        out += line.eol.toUtf8();
    }
    return out;
}

int OptionsFile::lastIndexOfKey(const QString &key) const {
    if (key.isEmpty()) {
        return -1;
    }
    for (int i = lines.size() - 1; i >= 0; --i) {
        if (lines.at(i).key == key) {
            return i;
        }
    }
    return -1;
}

QString OptionsFile::value(const QString &key) const {
    const int index = lastIndexOfKey(key);
    return index >= 0 ? lines.at(index).value : QString();
}

QStringList OptionsFile::keys() const {
    QStringList all;
    for (const OptionsLine &line : lines) {
        if (line.hasKey() && !all.contains(line.key)) {
            all.append(line.key);
        }
    }
    return all;
}

void OptionsFile::setValue(const QString &key, const QString &value) {
    if (key.isEmpty()) {
        return;
    }
    const int index = lastIndexOfKey(key);
    if (index >= 0) {
        lines[index].value = value;
        lines[index].edited = true;
        return;
    }
    // 新加一行：原来最后一行没有换行的话先给它补上（不补的话新行会粘在它后面）。
    if (!lines.isEmpty() && lines.last().eol.isEmpty()) {
        lines.last().eol = defaultEolFor(*this);
    }
    OptionsLine line;
    line.key = key;
    line.value = value;
    line.raw = key.toUtf8() + ':' + value.toUtf8();
    line.eol = defaultEolFor(*this);
    line.edited = true;
    lines.append(line);
}

// ---------------------------------------------------------------------------
// 分组
// ---------------------------------------------------------------------------

QString optionsFieldKey(OptionsField field) {
    switch (field) {
    case OptionsField::Game:
        return QStringLiteral("game");
    case OptionsField::Hotkey:
        return QStringLiteral("hotkey");
    case OptionsField::Hardware:
        return QStringLiteral("hardware");
    case OptionsField::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

OptionsField optionsFieldOf(const QString &key) {
    if (key.isEmpty()) {
        return OptionsField::Unknown;
    }
    if (key.startsWith(QLatin1String("key_"))) {
        return OptionsField::Hotkey;
    }
    if (hardwareKeys().contains(key)) {
        return OptionsField::Hardware;
    }
    // 两个**前缀家族**照它们的命名规矩认（游戏自己是这么起的）：`soundCategory_*` 是
    // 音量那几条、`modelPart_*` 是皮肤部件。它们的尾巴是游戏自己排的枚举，不是一个一个
    // 能列完的清单。
    if (gameKeys().contains(key) || key.startsWith(QLatin1String("soundCategory_")) ||
        key.startsWith(QLatin1String("modelPart_"))) {
        return OptionsField::Game;
    }
    // 剩下的（`resourcePacks` / `lastServer` / `tutorialStep` / 模组写的……）**认不出来**：
    // 不选、不删、不动，只留个位置让用户自己看。
    return OptionsField::Unknown;
}

bool optionsDefaultSelected(const QString &key) {
    const OptionsField field = optionsFieldOf(key);
    return field == OptionsField::Game || field == OptionsField::Hotkey;
}

QList<OptionsEntryGroup> groupOptions(const OptionsFile &source) {
    QList<OptionsEntryGroup> groups;
    for (const OptionsField field : allFields()) {
        OptionsEntryGroup group;
        group.field = field;
        groups.append(group);
    }
    for (const OptionsLine &line : source.lines) {
        const OptionsField field = optionsFieldOf(line.key);
        for (OptionsEntryGroup &group : groups) {
            if (group.field == field) {
                group.lines.append(line);
                break;
            }
        }
    }
    return groups;
}

QSet<QString> defaultSelection(const OptionsFile &source) {
    QSet<QString> selected;
    for (const QString &key : source.keys()) {
        if (optionsDefaultSelected(key)) {
            selected.insert(key);
        }
    }
    return selected;
}

// ---------------------------------------------------------------------------
// 冲突
// ---------------------------------------------------------------------------

QList<OptionsConflict> optionsConflicts(const OptionsFile &source, const OptionsFile &target,
                                        const QSet<QString> &selected) {
    // 原版允许默认键共享。自定义键加入同一个绑定时仍报告冲突。
    static const QHash<QString, QSet<QString>> sharedDefaults{
        {"key.keyboard.a", {"key_key.left", "key_key.debug.reloadChunk"}},
        {"key.keyboard.s", {"key_key.back", "key_key.debug.dumpDynamicTextures"}},
        {"key.keyboard.d", {"key_key.right", "key_key.debug.clearChat"}},
        {"key.keyboard.t", {"key_key.chat", "key_key.debug.reloadResourcePacks"}},
        {"key.keyboard.p", {"key_key.socialInteractions", "key_key.debug.focusPause"}},
        {"key.mouse.middle", {"key_key.pickItem", "key_key.spectatorHotbar"}},
        {"key.keyboard.c", {"key_key.saveToolbarActivator", "key_key.debug.crash", "key_key.debug.copyLocation"}},
        {"key.keyboard.l", {"key_key.advancements", "key_key.debug.profiling"}},
        {"key.keyboard.g", {"key_key.quickActions", "key_key.debug.showChunkBorders"}},
        {"key.keyboard.f4", {"key_key.toggleSpectatorShaderEffects", "key_key.debug.switchGameMode"}},
        {"key.keyboard.f3", {"key_key.debug.overlay", "key_key.debug.modifier"}},
        {"key.keyboard.1", {"key_key.debug.profilingChart", "key_key.hotbar.1"}},
        {"key.keyboard.2", {"key_key.debug.fpsCharts", "key_key.hotbar.2"}},
        {"key.keyboard.3", {"key_key.debug.networkCharts", "key_key.hotbar.3"}},
        {"key.keyboard.4", {"key_key.debug.lightmapTexture", "key_key.hotbar.4"}},
    };
    // ① 预设这一侧：勾中的标准按键，按**绑定串**分堆（空绑定不算 —— 未绑定可以有一堆）。
    QHash<QString, QStringList> byBinding;
    QSet<QString> sourceKeys;
    for (const OptionsLine &line : source.lines) {
        if (!line.hasKey() || !line.key.startsWith(QLatin1String("key_")) ||
            !selected.contains(line.key) || sourceKeys.contains(line.key)) {
            continue;
        }
        sourceKeys.insert(line.key);
        const QString binding = line.value.trimmed();
        if (!binding.isEmpty() && binding != "key.keyboard.unknown") {
            byBinding[binding] << line.key;
        }
    }

    // ② 目标这一侧：只算**这一趟不会被预设覆盖掉**的那些键（会被改掉的那条已经不占着
    // 这个键了，把它算进来等于报了一个应用完就不存在的冲突）。
    QHash<QString, QStringList> targetByBinding;
    QSet<QString> targetKeys;
    for (const OptionsLine &line : target.lines) {
        if (!line.hasKey() || !line.key.startsWith(QLatin1String("key_")) ||
            sourceKeys.contains(line.key) || targetKeys.contains(line.key)) {
            continue;
        }
        targetKeys.insert(line.key);
        const QString binding = line.value.trimmed();
        if (!binding.isEmpty() && binding != "key.keyboard.unknown") {
            targetByBinding[binding] << line.key;
        }
    }

    QList<OptionsConflict> conflicts;
    for (auto it = byBinding.constBegin(); it != byBinding.constEnd(); ++it) {
        const QStringList otherKeys = targetByBinding.value(it.key());
        if (it.value().size() + otherKeys.size() < 2) {
            continue;  // 只有一条动作用它 ⇒ 不冲突（预设内部不重复、目标上也没别家）
        }
        const auto shared = sharedDefaults.constFind(it.key());
        if (shared != sharedDefaults.cend()) {
            const auto isDefault = [&shared](const QString &key) { return shared->contains(key); };
            if (std::all_of(it.value().cbegin(), it.value().cend(), isDefault) &&
                std::all_of(otherKeys.cbegin(), otherKeys.cend(), isDefault)) continue;
        }
        OptionsConflict conflict;
        conflict.binding = it.key();
        conflict.sourceKeys = it.value();
        conflict.targetKeys = otherKeys;
        conflicts.append(conflict);
    }
    // 定序：按绑定串排，界面上的行不会每次刷新都换位置。
    std::sort(conflicts.begin(), conflicts.end(),
              [](const OptionsConflict &a, const OptionsConflict &b) {
                  return a.binding < b.binding;
              });
    return conflicts;
}

// ---------------------------------------------------------------------------
// 段 ⇄ 文件
// ---------------------------------------------------------------------------

QString optionsSectionId(SectionKind kind) {
    // 这一层只管"这两个种类归谁"；id 本身怎么算**只有一处**（`PresetTypes::sectionIdFor()`）
    // —— 换一段就换一个算法的后果是同一段在两份预设里有两个身份。
    return sectionIdFor(kind);
}

QList<PresetSection> sectionsFromOptions(const OptionsFile &source, const QSet<QString> &selected) {
    QList<PresetSection> sections;
    for (const SectionKind kind : {SectionKind::VanillaOptions, SectionKind::VanillaKeys}) {
        PresetSection section;
        section.id = optionsSectionId(kind);
        section.kind = kind;
        // 「默认 Merge，Replace 必须明确确认」——默认档写在这儿，覆盖档由界面那一步
        // 让用户点头之后才写进这一段。
        section.mode = ApplyMode::Merge;

        QSet<QString> seen;
        for (const OptionsLine &line : source.lines) {
            if (!line.hasKey() || !selected.contains(line.key) || seen.contains(line.key) ||
                !inFillDomain(kind, line.key)) {
                continue;
            }
            seen.insert(line.key);
            QJsonObject item;
            item.insert(QStringLiteral("key"), line.key);
            // 空值也照样写下去（`key_xxx:` = 用户把它清成了"未绑定"）——"我这一趟就是
            // 要把它清空"和"我没有这一项"是两件事。
            item.insert(QStringLiteral("value"), line.value);
            section.items.append(item);
        }
        sections.append(section);
    }
    return sections;
}

OptionsFile optionsFromSections(const QList<PresetSection> &sections, bool *ok) {
    if (ok != nullptr) {
        *ok = true;
    }
    OptionsFile file;
    for (const PresetSection &section : sections) {
        if (!optionsHandlesSection(section.kind)) {
            continue;
        }
        for (const QJsonValue &value : section.items) {
            if (!value.isObject()) {
                if (ok != nullptr) {
                    *ok = false;
                }
                return OptionsFile();
            }
            const QJsonObject item = value.toObject();
            const QJsonValue key = item.value(QStringLiteral("key"));
            const QJsonValue itemValue = item.value(QStringLiteral("value"));
            // `value` 缺了**不算空串**：我们自己写下去的每一项都有这一个字段，缺了说明这一份
            // 被谁动过（「区分缺失、`null` 与错误类型」）。读不回来的段不许拿去改用户的
            // 实例 —— 这里回一个空文件 + `ok = false`，让调用方自己决定怎么报。
            if (!key.isString() || key.toString().isEmpty() || !itemValue.isString()) {
                if (ok != nullptr) {
                    *ok = false;
                }
                return OptionsFile();
            }
            file.setValue(key.toString(), itemValue.toString());
        }
    }
    return file;
}

// ---------------------------------------------------------------------------
// 计划 / 执行
// ---------------------------------------------------------------------------

bool optionsHandlesSection(SectionKind kind) {
    return kind == SectionKind::VanillaOptions || kind == SectionKind::VanillaKeys;
}

OptionsApplyResult applyOptions(SectionKind kind, const OptionsFile &target,
                                const OptionsFile &source, const QSet<QString> &selected,
                                ApplyMode mode, bool replaceConfirmed) {
    OptionsApplyResult result;
    if (!optionsHandlesSection(kind)) {
        result.bytes = target.toBytes();
        result.report.problem =
            QCoreApplication::translate("Presets", "此段不属于原版设置层管理范围。");
        return result;
    }

    // ---- ① 这一趟要写进去的键值（保源顺序；同一个键只算最后一条的值）----
    QList<PlannedChange> applied;
    QSet<QString> done;
    int skipped = 0;
    int domainKeys = 0;
    for (const OptionsLine &line : source.lines) {
        if (!line.hasKey() || !inFillDomain(kind, line.key) || done.contains(line.key)) {
            continue;
        }
        done.insert(line.key);
        ++domainKeys;
        if (mode == ApplyMode::Skip || !selected.contains(line.key)) {
            ++skipped;
            continue;
        }
        PlannedChange change;
        change.key = line.key;
        change.value = line.value;
        applied.append(change);
    }
    if (mode == ApplyMode::Skip) {
        // 「跳过」这一档：一样都不写，而预设里那几项全算"没动"。
        applied.clear();
        skipped = domainKeys;
    }

    // ---- ② 覆盖档要删的目标键 ----
    const QSet<QString> removed =
        mode == ApplyMode::Replace ? removedKeysFor(kind, target, applied) : QSet<QString>();

    int written = 0;
    int added = 0;
    for (const PlannedChange &change : applied) {
        const bool inTarget = target.contains(change.key);
        if (!inTarget) {
            ++added;
        }
        // 「缺了才拷」这一档：目标上已经有这个键的，一个字节都不动。
        if (mode == ApplyMode::CopyIfMissing && inTarget) {
            continue;
        }
        ++written;
    }
    result.report.written = written;
    result.report.added = added;
    result.report.removed = removed.size();
    result.report.skipped = skipped;
    // 这个数**只在覆盖档**填：别的档一行都不删，报出来只会让人以为我们在权衡什么。
    result.report.untouchable = mode == ApplyMode::Replace ? untouchableLineCount(target) : 0;
    // 按键冲突只归「原版按键」那一段报（也只有那一行带冲突数）：
    // 设置那一段管不着 `key_*`（`inFillDomain` 把它们挡在外面），在这儿报等于把别的段的
    // 账记到它头上。
    if (kind == SectionKind::VanillaKeys) {
        result.report.conflicts = optionsConflicts(source, target, selected);
    }

    // ---- ④ 「覆盖」要用户明确确认过才动手 ----
    if (mode == ApplyMode::Replace && !replaceConfirmed) {
        result.bytes = target.toBytes();
        result.report.needsConfirm = true;
        result.report.problem =
            QCoreApplication::translate("Presets", "覆盖将用预设替换目标中的同类配置。确认后执行。");
        return result;
    }

    // ---- ⑤ 真写：覆盖档先摘掉要删的键，再按源顺序写进去 ----
    OptionsFile out = target;
    if (!removed.isEmpty()) {
        QList<OptionsLine> kept;
        for (const OptionsLine &line : out.lines) {
            if (line.hasKey() && removed.contains(line.key)) {
                continue;
            }
            kept.append(line);
        }
        out.lines = kept;
    }
    for (const PlannedChange &change : applied) {
        if (mode == ApplyMode::CopyIfMissing && out.contains(change.key)) {
            continue;
        }
        out.setValue(change.key, change.value);
    }

    result.bytes = out.toBytes();
    result.report.ok = true;
    return result;
}

OptionsApplyReport planOptionsApply(SectionKind kind, const OptionsFile &source,
                                    const OptionsFile &target, const QSet<QString> &selected,
                                    ApplyMode mode, bool replaceConfirmed) {
    return applyOptions(kind, target, source, selected, mode, replaceConfirmed).report;
}

}  // namespace Presets
