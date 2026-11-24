/**
 * @file InstanceSettingsSignature.hpp
 * @brief 「实例设置这一页上，用户改了什么吗」的**唯一判据**（脏状态的签名）。
 *
 * 【为什么单独一层】
 *
 * 这一页的写盘是"哪里变了就立刻写那几项"（`InstanceSettingsDialog::notifyEdited()`
 * 那一发 `settingsEdited`）。判"变了没有"以前写成对话框里的私有函数
 * （`valueSignature()`），读的是十来个控件和几个归一化函数 —— 于是那条判据**没法被断言
 * 盯上**：验证工程驱动不了"用户真的动了某个控件"那一半，只能靠 mw_check 里"改一格、看
 * 盘上有没有变"的间接用例。挪到这儿以后它是纯函数：给一份值、回一份签名，两条值一样就是
 * 一样。★ 放在 `ui/` 而不是 `src/settings/`：这份值的类型（`Override` 那个
 * `std::optional` 组合）是界面这一侧读控件读出来的，而 `ui/` 不许 include `src/`。
 *
 * 【签名怎么算】
 *
 * 逐项摆成文本，顺序**就是写盘那边读的那几问的顺序**（`override()` / `appendJvmArgs()` /
 * `warnJavaMismatch()` / 图标 / 分组 / 置顶 / 显示哪几页）。两个约定：
 *
 * - 「有没有值」和「值本身」都要摆。`std::optional` 那一档写成 `v:<值>` / `-`：
 *   "跟随全局"和"自定义成空串 / 4"是两件事，只摆值会把它们混成同一个签名。
 * - 「显示哪几页」按调用方给的**顺序**拼（不能按 QHash 的遍历顺序 —— 每次都可能不一样，
 *   那样签名会自己变，等于每读一次就说"改了"）。
 *
 * NOTE: 加一项设置、或者写盘那边多读一问，**这里就要跟着加一格**；反过来也一样。
 */
#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <optional>

namespace InstanceSettingsSignature {

/// 一份"用户现在填的这些值"。字段与写盘那边读的那几问一一对应。
struct Inputs {
    std::optional<QString> javaPath;
    std::optional<int> xmx;
    std::optional<int> xms;
    std::optional<bool> autoMemory;
    std::optional<QString> extraJvmArgs;
    std::optional<QString> windowMode;
    std::optional<int> windowWidth;
    std::optional<int> windowHeight;
    /// 追加参数：调用方给的是**已经归一过**的清单（空行、首尾空白在 `appendJvmArgs()` 里
    /// 就没了）。两条顺序不同的清单在这里就是两份签名 —— 那本来就是两件不同的设置。
    QStringList appendArgs;
    bool warnJavaMismatch = true;
    /// 图标那一格：挑本地图时是 `localIconValue() + "|" + pickedPath`（换一张图 = 换一次
    /// 设置），否则是 `iconChoice()` 那一档的键。
    QString iconSignature;
    /// 左栏「显示哪些页」：**按界面顺序**给的 (页键, 勾没勾)。
    QList<QPair<QString, bool>> pageChecks;
    /// 分组按 **id**（组名会变，id 才是身份）。
    QString groupId;
    bool pinned = false;
};

/// 这一份值的签名。两条值一样 ⇒ 签名逐字一样；任何一格变了 ⇒ 签名一定不同。
QStringList signatureOf(const Inputs &inputs);

} // namespace InstanceSettingsSignature
