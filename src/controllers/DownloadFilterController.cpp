#include "controllers/DownloadFilterController.hpp"

#include <QComboBox>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTreeWidget>
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "LoadingState.hpp"
#include "manage/VersionManager.hpp"

DownloadFilterController::DownloadFilterController(MainWindow *owner) : QObject(owner), m_owner(*owner) {}

bool DownloadFilterController::kindForSearchRequestKey(const QString &key, ResourceCatalog::Kind *out,
                                         ResourceCatalog::Source *outSource) const {
    // 拼钥匙和认钥匙读的是同一个函数（searchRequestKey）—— 所以这里不解析字符串，
    // 一条一条比。有几类，比几次。
    //
    // NOTE: **来源也要比一圈**。钥匙里带来源（见 searchRequestKey 那段），
    // 只比"默认那个来源"的话，CurseForge 那一发回来时一把都对不上 —— 症状是
    // "切到 CurseForge 按了搜索，那一页什么都没有、也不报错"（结果被当成"不是我们
    // 发的"整发丢掉）。
    for (ResourceCatalog::Kind kind : ResourceCatalog::allKinds()) {
        for (ResourceCatalog::Source source : ResourceCatalog::allSources()) {
            if (ResourceCatalog::searchRequestKey(kind, source) == key) {
                if (out != nullptr) {
                    *out = kind;
                }
                if (outSource != nullptr) {
                    *outSource = source;
                }
                return true;
            }
        }
    }
    return false;
}

void DownloadFilterController::ensureResourcePageLoaded(ResourceCatalog::Kind kind) {
    QTreeWidget *tree = m_owner.m_ui->getDownloadResourceTree(ResourceCatalog::kindKey(kind));
    if (tree == nullptr) {
        return;
    }
    if (tree->topLevelItemCount() > 0) {
        // 这一页已经有东西了（用户搜过、或者刚才进来时搜了一次）。**不重搜**：来回换分类
        // 每次都问一遍，既慢、又是在拿限流额度换同一份东西。
        return;
    }
    m_owner.startResourceSearch(kind);
}

void DownloadFilterController::refreshDownloadCategory(int row) {
    const QStringList keys = MainWindowUI::downloadCategoryKeys();
    if (row < 0 || row >= keys.size()) {
        // 过滤器交上来的行号理论上一定在表里（那一行是 MainWindowUI 按这张表建出来的），
        // 越界只可能是"表和列表对不上了"那种时候 —— 什么都不做比按错一页去做强。
        return;
    }
    if (row == MainWindowUI::downloadCategoryServerIndex()) {
        if (auto *categories = m_owner.m_ui->getDownloadCategoryList()) categories->setCurrentRow(row);
        m_owner.m_versionManager->fetchAvailableVersions();
        return;
    }
    // 点的那一类要是现在没选中的，先切过去 —— 不切的话请求飞出去了、结果落在看不见的
    // 那一页上，用户看到的就是"点了没反应"。这一条对**五条一视同仁**（「游戏」也是：
    // 它刷新的是版本树，那棵树就长在它自己那一页上）。
    //
    // NOTE: 切过去那一下会触发 currentRowChanged → ensureResourcePageLoaded()，而那一页
    // 要是还空着，那儿**自己就会搜一次**。所以下面分两半：切过去的路上那一发已经
    // 发出去了就直接完事（再发一发是同一份东西问两遍，白烧一次限流额度）；页面上
    // 已经有东西了才轮到我们按现在的搜索词重搜一次 —— 那才是用户按这个按钮要的
    // 那件事：**重新问一遍**，而不是"第一次问"。
    QListWidget *categories = m_owner.m_ui->getDownloadCategoryList();
    const bool wasCurrent = categories == nullptr || categories->currentRow() == row;
    if (!wasCurrent) {
        categories->setCurrentRow(row);
    }
    if (row == MainWindowUI::downloadCategoryGameIndex()) {
        // 「游戏」那一页是**在线版本清单**，不是资源搜索（那张表上根本没有"游戏"这个键，
        // kindFromCategoryKey 也认不出来，所以它得自己一条分支）。
        //
        // 树里**不插**"正在…"那一行：refreshAvailableVersions() 每一发都重建**恰好那几条顶层**
        // （「最新版本」那一组 + 四条分类，那是它的活），中途插一行会被它整个冲掉；而且版本树
        // 本来就不会因为"正在等"而空着：那棵一直挂在那儿（fetchAvailableVersions 失败时也保留
        // 上一份好清单，见 onVersionsFetched）。等待的提示挂在筛选条右边那块空地上（一行加载态，
        // 过 150ms 才露），onVersionsFetched 收掉它 —— 成功失败都收，失败时旧清单照旧留着。
        LoadingState::begin(m_owner.m_ui->getVersionRefreshStatus(), MainWindowUI::versionRefreshingText());
        m_owner.appendLog(tr("正在重新获取游戏版本列表…"));
        m_owner.m_versionManager->fetchAvailableVersions();
        return;
    }
    ResourceCatalog::Kind kind = ResourceCatalog::Kind::Mod;
    if (!ResourceCatalog::kindFromCategoryKey(keys.at(row), &kind)) {
        return;  // 认不出来 = 这一行不是我们建的那几类之一，没有"重问一遍"这回事
    }
    if (!wasCurrent) {
        QTreeWidget *tree = m_owner.m_ui->getDownloadResourceTree(ResourceCatalog::kindKey(kind));
        if (tree != nullptr && tree->topLevelItemCount() > 0) {
            m_owner.startResourceSearch(kind);
        }
        return;
    }
    // 已经在这一页上：按搜索框里现在那个词重搜（空词 = 这一类里最热门的，和第一次进来同一个规矩）。
    // NOTE: **图标缓存一个字节都不动**。这一发重新走的是 search()，回来的那批 hit 里
    // project_id 和上次多半是同一批，requestIcon() 在内存/磁盘缓存里就命中了
    // （见 ResourceStore 里 requestIcon 那段"两级缓存"）—— "图标留缓存"正是
    // 这个：重新问一遍**不会**把已经存下来的图标丢掉、也不会重新下一遍。
    // 这一下**也不重新核对**（核对的闸是"启动器这一趟运行"一道，重搜不重置它，
    // 见 m_iconRevalidated）—— 核对要跟着"每次打开启动器"，不能跟着"每点一下"。
    // 唯一的例外是"这一批刚才放弃了"：那阵子没核成的会在这里得到第二次机会
    // （search() 会把那道闸放回去，见它开头那两句）。
    m_owner.startResourceSearch(kind);
}

// ---------- 每一页顶上那一行筛子----------
//
// 这一段的整段理由在头文件里那段：界面只搭空框，**内容全在这儿灌**，读也只从下面这几个
// 访问器读。谁要是绕开它们自己去 `combo->currentText()`，症状是"界面上筛了、请求里没筛"
// —— 不报错、也看不出哪儿不对。

ResourceCatalog::Source DownloadFilterController::downloadFilterSource(ResourceCatalog::Kind kind) const {
    ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth;
    QComboBox *combo = m_owner.m_ui->getDownloadFilterSourceCombo(ResourceCatalog::kindKey(kind));
    if (combo != nullptr) {
        // 认不出来（框是空的 / 里面不是我们灌的那个键）就停在 Modrinth —— 和"这一页
        // 没有来源框"是同一个结果，也正是没有来源框时该有的行为。
        ResourceCatalog::sourceFromKey(combo->currentData().toString(), &source);
    }
    return source;
}

QString DownloadFilterController::downloadFilterGameVersion(ResourceCatalog::Kind kind) const {
    // **读框里的字**，不是 currentData()。这一格是可输入的（要求「版本可自行输入」），
    // 手打一个版本号时列表里没有那一项、currentData() 是空的 —— 而框里明明写着那个版本号。
    QComboBox *combo = m_owner.m_ui->getDownloadFilterVersionCombo(ResourceCatalog::kindKey(kind));
    return combo == nullptr ? QString() : combo->currentText().trimmed();
}

QString DownloadFilterController::downloadFilterLoader(ResourceCatalog::Kind kind) const {
    // 别的页上这个控件**不存在**（MainWindowUI 里是"只有模组页才建"，不是建了再藏），
    // 所以空指针就是"这一页没有这一栏"。回空串 = 不筛，正是这一栏没有时该有的行为。
    QComboBox *combo = m_owner.m_ui->getDownloadFilterLoaderCombo(ResourceCatalog::kindKey(kind));
    return combo == nullptr ? QString() : combo->currentData().toString();
}

QString DownloadFilterController::downloadFilterCategory(ResourceCatalog::Kind kind) const {
    QComboBox *combo = m_owner.m_ui->getDownloadFilterTypeCombo(ResourceCatalog::kindKey(kind));
    return combo == nullptr ? QString() : combo->currentData().toString();
}

void DownloadFilterController::fillDownloadFilterVersionOptions() {
    // 版本那一格：**一整份在线清单**，没有「全部」那一项 —— 这一格是可输入的，
    // "不筛"就是框里空着（那时框里显示灰字「全部版本」，见 downloadFilterVersionPlaceholder）。
    const QStringList versions = m_owner.m_versionManager->availableVersions();
    for (ResourceCatalog::Kind kind : ResourceCatalog::allKinds()) {
        QComboBox *combo = m_owner.m_ui->getDownloadFilterVersionCombo(ResourceCatalog::kindKey(kind));
        if (combo == nullptr) {
            continue;
        }
        // 灌之前把用户手里那串字记下来、灌完放回去。清单是**异步**回来的（开机一次、
        // 用户按左栏那个刷新按钮又一次），而 clear() 会把框里编辑着的东西一起清掉：
        // 他刚打完版本号、清单正好回来，那串字就没了，可筛子还在生效 —— 界面和请求对不上。
        const QString current = combo->currentText();
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItems(versions);
        if (!current.isEmpty()) {
            combo->setCurrentText(current);
        }
    }
}

void DownloadFilterController::refreshDownloadFilterOptions(ResourceCatalog::Kind kind) {
    const QString key = ResourceCatalog::kindKey(kind);
    const ResourceCatalog::Source source = downloadFilterSource(kind);

    // 「模组加载器」那一栏（**只有模组页有**，别的页 getDownloadFilterLoaderCombo 回空指针）。
    if (QComboBox *loaderCombo = m_owner.m_ui->getDownloadFilterLoaderCombo(key)) {
        const QSignalBlocker blocker(loaderCombo);
        loaderCombo->clear();
        for (const ResourceCatalog::FilterOption &option : ResourceCatalog::loaderOptions(source)) {
            loaderCombo->addItem(option.label, option.key);
        }
        // 换来源之后退回「全部」（= 第一项）。两边的键根本不是一套（Modrinth 的 `fabric`
        // 对 CurseForge 的 `4`），留着上一个来源选中那一项，就是拿 A 家的号去问 B 家 ——
        // 而且它不报错，只会搜出来一片空。
        loaderCombo->setCurrentIndex(0);
    }

    // 「类型」那一栏。**回空表就把整个小组藏起来**（连标签一起，见 MainWindowUI 里那段）：
    // 摆一个只有「全部」的框等于骗人。现在只有 Modrinth 的数据包会空。
    QComboBox *typeCombo = m_owner.m_ui->getDownloadFilterTypeCombo(key);
    QWidget *typeBox = m_owner.m_ui->getDownloadFilterTypeBox(key);
    if (typeCombo != nullptr) {
        const QSignalBlocker blocker(typeCombo);
        typeCombo->clear();
        // 「全部」那一项是 categoryOptions() 自己带的第一项，这里不补也不动顺序。
        for (const ResourceCatalog::FilterOption &option :
             ResourceCatalog::categoryOptions(source, kind)) {
            typeCombo->addItem(option.label, option.key);
        }
        typeCombo->setCurrentIndex(typeCombo->count() > 0 ? 0 : -1);
    }
    if (typeBox != nullptr) {
        typeBox->setVisible(typeCombo != nullptr && typeCombo->count() > 0);
    }
}

void DownloadFilterController::fillDownloadFilters() {
    for (ResourceCatalog::Kind kind : ResourceCatalog::allKinds()) {
        const QString key = ResourceCatalog::kindKey(kind);
        // 来源那一格：两项，顺序照 allSources()（Modrinth 在前：它是不要 key 的那个）。
        // **没有「全部」这一项** —— 一个页面上同时只搜一个
        // 来源，那是这一格的语义。
        if (QComboBox *sourceCombo = m_owner.m_ui->getDownloadFilterSourceCombo(key)) {
            const QSignalBlocker blocker(sourceCombo);
            sourceCombo->clear();
            for (ResourceCatalog::Source source : ResourceCatalog::allSources()) {
                sourceCombo->addItem(ResourceCatalog::sourceLabel(source),
                                     ResourceCatalog::sourceKey(source));
            }
            sourceCombo->setCurrentIndex(0);
        }
        // 加载器 / 类型那两项跟着来源走，所以灌法是"按当前来源算一遍"。
        refreshDownloadFilterOptions(kind);
    }
    fillDownloadFilterVersionOptions();
}

void DownloadFilterController::resetDownloadFilters(ResourceCatalog::Kind kind) {
    const QString key = ResourceCatalog::kindKey(kind);
    // 名称那一格：就是搜索框。
    if (QLineEdit *edit = m_owner.m_ui->getDownloadSearchEdit(key)) {
        edit->clear();
    }
    // 版本那一格是可输入的，`setCurrentIndex(-1)` 不一定把框里的字清掉（值是从
    // currentText() 读的），所以**显式**再清一遍那串字。
    if (QComboBox *versionCombo = m_owner.m_ui->getDownloadFilterVersionCombo(key)) {
        const QSignalBlocker blocker(versionCombo);
        versionCombo->setCurrentIndex(-1);
        versionCombo->clearEditText();
    }
    // 加载器和类型那两格：第一项就是「全部」（键空串），回到它 = 不筛。
    if (QComboBox *loaderCombo = m_owner.m_ui->getDownloadFilterLoaderCombo(key)) {
        const QSignalBlocker blocker(loaderCombo);
        loaderCombo->setCurrentIndex(loaderCombo->count() > 0 ? 0 : -1);
    }
    if (QComboBox *typeCombo = m_owner.m_ui->getDownloadFilterTypeCombo(key)) {
        const QSignalBlocker blocker(typeCombo);
        typeCombo->setCurrentIndex(typeCombo->count() > 0 ? 0 : -1);
    }
    // 「来源」**不清**：它不是"条件"，是"去哪儿问"。清成"没有来源"这件事不存在
    // （这一格只有两项、总有一项是选中的），而且用户刚在设置里配好的 CurseForge key
    // 就是给这一格用的 —— 清一下把他踢回 Modrinth，那是替他做了个他没说的决定。
    //
    // NOTE: 这一层**只清**，搜不搜是调用方的事 —— 那颗键上接着"清完重搜一发"（
    // 要求「修改任何内容…都刷新一下」，接线见构造函数里那段）。
    m_owner.appendLog(tr("已清空%1页面的搜索条件。").arg(ResourceCatalog::kindLabel(kind)));
}
