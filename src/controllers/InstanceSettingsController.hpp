/**
 * @file InstanceSettingsController.hpp
 * @brief 「实例设置」那一页**写侧**的那一套：改名那条路（`rename()`）、以及改一格就落一格
 * （`apply()`）。
 *
 * 【为什么要有一个自己的 controller】
 *
 * `MainWindow::openInstanceSettingsNow()` 一千来行，它自己不是"一个功能"，而是**十几个闭环
 * 的接线台**：构造那一页、往里灌数据、把每条信号接出去、跳页、显示。真正有判断的那几件事
 * 被夹在中间 —— 其中一件就是"这个实例改名：能不能改、谁跟着变、改到一半失败算谁的"。
 *
 * 这一层收的就是那件事。判据只有一条：**盘上变不变**（改名、改设置都是"变"），
 * 所以它们和"打开那一页""往那几个格子里灌数据"是两件事。
 *
 * 【边界（谁在这一层里，谁不在）】
 *
 * · 在：改名的**五道闸**（正在跑的不许改 / 正在体检的不许改 / 正被别的作业占着不许改 /
 * 名字合不合法 / 数据层改不动）、改完之后**谁跟着走**（`LauncherSettings` 记的"上次启动的那一个"、分组
 * 归属键的老键新键、实例树、下拉框）、以及"哪一半成了、哪一半没成"这套分账。
 * · 在：改一格就落一格那一整套 —— 先补稳定 id（没有它这次覆盖就"跟着目录名走"）、再把
 * 归属键搬过去、然后把 `Edit` 那一份拼成要写进 meta 的字段（**"跟随全局"要删键、
 * 空串要删键**这套约定在这儿的落点）、图标那几个字节、写下去、以及"要不要重扫树"那个
 * 判据（比的是写之前和写之后盘上那个值，**不是**"这次碰没碰图标那一格"）、最后是
 * 分组/置顶那三条规则。
 * · 在：**身份键**那一条规矩（有稳定 id 用 id、没有退回目录名）—— `instanceKeyFor()`
 * 是全仓唯一出处，主窗口那个同名成员只是转发（两处各写一遍的话，读侧按目录名、写侧
 * 按 id，归属会在存下去的那一刻就对不上，而且不报任何东西）。
 * · 不在：**文案**。「正在运行，先关掉游戏再改名」这句话是主窗口按 `Notice::Kind` 挑的
 * （见下），这一层只说"哪一档、是谁、数据层给的原因是什么"。
 * · 不在：把新名字填进标题、把下拉框选中它、重扫实例树、以及 `setInstanceHomeGroup` /
 * `pinInstance` / `unpinInstance` 这三个入口 —— 那些是主窗口的活，走 `Host` 里那几个
 * 回调（那三个入口各自还会发 `groupsChanged`、写失败还会弹一句，都在那边）。
 * · 不在：**问用户**。"叫什么名字"是对话框/输入框那边问的（`renameRequested()`），
 * 走到这里时用户已经答过了；这一层一步都不问，所以验证工程能直接驱动它。
 * · 不在：**那个对话框类型**（`ui/InstanceSettingsDialog`）。主窗口读一份 `Edit` 出来再
 * 交进来 —— 这一层认的是"这些值"，不是"哪一格长什么样"。所以 `apply()` 是能被验证
 * 工程直接驱动的（`applyInstanceSettings()` 收的是对话框引用，驱动不了 —— 见
 * `tests/main.cpp` 里那条注记）。
 */

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include "settings/EffectiveJavaSettings.hpp"
#include "settings/GameWindowSettings.hpp"

#include <functional>

class VersionManager;
class InstanceGroupStore;
class LauncherSettings;

class InstanceSettingsController final : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 一件"要告诉用户的事"。**事实，不是句子** —— 标题与正文由主窗口按 `kind` 挑。
     *
     * `detail` 是数据层给的那一整句原因（可能带"回滚到什么程度"这种状态说明），原样带过来、
     * 原样给用户看：这一层不加工它，也加工不了（它不认识"哪种错该怎么说"）。
     */
    struct Notice {
        enum class Kind {
            /// 改名：这个实例正在运行（Windows 上 jar 被 JVM 锁着）。
            RenameBlockedRunning,
            /// 改名：这个实例正在做完整性检查（那一趟攥着改名前算好的绝对路径）。
            RenameBlockedScanning,
            /// 改名：这个实例正被另一项作业占着（安装 / 导入 / 复制 / 快照 / 还原 ——
            /// 判据是 `MainWindow::installingVersionIds()`）。那些作业攥着的是改名前的
            /// 绝对路径，改到一半它们会往一个已经不存在的目录里写（或者重新建出来）。
            RenameBlockedBusy,
            /// 改名：名字本身不合法（`InstanceFiles::validateRenameTarget()` 说不行）。
            RenameRejected,
            /// 改名：数据层改不动（原因里可能写着回滚到什么程度）。
            RenameFailed,
            /// 改名成了，但**分组归属**没跟着搬过去。
            RenameGroupNotMoved,
            /// 实例设置：连那个稳定 id 都写不下去（`wtfml.meta` 写不进去）⇒ 整次没保存。
            SettingsIdFailed,
            /// 实例设置成了，但**分组归属**没跟着搬过去（和改名那一条同款，只是没有新名字）。
            SettingsGroupNotMoved,
            /// 实例设置：用户挑的那张图标没能落盘（**其余各项照常保存**）。
            IconNotSaved,
            /// 实例设置：`wtfml.meta` 那一次写失败（数据层给的原因在 `detail` 里）。
            SettingsSaveFailed,
        };
        Kind kind = Kind::RenameFailed;
        /// 这件事说的是哪个实例（改名那几条里是**用户敲的名字**，别的挡里是版本 id）。
        QString instanceId;
        /// 数据层给的原因；没有原因的那几档是空串。
        QString detail;
    };

    /**
     * @brief 「实时期写」这一次要落盘的那些东西 —— **全是值，没有一个 ui 类型**。
     *
     * 由主窗口拿对话框现读一份出来（那个适配器只能留在那边：`InstanceSettingsDialog`
     * 是 `ui/` 的类型，而这一层不许 include `ui/`）。这里每一个字段的**含义**都对应
     * 对话框上的一格，但"那一格摆着的是什么"和"该不该写、写成什么"是两件事 —— 后者在这一层。
     *
     * NOTE: `java` / `window` 里"有没有值"这一位**就是**界面上「跟随全局 / 自定义」的状态
     * （见 `JavaSettingsOverride` 文件头）。所以"跟随全局的要**删键**"这条规矩在这一层
     * 才写得出来：`javaOverrideMetaFields()` / `gameWindowOverrideToJson()` 收的就是
     * 这两份带 optional 的值，`Null` = 删键。
     */
    struct Edit {
        /// Java 那五项（空 optional = 跟随全局 ⇒ 删键）。
        JavaSettingsOverride java;
        /// 追加参数（**一条一个元素**；空 = 删掉那个键）。**和 `java.extraJvmArgs`
        /// 不是一回事**：这一格是对话框底下那个追加框（`QStringList`），
        /// `java.extraJvmArgs` 是"覆盖全局那一份追加参数"（一整条串）。
        QStringList extraJvmArgs;
        /// 窗口那三项（模式 / 宽 / 高）。
        GameWindowOverride window;
        /// 「提醒 Java 版本不匹配」勾着没有（勾着 = 要提醒 ⇒ 删键，照那一套约定）。
        bool warnJavaMismatch = true;
        /// 图标那一格现在指着什么（空串 = 「默认」⇒ 删键）。
        QString iconChoice;
        /// 用户**这一次**刚挑的那张本地图（空串 = 这次没挑，别动盘上那份）。
        QString pickedIconPath;
        /// 挑的那张真落盘成功之后，上面那一格该写成什么（见实现里那段 NOTE）。
        QString localIconValue;
        /// 左栏显示哪几页（顶层对象，空对象 = 七条全显示 ⇒ 删键）。
        QJsonObject pageChoices;
        /// 「置顶」那颗勾现在勾着没有。
        bool pinned = false;
        /// 「分组…」那一格现在指着哪一组（空串 = 未分组）。
        QString chosenGroupId;
        /// **打开对话框那一刻**它的老家在哪一组（判"老家被用户改过没有"用）。
        ///
        /// 这一格必须由主窗口填：算它要问"按版本分类该归哪一组"（`homeGroupIdOf()`），
        /// 那一位住在主窗口那边（它是显示层那一套：树上那一行画在哪个组里用的也是它）。
        QString homeGroupAtOpen;
    };

    /**
     * @brief 这一层要问主窗口的几件事（以及干完之后要主窗口摆的几件）。
     *
     * 全是**同步**的：`notice()` 在那一句代码的位置上当场执行 —— 顺序变了用户看到的东西
     * 就变了（提示卡片是一张盖一张的），所以不许改成排队/异步。
     */
    struct Host {
        /// 这个实例现在跑着吗（`LauncherManager::isVersionRunning()`）。
        std::function<bool(const QString &versionId)> isRunning;
        /// 这个实例现在正在做完整性检查吗（主窗口那几位扫描状态）。
        std::function<bool(const QString &versionId)> isScanning;
        /// 这个实例现在正被别的作业占着吗（`MainWindow::installingVersionIds()` ——
        /// 安装 / 导入 / 复制 / 快照 / 还原那几类）。**必须问那一份合集**：那些作业
        /// 在改名前就算好了绝对路径，改名会让它们写到已经不存在的目录里去。
        std::function<bool(const QString &versionId)> isFileOperationBusy;
        /// 弹一句（主窗口按 `kind` 挑标题与正文，再走 `InlineMessage`）。
        std::function<void(const Notice &)> notice;
        /// 记一行日志（主窗口的 `appendLog()`）。
        std::function<void(const QString &text)> log;
        /// 实例树要重扫（图标 / 名字变了，树上那一格得跟着换）。
        std::function<void()> instanceTreeChanged;
        /// 改名成了：把下拉框选中这个新 id、刷新启动按钮那句话。
        std::function<void(const QString &newId)> instanceRenamed;
        /// 换这个实例的**老家**（主窗口的 `setInstanceHomeGroup()`，那边会发
        /// `groupsChanged` 把树重灌、写失败会弹一句）。
        std::function<void(const QString &versionId, const QString &groupId)> setHomeGroup;
        /// 收藏（主窗口的 `pinInstance()`）。
        std::function<void(const QString &versionId)> pin;
        /// 取消收藏（主窗口的 `unpinInstance()`）。
        std::function<void(const QString &versionId)> unpin;
    };

    /**
     * @param versions 元数据那一层（`renameInstance()` 真干活的地方）。
     * @param groups 分组归属（改名/换 id 之后键要跟着挪）。
     * @param settings 启动器设置（里面记着"上次启动的那一个"是哪个 id）。
     */
    InstanceSettingsController(VersionManager *versions, InstanceGroupStore *groups,
                               LauncherSettings *settings, Host host,
                               QObject *parent = nullptr);

    /**
     * @brief 这个实例的**身份键**：有稳定 id 用它，没有就退回目录名。
     *
     * **唯一出处**。主窗口那个同名成员转发到这里；`InstanceGroupStore` 文件头那段解释了
     * 为什么"有 id 用 id"（目录名会被改，改名之后按目录名记的东西全丢）。
     */
    static QString instanceKeyFor(const VersionManager *versions, const QString &versionId);

    /**
     * @brief 把 @p oldId 改名叫 @p newId（**用户已经答过名字了**）。
     *
     * 五道闸按顺序：正在跑 → 正在体检 → 正被别的作业占着 → 名字不合法 → 数据层改不动。任何一道拦下来都
     * 返回 false，并且**把原因弹出去**（`Notice`）—— 不循环重问：用户看到原因后可以自己
     * 再点一次，那样他能顺便改别的决定。
     *
     * 成了之后按顺序：`LauncherSettings` 里那个"上次启动的"→ 分组归属 → 实例树 →
     * 下拉框 → 日志。@return 名字真的改了没有（分组没搬过去**不算失败**，那是另一件事，
     * 用户看到的是"哪一半没成"）。
     */
    bool rename(const QString &oldId, const QString &newId);

    /**
     * @brief 把 @p edit 这一份覆盖写进这个实例的 `wtfml.meta`，并摆好分组/置顶。
     *
     * **实时写盘**：`settingsEdited()` 每变一格就来一次，所以这条路上任何"无条件重扫
     * 实例树"都等于用户每敲一下就全盘扫一遍目录（见实现里那个"图标变了没有"的判据）。
     *
     * 失败时弹窗（`Notice`）并返回 false；**分组没搬过去 / 图标没存上都不算失败** —— 各自
     * 报一声，其余各项照常保存（理由写在实现里那两段 NOTE 里：为一件小事把整次保存赔进去
     * 是替用户做主）。
     *
     * @param pickedIconStoredOut 非空时写入"用户刚挑的那张图**真的落进实例目录了**没有"
     * （没挑 = false；挑了指到一半失败也没成）。调用方靠这一位决定要不要让对话框
     * **忘掉那个源路径** —— 详见 `InstanceSettingsDialog::markPickedIconSaved()` 那段。
     *
     * @return 那份 meta 真的写下去了没有。
     */
    bool apply(const QString &versionId, const Edit &edit, bool *pickedIconStoredOut = nullptr);

    /// 这个实例的身份键（`instanceKeyFor()` 的成员版本，省得到处传 manager）。
    QString instanceKey(const QString &versionId) const;

private:
    VersionManager *m_versions = nullptr;
    InstanceGroupStore *m_groups = nullptr;
    LauncherSettings *m_settings = nullptr;
    Host m_host;
};
