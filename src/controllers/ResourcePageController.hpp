/**
 * @file ResourcePageController.hpp
 * @brief 「资源详情」那一页跟业务层之间那一层皮（原来散在 `MainWindow` 里的十二个函数）。
 *
 * 【为什么单独一层】
 *
 * 这一页有它自己的一整份状态（开没开、按着哪颗 chip、那一份全量版本清单、前置那一发的
 * 请求钥匙……），而这份状态**只在这一页开着的这段时间里有意义**。原来它和 `MainWindow`
 * 上其余几百个成员摆在一起，于是"开页/关页"要记得挨个改，漏一个的症状（比如 `open`
 * 忘了落下去）很难认。挪到这一层之后，这份状态和动它的那几条函数在同一个 .cpp 里。
 *
 * 【这一层不算网络、不画界面】
 *
 * 界面在 `ui/ResourceDetailPage`（一个字节都不上网，进参出参全是值）；网在
 * `ResourceCatalog`（`fetchProjects` / `fetchVersions`）。这一层只做三件事：把值拼给
 * 页面、把页面的信号翻译成"该问谁"、把问回来的东西按钥匙认人。
 *
 * NOTE: 这一层对 `MainWindow` 是 `friend`（见 `MainWindow.hpp` 那串）：它要读主窗口上
 * 几样**本来就属于下载流程**的东西（任务编号、下载器、进程级的资源清单缓存）。这不是
 * "顺手借点东西"，是因为那些东西的生命周期比这一页长得多 —— 反过来由这一页持有，
 * 关页就得判"还有谁在用"。
 */

#pragma once

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include "download/ResourceCatalog.hpp"

class MainWindow;
class ResourceDetailPage;

class ResourcePageController final : public QObject {
    Q_OBJECT

public:
    explicit ResourcePageController(MainWindow *owner);

    /// 这一页是**怎么打开**的（只有右上角那一格摆什么需要知道，见 MainWindow 那份原注释）。
    enum class DetailSource {
        Download,      ///< 下载页点一行（那四张表 / 版本树）。
        InstanceFiles  ///< 实例设置「文件管理」页点一条文件的名字。
    };

    /// 「资源详情」那一页**这次开着的时候**说的是哪个资源、列的是哪一份版本。
    ///
    /// 一个 struct 装在一起而不是散成六个成员：这几样是**同一份状态**，散开之后
    /// "开页/关页"要记得挨个改，漏一个的症状（比如 open 忘了落下去）很难认。
    struct State {
        bool open = false;
        /// 这一条是从哪一栏点开的。拼网址（`projectPageUrl` 要 kind）、下载时判
        /// "要不要过加载器那道闸"都要它。
        ResourceCatalog::Kind kind = ResourceCatalog::Kind::Mod;
        ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth;
        QString projectId;
        /// 在 MainWindow::m_resourceHits 里的那把钥匙（`iconCacheId()`）。**同时也是
        /// "这一发版本列表回来时该不该填进页面"的判据** —— 和树那条路的请求钥匙不是一套
        /// （那边带游戏版本）。
        QString iconKey;
        /// 问版本的**请求钥匙**：游戏版本和加载器都空（= 不筛，整份列表）。
        QString requestKey;
        /// 现在按着哪一颗 chip（空 = 「全部」）。筛和分组都由 MainWindow 重算
        /// （`ResourceCatalog::groupVersions` 是那一份规则）。
        QString chip;
        /// 上方右侧那个「显示快照版」勾选框（**默认不勾**）。一位同时管三件事 ——
        /// 那一排 chip 摆哪几颗、chip 怎么筛、组头写什么（见 ResourceCatalog 那三个函数的
        /// 文件注释）；三处传的必须是**同一个值**，不然症状是「点了 26.2 少了几个版本」。
        bool showSnapshots = false;
        /// 头卡上那个资源名。下载时拼 label 的前半截要用它。
        QString title;
        /// 开页时**是从哪个实例进来的**（空 = 不是从某个实例进来的）。`from` 是
        /// `InstanceFiles` 时就是那个实例的目录名，`Download` 时是"用户上一次是从哪个
        /// 实例进下载页的"（`m_downloadReturn.instanceId`）。
        ///
        /// 它只喂两件事：开页那一下 chip 预选到**哪个**游戏版本、以及哪个加载器的组排前面。
        /// **存下来**而不是开页时用一次就扔：勾「显示快照版」那一下会把 chip 那一排整个
        /// 重摆，那时还得知道"预选的是哪一个"—— 否则用户拨一下勾选框，按着的那一颗就被
        /// 甩回「全部」了。
        QString instanceId;
        /// 上面那个实例的加载器，**换算成小写那一套**（`VersionGroup::loader` 的写法，
        /// 见 ResourceCatalog::groupVersions 的 preferredLoader）。
        QString preferredLoader;
        /// 从实例进来时那个实例的**原样**游戏版本（`1.20.1`，空 = 没有预选）。顶排 chip 预选到它
        /// 的大版本（`1.20`），下面只把细版本正好是它的那几组展开，其余组默认收起。
        QString preselectedGameVersion;
        /// 开页时接口给的那一串**声明过的游戏版本**（`Hit::gameVersions`，旧的在前）。
        ///
        /// 留着它只为一件事：勾「显示快照版」那一下要把 chip 那一排整个换一份，而那时候
        /// 手上只有这一份声明表（`Hit` 存在 m_resourceHits 里，但那一份会被后面的重搜
        /// 换掉，而这一页还开着）。
        QStringList declaredGameVersions;
        /// **全量**那份版本清单（chip 只是从它里面筛）。
        QList<ResourceCatalog::VersionEntry> versions;
        /// 那份清单**回来过**了（哪怕回来的是空的）。
        ///
        /// 为什么要单记这一位："还没回来"和"这个资源一个版本都没发过"都表现为 versions
        /// 是空的，而这两件事在界面上是**两句不一样的话**（「正在读版本列表…」和
        /// 「这个资源一个版本都没发过」）。靠 versions.isEmpty() 分不开。
        bool loaded = false;
        /// chip 那一排和版本列表中间那一行说的是"哪几条前置"。
        ///
        /// 来源是**这一页当下摆在最上面那一版**的 `dependencies`（见 fillGroups）
        /// —— 存下来而不是每次重算，是因为名字那一发是异步的：回来的时候"最上面那一版"
        /// 可能已经跟着 chip 换掉了，而这一发回来该填的是**发出去时那一份**。
        QList<ResourceCatalog::Dependency> dependencies;
        /// 问那几个前置**叫什么**的那一发（`ModDependencies::projectsKeyFor("detail", …)`）。
        /// 空串 = 这一页这会儿没有在问名字。
        QString dependencyRequestKey;
        /// **这一页已经问过名字、而且没问着的**那几个项目号（R3）。
        ///
        /// "名字缓存里没有"不等于"该再问一次"：项目被删了（200 + 空数组）、被限流、网络
        /// 一时没通，这几种都会让名字永远进不了缓存 —— 而名字没到的那一格如果还是"该问"，
        /// 每回来一次就会立刻再发一发同一把钥匙的请求（详情页开着的时候一直打接口，和
        /// 限流互相放大）。所以**问过就算数**：这一页开着的时候同一个号只问一次，
        /// 收页/换页再清（下次打开重新问一遍）。
        QSet<QString> dependencyAskedIds;
    };

    /// 懒建那一次（建一次留着，关掉之后指针照样有效）。
    void setup();

    /// 打开这一条资源的详情页（`OverlayHost::showPage()`：摆上去就返回，**不卡调用方**）。
    ///
    /// @return 这一页**摆上去了没有**。`false` = 没开出来（覆盖层正忙 / 那一类的键不
    /// 认识 / 那把钥匙不在表里），调用方什么也别做 —— 和以前 `kResourcePageNotOpened`
    /// 那一档同义。
    ///
    /// @p onClosed 在这一页收掉时叫**一次**，参数是 `QDialog::result()` 那套语义
    /// （`Rejected` = 用户收掉了；`Accepted` = 页上那颗「模组设置」）。
    bool open(const QString &iconKey, const QString &kindKey, DetailSource from,
              const QString &instanceId, std::function<void(int)> onClosed = {});
    /// 详情页上某一颗 chip 被按了（参数是那颗的 key，空串 = 「全部」）。
    void onChipChanged(const QString &gameVersion);
    /// 详情页上方那个「显示快照版」勾选框被拨了。
    void onShowSnapshotsChanged(bool showSnapshots);
    /// 按当下这颗 chip 从**全量**那份清单里筛出来、分组、摆进页面（空的那一对也走它）。
    void fillGroups();
    /// 详情页那一发"前置叫什么"回来了。
    void onProjectsFinished(const QString &requestKey, const QList<ResourceCatalog::Hit> &hits,
                            const QString &problem);
    /// 详情页上点了某一个具体版本（`Row::key` = `VersionEntry::id`）= 下它。
    void downloadVersion(const QString &versionId);
    /// 某一单下载收尾（那一单是这一页发起的时才归这里管）。
    void onDownloadFinished(const QString &label, bool success, const QString &errorMessage);

    /// 那一整页（还没建出来时是空的）。
    ResourceDetailPage *page() const { return m_page; }
    /// 这一页当下的状态（"这一发版本列表回来该不该填"由调用方看）。
    State &state() { return m_state; }
    const State &state() const { return m_state; }

private:
    /// 把那一排 chip 摆成「这一份声明表 + 这一档（折 / 不折）」该有的样子。
    void applyChips(const QStringList &declaredGameVersions, bool showSnapshots,
                    const QString &wanted);
    /// 摆 chip 与版本列表中间那一行（"这一版要装的前置"）。
    void updateDependencies();

    MainWindow &m_owner;
    State m_state;
    ResourceDetailPage *m_page = nullptr;
};
