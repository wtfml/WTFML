/**
 * @file MainWindowInstanceTree.cpp
 * @brief 实例树、已安装版本下拉框、«启动» 那一行的投影：谁在忙、谁不能启动、树画成什么样。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"界面上列出来的那些实例和版本"：`installingVersionIds()`（谁不能启动的
 * 唯一出处）、`refreshInstalledVersions()`（下拉框 + 可启动清单）、`rebuildInstanceTree()`
 * （分组树）、`updateInstanceTreeRunState()` / `updateStartButtonText()`（运行态与按钮上那句
 * 字），以及在线版本清单 `refreshAvailableVersions()`。它不碰启动本身、不碰下载批次 ——
 * 从 `MainWindow.cpp` 分出来，"启动按钮为什么灰着"不必在整扇窗里翻。
 *
 * 【判据不在这一层】
 *
 * 一个实例能不能启动：`installingVersionIds()` 与那几个 busy 计数（`m_fileOperationOccupancy`）
 * 是唯一判据，主窗口别处只问它们；分组与排序的规则在 `manage/InstanceFilter`；显示名在
 * `manage/VersionDisplay` 那一套 `displayParts()` 里。
 */

#include "MainWindow.hpp"

#include "..\ui\VersionIcons.hpp"
#include "..\ui\ListRowDelegate.hpp"
#include "controllers/DownloadBatchController.hpp"
#include "controllers/InstallPageController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "controllers/PresetController.hpp"
#include "manage/InstanceFilter.hpp"
#include "manage/InstanceGroupStore.hpp"
#include "manage/LaunchHistory.hpp"
#include "manage/VersionManager.hpp"
#include "settings/LauncherSettings.hpp"
#include "util/AppPaths.hpp"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QList>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>

using namespace MainWindowViewData;

namespace {

QString instanceSubtitle(const VersionDisplayParts &parts, const LaunchHistory::PlayTime &playTime) {
    QString subtitle = parts.realVersion.isEmpty() ? parts.id : parts.realVersion;
    if (playTime.totalMs > 0) {
        subtitle += QStringLiteral(" · ") + MainWindow::tr("游玩 %1").arg(LaunchHistory::playTimeText(playTime.totalMs));
    }
    return subtitle;
}

}  // namespace

void MainWindow::markFileOperationBusy(const QString &versionId) {
    // 判据（引用计数、空 id、成对释放、没记过不许变负）都在 `FileOperationOccupancy`
    // 那一层：见 tests/core_modules/FileOperationOccupancyTests.cpp 的 FO 段。
    m_fileOperationOccupancy.mark(versionId);
}

void MainWindow::releaseFileOperationBusy(const QString &versionId) {
    m_fileOperationOccupancy.release(versionId);
}

QSet<QString> MainWindow::installingVersionIds() const {
    // 三个来源合起来才是"现在**不能启动**的那几个实例"。分开记是因为它们覆盖的时段
    // 不一样：VersionManager 那个只在一批文件真的在写的时候非空，而"点了导入、还在拉
    // 版本表"那几段是界面自己记的（数据层那会儿连批都还没开）。
    // 后台搬文件（模组迁移 / 「迁移」「升级副本」）两个入口共用这一份名单。
    QSet<QString> ids = m_fileOperationOccupancy.instanceIds();
    if (m_downloadBatches && m_downloadBatches->isRestoring()) {
        const auto all = QDir(AppPaths::versionsDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto &id : all) ids.insert(id);
    }
    const QString busy = m_versionManager->busyVersionId();
    if (!busy.isEmpty()) {
        ids.insert(busy);
    }
    // 那四档界面自己记的占用（导入 / 安装 / 补全 / 预取）只在"用户点的是它"那一刻置上、
    // 终态时清掉，覆盖面比 VersionManager 那个宽 —— 正好补上前奏那几段。
    ids.unite(m_installOccupancy.instanceIds());
    return ids;
}

void MainWindow::refreshInstalledVersions() {
    // 同步那条路：改名 / 删实例 / 下载收尾 / 改设置这些"用户刚做完一件事、要立刻看见"的场合
    // 用。扫的是本地磁盘（.minecraft/versions/<id>/<id>.jar），实例少时是几毫秒。
    //
    // 版本清单从网络回来那一下（20 份 63 ms、200 份 116 ms）走异步那条：见
    // `refreshInstalledVersionsAsync()`。
    ++m_installedRefreshToken;
    // 识别不出来的版本只在第一次上报：本函数一次会话会被调很多次（构造、下载完成、
    // 换文件夹都会走一遍），每回都报就是刷屏。第一次报过之后连扫都不扫了 —— 传
    // nullptr 时 installedVersions() 走的是不带 out 参数的那条路径。
    QStringList warnings;
    const QStringList installed = m_versionManager->installedVersions(
        m_reportedUnrecognizedVersions ? nullptr : &warnings);
    applyInstalledVersions(installed, warnings);
}

void MainWindow::refreshInstalledVersionsAsync() {
    // **不扫盘、不动界面**：只把请求发出去。结果回来之前下拉框和实例树保持上一次的样子
    // （`installedVersionsScanned` 的接收端在 MainWindow 构造里接好了）。
    m_pendingInstalledRefreshToken = ++m_installedRefreshToken;
    m_versionManager->requestInstalledVersions(!m_reportedUnrecognizedVersions);
}

void MainWindow::applyInstalledVersions(const QStringList &installed,
                                        const QStringList &warnings) {
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    if (!warnings.isEmpty()) {
        m_reportedUnrecognizedVersions = true;
        for (const QString &warning : warnings) {
            appendLog(warning);
        }
    }

    QComboBox *combo = m_ui->getInstalledVersionCombo();
    // 还在下载/安装的那几个**先不出现在启动选项里**：一份文件没下全的实例列在下拉框里，
    // 用户点它就是启动一个文件不全的版本（游戏崩在缺库上，而且看不出原因）。
    //
    // NOTE: 这一步同时把"启动"和"下载"两件事**按版本**分开了：装 1.20.1 的这十来分钟里，
    // 早就装好的 1.18.2 照样在列表里、照样能启动（见 onStartButtonClicked 里的闸口）。
    // **不能**改成"有任何一批下载在跑，启动按钮就整片灰掉+下拉框禁用"：那既拦不住真正
    // 危险的那一种，又把完全无关的另一种一起堵死了。
    const QSet<QString> installing = installingVersionIds();
    QStringList launchable;
    launchable.reserve(installed.size());
    for (const QString &id : installed) {
        if (!installing.contains(id)) {
            launchable.append(id);
        }
    }

    QList<VersionDisplayParts> parts;
    QList<VersionDisplayParts> managementParts;
    QSet<QString> previouslyVisible;
    for (const VersionDisplayParts &part : m_instancePageParts) {
        previouslyVisible.insert(part.id);
    }
    parts.reserve(launchable.size());
    managementParts.reserve(installed.size());
    for (const QString &id : installed) {
        // 每个版本读一次 <id>/wtfml.meta（几百字节的可选文件，绝大多数版本没有）。
        // 这个文件会被外面改写（用户手写、别的启动器），所以读侧缓存靠"在不在 + 修改
        // 时刻 + 字节数"失效，重命名这类走写侧的路写完当场丢掉那一格 —— 见
        // VersionManager::readVersionMeta()。本函数之后 instanceKey() / homeGroupIdOf()
        // 还要再问同一个文件两三次，那些问都落在同一格上。
        const VersionDisplayParts part = m_versionManager->displayParts(id);
        if (!installing.contains(id)) {
            parts.append(part);
        }
        // 文件操作只暂时禁止启动，不应让已经安装的实例从管理页消失。
        // 尚在安装、之前没露过面的新实例仍然不出现。
        if (!installing.contains(id)
            || (m_fileOperationOccupancy.contains(id) && previouslyVisible.contains(id))) {
            managementParts.append(part);
        }
    }
    // 第一次用这一份 .minecraft：把五个默认组（置顶 / 可安装mod / 原版 / 快照版 /
    // 远古版）种进去。
    //
    // 时机就选在这里：**有实例可摆**的时候才种（一个实例都没有时种下去，树里就是五个
    // 空组头加一个「未分组」，纯噪声）。这个函数每次刷实例都走一遍，但种只在文件还不
    // 存在的那一次真的发生（见 seedDefaultGroups()），代价是一次 QFile::exists。
    //
    // NOTE: 挡住信号。这一趟本来就要把树重画一遍（下面那一段），而 groupsChanged 接的
    // 正是本函数 —— 不挡的话会把自己再叫一遍，白扫一次盘。
    // NOTE: 只在文件不存在时种，所以这是**一次**的事：用户把那五个删光之后不会再长回来
    // （"默认可以删"就体现在 seedDefaultGroups() 里）。
    // NOTE: 种不下去（盘写不了）只说一句日志 —— 默认组是锦上添花，"没分过组就
    // 平铺"的样子照样能用，为它弹一个框反而吓人。
    if (!parts.isEmpty()) {
        const QSignalBlocker blocker(m_groupStore);
        QString seedError;
        if (!m_groupStore->seedDefaultGroups(&seedError)) {
            appendLog(seedError);
        }
    }

    // 主页上"已安装版本"只有下拉框这一个控件（右栏那张列表不存在，见
    // MainWindowUI 里主页那一段），所以这里只灌下拉框，同步选中项也只剩实例页那张列表。
    // 灌之前记下原来的当前实例和它的位置：被删时顺延到同一位置（resolveCurrentInstance()）。
    const QString previousCurrent = selectedVersionId(combo);
    const int previousIndex = combo->currentIndex();
    fillVersionCombo(combo, parts);

    // 实例页那张树：**扫盘这一段先把原料留好**（见 m_instancePageParts 那段 —— 搜索框每
    // 敲一个字都要重灌树，那一条路不许再扫盘），然后重灌一遍。筛选条件此刻是什么样，
    // 那一趟自己会读（`instanceFilterQuery()`）。
    m_instancePageParts = managementParts;
    m_instancePageMarks.clear();
    m_instancePageMarks.reserve(managementParts.size());
    for (const VersionDisplayParts &part : managementParts) {
        m_instancePageMarks.insert(part.id, QIcon(instanceRowMark(
                                                  part, m_versionManager,
                                                  MainWindowUI::instanceTreeIconSize())));
    }
    rebuildInstanceTree();

    // 占位文案随上下文变。**两个串都必须非空**：空占位文案会让 QComboBox 在往空
    // 模型里插第一批数据时静默选中第 0 项（qcombobox.cpp:1753-1759）—— 当前实例挑谁
    // 只由下面 resolveCurrentInstance() 那一步定，不能让 Qt 顺手替它定。
    //
    // 占位文案必须按 launchable 重设：写死成"暂无已安装版本"的话，灌完数据后
    // 明明装了好几个版本，画面上却说着一个都没有。
    // 判据同样用 launchable："一个能启动的都没有"才是"暂无已安装版本"（唯一那个正在
    // 装的时候，下拉框里确实一个都挑不了，那时候说"请选择一个版本"是句空话）。
    combo->setPlaceholderText(launchable.isEmpty() ? MainWindowUI::noInstalledVersionPlaceholder()
                                                   : MainWindowUI::chooseVersionPlaceholder());

    // 有实例必有当前实例：fillVersionCombo() 只保住原来那一个，没保住（被删了、刚启动、
    // 刚从零个装出第一个）就按规则挑一个。不挡信号：当前实例换了，主页、底栏、实例树、
    // 离线皮肤都要跟着刷（ConnectionsController 那条 currentIndexChanged）；挡掉的只是
    // 「记成用户手选」那一步（wtfmlAutoPick）。
    // 顺延只是"它暂时不在"时的替身：被挤掉的那一个（修复期间被滤掉、目录一时读不到）回来了，
    // 而用户这期间没手选过别的（手选会清掉 wtfmlDisplacedId，见 ConnectionsController），就换回它。
    const auto autoPick = [combo](const QString &id) {
        combo->setProperty("wtfmlAutoPick", true);
        combo->setCurrentIndex(combo->findData(id));
        combo->setProperty("wtfmlAutoPick", false);
    };
    const QString displaced = combo->property("wtfmlDisplacedId").toString();
    if (!displaced.isEmpty() && combo->findData(displaced) >= 0) {
        combo->setProperty("wtfmlDisplacedId", QVariant());
        if (selectedVersionId(combo) != displaced) autoPick(displaced);
    } else if (combo->currentIndex() < 0 && combo->count() > 0) {
        QStringList ids;
        ids.reserve(combo->count());
        for (int row = 0; row < combo->count(); ++row) {
            ids.append(combo->itemData(row).toString());
        }
        const QString picked =
            resolveCurrentInstance(ids, previousCurrent, previousIndex,
                                   m_settings->lastLaunchedVersion(), m_settings->lastSelectedVersion());
        // 只记第一个被挤掉的：替身自己再被挤掉时，要等回来的还是最初那一个。
        if (!previousCurrent.isEmpty() && displaced.isEmpty()) {
            combo->setProperty("wtfmlDisplacedId", previousCurrent);
        }
        autoPick(picked);
    }

    // 刷完必须手动同步按钮文字和 tooltip：上面 fillVersionCombo() 里套了
    // QSignalBlocker，重灌过程不会发信号，光靠 connect 那条接不到。
    updateStartButtonText();
    const qint64 elapsedMs = refreshTimer.elapsed();
    if (elapsedMs >= 250) {
        qInfo() << "refreshInstalledVersions:" << elapsedMs << "ms for" << installed.size()
                << "installed versions";
    }

    // 服务端那一页也顺手重扫一遍：它扫的是 `<数据目录>/servers`，和上面那条实例目录同源，
    // 而"换数据目录"正是这个函数被叫的场合之一（另一处是启动那一刻）。两页各自重灌，
    // 谁也不去猜对方缓存了什么。
    refreshServerList();
    // 「预设」那一页的两个实例下拉框跟着这份清单走（页面还没建出来时它自己什么都不做，
    // 建过之后按 id 保留当前选择）。**不能只在切到「预设」那一档时灌**：用户切到别的
    // 顶栏页之后后台装好了一份实例，再切回来时模式行还停在「预设」上，`open()` 不会被
    // 叫到 —— 下拉框里就少那一份，用户以为没装上。
    if (m_presetController != nullptr) {
        m_presetController->refreshInstances();
    }
    updateHomeRecent();
}

InstanceFilter::Query MainWindow::instanceFilterQuery() const {
    InstanceFilter::Query query;
    if (const QLineEdit *search = m_ui->getInstanceSearchEdit()) {
        query.text = search->text();
    }
    // 原版 / 模组是**一对互斥**的档：两颗都亮着时按"后点的那颗说了算"判不出来，所以
    // 干脆按一颗读（两颗同时亮着只可能是代码里摆错了，那时候按「原版」算 —— 界面上
    // 两颗都亮着的样子本身就是错的，不必为它编一套语义）。
    const auto checked = [this](MainWindowUI::InstanceFilterButton which) {
        const QPushButton *button = m_ui->getInstanceFilterButton(which);
        return button != nullptr && button->isChecked();
    };
    const bool vanilla = checked(MainWindowUI::InstanceFilterButton::Vanilla);
    const bool modded = checked(MainWindowUI::InstanceFilterButton::Modded);
    if (vanilla && !modded) {
        query.kind = InstanceFilter::Kind::Vanilla;
    } else if (modded && !vanilla) {
        query.kind = InstanceFilter::Kind::Modded;
    }
    query.updatableOnly = checked(MainWindowUI::InstanceFilterButton::Updatable);
    query.recentOnly = checked(MainWindowUI::InstanceFilterButton::Recent);
    return query;
}

InstanceFilter::Facts MainWindow::instanceFilterFacts(const VersionDisplayParts &part) const {
    InstanceFilter::Facts facts;
    // 加载器名 / 版本走 `instanceLoaderName()`（= `InstancePagesController::loaderName()`），
    // **不直接读 `part.loaderName`**：那一格只有我们自己的安装器会写，PCL / HMCL 合并出来的
    // 实例（加载器只写在版本 json 的 `libraries` 里）在那儿是空的。读它的话，「模组」那一档
    // 筛不出这种实例（它明明带着 Fabric），搜「fabric」也搜不到。
    QString loaderVersion;
    const QString loaderName = instanceLoaderName(part, m_versionManager, &loaderVersion);
    facts.texts = InstanceFilter::textsFor(part, loaderName, loaderVersion);
    facts.hasLoader = !loaderName.isEmpty();
    // 「可更新」认的是**实例设置里那趟检查**的结果（键在不在 = 查过没有，见
    // m_instanceOutdatedPaths 那段）。没查过的实例在这一档里筛不出来 —— 那是对的，
    // 谁也没说过它有更新。
    facts.updatable = !m_instanceOutdatedPaths.value(part.id).isEmpty();
    // 「最近运行」认的是启动账本（`wtfml.history`，跟着当前游戏文件夹走）。账本只留最近
    // 二十来条 ⇒ 很久没玩过的实例会从这一档里掉出去，口径写在那一颗的 tooltip 里。
    for (const LaunchHistory::Record &record : m_launchHistory->records()) {
        if (record.instanceId == part.id) {
            facts.recent = true;
            break;
        }
    }
    return facts;
}

void MainWindow::rebuildInstanceTree() {
    QTreeWidget *tree = m_ui->getInstanceList();
    const QList<VersionDisplayParts> parts = m_instancePageParts;
    // 「管理页右侧每次打开都在最底下」：下面 syncTreeToCombo() 选中的是当前实例，Qt 会
    // 滚到它那儿；开机那一趟树还藏着，这一滚被攒到第一次显示、排完版才补，用户看到的就
    // 永远是列表底部。记下重建前的位置，等排版真结束再放回去（见 restoreTreeScroll()）。
    const int keepTreeScroll = tree->verticalScrollBar()->value();

    // ---- ① 筛：过不了的实例**根本不进桶**（不是"画好了再藏起来"）----
    //
    // 这一页不需要"画好了再手工隐藏行"：这棵树**每次都是整个重建的**（fillInstanceTree
    // 里第一句就是 tree->clear()），要少画几行只有一个正当办法 —— 不建它。
    const InstanceFilter::Query query = instanceFilterQuery();
    const bool filtering = query.isActive();
    QList<VersionDisplayParts> shown;
    shown.reserve(parts.size());
    for (const VersionDisplayParts &part : parts) {
        if (filtering && !InstanceFilter::matches(query, instanceFilterFacts(part))) {
            continue;
        }
        shown.append(part);
    }

    // ---- ② 按分组摆（归属照旧由 homeGroupIdOf() 答，筛子一个字都不改它）----
    //
    // 归属键走 instanceKey()（稳定 id，没有就退回目录名）。
    //
    // NOTE: 每个实例这里要读两次 wtfml.meta（上面 displayParts() 一次、这里 instanceId()
    // 一次）。两者不合并：把 instanceId 塞进 VersionDisplayParts 会让"显示名怎么拼"和
    // "归属怎么认"纠缠在一起，那是个纯显示用的结构。第二次读现在不再开文件 ——
    // `VersionManager::readVersionMeta()` 按"在不在 / 修改时刻 / 字节数"缓存了读回来的
    // 那一份，这里是一次 stat（见那边 `m_metaCache` 那段）。
    const QList<InstanceGroupStore::Group> groups = m_groupStore->groups();
    QList<InstanceGroupBucket> buckets;
    QHash<QString, int> bucketOfGroup;
    const QSet<QString> knownGroupIds = knownGroupIdsOf(m_groupStore);
    for (const InstanceGroupStore::Group &group : groups) {
        InstanceGroupBucket bucket;
        bucket.groupId = group.id;
        bucket.name = group.name;
        // 筛着的时候**每一组都摆成展开的**（「清空搜索恢复原展开状态」前半句）。
        // 不这么做的话，命中条目躲在收起来的组里 = 用户看着像"搜不到"。那个展开状态
        // **不落盘**（下面灌树只管画，写盘那条路在 onInstanceTreeActivated 里、被
        // `filtering` 挡着），所以清空之后各组回到 wtfml.folders 里记着的样子。
        bucket.expanded = filtering ? true : group.expanded;
        bucketOfGroup.insert(group.id, buckets.size());
        buckets.append(bucket);
    }
    const QString pinnedGroupId = m_groupStore->pinnedGroupId();
    QSet<QString> pinnedInstanceIds;
    QList<VersionDisplayParts> ungrouped;
    for (const VersionDisplayParts &part : shown) {
        const QString key = instanceKey(part.id);
        const QString homeId = homeGroupIdOf(m_groupStore, key, part, m_versionManager,
                                             knownGroupIds);
        const auto found = bucketOfGroup.constFind(homeId);
        if (found != bucketOfGroup.constEnd()) {
            buckets[found.value()].parts.append(part);
        } else {
            ungrouped.append(part);
        }

        // 两边都显示：置顶着的那一个在置顶组里**再摆一条同名
        // 的**，老家那一条从头到尾不动。取消置顶就是把这一条去掉。
        if (pinnedGroupId.isEmpty() || !m_groupStore->isPinned(key)) {
            continue;
        }
        pinnedInstanceIds.insert(part.id);
        if (homeId == pinnedGroupId) {
            continue;
        }
        const auto pinnedBucket = bucketOfGroup.constFind(pinnedGroupId);
        if (pinnedBucket != bucketOfGroup.constEnd()) {
            buckets[pinnedBucket.value()].parts.append(part);
        }
    }
    if (groups.isEmpty()) {
        // 一个真实分组都没有：平铺，一个组头都不插。
        buckets.clear();
        InstanceGroupBucket all;
        all.parts = shown;
        buckets.append(all);
    } else {
        InstanceGroupBucket rest;
        rest.name = MainWindowUI::ungroupedGroupText();
        rest.parts = ungrouped;
        buckets.append(rest);
    }
    fillInstanceTree(tree, buckets, runningVersionIds(),
                     m_instancePageMarks, pinnedGroupId, pinnedInstanceIds);
    QHash<QString, VersionDisplayParts> partOf;
    for (const VersionDisplayParts &part : shown) partOf.insert(part.id, part);
    const auto completedPlayTimes = m_launchHistory->completedPlayTimes();
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
        // 列表行第二行与行尾（ListRowDelegate）：游戏版本 / 加载器。
        const auto found = partOf.constFind(instanceIdOf(*it));
        if (found != partOf.constEnd()) {
            QString loaderVersion;
            const QString loader = instanceLoaderName(*found, m_versionManager, &loaderVersion);
            const auto playTime = completedPlayTimes.value(m_versionManager->instanceId(found->id));
            (*it)->setData(0, ListRowDelegate::SubtitleRole, instanceSubtitle(*found, playTime));
            (*it)->setData(0, ListRowDelegate::MetaRole,
                           loader.isEmpty() ? tr("原版") : (loader + QLatin1Char(' ') + loaderVersion).trimmed());
            // 封面：用户挑过图标用挑的那张，没挑按实例名稳定配色的等距方块（和主页一致）；
            // 加载器那一列的小图标用不带自选图标的那张行标（铁砧 / 草方块 / 快照…）。
            const QIcon mark = m_instancePageMarks.value(found->id);
            VersionDisplayParts plain = *found;
            plain.icon.clear();
            (*it)->setData(0, ListRowDelegate::CoverRole,
                           found->icon.isEmpty()
                               ? QIcon(VersionIcons::isoBlock(VersionIcons::isoBlockKindFor((*it)->text(0)), 48))
                               : mark);
            (*it)->setData(0, ListRowDelegate::MetaIconRole,
                           found->icon.isEmpty() ? mark
                                                 : QIcon(instanceRowMark(plain, m_versionManager,
                                                                         MainWindowUI::instanceTreeIconSize())));
        }
        if (m_fileOperationOccupancy.contains(instanceIdOf(*it))) {
            (*it)->setText(0, (*it)->text(0) + tr("（文件操作中）"));
            (*it)->setToolTip(0, tr("正在处理实例文件，完成后可再次启动或修改。"));
        }
    }
    syncTreeToCombo(m_ui->getInstalledVersionCombo(), tree);
    restoreTreeScroll(tree, keepTreeScroll);

    // ---- ③ 顶上那行说明：这一份文件夹里有几个实例（筛着的时候再说一句筛出来几个）----
    //
    // 数字与管理树一致：包含暂时文件操作中的已安装实例，不含尚在安装的新实例。这一行是
    // 实例页顶上那句"这里有几个实例"，和树里数得出来的行数必须是同一个数 —— 报
    // installed.size() 的话，正在装的那一个会让这句话比树上多一个，看着像漏画了。
    // 筛着的时候两个数**都要**：只报筛出来的那个数，用户会以为自己的实例没了。
    const QString root = AppPaths::minecraftDir();
    QString folderText = QDir::toNativeSeparators(root);
    if (!QDir(root).exists()) {
        folderText += tr("（目录已丢失）");
    } else {
        folderText += filtering
                          ? tr("（%1 个实例，筛出 %2 个）").arg(parts.size()).arg(shown.size())
                          : tr("（%1 个实例）").arg(parts.size());
    }
    m_ui->setInstanceFolderText(folderText);

    // ---- ④ 一条都没剩下时那一句话 ----
    //
    // 只在**真筛过**的时候露：不筛时树是空的（一个实例都没有）是另一件事，那句话由上面
    // 那行说明（「（0 个实例）」）去说 —— 混成一句的话，一个装着 60 个实例、只是还没扫完
    // 或者盘不在的用户会看见"没有符合筛选条件的实例"。
    if (QLabel *noMatch = m_ui->getInstanceNoMatchLabel()) {
        noMatch->setVisible(filtering && shown.isEmpty());
    }
    // 没在筛、盘上那个文件夹也在、就是一个实例都没有：给一句下一步去哪。
    if (QLabel *empty = m_ui->getInstanceEmptyLabel()) {
        // 空态是整块（插画 + 标题 + 引导 + 按钮）：标签跟着容器开关，列表与筛选行让位。
        const bool showEmpty = !filtering && parts.isEmpty() && QDir(root).exists();
        empty->setVisible(showEmpty);
        empty->parentWidget()->setVisible(showEmpty);
        tree->setVisible(!showEmpty);
        if (QWidget *filters = m_ui->getInstanceFilterRow()) filters->setVisible(!showEmpty);
    }
}

QSet<QString> MainWindow::runningVersionIds() const {
    QSet<QString> ids;
    const QList<LauncherManager::GameSession> games = m_launcherManager->runningGames();
    for (const LauncherManager::GameSession &game : games) {
        // 同一个实例多开：这里天然去重（QSet），而"它在跑"这件事本来就只要一个是。
        ids.insert(game.versionId);
    }
    return ids;
}

void MainWindow::updateInstanceTreeRunState() {
    QTreeWidget *tree = m_ui->getInstanceList();
    // 挡信号：改的是文字，不该被当成"用户在树里点了什么"（树上的点击会同步到下拉框）。
    const QSignalBlocker blocker(tree);
    const auto completedPlayTimes = m_launchHistory->completedPlayTimes();
    QTreeWidgetItemIterator it(tree);
    while (*it != nullptr) {
        const QString id = instanceIdOf(*it);
        if (!id.isEmpty()) {
            // 重新拼一遍而不是"在旧文本上加减后缀"：加减法要认得旧文本长什么样。
            // 每回从头拼，和整棵重灌（refreshInstalledVersions()）的结果就一定一样。
            const auto parts = m_versionManager->displayParts(id);
            QString text = instanceTreeItemText(parts, m_launcherManager->isVersionRunning(id));
            if (m_fileOperationOccupancy.contains(id)) {
                text += tr("（文件操作中）");
            }
            (*it)->setText(0, text);
            const auto playTime = completedPlayTimes.value(m_versionManager->instanceId(id));
            (*it)->setData(0, ListRowDelegate::SubtitleRole, instanceSubtitle(parts, playTime));
        }
        ++it;
    }
}

void MainWindow::updateStartButtonText() {
    // 按钮文字分两行，第二行显示当前选中的版本，"点下去会启动哪个"不用再回头
    // 看上面的下拉框。名字可能很长（26.2-snapshot-1，或"我的整合包-1.20.1-Forge"），
    // 交给 QPushButton 自己按宽度决定换不换行，这里只保证第一行永远是"启动游戏"。
    QComboBox *combo = m_ui->getInstalledVersionCombo();
    const QString shown =
        combo->currentIndex() < 0 ? tr("未选择版本") : combo->currentText();
    m_ui->getStartButton()->setText(tr("启动游戏\n%1").arg(shown));

    // 下拉框的 tooltip 挂**裸 id**，不是显示名。两个理由：
    // 1. 下拉框设了 minimumContentsLength(12)，长显示名会被省略号截断；
    // 2. 更要紧的是，id 在界面上**任何开关组合下都不会出现** —— 显示名那边参与
    // 拼装的"实际版本"是 parts.realVersion（为空才退化成 id），所以只要
    // wtfml.meta 里写了 realVersion，目录名就彻底从界面上消失了。而它才是真正
    // 进 --version 和文件路径的那个值。用户要排查"怎么启动错了版本"时，
    // 悬停是唯一能看到它的地方。
    // tooltip 必须走这条函数（而不是在 refreshInstalledVersions 里直接设），因为
    // fillVersionCombo() 套了 QSignalBlocker，刷新路径只能靠这一处收尾。
    const QString id = selectedVersionId(combo);
    combo->setToolTip(id.isEmpty() ? QString() : tr("版本目录名：%1").arg(id));
}

void MainWindow::refreshAvailableVersions() {
    // 只填树里那几条（开头是「最新版本」那一组 + 那四条分类）：它们唯一的数据源
    // 就是在线的版本清单（和已安装那一摊完全不相干 —— 清单为空时也不许退回显示本地
    // 已安装版本：两个数据源混在一张列表里，用户分不清点下去到底是启动还是下载）。
    // 四堆怎么分是 VersionManager 的事（它那边只有 VersionCategory 一处判据，含那张写死的
    // 愚人节版名单），"最新的两条是哪两条"同样是它的事（latestVersions()）。
    //
    // 条目文本就是**版本号本身**：在线清单 900+ 条逐条去读 wtfml.meta 是不可接受的
    // I/O，而且那些版本压根还没下载、磁盘上没有元数据可读。
    const QList<VersionManager::VersionChannel> channels =
        m_versionManager->availableVersionChannels();

    // 重灌之前记住"哪些分类是展开的"、"当前选中的是哪一条" —— 清单刷新（每次启动
    // 一次，拉回来还会再刷一次）不该把用户正在看的那一摊收起来。
    QTreeWidget *tree = m_ui->getVersionTree();
    QSet<QString> expandedKeys;
    QString selectedVersion;
    // 展开状态**逐个顶层条目读**，不区分"有没有选中项"。
    // NOTE: 不能写成 if/else 只记"选中项所在的那个分类"：那样同时展开着"正式版"和
    // "快照"、并且在正式版里点过一个版本号之后，下次重灌会把快照那条收起来
    // —— 而清单刷新（启动时那一次、以及拉回来之后那一次）本来就不该动用户
    // 看的那一摊。展开状态和选中项是两件互不相干的事，得分别记。
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *top = tree->topLevelItem(i);
        if (top->isExpanded()) {
            expandedKeys.insert(
                top->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toString());
        }
    }
    if (QTreeWidgetItem *current = tree->currentItem()) {
        if (QTreeWidgetItem *parent = current->parent()) {
            // 选中的是某个分类下的版本号。这个分类必须保住展开 —— 否则重灌之后选中
            // 的那条藏在收起来的分类里，用户看到的像是"选中的版本丢了"。
            expandedKeys.insert(
                parent->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toString());
            selectedVersion =
                current->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toString();
        }
    }

    // 「最新版本」那一组是**默认展开**的：树最上方放一个「最新版本」类，里面放最新的
    // 正式版和快照。这一格只**摆一次**：用户把它收起来之后，
    // 清单刷新（每次启动那一次、以及拉回来那一次）不该又把它撑开 —— 而"记住哪些分类是展开的"
    // 这笔账上面刚记完，靠的就是这份集合，所以这里直接往里面塞一格，走的是**和用户自己展开
    // 完全一样**的那条路（重灌之后照旧按 isExpanded() 记回来），不是"每一发都强行展开"。
    //
    // NOTE: 为什么需要一个成员来记"摆过一次"：头一发时树是空的 ⇒ expandedKeys 也是空的
    // ⇒ 不加这一格它从一开始就是收着的。而"树是不是空的"这个判据不能当替身 ——
    // refreshAvailableVersions() 之外还有别人能清这棵树（那样用户收起来的东西会被
    // 又撑开一次），用成员就与"谁清了树"无关。
    if (!mLatestGroupExpandSeeded) {
        expandedKeys.insert(VersionManager::latestGroupKey());
        mLatestGroupExpandSeeded = true;
    }

    const QSignalBlocker blocker(tree);
    const bool updatesEnabled = tree->updatesEnabled();
    tree->setUpdatesEnabled(false);
    tree->clear();

    // 摆在**最上面**：它讲的是"现在最值得装的这两条"，放在四个分类后面等于没摆（那四栏各自
    // 几百条，谁也不会滚到底去看）。它不是第五个分类，所以有几件事刻意和那四条不一样：
    // · 那四条是清单的**全集划分**（一条版本不重不漏地落进某一栏），这一组是**从里面挑
    // 两条出来** —— 同一个版本号在这两处各有一份（它自己那一栏里那条还在）；
    // · 身份不来自 VersionCategory::channelKey()，走 VersionManager::latestGroupKey()
    // （那个键保证和四个分类键两两不同，展开状态那笔账才串不了）；
    // · 里面那两条**只认版本号**（和别处的版本号一样：点一下就是"基于它装"），
    // 所以它前面那个小图标按它**自己属于哪一类**给（正式版草方块 / 快照版覆雪草块）。
    //
    // NOTE: 清单没拉回来时（离线、或者第一发就失败）这一组照旧摆着，只是里面空的 ——
    // 和那四条空分类是同一个规矩（"这一条压根不出现"和"点开是空的"在界面上是两件
    // 很不一样的事，见 availableVersionChannels() 那段）。
    auto *latestTop = new QTreeWidgetItem(tree);
    latestTop->setData(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole,
                       VersionManager::latestGroupKey());
    latestTop->setText(MainWindowUI::versionTreeKeyColumn(), VersionManager::latestGroupText());
    for (const VersionManager::LatestVersion &entry : m_versionManager->availableLatestVersions()) {
        auto *child = new QTreeWidgetItem(latestTop);
        child->setData(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole, entry.version.id);
        child->setText(MainWindowUI::versionTreeKeyColumn(), entry.version.id);
        child->setData(MainWindowUI::versionTreeKeyColumn(), MainWindowUI::rowSubTextRole(),
                       releaseDateText(entry.version.releaseTime));
        const QPixmap mark =
            VersionIcons::versionChannelMark(VersionCategory::channelIndex(entry.channel),
                                             MainWindowUI::versionTreeIconSize());
        if (!mark.isNull()) {
            child->setData(MainWindowUI::versionTreeKeyColumn(), MainWindowUI::rowIconRole(), mark);
        }
    }
    if (expandedKeys.contains(VersionManager::latestGroupKey())) {
        latestTop->setExpanded(true);
    }

    for (const VersionManager::VersionChannel &channel : channels) {
        auto *top = new QTreeWidgetItem(tree);
        // 身份在 userData（= 分类键），文字只是给人看的 —— 和左栏那列分类同一个约定。
        top->setData(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole, channel.key);
        top->setText(MainWindowUI::versionTreeKeyColumn(),
                     channel.text.isEmpty() ? channel.key : channel.text);
        // NOTE: 这里**不要**去动 setChildIndicatorPolicy —— 想关掉 Qt 自己那个"左边
        // 一个小三角"的话，那是关错了地方：QTreeModel::hasChildren() 是按这个
        // policy 直接 return false 的（QTreeWidget 源码里那条 switch），于是
        // **整个分类的版本号一条都排不进布局**，树上只剩四条光秃秃的分类、
        // 点开也是空的。真要把左边那个三角去掉，走 rootIsDecorated(false)
        // （见 MainWindowUI 里建树那一段）：它只影响装饰格的绘制，不动
        // "有没有孩子"这件事。
        // 右边那个箭头仍然由 VersionTreeDelegate 画在行最右边，
        // 一个版本都没有的分类它连箭头都不画。
        // 版本号前面那个小图标**按分类**给（快照版用白色覆雪草块，远古版用土块，
        // 愚人节版用幸运方块，原版不变）。分类 → 图标那张表在
        // VersionIcons::versionChannelMark()，这里只把分类的**下标**递过去 —— ui/ 那边
        // 不认 VersionCategory（见 VersionIcons.hpp 那段"纯函数"的说明），所以分类 → 下标
        // 这一步必须走 VersionCategory::channelIndex()，别在这儿另写一个 switch：
        // 写歪了的症状是"快照那一栏前面白草、远古那一栏还是绿草"，不报错、只是难看。
        const QPixmap channelMark =
            VersionIcons::versionChannelMark(VersionCategory::channelIndex(channel.channel),
                                             MainWindowUI::versionTreeIconSize());
        for (const MinecraftVersion &version : channel.versions) {
            auto *child = new QTreeWidgetItem(top);
            child->setData(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole, version.id);
            child->setText(MainWindowUI::versionTreeKeyColumn(), version.id);
            // 版本号下面那行小字：发布时间（精确到日）。清单里没有 / 认不出来的，那行就不画。
            child->setData(MainWindowUI::versionTreeKeyColumn(), MainWindowUI::rowSubTextRole(),
                           releaseDateText(version.releaseTime));
            if (!channelMark.isNull()) {
                child->setData(MainWindowUI::versionTreeKeyColumn(), MainWindowUI::rowIconRole(),
                               channelMark);
            }
        }
        if (expandedKeys.contains(channel.key)) {
            top->setExpanded(true);
        }
        // 把选中那条找回来：**只在四条分类里找，不找「最新版本」那一组**。
        // Why: 同一个版本号在树上有两份（它自己那一栏里一条、最新那一组里可能还有一条），
        // 两个都是真的、点哪个都是"基于它装"。这一搜要是从头开始（「最新版本」现在在
        // 第一条），用户本来在"正式版"里选着的那条**每次刷新都会悄悄跳到最上面那一组**
        // 去 —— 他没动过手，高亮自己跑了。只在分类里找：高亮永远落在用户自己点过的
        // 那一栏里。代价：要是用户当初是从「最新版本」那一组点进去的，
        // 刷新之后高亮会落到它自己那一栏里 —— 那也仍在屏幕上、仍是同一条版本。
        if (!selectedVersion.isEmpty()) {
            for (int i = 0; i < top->childCount(); ++i) {
                if (top->child(i)->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole)
                        .toString() == selectedVersion) {
                    tree->setCurrentItem(top->child(i));
                    break;
                }
            }
        }
    }

    m_ui->applyVersionFilter();
    tree->setUpdatesEnabled(updatesEnabled);
}

void MainWindow::openInstallPage(const QString &gameVersion) {
    // 薄壳：这一页的全部状态与摆法在 `InstallPageController`（见那个头文件开头的解释）。
    m_installPage->openPage(gameVersion);
}

void MainWindow::backToVersionTree() {
    m_installPage->backToVersionTree();
}


