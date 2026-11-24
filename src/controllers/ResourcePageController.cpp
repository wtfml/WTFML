#include "controllers/ResourcePageController.hpp"
#include "TextMarks.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>
#include "MainWindow.hpp"
#include "MainWindowViewData.hpp"
#include "OverlayHost.hpp"
#include "ResourceDetailPage.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "download/ResourceStore.hpp"
#include "download/ModDependencies.hpp"
#include "download/ResourceCatalog.hpp"
#include "InlineMessage.hpp"

using namespace MainWindowViewData;

ResourcePageController::ResourcePageController(MainWindow *owner) : QObject(owner), m_owner(*owner) {}

void ResourcePageController::setup() {
    if (m_page != nullptr) {
        return;
    }
    m_page = new ResourceDetailPage(&m_owner);
    connect(m_page, &ResourceDetailPage::modSettingsRequested, this, [this] {
        if (m_state.open && m_page->modSettingsSlotShown()
            && !m_state.instanceId.isEmpty()) {
            m_page->accept();
        }
    });
    connect(m_page, &ResourceDetailPage::chipChanged, this,
            &ResourcePageController::onChipChanged);
    connect(m_page, &ResourceDetailPage::showSnapshotsChanged, this,
            &ResourcePageController::onShowSnapshotsChanged);
    connect(m_page, &ResourceDetailPage::rowActivated, this,
            &ResourcePageController::downloadVersion);
    // 开浏览器 / 复制名字**由这一层动手**（那一页只发信号）：开浏览器要走
    // QDesktopServices（验证工程把 url handler 换掉就能拦，见那一页的文件头），
    // 复制要能进日志。
    connect(m_page, &ResourceDetailPage::openExternalRequested, this,
            [this](const QString &url) {
                if (url.isEmpty()) {
                    return;
                }
                m_owner.appendLog(tr("正在打开 %1。").arg(url));
                // 和"打开实例文件夹"那条走的是同一个东西（都是"把这件事交给系统"）。
                // 开不了要说一句：那颗按钮看着像是我们自己开的页面，静静地什么都不发生，
                // 用户只会一遍遍点它。
                if (!QDesktopServices::openUrl(QUrl(url))) {
                    InlineMessage::warning(m_owner.m_ui->overlay(), tr("无法打开默认浏览器"),
                                           tr("无法打开网址：\n%1").arg(url));
                }
            });
    connect(m_page, &ResourceDetailPage::copyNameRequested, this,
            [this](const QString &text) {
                if (text.isEmpty()) {
                    return;
                }
                // 复制完**不弹话**：这一下是用户自己按的，而剪贴板里那串东西他自己
                // 粘一下就看得见。进日志留个痕就够（排查"我复制的怎么不对"时有用）。
                if (QClipboard *clipboard = QApplication::clipboard()) {
                    clipboard->setText(text);
                    m_owner.appendLog(tr("已复制名称：%1").arg(text));
                }
            });
}

void ResourcePageController::applyChips(const QStringList &declaredGameVersions,
                                        bool showSnapshots, const QString &wanted) {
    if (m_page == nullptr) {
        return;
    }
    // 第一颗是「全部」（key 空串 = 不筛）。其余每一颗的 key **和文字是同一串** ——
    // 这一排上没有"显示名"这回事（游戏版本号本身就是用户在界面上要找的那个东西）。
    // 顶排摆的是**大版本组**（1.21、1.20……，最后一个「其他」），点一个 = 按这个大版本筛，
    // 具体到哪个细版本由下面那些文件分组的标题说。组的算法见 gameVersionSeriesOf。
    QList<ResourceDetail::Chip> chips;
    ResourceDetail::Chip all;
    all.text = tr("全部");
    chips.append(all);
    for (const QString &series :
         ResourceCatalog::gameVersionSeriesChoices(declaredGameVersions, showSnapshots)) {
        ResourceDetail::Chip chip;
        chip.key = series;
        chip.text = series == ResourceCatalog::otherSeriesKey() ? tr("其他") : series;
        chips.append(chip);
    }
    m_page->setChips(chips, wanted);
}

bool ResourcePageController::open(const QString &iconKey, const QString &kindKey,
                                  DetailSource from, const QString &instanceId,
                                  std::function<void(int)> onClosed) {
    OverlayHost *overlay = m_owner.m_ui->overlay();
    if (overlay == nullptr || overlay->isOpen()) {
        // 已经有覆盖层开着（另一页、或者一条提示）。这一层不做页面栈，这一次就不开了 ——
        // 和 `m_owner.openPage()` 返回 nullptr 是同一件事（见 OverlayHost.hpp 那段）。
        return false;
    }
    ResourceCatalog::Kind kind = ResourceCatalog::Kind::Mod;
    if (!ResourceCatalog::kindFromCategoryKey(kindKey, &kind)) {
        return false;  // 这一行不是我们建的（正常走不到）
    }
    const auto hit = m_owner.m_resourceHits.constFind(iconKey);
    if (iconKey.isEmpty() || hit == m_owner.m_resourceHits.constEnd()) {
        // 条目上那把钥匙在表里找不到 = 这一行是上一批搜索留下的（重搜过）。用户没做错
        // 什么，不弹窗（和点第三层时那句"清单里没它"同一个规矩）。
        return false;
    }
    setup();
    ResourceDetailPage *page = m_page;
    if (page == nullptr) {
        return false;
    }

    // 这一次说的是哪个资源、列的是哪一份。**开页之前就要摆好**：下面那一句一摆上去就
    // 返回调用方了，收页之后要读的这些字段得在那一刻就已经是这一份。
    m_state.open = true;
    m_state.kind = kind;
    m_state.source = hit->source;
    m_state.projectId = hit->projectId;
    m_state.iconKey = iconKey;
    m_state.title = hit->title;
    m_state.chip.clear();
    m_state.showSnapshots = false;
    m_state.versions.clear();
    m_state.loaded = false;
    // 前置名字"这一页问过谁"跟着新页从头算（R3）：上一个资源问不着的那几个号不该让
    // 这一个资源的前置也问不出来。
    m_state.dependencyAskedIds.clear();
    m_state.dependencyRequestKey.clear();
    // 钥匙里游戏版本和加载器都空 = **不筛**（"这个资源一共发过哪些版本"）。
    // 这也正是它和树那条路的钥匙不会撞的原因：树上那把一定带着一个游戏版本号。
    m_state.requestKey = ResourceCatalog::versionsRequestKey(
        hit->projectId, QString(), QString(), hit->source);

    ResourceDetail::Header header;
    header.title = hit->title;
    header.description = hit->description;
    header.metaLine = ResourceCatalog::hitMetaLine(*hit);
    // 「转到 Modrinth / CurseForge」那两颗：网址是**那家自己网页的形状**（见
    // projectPageUrl，类名拼错了回 404 而不报错）。拼不出来（slug 空）那一页就不画那颗
    // 按钮 —— 摆一颗点了没反应的键比不摆糟。
    header.openLabel = ResourceCatalog::sourceLabel(hit->source);
    header.openUrl = ResourceCatalog::projectPageUrl(hit->source, kind, hit->slug);
    // MC 百科那边**没有按 id 直接跳的路**（它是另一套号），只能拿名字去搜 —— 这是
    // "跳到那儿附近"，不是"跳到那一页"。
    header.mcmodUrl = ResourceCatalog::mcmodSearchUrl(hit->title);
    header.nameToCopy = hit->title;
    page->setHeader(header);

    // 中间那一排游戏版本 chip：**搜索响应里本来就带着**"这个资源支持哪些游戏版本"
    // （`Hit::gameVersions`，顺序由 gameVersionChoices 统一成新的在前），所以开页这一下
    // 不用为它多发一条请求 —— 勾「显示快照版」那一下也一样，重算用的是存下来的这一份。
    //
    // NOTE: 这一份**声明表要留着**（不止是拿来摆一次 chip）：勾选框一拨，chip 那一排整个
    // 换一份（折 / 不折是两份不一样的表），那时候手上只有开页时存下来的它。
    m_state.declaredGameVersions = hit->gameVersions;

    // ---------- 从某个实例进来的那一次：预选到**那个实例的**版本 / 加载器----------
    //
    // 从某个实例点进来时，三件事一起落到这儿：版本预选到当前实例对应的那个、
    // 对应加载器的版本排前面、资源包同理（展示为对应版本适用的）：
    // ① chip 预选到那个实例的游戏版本 ⇒ 底下那一份版本列表**只列适用于那个版本的**
    // （筛就是 `versionsForGameVersion()`，本来就是每按一颗 chip 做一次的事）；
    // ② 分组时把那个实例的加载器的组排到前面（`groupVersions()` 的 `preferredLoader`）；
    // ③ 预选**存进 `m_state`**：勾「显示快照版」那一下要把 chip 那一排整个重摆，
    // 那时还得知道"按着的是哪一个"，不然用户拨一下勾选框就被甩回「全部」了。
    //
    // NOTE: 那两份值是**现在**读的（`instanceGameVersion` 读盘）。开页这一下只读一次，
    // 一点问题没有；**别**把它挪进 `fillGroups()` 那种每按一颗 chip 就
    // 跑一遍的路径（那是每按一下读一次盘）。
    // NOTE: 加载器要从**显示名**（「Fabric」/「NeoForge」）换算成**小写那一套**
    // （`VersionGroup::loader` 的写法），换算在下面那个 helper 里 —— 直接递显示名进去
    // 会一个字都比不上，症状是"排在前面"永久不生效而屏幕上一点异常都没有。
    m_state.instanceId = instanceId;
    m_state.preferredLoader.clear();
    QString wantedChip;
    m_state.preselectedGameVersion.clear();
    if (!instanceId.isEmpty()) {
        m_state.preferredLoader = loaderGroupKey(
            instanceLoaderName(m_owner.m_versionManager->displayParts(instanceId), m_owner.m_versionManager, nullptr));
        // 顶排预选到实例版本的**大版本**（`1.20.1` → `1.20`）。这个资源压根没声明那个大版本
        // （顶排没有那一颗）时，setChips 会纠正成「全部」，和用户自己点了一颗表里没有的 chip 一样。
        m_state.preselectedGameVersion = m_owner.m_versionManager->instanceGameVersion(instanceId);
        const QString series =
            ResourceCatalog::gameVersionSeriesOf(m_state.preselectedGameVersion);
        wantedChip = series.isEmpty() && !m_state.preselectedGameVersion.isEmpty()
                         ? ResourceCatalog::otherSeriesKey()
                         : series;
    }
    m_state.showSnapshots = ResourceCatalog::isSnapshotGameVersion(m_state.preselectedGameVersion);
    page->setShowSnapshots(m_state.showSnapshots);
    applyChips(hit->gameVersions, m_state.showSnapshots, wantedChip);
    // 按着的是哪一颗**以页面为准**（和 onResourcePageShowSnapshotsChanged 里那一句逐字
    // 同一个理由）：上一步要是把"表里没有的那个 key"纠正成了「全部」，两位得说的是同一
    // 件事。**这一句不能省** —— `m_state.chip` 就是 `fillGroups()`
    // 拿来筛的那一份，光把 chip 按下去而这一位还空着，症状是"那一颗亮着、底下却列着
    // 全部版本"（看着像筛没生效）。
    m_state.chip = m_page->currentChip();

    // 详情页复用，按本次来路更新「模组设置」入口。上面已按实例版本预选过滤条件。
    page->setModSettingsSlot(from == DetailSource::InstanceFiles && kind == ResourceCatalog::Kind::Mod);
    // 图标是异步来的：先清掉上一次那一张（不然会留着上一个资源的图标，比空着糟），
    // 那一格的位置照旧留着，图回来时整张卡不会往左跳一下。
    page->setIcon(QPixmap());
    page->setLoading(tr("正在读版本列表…"));
    // 中间那一行也**每次开页清一次**（同上：这一页是建一次留着的，不清就留着上一个
    // 资源的"要装的前置"，而那一行看着像是这个资源的）。
    m_state.dependencies.clear();
    m_state.dependencyRequestKey.clear();
    page->setDependencies(QString());

    // 图标：列表那几行早就为这一条要过了（见 fillResourceHitRows 最后那句），内存/盘上
    // 命中就是**同步**回来的 —— 于是这一句常常在下面 showPage 之前就把图摆好了。
    m_owner.m_resourceStore->requestIcon(*hit);
    // 版本列表（整份，不筛）。答复落在 onResourceVersionsFinished 里，判据是
    // `m_state.requestKey` —— 和树那条路按行重算钥匙的做法不同：那一页没有
    // 条目可查（它自己那份清单是空的），所以这儿直接比钥匙。
    m_owner.m_resourceStore->fetchVersions(m_state.requestKey, hit->projectId, hit->source,
                                   QString());

    // **最后**才开页：摆上去就返回，收页之后才走下面那条钩子。
    // NOTE: 上面发出去的两条请求都是异步的，答复在开页之后落地 —— 所以"先发请求、
    // 再开页"和"先开页、再发请求"对用户是同一件事。
    if (!overlay->showPage(page, page->titleText())) {
        // 摆不上去（布局还没搭好那种）：这一趟就当没开。状态原样落回去，和没进来一样。
        m_state.open = false;
        return false;
    }
    // 收页钩子：所有收页路（返回箭头 / Esc / 页上那颗「模组设置」/ `dismiss()` /
    // `evict()` / `closeOverlay()`）全汇到 `pageClosed()` 那一处。接在**页面对象**上
    // 当上下文 —— 这一页是懒建留着的，下一次开页会重新接一条；连接跟着页面对象活，
    // 而不是跟着某一次开页活。
    //
    // NOTE: 状态在这里落下去（`m_state.open = false`），**页面本身留着**（它是懒建的
    // 那一份），下次点开还是它，setHeader/setChips 会把上一次那些字全换掉。
    connect(
        overlay, &OverlayHost::pageClosed, page,
        [this, page, onClosed](QWidget *closed) {
            if (closed != page) {
                return;
            }
            m_state.open = false;
            m_state.dependencyAskedIds.clear();
            if (onClosed) {
                onClosed(page->result());
            }
        },
        Qt::SingleShotConnection);
    return true;
}

void ResourcePageController::onChipChanged(const QString &gameVersion) {
    if (!m_state.open) {
        return;
    }
    m_state.chip = gameVersion;
    fillGroups();
}

void ResourcePageController::onShowSnapshotsChanged(bool showSnapshots) {
    if (!m_state.open || m_page == nullptr) {
        return;
    }
    if (m_state.showSnapshots == showSnapshots) {
        // 值没变（比如开页那一下把勾选框摆回去）。**不往下走**：下面那一段会把 chip 那一排
        // 整个重建，而重建会清掉用户正按着的那一颗的按压状态（虽然换算之后还是它）——
        // 白白闪一下不值当。
        return;
    }
    m_state.showSnapshots = showSnapshots;
    // chip 那一排整个换一份（折与不折是两份不一样的表）。顶排的 key 是大版本，26.2-rc1 和
    // 26.2 同属 `26.2`，所以按着的那一颗原样保留；那一组在另一档里没有了（只声明过周快照的
    // 「其他」）就由 setChips 纠正成「全部」。
    applyChips(m_state.declaredGameVersions, showSnapshots, m_state.chip);
    // 按着的是哪一颗**以页面为准**：换算出来的那一颗要是表里没有（对不上），`setChips`
    // 会把它纠正成「全部」，这儿跟着抄回来 —— 两个状态各说各的才是真出问题的地方。
    m_state.chip = m_page->currentChip();
    // setChips **不发** chipChanged（它不是"用户点了"那一发）—— 所以这一句得自己来，
    // 它就是「更改以后自动刷新版本列表」。
    fillGroups();
}

void ResourcePageController::fillGroups() {
    if (!m_state.open || m_page == nullptr) {
        return;
    }
    // 还没回来：chip 是在这一页上点的，而版本列表可能还在路上（用户手快）。
    // **不能**在这儿说"这一档没有版本" —— 那是在替一份还没到的数据回答。
    if (!m_state.loaded) {
        m_page->setLoading(tr("正在读版本列表…"));
        return;
    }
    // 这一页当下摆在**最上面那一版**（= 用户一眼看到、最可能点的那一条）。下面那一段
    // 版本列表中间那一行说的是"这一版要装的前置"，而"这一版"
    // 指的就是它。取的是**分组之后**的最前一条（`groupVersions` 把新游戏版本那一组摆在
    // 最前、组内也是新的在前），不是 filtered 的第一条 —— 两条在"筛了某一档"时能不一样。
    // NOTE: 存的是**一份拷贝**不是指针：`groupVersions()` 那一串是这一句里的临时表，
    // 指着它出了这个 for 就是悬空的（而这一页的状态要活到下一次刷）。
    ResourceCatalog::VersionEntry topmost;
    bool hasTopmost = false;

    // 筛（看的是这一版**声明过**的每一个游戏版本，不是它落在哪个组里 —— 那条规矩和它的
    // 坑都在 versionsForGameVersion 的注释里），再按「游戏版本 + 加载器」分组。
    // 两步**分开**：合并了就是上面那条坑（筛 1.20.1 会漏掉一半支持它的版本）。
    const QList<ResourceCatalog::VersionEntry> filtered = ResourceCatalog::versionsForSeries(
        m_state.versions, m_state.chip, m_state.showSnapshots);
    // 从实例进来时：声明过实例那个细版本的组摊开，其余组收起。看的是组里条目声明过的版本，
    // 不是组头写的那个（一条同时支持 1.20.1 / 1.20.2 的文件落在 1.20.2 那一组）。
    const QString pre = m_state.preselectedGameVersion;
    const QString preFolded = ResourceCatalog::majorGameVersionOf(pre);
    const auto declaresPreselected = [&](const ResourceCatalog::VersionEntry &entry) {
        for (const QString &declared : entry.gameVersions) {
            if (declared == pre ||
                (!m_state.showSnapshots && ResourceCatalog::majorGameVersionOf(declared) == preFolded)) {
                return true;
            }
        }
        return false;
    };
    QList<ResourceDetail::Group> groups;
    // 第三个参数 = 从实例进来时那个实例的加载器（**小写**，见 loaderGroupKey）：同一个游戏
    // 版本里把它的组排到前面（要求「对应的模组加载器的版本放在前面」）。不是从实例
    // 进来的那一次是空串 = 照旧按固定的 fabric/forge/neoforge/quilt 次序。
    for (const ResourceCatalog::VersionGroup &group :
         ResourceCatalog::groupVersions(filtered, m_state.showSnapshots,
                                        m_state.preferredLoader)) {
        ResourceDetail::Group out;
        // 组头写的是**这一条落在哪个游戏版本下**（分组规则见 groupVersions）。和上面选的
        // 那一档不一致是正常的（一条支持两档的版本落在新的那一档下），**不加**「（所选
        // 版本）」那种后缀：每一组都挂一句一样的话，那一句就不是信息了。
        out.title = group.title();
        for (const ResourceCatalog::VersionEntry &entry : group.versions) {
            if (!pre.isEmpty() && declaresPreselected(entry)) {
                out.expanded = true;
            }
            if (!hasTopmost) {
                topmost = entry;
                hasTopmost = true;
            }
            ResourceDetail::Row row;
            row.key = entry.id;
            // 正文写**版本号**（不是文件名）：这一页一屏几十条版本，认人的是版本号；
            // 文件名在小字那一截里（versionRowSubText 的第二截）。树那条路正相反
            // （那一层是"挑一个文件下来"，写的就是落盘那个名字）—— 两处不一样是有意的。
            row.title = entry.versionNumber;
            row.subText = ResourceCatalog::versionRowSubText(entry);
            row.channel = ResourceCatalog::releaseChannelRank(entry.versionType);
            const ResourceCatalog::FileEntry *file = entry.primaryFile();
            if (file != nullptr && !file->filename.isEmpty()) {
                // 鼠标停上去看**要下哪个文件**（正文放不下、也不该放）。
                row.tooltip = file->filename;
            }
            out.rows.append(row);
        }
        groups.append(out);
    }
    // 中间那一行（"这一版要装的前置"）跟着**这一次摆上去的那一份**走 —— 列表空的（下面
    // 两种空）就整个收起来：那时候屏幕上一条版本都没有，"这一版"指的是哪一版就说不清了。
    m_state.dependencies =
        hasTopmost ? ModDependencies::displayedRequiredDependencies(topmost)
                   : QList<ResourceCatalog::Dependency>();
    updateDependencies();

    if (groups.isEmpty()) {
        // 两种"空"**不是同一句话**：选了某一档之后一条都没有（这一档上没有），和这个资源
        // 一条版本都没发过（那一页自己那句，`setGroups()` 收到空表时摆的就是它）。
        if (m_state.chip.isEmpty()) {
            m_page->setGroups({});
        } else {
            m_page->setPlaceholder(tr("这个游戏版本上没有可下载的版本。"));
        }
        return;
    }
    m_page->setGroups(groups);
}

void ResourcePageController::updateDependencies() {
    if (!m_state.open || m_page == nullptr) {
        return;
    }
    const QList<ResourceCatalog::Dependency> &dependencies = m_state.dependencies;
    if (dependencies.isEmpty()) {
        m_page->setDependencies(QString());   // 没有前置 = 这一行不画
        m_state.dependencyRequestKey.clear();
        return;
    }

    // 名字：先查会话缓存，缺的那几个凑成一批去问（**只问缺的**——已经知道的不该再问一遍，
    // 而这一行每换一次 chip 就要重算一次）。
    QStringList names;
    names.reserve(dependencies.size());
    QStringList unknownIds;
    for (const ResourceCatalog::Dependency &dependency : dependencies) {
        const QString name =
            m_owner.m_projectTitles.value(m_owner.projectTitleKey(m_state.source, dependency.projectId));
        if (name.isEmpty()) {
            // 这一页已经问过、而且没问着的（项目被删 / 限流 / 网络没通）**不再问**：
            // 再问一次的结果多半还是空的，而"回来一次就再发一发"正是 R3 那个死循环。
            // 这一页开着的时候只问一次，收页再清（见 `dependencyAskedIds`）。
            if (!unknownIds.contains(dependency.projectId) &&
                !m_state.dependencyAskedIds.contains(dependency.projectId)) {
                unknownIds.append(dependency.projectId);
            }
        }
        names.append(name);   // 空名字也占一格（下面按"有没有空串"判整个名单全不全）
    }

    if (unknownIds.isEmpty()) {
        m_page->setDependencies(ModDependencies::detailPrerequisiteText(names));
        m_state.dependencyRequestKey.clear();
        return;
    }

    // 名字还没到：**先摆"有几条"那句**，别空着 —— 空着的症状是"这一版到底要不要前置"
    // 看不出来，而那正是用户要这一行的理由。名字回来之后再换成那一串。
    m_page->setDependencies(
        ModDependencies::detailPrerequisiteCountText(dependencies.size()));
    m_state.dependencyRequestKey = ModDependencies::projectsKeyFor(
        QStringLiteral("detail"), unknownIds, m_state.source);
    // 记下来"这几个号这一页已经问过了"：回来的是空表/报错，名字仍然不在缓存里，下一趟
    // 走到这里时它们**不会再被凑进 unknownIds**（见 `dependencyAskedIds`）。成功拿到名字
    // 的那几个本来就会命中缓存，记不记都一样。
    for (const QString &id : unknownIds) {
        m_state.dependencyAskedIds.insert(id);
    }
    m_owner.m_resourceStore->fetchProjects(m_state.dependencyRequestKey, unknownIds,
                                   m_state.source);
}

void ResourcePageController::onProjectsFinished(const QString &requestKey,
                                            const QList<ResourceCatalog::Hit> &hits,
                                            const QString &problem) {
    // 不是这一页等的那一发就丢。**别在这里顺手替"前置那一趟"收名字**：两边的来源可能
    // 不一样（详情页那个资源是 Modrinth 的、要装进去的那个实例里那个模组是 CurseForge
    // 的），而来源是那把键的一半 —— 拿错来源写进缓存的名字会挂在**错误的那把键**上，
    // 于是另一个人永远查不到它（症状和"名字没取到"一模一样）。各自收各自的。
    if (!m_state.open || requestKey.isEmpty()
        || requestKey != m_state.dependencyRequestKey) {
        return;
    }
    m_owner.rememberProjectTitles(hits, m_state.source);
    // 没取到**不是错误**（那一页上还有"有几条"那句），所以不弹窗、只记一句日志 ——
    // 和前置那一趟同一条规矩（见 `ResourceInstallController::onProjectsFinished`）。
    if (!problem.isEmpty()) {
        m_owner.appendLog(tr("前置模组的名字没取到：%1").arg(problem));
    }
    // 名字已经进缓存了：再走一次那一段就会走到"全都有了"那条路上去。**问不着也一样**：
    // 那几个号已经记进 `dependencyAskedIds`，不会再被凑成新的一发（见 `updateDependencies`
    // 那段 —— 这里曾经写的是"缺的那几个现在是空的、不会再发"，可"空"恰恰是它每回来一次
    // 就再发一次的原因，R3 就是这条注释与事实相反的地方）。
    updateDependencies();
}


void ResourcePageController::downloadVersion(const QString &versionId) {
    if (!m_state.open || versionId.isEmpty()) {
        return;
    }
    for (const ResourceCatalog::VersionEntry &entry : m_state.versions) {
        if (entry.id == versionId) {
            // 和树上点第三层那一下**是同一件事**（选哪个实例、落到哪个目录、加载器那道闸、
            // 整合包下完接导入，全在那边那一个函数里）。
            m_owner.installResourceVersion(m_state.kind, m_state.title, entry,
                                   m_state.source);
            return;
        }
    }
    // 行在、这份清单里没有它：正常走不到（那些行就是从这份清单摆出来的）。
}

void ResourcePageController::onDownloadFinished(const QString &label, bool success,
                                            const QString &errorMessage) {
    // 这一批是不是「更新中心」那一批？是的话收尾归那一边 —— 那边
    // 要说的是"**每个资源**的结果"（任务账本里那几步 + 一行一条的日志），而不是一句
    // "下好了 / 没下下来"。所以这里**整个交出去**（不弹那个通用框：那边失败时会弹一份
    // 带着逐条结果的框，两个框叠着等于让用户白点一次）。
    if (!m_owner.m_updateBatch.label.isEmpty() && m_owner.m_updateBatch.label == label) {
        m_owner.finishUpdateBatch(success, errorMessage);
        return;
    }

    // 这一发是不是**用户自己按的取消**（见 `DownloadTaskLedgerController::userCancelInFlight()`）：
    // 取消和真失败走的是同一条终态信号，可账本里它们该落成两档。
    const bool userCancelled = !success && m_owner.m_downloadTaskLedger->userCancelInFlight();

    // 整合包那一页下回来的 .mrpack：下完要接着走导入那条路。这一单**只活一次**，所以
    // 取走一次就用掉了 —— 不清的话，用户手动导入过一个包之后，下一个下完的模组也会顺手
    // 再触发一次导入。
    //
    // NOTE: 按 `label` **只取自己那一单**（不是整根成员一刀切）。不分单的话，任何一次
    // 下载完成都会把这笔记录读走：整合包还在下、用户又点了个模组，模组先下完就把
    // 记录吃了 —— 拿半截 .mrpack 去导入，而且整合包真下完时再没人接它。
    // 用 take() 而不是 value()+remove()：这一单**只活一次**，取走本身就是"用掉了"。
    //
    // **只在成功这一支取**：挂在函数开头的话，失败那一发也会把记录吃掉 —— 于是
    // "整合包下到一半失败 → 点重试 → 下回来了"的结局是：文件在缓存里躺着，导入却永远
    // 不会开始（界面上那句「已经装进实例里了」也永远等不到）。失败时留着这一单，重试
    // 成功那一发照样认领得到；用户再也不重试的话，它只是内存里一条记录（若用户主动
    // 取消，则由下面的 userCancelled 分支清掉）。
    if (success && m_owner.m_pendingModpackImports.contains(label)) {
        const MainWindow::PendingModpackImport pending = m_owner.m_pendingModpackImports.take(label);
        const bool sameLedgerTask =
            !m_owner.m_downloadTaskLedger->resourceLabel().isEmpty()
            && label == m_owner.m_downloadTaskLedger->resourceLabel()
            && m_owner.m_downloadTaskLedger->task() != Tasks::invalidTaskId();
        const Tasks::TaskId activeTask =
            sameLedgerTask ? m_owner.m_downloadTaskLedger->task() : pending.taskId;
        if (sameLedgerTask) {
            m_owner.m_downloadTaskLedger->clearTask();
            m_owner.m_downloadTaskLedger->setResourceLabel(QString());
            m_owner.m_downloadTaskLedger->clearOwnerIf(activeTask);
        }
        if (activeTask != Tasks::invalidTaskId() && m_owner.m_activityCenter->tasks()->has(activeTask)) {
            Tasks::TaskManager *tasks = m_owner.m_activityCenter->tasks();
            if (tasks->snapshot(activeTask).steps.isEmpty()) {
                tasks->setSteps(activeTask, MainWindowViewData::modpackOneClickSteps());
            }
            tasks->setStepState(activeTask, 0, Tasks::TaskStepState::Done);
            tasks->setStepState(activeTask, 1, Tasks::TaskStepState::Running,
                                tr("整合包文件已下载，正在解析清单…"));
            tasks->setProgress(activeTask, 0, 0);
            tasks->setDetail(activeTask, tr("正在解析整合包清单…"));
        }
        m_owner.appendLog(TextMarks::done(tr("%1 下好了：%2").arg(label, errorMessage)));
        // 不弹"下载完成"：整合包下回来只是第一步，紧接着就会进入一键导入（不在此提前
        // finishTask），中间夹一个"下好了"等于让他白点一次。
        //
        // NOTE: **排到下一轮**再走导入，不能在这里直接调。这个函数是在
        // DownloadManager 的 allDownloadsFinished 信号栈里被调到的，而导入那条路
        // 马上就会 beginBatch() 开新的一批 —— 在别人的信号栈里开批次会崩
        // （取证见 build/tools/crashdump/，VersionManager 里那几处
        // deferXxxStep 是同一个理由）。
        QTimer::singleShot(0, this, [this, pending, activeTask] {
            m_owner.importModpackOneClick(pending.path, pending.instanceName, activeTask);
        });
        return;
    }

    if (!m_owner.m_downloadTaskLedger->resourceLabel().isEmpty()
        && label == m_owner.m_downloadTaskLedger->resourceLabel()
        && m_owner.m_downloadTaskLedger->task() != Tasks::invalidTaskId()) {
        const Tasks::TaskId task = m_owner.m_downloadTaskLedger->task();
        m_owner.m_downloadTaskLedger->clearTask();
        m_owner.m_downloadTaskLedger->setResourceLabel(QString());
        if (!userCancelled && !success && m_owner.m_pendingModpackImports.contains(label)) {
            m_owner.m_activityCenter->tasks()->setStepState(
                task, 0, Tasks::TaskStepState::Failed, errorMessage);
        }
        if (userCancelled) {
            if (m_owner.m_pendingModpackImports.contains(label)) {
                m_owner.m_pendingModpackImports.remove(label);
            }
            m_owner.m_activityCenter->tasks()->cancel(task);
            // 重试资格跟着松开（别的任务收尾时不许把它挪走 —— 见 owner() 那段）。
            m_owner.m_downloadTaskLedger->clearOwnerIf(task);
        } else {
            m_owner.finishTask(task, success, success ? QString() : errorMessage);
        }
    }

    if (!success) {
        if (userCancelled) {
            // 取消不是失败：日志照写，不弹「下载失败」、也不算一次没看过的失败。
            m_owner.appendLog(TextMarks::failed(tr("%1 已由用户取消下载。").arg(label)));
            return;
        }
        m_owner.appendLog(TextMarks::failed(tr("%1 下载失败：%2").arg(label, errorMessage)));
        InlineMessage::warning(m_owner.m_ui->overlay(), tr("下载失败"),
                             tr("%1 有文件下载失败。\n\n%2").arg(label, errorMessage));
        return;
    }

    m_owner.appendLog(TextMarks::done(tr("%1 下载完成：%2").arg(label, errorMessage)));
    InlineMessage::information(m_owner.m_ui->overlay(), tr("下载完成"),
                             tr("%1 已经装进实例里了。\n\n%2").arg(label, errorMessage));
}
