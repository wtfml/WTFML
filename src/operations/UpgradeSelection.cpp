/**
 * @file UpgradeSelection.cpp
 * @brief 见头文件：九类选择、指纹与依赖提示（纯逻辑，不碰控件、不碰盘）。
 */
#include "UpgradeSelection.hpp"
#include <QCoreApplication>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace UpgradeSelection {

QList<Category> categories() {
    // 顺序 = 规范 4 节的三列表格阅读顺序；键沿用 `TransferPlanner::Item::key` 的叫法。
    return {
        {QStringLiteral("saves"), QCoreApplication::translate("UpgradeSelection", "游戏存档"), true},
        {QStringLiteral("mods"), QCoreApplication::translate("UpgradeSelection", "模组文件"), true},
        {QStringLiteral("config"), QCoreApplication::translate("UpgradeSelection", "模组配置"), true},
        {QStringLiteral("options"), QCoreApplication::translate("UpgradeSelection", "游戏设置"), true},
        {QStringLiteral("resourcepacks"), QCoreApplication::translate("UpgradeSelection", "资源包"), true},
        {QStringLiteral("shaderpacks"), QCoreApplication::translate("UpgradeSelection", "光影包"), true},
        {QStringLiteral("screenshots"), QCoreApplication::translate("UpgradeSelection", "截图"), false},
        {QStringLiteral("schematics"), QCoreApplication::translate("UpgradeSelection", "原理图"), false},
        {QStringLiteral("servers"), QCoreApplication::translate("UpgradeSelection", "服务器列表"), true},
    };
}

QStringList keys() {
    QStringList out;
    const QList<Category> all = categories();
    out.reserve(all.size());
    for (const Category &category : all) {
        out.append(category.key);
    }
    return out;
}

bool known(const QString &key) {
    return keys().contains(key);
}

bool State::contains(const QString &key) const {
    return selected.contains(key);
}

bool State::set(const QString &key, bool on) {
    if (!known(key)) {
        return false;
    }
    if (on) {
        if (!selected.contains(key)) {
            selected.append(key);
        }
    } else {
        selected.removeAll(key);
    }
    // 永远按 `keys()` 的顺序摆：指纹与落盘都不该受"先勾哪一类"影响。
    const QStringList order = keys();
    QStringList orderedSelection;
    for (const QString &candidate : order) {
        if (selected.contains(candidate)) {
            orderedSelection.append(candidate);
        }
    }
    selected = orderedSelection;
    return true;
}

void State::selectAll() {
    selected = keys();
}

void State::clear() {
    selected.clear();
}

QStringList State::ordered() const {
    return selected;
}

Plan makePlan(const QList<TransferPlanner::Item> &items, const QString &sourceId,
              const QString &targetId) {
    Plan plan;
    plan.sourceId = sourceId;
    plan.targetId = targetId;
    if (items.isEmpty()) {
        plan.ok = false;
        plan.problem = QCoreApplication::translate("UpgradeSelection", "此实例无可迁移的内容。");
        return plan;
    }
    QSet<QString> present;
    for (const TransferPlanner::Item &item : items) {
        if (item.exists && known(item.key)) {
            present.insert(item.key);
        }
    }
    for (const Category &category : categories()) {
        if (category.defaultSelected && present.contains(category.key)) {
            plan.state.selected.append(category.key);
        }
    }
    return plan;
}

QString fingerprint(const Plan &plan) {
    const QString material = plan.sourceId + QLatin1Char('\n') + plan.targetId + QLatin1Char('\n')
                             + plan.state.ordered().join(QLatin1Char(','));
    return QString::fromLatin1(
        QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QStringList warnings(const Plan &plan) {
    QStringList out;
    const State &state = plan.state;
    if (state.contains(QStringLiteral("config")) && !state.contains(QStringLiteral("mods"))) {
        out.append(QCoreApplication::translate("UpgradeSelection", "已选中模组配置，未选中模组文件。部分配置可能在新实例中无效。"));
    }
    if (state.contains(QStringLiteral("options"))
        && !state.contains(QStringLiteral("resourcepacks"))) {
        out.append(QCoreApplication::translate("UpgradeSelection", "已选中游戏设置，未选中资源包。新实例的设置可能引用缺失的资源包。"));
    }
    if (state.contains(QStringLiteral("shaderpacks"))) {
        out.append(QCoreApplication::translate("UpgradeSelection", "光影包仅在安装对应渲染器后生效；新实例不会自动安装渲染器。"));
    }
    return out;
}

QString categoryKeyForPlannerKey(const QString &plannerKey) {
    if (plannerKey == QStringLiteral("structure")) {
        // 规范里的「原理图」按真实目录还包括 `schematics/`；那一段的路径统一属于后续 U4，
        // 这里先把现有的 `structures/` 认成同一类，别让它在界面上消失。
        return QStringLiteral("schematics");
    }
    if (plannerKey == QStringLiteral("defaultconfigs")) {
        return QStringLiteral("config");
    }
    if (known(plannerKey)) {
        return plannerKey;
    }
    return QString();
}

QList<TransferPlanner::Item> itemsFor(const QList<TransferPlanner::Item> &items,
                                      const QString &categoryKey) {
    QList<TransferPlanner::Item> out;
    for (const TransferPlanner::Item &item : items) {
        if (categoryKeyForPlannerKey(item.key) == categoryKey) {
            out.append(item);
        }
    }
    return out;
}

Amounts amounts(const QList<TransferPlanner::Item> &items, const QStringList &categoryKeys) {
    Amounts total;
    for (const QString &key : categoryKeys) {
        for (const TransferPlanner::Item &item : itemsFor(items, key)) {
            if (!item.exists) {
                continue;
            }
            total.files += item.fileCount;
            total.bytes += item.bytes;
        }
    }
    return total;
}

Amounts selectedAmounts(const Plan &plan, const QList<TransferPlanner::Item> &items) {
    return amounts(items, plan.state.ordered());
}

QStringList rootsFor(const QString &categoryKey) {
    Plan one;
    one.state.set(categoryKey, true);
    return selectedRoots(one);
}

Links scanLinks(const QString &sourceDir, const QStringList &roots) {
    Links out;
    for (const QString &root : roots) {
        const QString top = sourceDir + QLatin1Char('/') + root;
        QList<QString> stack{top};
        while (!stack.isEmpty()) {
            const QString path = stack.takeLast();
            const QFileInfo info(path);
            if (!info.exists()) {
                continue;
            }
            const bool link = info.isSymLink() || !info.junctionTarget().isEmpty();
            if (link) {
                ++out.count;
                if (out.samples.size() < 3) {
                    out.samples.append(QDir(sourceDir).relativeFilePath(path));
                }
                continue;  // 链接不跟进去
            }
            if (info.isDir()) {
                const QFileInfoList entries =
                    QDir(path).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                                                 | QDir::Hidden | QDir::System);
                for (const QFileInfo &entry : entries) {
                    stack.append(entry.absoluteFilePath());
                }
            }
        }
    }
    return out;
}

QStringList selectedRoots(const Plan &plan) {
    QStringList out;
    for (const QString &key : plan.state.ordered()) {
        if (key == QStringLiteral("saves")) {
            out.append(QStringLiteral("saves"));
        } else if (key == QStringLiteral("mods")) {
            out.append(QStringLiteral("mods"));
        } else if (key == QStringLiteral("config")) {
            out.append(QStringLiteral("config"));
            out.append(QStringLiteral("defaultconfigs"));
        } else if (key == QStringLiteral("options")) {
            out.append(QStringLiteral("options.txt"));
        } else if (key == QStringLiteral("resourcepacks")) {
            out.append(QStringLiteral("resourcepacks"));
        } else if (key == QStringLiteral("shaderpacks")) {
            out.append(QStringLiteral("shaderpacks"));
        } else if (key == QStringLiteral("screenshots")) {
            out.append(QStringLiteral("screenshots"));
        } else if (key == QStringLiteral("schematics")) {
            out.append(QStringLiteral("structures"));
        } else if (key == QStringLiteral("servers")) {
            out.append(QStringLiteral("servers.dat"));
        }
    }
    out.removeDuplicates();
    return out;
}

}  // namespace UpgradeSelection




