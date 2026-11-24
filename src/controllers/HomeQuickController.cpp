#include "HomeQuickController.hpp"

#include <QCoreApplication>
#include <QHash>
#include <QObject>

#include <algorithm>

HomeQuickController::Rows HomeQuickController::build(
    const QList<Instance> &installed, const QList<LaunchHistory::Record> &history,
    const QString &currentId, int favoriteCount, int recentCount) {
    Rows rows;
    QHash<QString, Instance> installedById;
    for (const Instance &instance : installed) {
        if (!instance.id.isEmpty()) {
            installedById.insert(instance.id, instance);
        }
    }

    QHash<QString, LaunchHistory::Record> latestByInstance;
    QStringList recentIds;
    for (const LaunchHistory::Record &entry : history) {
        if (!entry.isValid() || entry.result == LaunchHistory::Result::StartFailed
            || latestByInstance.contains(entry.instanceId)) {
            continue;
        }
        latestByInstance.insert(entry.instanceId, entry);
        recentIds.append(entry.instanceId);
    }

    const auto makeItem = [&](const Instance &instance) {
        MainWindowUI::HomeQuickItem item;
        item.key = instance.id;
        item.name = instance.name;
        item.icon = instance.icon;
        item.current = instance.id == currentId;
        item.favorite = instance.favorite;
        item.recent = latestByInstance.contains(instance.id);
        item.detail = instance.version;
        if (item.recent) {
            const LaunchHistory::Record record = latestByInstance.value(instance.id);
            item.detail += QLatin1Char('\n') + QCoreApplication::translate("LaunchHistory", "上次游玩 %1")
                .arg(LaunchHistory::whenText(record.startedAt));
            const QString played = LaunchHistory::durationText(record.durationSeconds);
            if (!played.isEmpty()) {
                item.detail += item.detail.isEmpty() ? played : QStringLiteral(" / ") + played;
            }
            item.detail += LaunchHistory::resultNote(record);
        }
        return item;
    };

    // 「当前」不是另一份持久化状态：它严格来自主页启动下拉框当前选中的实例，和真正按下
    // 启动按钮时会启动谁保持同一个来源（有实例必有当前实例，见 resolveCurrentInstance()）。
    // 一个实例都没有时才是空的那一张（「还没有实例」+「下载游戏」）。
    if (const auto current = installedById.constFind(currentId); current != installedById.constEnd()) {
        rows.current.append(makeItem(current.value()));
    } else if (!installed.isEmpty()) {
        rows.current.append(makeItem(installed.first()));
    } else {
        MainWindowUI::HomeQuickItem slot;
        slot.empty = true;
        slot.name = QObject::tr("还没有实例");
        rows.current.append(slot);
    }

    for (const Instance &instance : installed) {
        if (instance.favorite && rows.favorites.size() < favoriteCount) {
            rows.favorites.append(makeItem(instance));
        }
    }
    std::stable_sort(rows.favorites.begin(), rows.favorites.end(),
                     [&](const auto &left, const auto &right) {
                         const int leftRank = recentIds.indexOf(left.key);
                         const int rightRank = recentIds.indexOf(right.key);
                         if (leftRank < 0) return false;
                         if (rightRank < 0) return true;
                         return leftRank < rightRank;
                     });

    for (const QString &id : recentIds) {
        if (rows.recent.size() >= recentCount) {
            break;
        }
        const auto found = installedById.constFind(id);
        if (found != installedById.constEnd()) {
            rows.recent.append(makeItem(found.value()));
        }
    }

    return rows;
}
