/**
 * @file ServerListController.hpp
 * @brief 「服务器列表」那一页上会动 `servers.dat` 的那三件事：**改一台 / 加一台 / 删一台**。
 *
 * 【为什么要有一个自己的 controller】
 *
 * `MainWindow::openInstanceSettingsNow()` 那个一千行的接线台上，服务器那一支占着最长的一段：
 * 三条链路各自"问两句 → 落盘 → 记一行 → 重读重测"，每一段都夹在 `connect` 和
 * `InlineMessage::getText()` 中间。真正有判断的四件事被埋在里面：
 *
 * ① **一个字节都不写**：改一台时两样都没变就直接回头（空转一遍会把 mtime 推新、
 * 把 gzip 那一层重压一次 —— 盘上那份东西凭什么变？）；
 * ② **地址必填**：加一台时空地址**在问话之后立刻**挡下来，不让它一路走到落盘再报错；
 * ③ **「（无名）」这条显示规矩**（见 `displayName()`）—— 它同时出现在日志和"删哪一台"
 * 那句问话里，两处**必须**是同一个出处；
 * ④ **改完 / 加完 / 删完一律重读重测**（`changed()`）—— 不新开一套"只换一行"的路：
 * 新表会让那一台的图标 / MOTD / 延迟回到"还没测"。
 *
 * 【边界（谁在这一层里，谁不在）】
 *
 * · 在：上面那四条判据、三次落盘（`ServerListFile`）、三行日志。
 * · 在：**"要问用户哪一句"这件事的划分**（`Ask`）—— 但**那句话本身不在这一层**：
 * 只有主窗口手里有 `OverlayHost`，所以四句问话从 `Host::ask()` 回出去。
 * · 不在：**测速**。这一层只喊一声 `changed()`，重读那份 servers.dat 和重测一遍
 * 都在主窗口那一层（"测速"是另一件事，见 `pingServers()`）。
 * · 不在：「加入服务器」那颗和地址右键复制 —— 那两件**一个字节都不写盘**（一个是
 * `startQuickPlay()`、一个进剪贴板），和这三件不是一类。
 * · 不在：读侧（`loadServerList()` / `serverListOf()`），那是把盘上那份摆到界面上。
 */

#pragma once

#include <QObject>
#include <QString>

#include <QList>
#include <QPair>

#include <functional>

class ServerListController final : public QObject {
    Q_OBJECT

public:
    /// 要问用户的哪一句。**判断的是"问哪一格"，不是"那句话怎么说"**（文案在主窗口那儿）。
    ///
    /// 四档而不是两档："加一台"那一档的**名字**要明说"可以留空"（`servers.dat` 里 `name`
    /// 真能是空串，游戏那边显示成未命名），而"改一台"那一档不该这么说（那一台本来就有名字）。
    enum class Ask {
        /// 「编辑服务器」：这一台叫什么（预填原来那个）。
        EditName,
        /// 「编辑服务器」：地址是什么（预填原来那个）。
        EditAddress,
        /// 「添加服务器」：这一台叫什么（**可以留空**，不预填）。
        AddName,
        /// 「添加服务器」：地址是什么（不预填）。
        AddAddress,
    };

    /// 一件"要告诉用户的事"。**事实，不是句子** —— 标题与正文由主窗口按 `kind` 挑。
    ///
    /// `detail` 是数据层（`ServerListFile`）给的那一整句原因，原样带过来、原样给用户看。
    struct Notice {
        enum class Kind {
            /// 改一台：`updateEntry()` 说不行（原因在 `detail` 里）。
            UpdateFailed,
            /// 加一台：地址是空的 —— **在问话之后立刻挡**，不让它走到落盘再报错。
            AddRejected,
            /// 加一台：`addEntry()` 说不行（原因在 `detail` 里）。
            AddFailed,
            /// 删一台：`removeEntry()` 说不行（原因在 `detail` 里）。
            RemoveFailed,
        };
        Kind kind = Kind::UpdateFailed;
        QString detail;
    };

    /**
     * @brief 这一层要问主窗口的几件事（以及干完之后要主窗口摆的）。
     *
     * 全是**同步**的：`ask()` 在那一句代码的位置上当场执行 —— 顺序变了用户看到的顺序就变了。
     */
    struct Host {
        /// 问一句。@p initial 是预填（空 = 不预填）；@return false = 用户取消了。
        std::function<bool(Ask ask, const QString &initial, QString *out)> ask;
        /// 「删这一台吗」那一句（`who` 已经过了 `displayName()`，直接用）。
        std::function<bool(const QString &who, const QString &address)> confirmRemove;
        /// 弹一句（主窗口按 `kind` 挑标题与正文，再走 `InlineMessage`）。
        std::function<void(const Notice &)> notice;
        /// 记一行日志（主窗口的 `appendLog()`）。
        std::function<void(const QString &text)> log;
        /// 那份 `servers.dat` 变了：重读一遍、再顺手测一遍（主窗口那一层）。
        ///
        /// 带 @p instanceId 而不是收一份捕获好的：那三条链路上 @p instanceId 是**按引用**
        /// 活着的那个 `targetId`（改名之后它变了），而这一层每次调用都现传一次。
        std::function<void(const QString &instanceId)> changed;
        /// 「删选中的 N 台吗」那一句（批量删除）。放在末尾：前面几格按位置初始化。
        std::function<bool(int count)> confirmRemoveMany;
    };

    explicit ServerListController(Host host, QObject *parent = nullptr);

    /// 一台服务器在界面上的显示名：**空名字那一档是「（无名）」**。
    ///
    /// **唯一出处**。它同时出现在三行日志和"删哪一台"那句问话里 —— 两处各写一遍的话，
    /// 迟早会一处写「（无名）」、一处写「未命名」，而用户看到的是"日志和问话说的不是同一台"。
    ///
    /// NOTE: 空串是**合法**的（`servers.dat` 里的 `name` 真能是空的，游戏那边显示成未命名），
    /// 所以这不是"缺了个值"，是一条正经的显示规矩。
    static QString displayName(const QString &name);

    /**
     * @brief 改一台：两句问话（先说名字、再报地址）→ 两样都没变就回头 → 落盘 → 记一行。
     *
     * 两句的顺序和「导入世界」那一套一致。任何一句取消、或者两样都没变，都返回 false
     * 并且**什么都不写**。
     */
    bool edit(const QString &instanceId, const QString &oldName, const QString &oldAddress);

    /**
     * @brief 加一台：两句问话 → 地址必填那道闸 → 落盘 → 记一行。
     *
     * 走 `ServerListFile::addEntry()` —— 那个入口在**文件根本不存在**时也不当失败
     * （那正是"这个实例第一次加服务器"）。
     */
    bool add(const QString &instanceId);

    /**
     * @brief 删一台：先问一句（**点名字**，见 `displayName()`）→ 落盘 → 记一行。
     *
     * 「认哪一条」靠**按值匹配**（名字 + 地址，退一步只按地址），不许按下标 ——
     * 理由见 `ServerListFile` 那段。
     */
    bool remove(const QString &instanceId, const QString &name, const QString &address);

    /**
     * @brief 删多台：问一句（点数量）→ 一次读写 `servers.dat` → 记一行 → 重读重测。
     *
     * 任何一条认不出就整批不写，选择保持原样（界面不重读）。
     * @param entries (名字, 地址) 列表，每条的含义同 `remove()`。
     */
    bool removeMany(const QString &instanceId, const QList<QPair<QString, QString>> &entries);

private:
    Host m_host;
};
