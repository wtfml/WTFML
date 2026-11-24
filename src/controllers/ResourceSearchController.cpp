#include "ResourceSearchController.hpp"
#include "TextMarks.hpp"
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "ResourceDetailPage.hpp"
#include "InlineMessage.hpp"
#include "LoadingState.hpp"
#include "controllers/ResourcePageController.hpp"
#include "controllers/InstanceIconController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "download/ResourceStore.hpp"
#include "manage/VersionManager.hpp"
#include <QComboBox>
#include <QLineEdit>
#include <QSet>
#include <QTreeWidget>

namespace {
constexpr int kVersionsRequestKeyRole = Qt::UserRole + 41;

/// 新版本号（26.1 / 26.2 …）在下载树里归成 `26.x` 一行；1.21 这类旧写法保持原样。
QString downloadVersionRowKey(const QString &version)
{
    const QString series = ResourceCatalog::gameVersionSeriesOf(version);
    if (series.isEmpty()) return version;
    bool ok = false;
    const int major = series.section(QLatin1Char('.'), 0, 0).toInt(&ok);
    return ok && major >= 26 ? QStringLiteral("%1.x").arg(major) : version;
}

bool isDownloadVersionSeriesKey(const QString &key)
{
    return key.endsWith(QStringLiteral(".x"));
}

QString downloadVersionSeries(const QString &key)
{
    return isDownloadVersionSeriesKey(key) ? key.left(key.size() - 2) : QString();
}

}  // namespace

ResourceSearchController::ResourceSearchController(MainWindow *owner) : QObject(owner), m_owner(*owner) {}

void ResourceSearchController::startResourceSearch(ResourceCatalog::Kind kind) {
    const QString key = ResourceCatalog::kindKey(kind);
    QTreeWidget *tree = m_owner.m_ui->getDownloadResourceTree(key);
    if (tree == nullptr) {
        return;
    }
    const QLineEdit *edit = m_owner.m_ui->getDownloadSearchEdit(key);
    const QString query = edit == nullptr ? QString() : edit->text().trimmed();

    // 上一批结果连同展开状态一起清掉，盖上整块加载态（150ms 还没回来才露面）。不盖的话，
    // 用户按回车之后界面**完全没有反应**（请求一秒上下才回来），而"点了没反应"是用户唯一
    // 会当成启动器坏了的情况。
    tree->clear();
    LoadingState::begin(tree, query.isEmpty()
                                  ? MainWindow::tr("正在获取最热门的%1…").arg(ResourceCatalog::kindLabel(kind))
                                  : MainWindow::tr("正在搜索「%1」…").arg(query));
    // 记下**这一发真的问了什么**：回来时那句「没有搜到「xx」」里的 xx 要是那一次的词。
    // 读编辑框里现在的字是错的 —— 用户完全可能在等结果的时候又敲了几个字（那几下还没发出去，
    // 回车才算一发）。
    m_resourceQueries.insert(key, query);

    // 另外四个筛子（版本 / 加载器 / 类型 / 来源）在这儿一次读完。读法全在 downloadFilterXxx()
    // 那四个访问器里（**唯一出处**），这里只负责把它们递给 store —— 空串一律 = 不筛，
    // 所以"没选"和"没有这一栏"在这里是同一件事，不用分岔。
    const ResourceCatalog::Source source = m_owner.downloadFilterSource(kind);
    const QString gameVersion = m_owner.downloadFilterGameVersion(kind);
    const QString loader = m_owner.downloadFilterLoader(kind);
    const QString category = m_owner.downloadFilterCategory(kind);

    m_owner.appendLog(query.isEmpty() ? MainWindow::tr("正在获取最热门的%1…").arg(ResourceCatalog::kindLabel(kind))
                              : MainWindow::tr("正在搜索%1「%2」…")
                                    .arg(ResourceCatalog::kindLabel(kind), query));
    // 日志里把这一发**带了哪些条件**说全（那两格的值写的是框里显示的字，不是键 ——
    // CurseForge 的类型键是 `6814` 这种号，写进日志没人看得懂）。搜出来"怎么是这么几条"，
    // 十有八九就是某个筛子在起作用；不写下来的话只能对着结果猜。
    QStringList conditions;
    conditions.append(MainWindow::tr("来源 %1").arg(ResourceCatalog::sourceLabel(source)));
    if (!gameVersion.isEmpty()) {
        conditions.append(MainWindow::tr("版本 %1").arg(gameVersion));
    }
    const auto shownText = [](QComboBox *combo) {
        return combo == nullptr ? QString() : combo->currentText();
    };
    if (!loader.isEmpty()) {
        conditions.append(
            MainWindow::tr("模组加载器 %1").arg(shownText(m_owner.m_ui->getDownloadFilterLoaderCombo(key))));
    }
    if (!category.isEmpty()) {
        conditions.append(MainWindow::tr("类型 %1").arg(shownText(m_owner.m_ui->getDownloadFilterTypeCombo(key))));
    }
    m_owner.appendLog(MainWindow::tr("搜索条件：%1").arg(conditions.join(QStringLiteral(" / "))));

    // 钥匙里要带来源（见 ResourceCatalog::searchRequestKey 那段）：用户刚点完 Modrinth 又
    // 切成 CurseForge 时，前一趟回来的结果不该被当成"我现在等的那一发"填进树里。
    m_owner.m_resourceStore->search(ResourceCatalog::searchRequestKey(kind, source), kind, source, query,
                            gameVersion, loader, category);
}

void ResourceSearchController::fillResourceHitRows(QTreeWidget *tree, ResourceCatalog::Kind kind,
                                     const ResourceCatalog::SearchResult &result) {
    if (tree == nullptr) {
        return;
    }
    const int column = MainWindowUI::versionTreeKeyColumn();
    for (const ResourceCatalog::Hit &hit : result.hits) {
        auto *item = new QTreeWidgetItem(tree);
        // 身份 = project_id：图标回来时按它在树里找回这一行、点开游戏版本时按它拼版本请求。
        item->setData(column, Qt::UserRole, hit.projectId);
        // 这一条是哪一类。点最底下那一层要按它算"下到哪儿去"（见 resourceKindRole()）——
        // 存的是**分类键**（不是枚举的整数值），回来时走 kindFromCategoryKey 认，
        // 认不出来就是"这一行不是我们建的"。
        item->setData(column, MainWindowUI::resourceKindRole(), ResourceCatalog::kindKey(kind));
        // 这一条是从哪个来源搜出来的（点第二层要问对那一侧，见 resourceSourceRole()）。
        item->setData(column, MainWindowUI::resourceSourceRole(),
                      ResourceCatalog::sourceKey(hit.source));
        // 图标那一格的**身份**（带来源前缀，见 resourceIconIdRole()）：图标回来时按它认行。
        item->setData(column, MainWindowUI::resourceIconIdRole(), hit.iconCacheId());
        item->setText(column, hit.title);

        // 第二行小字：「作者 · 下载量」。下载量那串字只有一处出处（formatDownloads），
        // 界面上别处显示下载量也要读它。作者缺了就少一段。
        QStringList bits;
        if (!hit.author.isEmpty()) {
            bits.append(hit.author);
        }
        bits.append(ResourceCatalog::formatDownloads(hit.downloads));
        item->setData(column, MainWindowUI::rowSubTextRole(), bits.join(QStringLiteral(" / ")));

        // 简介一行放不下（代理只画一行、超了截断），鼠标停上去看全文。
        // 空简介不挂 tooltip —— 挂一个空的等于鼠标一停就冒一个空框。
        if (!hit.description.isEmpty()) {
            item->setToolTip(column, hit.description);
        }

        // 第二层：游戏版本。**不用再问接口** —— 搜索响应里本来就带着"这个资源支持哪些
        // 游戏版本"（Hit::gameVersions）。顺序由 gameVersionChoices() 统一成"新的在前"。
        // 第二层**一律折**（`false`）：那枚「显示快照版」勾选框在详情页顶上，这一棵树
        // 没有它 —— 要的是"快照版归到对应的大版本里"，这一层就是那句话的落点。
        const QStringList choices = ResourceCatalog::gameVersionChoices(hit.gameVersions, false);
        if (choices.isEmpty()) {
            // 一个游戏版本都没列（数据包那类）。摆一行灰的说清楚 —— 不然这一行看着能展开、
            // 点开却是空的。
            m_owner.addResourcePlaceholderChild(item, MainWindow::tr("这个%1没有列出支持的游戏版本。")
                                                  .arg(ResourceCatalog::kindLabel(kind)));
        }
        QStringList rows;
        for (const QString &gameVersion : choices) {
            const QString row = downloadVersionRowKey(gameVersion);
            if (!rows.contains(row)) rows.append(row);
        }
        for (const QString &row : rows) {
            auto *gameItem = new QTreeWidgetItem(item);
            // 身份 = 行 key（26.x 或 1.21）。第三层点开才建。
            gameItem->setData(column, Qt::UserRole, row);
            gameItem->setText(column, row);
        }

        // 整条 hit 存一份（键就是上面那个图标身份，也是"这一行是哪个资源"的钥匙）。
        // 条目上那四个角色装不下简介 / slug / 更新时间，而点开详情页三样都要
        // （见 m_owner.m_resourceHits 那段 NOTE）。
        m_owner.m_resourceHits.insert(hit.iconCacheId(), hit);
        // 图标**最后**要：ResourceStore 命中内存缓存时是同步发 iconReady 的，那一行得先在
        // 那儿（见 requestIcon 的 NOTE）—— 所以这一句必须排在最后。
        // 收的是整条 hit 不是 id：要挑地址（能用 webp 就走那条小 7 倍的，见 requestIcon）。
        m_owner.m_resourceStore->requestIcon(hit);
    }
}

void ResourceSearchController::onResourceSearchFinished(const QString &requestKey,
                                         const ResourceCatalog::SearchResult &result) {
    ResourceCatalog::Kind kind = ResourceCatalog::Kind::Mod;
    ResourceCatalog::Source fromSource = ResourceCatalog::Source::Modrinth;
    if (!m_owner.kindForSearchRequestKey(requestKey, &kind, &fromSource)) {
        // 不是我们认得的那几页发的（别的东西也用这个 store 时才会走到）。
        return;
    }
    // 这一发回来的时候，用户可能已经把来源换成另一个了 —— 两个来源的请求是**各自飞的**
    // （钥匙不同，store 不会互相掐掉）。不是这一页**现在**选的那个来源就丢掉：留着会把
    // 上一家那批结果摆在一个写着另一家的页面上，而页面上那句"来源 CurseForge"就成了假话。
    if (fromSource != m_owner.downloadFilterSource(kind)) {
        return;
    }
    const QString key = ResourceCatalog::kindKey(kind);
    QTreeWidget *tree = m_owner.m_ui->getDownloadResourceTree(key);
    if (tree == nullptr) {
        return;
    }
    // 「正在搜索…」那一行先撤掉（下面每一条分支都会摆上自己那一行/那一批）。
    tree->clear();

    if (!result.ok) {
        // 「没搜成」和「没搜到」是**两句不同的话**（加载器树那边「无可用版本」和
        // 「列表没取到」也是这么分的）。这里说的是前一句。
        //
        // NOTE: 这里**不弹窗**。产品那条"错误要弹窗"管的是"必须让用户看见的东西不能只写
        // 日志"（日志面板不存在，写进去等于没写）；这一句**不是**那种：那一行就
        // 摆在用户刚点的那一页上、就是他正在看的地方，而且话是接口/网络自己说的
        // （超时 / 连不上 / 还是"你问得太勤了，过一分钟再试" —— 最后那种正是最该
        // 看清、又最不该用一个模态框打断他的）。回车再按一次就是重试。
        // 这一条和加载器树那条「列表没取到」是同一种形状，所以做法也一样。
        // 真要弹窗的是那些**没有别的地方能显示**的错（见 onResourceDownloadFinished）。
        m_owner.appendLog(TextMarks::failed(tr("搜索%1失败：%2").arg(ResourceCatalog::kindLabel(kind), result.problem)));
        LoadingState::fail(tree, result.problem, [this, kind] { startResourceSearch(kind); });
        return;
    }
    LoadingState::finish(tree);
    if (result.hits.isEmpty()) {
        // **问到了、就是没有** —— 不算错，也**不弹窗**（一个"什么都没发生"的弹窗比没有
        // 更烦人）。用记下来的那个搜索词，别读编辑框里现在的字（理由见 startResourceSearch）。
        const QString query = m_resourceQueries.value(key);
        m_owner.appendLog(MainWindow::tr("%1 一条也没有搜到（%2）。")
                      .arg(ResourceCatalog::kindLabel(kind), query.isEmpty() ? MainWindow::tr("空搜索词") : query));
        m_owner.addResourcePlaceholderRow(tree, query.isEmpty()
                                            ? MainWindow::tr("这一类里一条都没有。")
                                            : MainWindow::tr("没有搜到「%1」。").arg(query));
        return;
    }
    fillResourceHitRows(tree, kind, result);
}

void ResourceSearchController::onResourceIconReady(const QString &iconId, const QPixmap &icon) {
    // 没有图标（作者没传过 / 这次没取到 / 被限流）也**要**给这一行挂上东西：一个全透明的
    // 方块。为什么：代理是"条目给了图标，文字才让出前面那一格"的（见 VersionTreeDelegate
    // 里 iconRect 那段），什么都不挂的话这一行的字会顶到最左边，和上下那些有图标的行
    // 参差不齐 —— 而"这一列没对齐"在用户眼里是"这软件做得很糙"，不是"这个模组没图标"。
    // 全透明是为了不再多画一个"没有图标"的形状（那个形状本身也是要设计的东西）。
    // NOTE: **不是错误**、不弹窗：项目没上传过图标是完全正常的一件事。
    QPixmap mark = icon;
    if (mark.isNull()) {
        mark = QPixmap(MainWindowUI::resourceTreeIconSize(), MainWindowUI::resourceTreeIconSize());
        mark.fill(Qt::transparent);
    }
    const int column = MainWindowUI::versionTreeKeyColumn();
    // 按**图标身份**（`Hit::iconCacheId()`，见 resourceIconIdRole）找回**现在**树上那一行 ——
    // 不按裸 project_id：两个来源的 id 空间会撞，撞上就是把另一个来源同号那个资源的图标
    // 画到这一行上（而且不报错）。也不存条目指针：重搜会把整棵树清掉，存下来的指针就是
    // 悬空的 —— 按身份找，找得到才画，找不到就什么都不做。
    for (ResourceCatalog::Kind kind : ResourceCatalog::allKinds()) {
        QTreeWidget *tree = m_owner.m_ui->getDownloadResourceTree(ResourceCatalog::kindKey(kind));
        if (tree == nullptr) {
            continue;
        }
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = tree->topLevelItem(i);
            if (item->data(column, MainWindowUI::resourceIconIdRole()).toString() != iconId) {
                continue;
            }
            item->setData(column, MainWindowUI::rowIconRole(), mark);
        }
    }
    // 详情页正开着、回来的又正好是它那一张 → 换上头卡那一格。
    //
    // NOTE: 判据是**图标身份**而不是"页开着"就换：图标是一条一条回来的，页上那一格只该
    // 认自己这一条（换个说法：这是同一套"按身份认行"的规矩，只不过这儿认的是那一页）。
    // 头卡那一格比列表大（64 vs 32），缩放由那一页自己做（见 setIcon）。
    const auto &pageState = m_owner.m_resourcePage->state();
    if (pageState.open && iconId == pageState.iconKey
        && m_owner.m_resourcePage->page() != nullptr) {
        m_owner.m_resourcePage->page()->setIcon(icon);
    }

    // ---------- 「文件管理」那几页----------
    //
    // 同一个身份也可能是**实例里某个文件**换来的那一个（见 requestInstanceFileIcons）——
    // 这一格和上面那棵树是两回事（那边是"搜出来的资源"，这边是"盘上躺着的文件"），
    // 所以两条路都要走一遍，谁都不抢谁。
    //
    // 这一句**必须排在 m_owner.m_resourceStore->requestIcon() 之后**（也就是"图标回来时"）：
    // 盘上已经有缓存的话 requestIcon() 是**同步**发 iconReady 的（见它那条 NOTE），
    // 所以这里是"记了账、立刻就用上"，而记账那一步在 requestIcon 之前就写完了。
    m_owner.m_instanceIconController->rememberResourceIcon(iconId, icon);
}

void ResourceSearchController::onResourceItemActivated(QTreeWidgetItem *item) {
    if (item == nullptr) {
        return;
    }
    // 灰的那些（「正在搜索…」「没有搜到…」「正在获取…」「这个游戏版本上没有…」）不是能点的
    // 东西：点它什么都不该发生。放在最前面 —— "点不动"这件事对三层是同一个道理。
    if (item->isDisabled()) {
        return;
    }
    const int depth = m_owner.resourceRowDepth(item);
    if (depth == 0 || depth == 1) {
        // 第一层（一个资源）和第二层（一个游戏版本）都是**只展开 / 收起**：
        // "点开"是"看看有什么"，看一眼不等于选了它。
        const bool expand = !item->isExpanded();
        item->setExpanded(expand);
        if (depth == 1 && expand) requestResourceVersions(item);
        return;
    }
    if (depth == 2) {
        // 第三层（一个版本号）：**这一下就是"下它"**。
        downloadResourceVersion(item);
    }
}

bool ResourceSearchController::requestResourceVersions(QTreeWidgetItem *item) {
    const QTreeWidgetItem *resourceItem = item->parent();
    const QString projectId = resourceItem == nullptr
                                  ? QString()
                                  : resourceItem->data(MainWindowUI::versionTreeKeyColumn(),
                                                       Qt::UserRole).toString();
    // 这一条是从哪个来源搜出来的（见 resourceSourceRole()）：**必须问对那一侧** ——
    // 两个来源的 id 空间会撞，问错了不报错，只会显示另一个资源的版本列表。
    // 认不出来（那一行不是我们建的）就当 Modrinth，和"没选来源"的默认一致。
    ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth;
    if (resourceItem != nullptr) {
        ResourceCatalog::sourceFromKey(
            resourceItem->data(MainWindowUI::versionTreeKeyColumn(),
                               MainWindowUI::resourceSourceRole()).toString(),
            &source);
    }
    const QString gameVersion =
        item->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toString();
    if (projectId.isEmpty() || gameVersion.isEmpty()) {
        return false;
    }
    QString loader;
    ResourceCatalog::Kind kind;
    if (m_owner.m_downloadReturn.valid() &&
        ResourceCatalog::kindFromCategoryKey(resourceItem->data(MainWindowUI::versionTreeKeyColumn(),
            MainWindowUI::resourceKindRole()).toString(), &kind) && kind == ResourceCatalog::Kind::Mod) {
        const auto parts = m_owner.m_versionManager->displayParts(m_owner.m_downloadReturn.instanceId);
        loader = MainWindowViewData::loaderGroupKey(
            MainWindowViewData::instanceLoaderName(parts, m_owner.m_versionManager, nullptr));
    }
    // 26.x 是本地归并出来的行：问接口时不要带单个版本号，回来在 fillResourceVersionRows()
    // 里按 series 过滤；否则接口只认 26.1 这种精确值，这一行会是空的。
    const QString requestVersion = isDownloadVersionSeriesKey(gameVersion) ? QString() : gameVersion;
    const QString key = ResourceCatalog::versionsRequestKey(projectId, gameVersion, loader, source);
    if (item->childCount() > 0 &&
        item->data(MainWindowUI::versionTreeKeyColumn(), kVersionsRequestKeyRole).toString() == key) {
        return false;
    }
    qDeleteAll(item->takeChildren());
    LoadingState::beginRow(item, MainWindow::tr("正在获取…"));
    item->setData(MainWindowUI::versionTreeKeyColumn(), kVersionsRequestKeyRole, key);
    m_owner.m_resourceStore->fetchVersions(key, projectId, source, requestVersion, loader);
    return true;
}

void ResourceSearchController::fillResourceVersionRows(QTreeWidgetItem *gameItem,
                                        const QList<ResourceCatalog::VersionEntry> &versions,
                                        const QString &problem) {
    if (gameItem == nullptr) {
        return;
    }
    // 先撤掉「正在获取…」。这一层点开之前一定是空的，所以这里清掉的只有那一行占位。
    while (gameItem->childCount() > 0) {
        delete gameItem->takeChild(0);
    }
    const int column = MainWindowUI::versionTreeKeyColumn();

    if (!problem.isEmpty()) {
        // 「没取到」（超时 / 限流 / 那条资源被作者删了）。**不弹窗**：要的是"展开看看"，
        // 进来已经有话说了；行尾「重试」= 删掉这一行、原样再问一次。
        LoadingState::failRow(LoadingState::beginRow(gameItem), problem, [this, gameItem] {
            qDeleteAll(gameItem->takeChildren());
            requestResourceVersions(gameItem);
        });
        return;
    }
    const QString series = downloadVersionSeries(
        gameItem->data(column, Qt::UserRole).toString());
    QList<ResourceCatalog::VersionEntry> shown;
    shown.reserve(versions.size());
    for (const ResourceCatalog::VersionEntry &version : versions) {
        if (series.isEmpty()) {
            shown.append(version);
            continue;
        }
        for (const QString &gameVersion : version.gameVersions) {
            if (ResourceCatalog::gameVersionSeriesOf(gameVersion).section(QLatin1Char('.'), 0, 0) == series) {
                shown.append(version);
                break;
            }
        }
    }
    if (shown.isEmpty()) {
        m_owner.addResourcePlaceholderChild(gameItem, MainWindow::tr("这个游戏版本上没有可下载的版本。"));
        return;
    }

    // 三层行前面那个图标：跟着它所属的那个资源（同一列里三个草方块会让人以为是三个
    // Minecraft 版本）。资源那条的图标是异步回来的，这里读到的可能还是空的 —— 那就
    // 按默认规矩留给代理（空的等于没给）。
    const QVariant resourceIcon =
        gameItem->parent() == nullptr
            ? QVariant()
            : gameItem->parent()->data(column, MainWindowUI::rowIconRole());

    for (const ResourceCatalog::VersionEntry &version : shown) {
        auto *item = new QTreeWidgetItem(gameItem);
        // 身份 = 版本 **id**（不是版本号）：点它那一下要拿它回 m_owner.m_resourceVersions 里换
        // "这一版的文件是哪几个"，而文件清单是按 id 找的。
        item->setData(column, Qt::UserRole, version.id);
        // 正文写**文件名**：接下来要落到 mods/ 里的就是这个名字（作者给这一版起的名字
        // 常常已经在文件名里写全了，两个都写一行放不下）。
        const ResourceCatalog::FileEntry *file = version.primaryFile();
        item->setText(column, file == nullptr ? version.versionNumber : file->filename);
        // 第二行小字：release/beta/alpha · 加载器 · 发布日期（各缺各的就不补）。
        QStringList bits;
        if (!version.versionType.isEmpty()) {
            bits.append(version.versionType);
        }
        if (!version.loaders.isEmpty()) {
            bits.append(version.loaders.join(QLatin1Char('/')));
        }
        if (!version.datePublished.isEmpty()) {
            // ISO 8601（`T…`）只取日期那一段。
            bits.append(version.datePublished.left(10));
        }
        item->setData(column, MainWindowUI::rowSubTextRole(), bits.join(QStringLiteral(" / ")));
        if (resourceIcon.isValid()) {
            item->setData(column, MainWindowUI::rowIconRole(), resourceIcon);
        }
    }
}

void ResourceSearchController::onResourceVersionsFinished(const QString &requestKey,
                                           const QList<ResourceCatalog::VersionEntry> &versions,
                                           const QString &problem) {
    const int column = MainWindowUI::versionTreeKeyColumn();
    // 先看是不是**详情页**那一发（钥匙里游戏版本、加载器都是空的，和树上那些一定带一个
    // 游戏版本的钥匙不会撞，见 openResourceDetailPage）。
    auto &resourcePage = m_owner.m_resourcePage->state();
    if (resourcePage.open && requestKey == resourcePage.requestKey) {
        if (!problem.isEmpty()) {
            // 「没取到」（超时 / 限流 / 那条资源被作者删了）。**不弹窗**：用户是点进这一页
            // 来看东西的，页上已经有话说了；再点一次这个资源就是重新问一次。
            resourcePage.versions.clear();
            resourcePage.loaded = true;   // 回来过了（回的是"没取到"）
            // 「重试」= 同一把钥匙再问一次（答复照旧落回这里）。
            m_owner.m_resourcePage->page()->setFailed(problem, [this] {
                auto &state = m_owner.m_resourcePage->state();
                if (!state.open) return;
                state.loaded = false;
                m_owner.m_resourcePage->page()->setLoading(MainWindow::tr("正在读版本列表…"));
                m_owner.m_resourceStore->fetchVersions(state.requestKey, state.projectId, state.source, QString());
            });
            return;
        }
        resourcePage.versions = versions;
        resourcePage.loaded = true;
        // 摆哪一份由 chip 决定（这一步同时也处理"筛完一条都不剩"和"一条都没发过"两种空）。
        m_owner.fillResourcePageGroups();
        return;
    }
    // 请求键随版本行保存，切换实例后迟到的结果不会落到不同加载器的列表上。
    for (ResourceCatalog::Kind kind : ResourceCatalog::allKinds()) {
        QTreeWidget *tree = m_owner.m_ui->getDownloadResourceTree(ResourceCatalog::kindKey(kind));
        if (tree == nullptr) {
            continue;
        }
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *resourceItem = tree->topLevelItem(i);
            if (resourceItem->data(column, Qt::UserRole).toString().isEmpty()) {
                continue;
            }
            for (int j = 0; j < resourceItem->childCount(); ++j) {
                QTreeWidgetItem *gameItem = resourceItem->child(j);
                const QString gameVersion = gameItem->data(column, Qt::UserRole).toString();
                const QString sentKey = gameItem->data(column, kVersionsRequestKeyRole).toString();
                if (gameVersion.isEmpty() || sentKey != requestKey) {
                    continue;
                }
                // 就是它。清单存下来（点第三层那一下要用 URL / sha1 / 大小），再摆第三层。
                // NOTE: 这张表只往里放、不清理（键是"资源 × 游戏版本"，值是一份纯数据的
                // 小清单）。不清理不会读到旧数据：第三层那些行只在问过之后才存在，
                // 而问过就一定覆盖过同一个键。
                m_owner.m_resourceVersions.insert(requestKey, versions);
                fillResourceVersionRows(gameItem, versions, problem);
                return;
            }
        }
    }
    // 树里找不到对应的行 = 用户重搜过了，这一发的结果已经不是现在这棵树上的东西了。
    // 丢掉是对的 —— 把旧结果填进新列表比丢掉糟得多。
}

void ResourceSearchController::downloadResourceVersion(QTreeWidgetItem *item) {
    if (item == nullptr) {
        return;
    }
    const int column = MainWindowUI::versionTreeKeyColumn();
    QTreeWidgetItem *gameItem = item->parent();
    QTreeWidgetItem *resourceItem = gameItem == nullptr ? nullptr : gameItem->parent();
    if (gameItem == nullptr || resourceItem == nullptr) {
        return;
    }
    if (requestResourceVersions(gameItem)) return;
    const QString versionId = item->data(column, Qt::UserRole).toString();
    ResourceCatalog::Kind kind = ResourceCatalog::Kind::Mod;
    if (!ResourceCatalog::kindFromCategoryKey(
            resourceItem->data(column, MainWindowUI::resourceKindRole()).toString(), &kind)) {
        return;  // 这一行不是我们建的（正常走不到）
    }
    // 这一版的文件清单：随"点开游戏版本"那一发回来的。没有它就下不了（正常走不到 ——
    // 有第三层这些行就说明那一发回来过）。说清楚，别静静地什么都不做。
    //
    ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth;
    ResourceCatalog::sourceFromKey(
        resourceItem->data(column, MainWindowUI::resourceSourceRole()).toString(), &source);
    const QString requestKey = gameItem->data(column, kVersionsRequestKeyRole).toString();
    const auto cached = m_owner.m_resourceVersions.constFind(requestKey);
    if (cached == m_owner.m_resourceVersions.constEnd()) {
        InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("下载失败"),
            MainWindow::tr("尚未取得此版本的下载信息。请折叠后重新展开上方的游戏版本，再试一次。"));
        return;
    }
    const ResourceCatalog::VersionEntry *chosen = nullptr;
    for (const ResourceCatalog::VersionEntry &entry : *cached) {
        if (entry.id == versionId) {
            chosen = &entry;
            break;
        }
    }
    if (chosen == nullptr) {
        // 行在、清单里没有它 = 这一行是上一次搜索留下的（重搜过）。用户没做错什么，不弹窗。
        return;
    }
    // 剩下的（选哪个实例、落到哪个目录、文件清单、加载器那道闸）详情页那条路也要走一遍，
    // 全在 installResourceVersion 里 —— **这一条路不自己再算一份**。
    m_owner.installResourceVersion(kind, resourceItem->text(column), *chosen, source);
}

