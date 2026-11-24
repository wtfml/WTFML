/**
 * @file VersionCategory.hpp
 * @brief 下载页那四个分类（正式版 / 快照版 / 远古版 / 愚人节版）的**纯函数**部分。
 * @note 这里不联网、不碰磁盘，只是"version 的 id + type 进，分类出"。真正把 900 多个版本
 * 分堆的是 VersionManager::availableVersionChannels()，它逐条调这里的 channelFor()。
 *
 * 【为什么前三个分类是白捡的、第四个不是】
 *
 * 【愚人节版为什么是一张写死的名单、而不是一条日期规则】
 *
 * 所以名单是写死的，判据就是"id 在这张表里"。这不是偷懒：PCL 也是这么做的（它的名单是从
 * `Plain Craft Launcher 2.exe` 的字符串堆里抠出来的，见本文件底部那段）。**验证工程拿真清单
 * 反过来核过这张表**：9 条全在清单里、且每一条的发布日期都在 3/31~4/1 这几天（这条断言
 * 抓的是"名单里打错一个字符" —— 打错的那条会立刻掉出这个日期窗口）。
 */
#pragma once

#include <QString>
#include <QStringList>

namespace VersionCategory {

/// 下载页「游戏」那一页里的四个分类。**顺序就是界面上从上到下的顺序**。
enum class Channel {
    Release,     ///< 正式版（type == "release"）
    Snapshot,    ///< 快照版（type == "snapshot"，以及将来没见过的 type）
    Old,         ///< 远古版（type == "old_beta" / "old_alpha"）
    AprilFools,  ///< 愚人节版（id 在那张名单里，**和 type 无关**）
};

/// 一共几个分类（界面那四条选项的个数）。
inline int channelCount() {
    return 4;
}

/// 界面上第 index 条对应哪个分类。越界时回 Release（调用方按 channelCount() 循环，
/// 不该越界；这里回一个**确定**的分类而不是抛/回 -1，是为了让"越界"不至于变成
/// `setCurrentIndex(-1)` 那种"点一下界面就空了"的毛病）。
inline Channel channelAt(int index) {
    switch (index) {
    case 0:
        return Channel::Release;
    case 1:
        return Channel::Snapshot;
    case 2:
        return Channel::Old;
    case 3:
        return Channel::AprilFools;
    default:
        return Channel::Release;
    }
}

/// channelAt() 的反函数。四处（表、循环、界面下标）靠这两个函数对齐，不各写各的 switch。
inline int channelIndex(Channel channel) {
    switch (channel) {
    case Channel::Release:
        return 0;
    case Channel::Snapshot:
        return 1;
    case Channel::Old:
        return 2;
    case Channel::AprilFools:
        return 3;
    }
    return 0;
}

/// 分类的**键**（`"release"` / `"snapshot"` / `"old"` / `"aprilfools"`）。
///
/// 给界面认条目用的（进 `Qt::UserRole`），和 channelText() 分开是刻意的：文字是产品文案，
/// 改一个字的说法不该把认条目的代码全打断。
inline QString channelKey(Channel channel) {
    switch (channel) {
    case Channel::Release:
        return QStringLiteral("release");
    case Channel::Snapshot:
        return QStringLiteral("snapshot");
    case Channel::Old:
        return QStringLiteral("old");
    case Channel::AprilFools:
        return QStringLiteral("aprilfools");
    }
    return QString();
}

/// 分类给用户看的文字（`正式版` / `快照版` / `远古版` / `愚人节版`）。
inline QString channelText(Channel channel) {
    switch (channel) {
    case Channel::Release:
        return QStringLiteral("正式版");
    case Channel::Snapshot:
        return QStringLiteral("快照版");
    case Channel::Old:
        return QStringLiteral("远古版");
    case Channel::AprilFools:
        return QStringLiteral("愚人节版");
    }
    return QString();
}

/// 四个键，按界面顺序。
inline QStringList channelKeys() {
    QStringList keys;
    for (int i = 0; i < channelCount(); ++i) {
        keys.append(channelKey(channelAt(i)));
    }
    return keys;
}

/// 四段文字，按界面顺序。
inline QStringList channelTexts() {
    QStringList texts;
    for (int i = 0; i < channelCount(); ++i) {
        texts.append(channelText(channelAt(i)));
    }
    return texts;
}

/**
 * @brief 愚人节版的全部 id（**小写**，比对时两边都转小写）。
 *
 * 9 条，全部在 那份清单里核过。表里刻意**只有 id**：日期、type 都不进这张表，
 * 那两样在上面那段说明里已经证明是靠不住的。
 *
 * NOTE: 大小写在这里是真问题：清单里那两条是 `3D Shareware v1.34`（有大写、**还有空格**）
 * 和 `1.RV-Pre1`，所以比对必须 toLower()，而且**不能**用 isSafeVersionId() 那种白名单
 * 去预筛 —— 空格会过不了那个白名单，而这条 id 是官方清单里真实存在的。
 *
 * NOTE: PCL 那份名单里还有三条**不是版本 id** 的东西（`experimental`、`lore`、简称 `20w14`），
 * 是从 exe 字符串堆里一起抠出来的邻近字面量、或那条版本的俗称。这里不收：这张表的用途
 * 是"给清单里每一条判个类"，而清单里根本不会有 `experimental` 这种 id —— 收进来只会让
 * "名单里的每一条都能在清单里找到"这条断言失效，反而看不出哪天打错了字。
 */
inline QStringList aprilFoolsVersionIds() {
    return QStringList{
        QStringLiteral("15w14a"),                // 2015「爱与拥抱」
        QStringLiteral("1.rv-pre1"),             // 2016「潮流更新」
        QStringLiteral("3d shareware v1.34"),    // 2019「3D 共享版」
        QStringLiteral("20w14infinite"),         // 2020「无限维度」
        QStringLiteral("22w13oneblockatatime"),  // 2022「一个方块」
        QStringLiteral("23w13a_or_b"),           // 2023「投票更新」
        QStringLiteral("24w14potato"),           // 2024「毒马铃薯」
        QStringLiteral("25w14craftmine"),        // 2025「合成器」
        QStringLiteral("26w14a"),                // 2026
    };
}

/// 这个 id 是不是愚人节版。大小写不敏感（理由见上面那条 NOTE）。
inline bool isAprilFoolsVersion(const QString &id) {
    static const QStringList kIds = aprilFoolsVersionIds();
    const QString lowered = id.toLower();
    for (const QString &candidate : kIds) {
        if (lowered == candidate) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 给清单里的一条判个分类。
 * @param id 版本 id（清单里的 `id`）。
 * @param type 清单里的 `type`（`release` / `snapshot` / `old_beta` / `old_alpha`）。
 *
 * 愚人节版**先判**：那 9 条的 type 全是 `snapshot`，按 type 判会把它们全塞进快照版那一栏。
 *
 * NOTE: 将来出现没见过的 type 时落到**快照版**。四栏是全集的划分，总得有个地方接；而"少显示
 * 一个版本"比"多显示一个"更难被发现 —— 落到快照版是往"看得见"那边偏。
 */
inline Channel channelFor(const QString &id, const QString &type) {
    if (isAprilFoolsVersion(id)) {
        return Channel::AprilFools;
    }
    if (type == QLatin1String("release")) {
        return Channel::Release;
    }
    if (type == QLatin1String("old_beta") || type == QLatin1String("old_alpha")) {
        return Channel::Old;
    }
    return Channel::Snapshot;
}

}  // namespace VersionCategory
