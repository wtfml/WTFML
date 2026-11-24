/**
 * @file ResourcePackPreset.cpp
 * @brief 实现 ResourcePackPreset.hpp（只碰 options.txt 里 `resourcePacks` 那一行）。
 */

#include "ResourcePackPreset.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <limits>

#include "GenericFiles.hpp"
#include "manage/ResourcePackOptions.hpp"

namespace Presets {

namespace {

[[maybe_unused]] QString packText(const char *source) { return QCoreApplication::translate("Presets", source); }

constexpr auto kResourcePackPrefix = "resourcepacks/";
constexpr auto kLocalIdPrefix = "file/";

/// 一根链接（symbolic link / junction）。★ 两个都要判（见 `InstanceFiles.hpp`）。
bool isLink(const QFileInfo &info) { return info.isJunction() || info.isSymLink(); }

/// 段里那一项的资源包文件名（`resourcepacks/BSL.zip` → `BSL.zip`）；不是这一组 ⇒ 空串。
QString packLeafOf(const QString &relativePath) {
    if (!relativePath.startsWith(QLatin1String(kResourcePackPrefix))) {
        return QString();
    }
    const QString leaf = relativePath.mid(int(qstrlen(kResourcePackPrefix)));
    return leaf.isEmpty() || leaf.contains(QLatin1Char('/')) ? QString() : leaf;
}

QString localIdOf(const QString &leaf) {
    return QString::fromLatin1(kLocalIdPrefix) + leaf;
}

/// 读段里那一格布尔值。返回 false 表示这一项**没有**启用信息。
bool itemEnabled(const QJsonObject &item, bool *present) {
    const QJsonValue value = item.value(QStringLiteral("enabled"));
    *present = value.isBool();
    return value.toBool();
}

int itemPriority(const QJsonObject &item, int fallback) {
    const QJsonValue value = item.value(QStringLiteral("priority"));
    return value.isDouble() ? value.toInt(fallback) : fallback;
}

}  // namespace

ResourcePackOrderState readResourcePackOrderState(const QString &instanceRoot) {
    ResourcePackOrderState state;
    if (instanceRoot.trimmed().isEmpty()) {
        state.problem = QCoreApplication::translate("Presets", "未指定实例目录。");
        return state;
    }
    const ResourcePackOptions::Snapshot snapshot =
        ResourcePackOptions::read(instanceRoot + QStringLiteral("/options.txt"));
    if (!snapshot.problem.isEmpty()) {
        state.problem = snapshot.problem;
        return state;
    }
    if (!snapshot.exists) {
        return state;  // 新实例：没有 options.txt，也就没有可收的启用状态。
    }
    state.hasList = snapshot.document.valueOffset >= 0;
    if (state.hasList) {
        state.enabledOrder = snapshot.document.selected;
    }
    return state;
}

bool annotateResourcePackSection(PresetSection &section, const ResourcePackOrderState &state) {
    if (!state.hasList) {
        return false;  // 没读到那一行 ⇒ 一格都不加（"没读到"不冒充"全是关着"）。
    }

    QJsonArray items;
    for (const QJsonValue &value : section.items) {
        if (!value.isObject()) {
            items.append(value);
            continue;
        }
        QJsonObject item = value.toObject();
        const QString leaf = packLeafOf(item.value(QStringLiteral("path")).toString());
        if (leaf.isEmpty()) {
            items.append(item);
            continue;
        }
        const QString id = localIdOf(leaf);
        const int index = state.enabledOrder.indexOf(id);
        item.insert(QStringLiteral("enabled"), index >= 0);
        if (index >= 0) {
            item.insert(QStringLiteral("priority"), index);
        }
        items.append(item);
    }
    section.items = items;
    return !items.isEmpty();
}

bool resourcePackSectionHasOrdering(const PresetSection &section) {
    for (const QJsonValue &value : section.items) {
        if (!value.isObject()) {
            continue;
        }
        bool present = false;
        itemEnabled(value.toObject(), &present);
        if (present) {
            return true;
        }
    }
    return false;
}

ResourcePackOrderReport applyResourcePackOrdering(const QString &instanceRoot,
                                                  const PresetSection &section,
                                                  const QString &backupRoot) {
    ResourcePackOrderReport report;
    report.hasOrdering = resourcePackSectionHasOrdering(section);
    if (!report.hasOrdering) {
        report.details.append(packText("这一份预设没有带启用顺序（提取时没读到 options.txt），"
                                       "资源包只部署、不启用。"));
        return report;
    }

    if (instanceRoot.trimmed().isEmpty()) {
        report.ok = false;
        report.problem = QCoreApplication::translate("Presets", "未指定实例目录，无法保存资源包启用顺序。");
        return report;
    }

    // ---------- 预设点名的那些 ----------
    struct Wanted {
        QString leaf;
        bool enabled = false;
        int priority = 0;
    };
    QList<Wanted> wanted;
    QSet<QString> named;
    for (const QJsonValue &value : section.items) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject item = value.toObject();
        const QString leaf = packLeafOf(item.value(QStringLiteral("path")).toString());
        if (leaf.isEmpty()) {
            continue;
        }
        bool present = false;
        const bool enabled = itemEnabled(item, &present);
        if (!present) {
            continue;
        }
        Wanted one;
        one.leaf = leaf;
        one.enabled = enabled;
        one.priority = itemPriority(item, std::numeric_limits<int>::max());
        wanted.append(one);
        named.insert(leaf);
    }
    if (wanted.isEmpty()) {
        report.details.append(QCoreApplication::translate("Presets", "此预设的资源包项无启用信息，仅部署、不启用。"));
        return report;
    }

    // ---------- 目标现在这一份 ----------
    const QString optionsPath = instanceRoot + QStringLiteral("/options.txt");
    const ResourcePackOptions::Snapshot snapshot = ResourcePackOptions::read(optionsPath);
    if (!snapshot.problem.isEmpty()) {
        report.ok = false;
        report.problem = snapshot.problem;
        return report;
    }
    if (snapshot.exists && snapshot.document.valueOffset < 0) {
        // 老版本的 options.txt 里没有这一行 —— 写一个它不认识的键下去不算"启用成功"。
        report.details.append(packText("目标实例的 options.txt 里没有资源包列表（可能是很老的"
                                       "版本），资源包只部署、没有自动启用。"));
        return report;
    }

    // 目标 `resourcepacks/` 里现在真有的那些（save() 只允许"本来就在列表里"或"这里有的"）。
    QSet<QString> available;
    const QString packsDir = instanceRoot + QStringLiteral("/") + QLatin1String(kResourcePackPrefix);
    const QFileInfo packsInfo(packsDir);
    if (packsInfo.isDir() && !isLink(packsInfo)) {
        const QFileInfoList entries =
            QDir(packsDir).entryInfoList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System |
                                             QDir::NoDotAndDotDot,
                                         QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (isLink(entry)) {
                continue;
            }
            available.insert(localIdOf(entry.fileName()));
        }
    }

    const QStringList before = snapshot.document.selected;

    // 预设点名的那些先从现有列表里摘掉：打开的要按预设的 priority 重新排到末尾。
    QStringList kept;
    for (const QString &id : before) {
        if (ResourcePackOptions::isLocal(id) && named.contains(id.mid(5))) {
            continue;
        }
        kept.append(id);
    }

    // 打开的：按 priority 从小到大（低优先级在前），同档按预设里的次序。
    QList<Wanted> enabled;
    for (const Wanted &one : wanted) {
        if (one.enabled) {
            enabled.append(one);
        }
    }
    std::stable_sort(enabled.begin(), enabled.end(),
                     [](const Wanted &a, const Wanted &b) { return a.priority < b.priority; });

    QStringList after = kept;
    for (const Wanted &one : enabled) {
        const QString id = localIdOf(one.leaf);
        if (!available.contains(id)) {
            report.details.append(QCoreApplication::translate("Presets", "「%1」未出现在目标的 resourcepacks/ 中，未启用。")
                                      .arg(one.leaf));
            continue;
        }
        after.append(id);
        ++report.enabled;
    }

    for (const Wanted &one : wanted) {
        if (one.enabled) {
            continue;
        }
        const QString id = localIdOf(one.leaf);
        if (before.contains(id)) {
            ++report.disabled;
        }
    }

    if (after == before) {
        report.details.append(QCoreApplication::translate("Presets", "资源包启用顺序已与预设一致，未改写 options.txt。"));
        return report;
    }

    QString problem;
    if (!ResourcePackOptions::save(optionsPath, snapshot.document.bytes, snapshot.exists, after,
                                   available, backupRoot, &problem)) {
        report.ok = false;
        report.problem = problem.isEmpty()
                             ? QCoreApplication::translate("Presets", "资源包启用顺序未写入。")
                             : problem;
        return report;
    }
    report.wrote = true;
    if (report.enabled > 0) {
        report.details.append(QCoreApplication::translate("Presets", "已按预设中保存的顺序启用 %1 个资源包。")
                                  .arg(report.enabled));
    }
    if (report.disabled > 0) {
        report.details.append(QCoreApplication::translate("Presets", "按预设禁用了 %1 个资源包。").arg(report.disabled));
    }
    return report;
}

}  // namespace Presets
