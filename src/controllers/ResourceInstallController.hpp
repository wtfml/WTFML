/**
 * @file ResourceInstallController.hpp
 * @brief "把某一份资源装进某个实例"这一整条路：前置检查、落点、谁在动盘、开起来之后算谁的主人。
 *
 * 【为什么这一层值得单独站着】
 *
 * 这一段原来是 `MainWindow` 里的五个函数（`installResourceVersion`、`startDependencyCheck`、
 * `onDependencyProjectsFinished`、`onDependencyVersionsFinished`、`finishDependencyCheck`）
 * 加二百来行 `installResourceVersionNow()`。它们共享**一份状态**（`DependencyCheck`：
 * 正在问哪几个前置、还差几发回信、算的是哪个实例），而那份状态只在这一趟点击到"下好了"
 * 之间有 意义 —— 和主窗口上其余几百个成员摆在一起时，"这一趟结束了没有"要满地找。
 *
 * 【这一趟的形状（顺序不能换）】
 *
 * ① 点下载 → `install()` 确定落点，再看目标实例还缺哪些必装前置；
 * ② 一条不缺 → 连一个请求都不多发；
 * ③ 有缺的 → 问一次"这几个号叫什么"，再逐条问"它有哪些版本"（筛的是**目标实例**的
 *    游戏版本 + 加载器）；
 * ④ 全问完 → 弹一个框，把缺的那几个列出来，两颗按钮：「一起下载」/「只下这个」；
 * ⑤ 一起下载 → 把 `Plan::downloads` 和主模组**并成一批**下。
 *
 * 判断那一半（缺哪些 / 去哪问 / 挑哪一版 / 挑不出来怎么说）全在 `ModDependencies` 里
 * （纯函数，离线可测），这一层只接线。
 *
 * 【`installNow()` 那段的价值（每一条都带着"为什么不能反过来写"）】
 *
 * · **落点**：实例里那四类子目录，还是整合包的缓存目录（整合包不能进 `versions/` ——
 *   那底下每一个子目录都会被当成一个实例）；
 * · **前置的落点按模组算**：前置永远是模组（光影的前置是 iris 之类），不能跟着主文件的
 *   目录走 —— 跟着进 `shaderpacks/` 的话游戏不加载它，光影仍不可用而界面说装好了；
 * · **选目标**：内置实例选择器或系统文件夹选择，整合包仍走原有的缓存和新建实例流程；
 * · **谁在动盘**：只有已确定的实例目标才记实例占用、检查加载器和前置；
 * · **加载器对不上**：只管模组，判据刻意做得窄（见实现里那五条）；
 * · **任务号必须在 `downloadResourceFiles()` 之前建**：盘上都已经有的时候那一批会
 *   **同步**收尾，先建号才认领得到那一条终态。
 *
 * 【边界】
 *
 * · 在：选择落点、那几次网络问话、`downloadResourceFiles()`
 *   那一发、整合包待导入表的登记与作废。
 * · 不在：下载本身（`VersionManager`）、下载收尾（`DownloadCompletionController`）。
 */

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <optional>

#include "download/ModDependencies.hpp"
#include "download/ResourceCatalog.hpp"

class MainWindow;

class ResourceInstallController final : public QObject {
public:
    explicit ResourceInstallController(MainWindow *owner);

    /// 标题和版本按值收，选目标时的嵌套事件循环可能关闭详情页或刷新版本清单。
    void install(ResourceCatalog::Kind kind, QString displayTitle,
                 ResourceCatalog::VersionEntry version, ResourceCatalog::Source source);

    /// 名字那一发回来了（`ResourceStore::projectsFinished`）。
    void onProjectsFinished(const QString &requestKey, const QList<ResourceCatalog::Hit> &hits,
                            const QString &problem);
    /// 某一条前置的版本列表回来了（`ResourceStore::versionsFinished`）。
    void onVersionsFinished(const QString &requestKey,
                            const QList<ResourceCatalog::VersionEntry> &versions,
                            const QString &problem);

private:
    struct InstallTarget {
        QString directory;
        QString instanceId;
        QString instanceDirectory;
    };

    std::optional<InstallTarget> selectTarget(ResourceCatalog::Kind kind,
                                             const ResourceCatalog::VersionEntry &version);
    bool targetStillAvailable(const InstallTarget &target);
    void installNow(ResourceCatalog::Kind kind, const QString &displayTitle,
                    const ResourceCatalog::VersionEntry &version,
                    const QList<ModDependencies::Resolved> &extras, const InstallTarget &target);

    /// 正在跑的这一趟前置检查（**只有一趟**：用户连点两下"下载"时第二下不该再起一趟 ——
    /// 两趟会各问一遍、各弹一个框，而第一趟已经弹着的那个框正转着嵌套事件循环）。
    struct DependencyCheck {
        bool active = false;
        ResourceCatalog::Kind kind = ResourceCatalog::Kind::Mod;
        QString title;
        ResourceCatalog::VersionEntry version;
        InstallTarget destination;
        ModDependencies::Target target;
        /// 还缺的那些（`missingRequiredDependencies()` 的结果，顺序原样）。
        QList<ModDependencies::Lookup> missing;
        /// 名字那一发的钥匙（回来的不是这一把就丢 —— 用户完全可能中途又点了别的）。
        QString titleRequestKey;
        // NOTE: 这一趟**自己没有一份"项目号 → 名字"**：名字从主窗口那个会话级缓存
        // （`m_projectTitles`）里查（详情页那一行和这一趟共用它）。
        // 在这儿再存一份的症状是"同一个号在详情页上已经叫 Sodium 了、弹框里还是
        // 一串数字"—— 两份名字各写各的，谁也不报错。
        // / 钥匙 → 那一发的版本列表。**缺席 ≠ 空表**：前者是"没问到"（网络），后者是
        // / "它就是没有版本"（`ModDependencies::buildPlan` 那两句话分得清清楚楚）。
        QHash<QString, QList<ResourceCatalog::VersionEntry>> answers;
        /// 还差哪几发没回来（空 → 到齐了，见 `finishDependencyCheck()`）。
        ///
        /// **存钥匙，不是一个计数**（R7）：同一版的前置清单里同一个项目出现两次时，
        /// 两把钥匙拼出来是同一个串（`ResourceStore` 那边第二发会把第一发掐掉、而且
        /// 不回调），而计数会先加两下 —— 只回来一发，计数停在 1，这一趟永远不往下走。
        /// 之后所有模组/光影安装都被 `active` 挡着，直到重启。钥匙集合天然幂等：
        /// 同一把只等一发，回一发就移掉一格。
        QSet<QString> pendingKeys;
        /// 这个实例的 mods **认过没有**（见 `MainWindow::installedModKeys()` 的 `known`
        /// 出参）。只决定弹框那句话："这几个还没装" ↔ "无法确认你装了没有" ——
        /// **空表和"确实一个都没装"是两件事**，混成一句等于替用户认下一件我们不知道的事。
        bool installedKnown = false;
    };

    /// 起一趟前置检查：问名字 → 逐条问版本 → `finishDependencyCheck()`。
    void startDependencyCheck(ResourceCatalog::Kind kind, const QString &displayTitle,
                              const ResourceCatalog::VersionEntry &version,
                              const InstallTarget &destination,
                              const ModDependencies::Target &target,
                              const QList<ModDependencies::Lookup> &missing, bool installedKnown);
    /// 全问完了：算 Plan、弹框、按用户那一按继续装（或者只装主模组）。
    void finishDependencyCheck();

    MainWindow &m_owner;
    bool m_installing = false;
    DependencyCheck m_check;
};
