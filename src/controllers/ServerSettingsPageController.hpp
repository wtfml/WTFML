/**
 * @file ServerSettingsPageController.hpp
 * @brief 「服务端设置」那一页的**全部状态**，以及把它翻成盘上那一份、再翻回界面的那几条
 *        （原来直接摊在 `MainWindow` 里）。
 *
 * 【为什么单独一层】
 *
 * 这一页有六样东西是**成套的**：页面对象本身、"这一页现在说的是哪个服务端"、这一页现在
 * 能不能改（服务端在跑 = 不能）、体检里"那个端口现在有人占着吗"的结论，以及那份原文的
 * 两个记账格子。它们必须同进同退 —— 开页记 id、动作按 id 认人、关页清掉；服务端一启动，
 * 整个页面的可编辑性、体检单、保存带三处要**同一次**跟着变。原来这几样和 `MainWindow` 上
 * 其余几百个成员摆在一起，"漏了一条"的症状很难认：最典型的是"服务端明明停了，那一页还是
 * 灰的"（`m_serverSettingsEditable` 没跟着重算）。
 *
 * 【这一层不判、不画】
 *
 * * 判据在 `ServerPropertiesForm`（那十几个旋钮的表与值域）、`ServerModel`（原文怎么解）、
 *   `ServerCheck` + `ServerPortCheck`（体检）、`BackupFiles`（世界备份）；
 * * 页面在 `ui/ServerSettingsPage`（只画不判、只发信号）；
 * * 真正落盘的那一笔在 `ServerSettingsController::save()`。
 *
 * 这一层干的只有三件事：把盘上的事实翻成页面上那几行、把用户按下去的那一下翻成一个动作、
 * 在"换服务端 / 存完 / 服务端起停"之后把这一页**整页**摆回与盘一致的样子。
 *
 * 【两种编辑方式为什么打不起来】
 *
 * 这一层手里始终握着页面上那份**原文**（`page->rawText()`），它是唯一会被写回盘上的东西；
 * 表单里改过的那几项是叠在它上面的补丁（`ServerSettingsPage::changedOnly()`）。保存 =
 * `parseProperties(原文)` → `applyEdits(补丁)` → `writeProperties()`（见
 * `ServerSettingsController::save()`）—— 于是注释、用户手写的别的键、键的顺序、行尾，
 * 一个字节都不动。
 *
 * NOTE: 这一层对 `MainWindow` 是 `friend`（见 `MainWindow.hpp` 那一串）：它要读/写主窗口上
 * 几样**本来就属于服务端那条流程**的东西（工作区里那个进程、设置控制器、覆盖层、日志、
 * 备份 / 玩家 / 内容那几条既有入口）。这些的生命周期比这一页长得多，所以不搬进来。
 */

#pragma once

#include "server/ServerCheck.hpp"
#include "server/ServerPortCheck.hpp"

#include <QObject>
#include <QString>

class MainWindow;
class ServerSettingsPage;

class ServerSettingsPageController final : public QObject {
public:
    explicit ServerSettingsPageController(MainWindow *owner);

    /// 开这一页（覆盖层整页）。已经有覆盖层开着 = 什么都不做（见 .cpp 里那段）。
    void open(const QString &serverId);

    /// 把这一页四页都按**盘上现在的样子**重摆一遍（开页 / 放弃改动 / 保存之后 / 备份收尾 /
    /// 服务端起停之后）。**没开着那一页就什么都不做** —— 这是那一页内容的唯一出处。
    void refresh();

    /// @p serverId 那一份世界备份收尾了：正开着它那一页就重摆一遍，否则什么都不做。
    void refreshIfShowing(const QString &serverId);

    /// 服务端的运行状态变了（起 / 停 / 崩）：正开着它那一页就**只**翻"在不在跑"这一套
    /// —— 顶上那两句状态、只读闸、体检单、保存带 —— 不重摆表单与原文。
    ///
    /// NOTE: 这里**不能**走 `refresh()`：那会把表单基线换到盘上那一份，用户还没来得及
    /// 保存的改动会当场消失。而在跑的时候整页只读（服务端退出时会把它内存里那份
    /// `server.properties` 写回盘上），所以这一套必须跟着状态翻。
    void refreshRunStateIfShowing(const QString &serverId);

    /// 用户按了「保存」：原文 + 表单补丁 → `server.properties`。
    void save();
    /// 用户按了「放弃改动」：把这一页按盘上那一份重新摆一遍（**不碰盘**）。
    void revert();
    /// 跑一遍体检（开页 / 保存之后 / 用户点了「重新检查」），把结论摆到那一页上。
    /// 顺带把端口那一次真监听的结论记下来（它不是个"随时都准"的事实）。
    void runCheck();

    /// 页面上那颗「重新检查」与保存带那一条小字（页面信号驱动的两个入口）。
    void updateSaveState();
    /// 切页那一下：把"表单补丁"和"原始文本"两边的编辑合起来（见 `sectionChanged` 那段）。
    void mergeEdits(int fromSection, int toSection);

    /// 这一页现在说的是哪一个服务端（没开着 = 空串）。
    const QString &serverId() const { return m_serverId; }
    /// 页面对象本身（没开过 = nullptr）。
    ///
    /// NOTE: **不叫 `page()`**：调用方那一头几乎每一段开头都是
    /// `ServerSettingsPage *page = owner->m_serverSettings->…`，而 C++ 里声明符的名字在
    /// 初始化式之前就进了作用域 —— 那样写会被解析成"对那个还没初始化的局部变量取成员"。
    ServerSettingsPage *settingsPage() const { return m_page; }
    /// 这一页现在能不能改（服务端在跑 = 不能）。
    bool editable() const { return m_editable; }
    /// 这一页此刻说的就是 @p serverId 吗（**认这一格，不许去问"列表上现在选中谁"**）。
    bool isShowing(const QString &serverId) const {
        return m_page != nullptr && !m_serverId.isEmpty() && m_serverId == serverId;
    }

private:
    /// 体检的原料：**全是现查出来的事实**（判据那一层一个都不自己查）。
    ServerCheck::Facts checkFacts() const;

    /// 把"这个服务端现在在不在跑"翻到页面上：状态那两句 + 只读闸（唯一出处）。
    /// `refresh()` 与 `refreshRunStateIfShowing()` 都走这里，免得两边的措辞和判据分叉。
    void applyRunState(ServerSettingsPage *page, const QString &serverId);

    MainWindow &m_owner;
    /// 这一页（懒建一次、之后复用，和控制台那一页同一路数）。
    ServerSettingsPage *m_page = nullptr;
    /// 这一页**现在说的是哪一个**服务端（见文件头：认它，不问列表选中谁）。
    QString m_serverId;
    /// 这一页现在能不能改（服务端在跑 = 不能）。
    bool m_editable = true;
    /// 体检里"那个端口现在有人占着吗"的结论（`ServerPortCheck::stateOf()` 那一发）。
    /// **开页 / 重新检查 / 保存之后**各查一次 —— 用户可能刚把占着它的那个程序关掉。
    ServerPortCheck::State m_portState = ServerPortCheck::State::Unknown;
};
