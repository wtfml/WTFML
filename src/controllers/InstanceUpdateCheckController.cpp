#include "ActivityCenterController.hpp"
#include "InstanceUpdateCheckController.hpp"
#include <QCoreApplication>
#include "UpdateCenterController.hpp"
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "InlineMessage.hpp"
#include "InstanceFilesPage.hpp"
#include "InstanceSettingsDialog.hpp"
#include "settings/LauncherSettings.hpp"
#include "download/ResourceStore.hpp"
#include "util/AppPaths.hpp"
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QTimer>

InstanceUpdateCheckController::InstanceUpdateCheckController(MainWindow *owner) : QObject(owner), m_owner(*owner) {}

InstanceUpdateCheckController::~InstanceUpdateCheckController() { waitForLookup(); }

void InstanceUpdateCheckController::waitForLookup() {
    if (m_updateCheck.thread != nullptr) m_updateCheck.thread->wait();
}

void InstanceUpdateCheckController::checkInstanceUpdates(const QString &instanceId, InstanceSettingsDialog *dialog,
                                     const QStringList &selectedPaths) {
    if (instanceId.isEmpty()) {
        return;
    }
    if (m_updateCheck.thread != nullptr) {
        if (m_updateCheck.instanceId == instanceId && dialog != nullptr)
            m_updateCheck.dialog = dialog;
        else if (dialog != nullptr)
            m_pendingChecks.insert(instanceId, dialog);
        return;
    }

    // ① 候选文件（四个"能下载的资源"目录，不含存档/原理图 —— 那两样没有"更新"这回事）。
    QStringList paths;
    const QSet<QString> selectedScope(selectedPaths.begin(), selectedPaths.end());
    QHash<QString, ResourceCatalog::Kind> pathKinds;
    const QHash<QString, QString> dirs = m_owner.instanceFileDirs(instanceId);
    const QStringList wantedDirs = {QStringLiteral("mod"), QStringLiteral("resourcepack"),
                                    QStringLiteral("datapack"), QStringLiteral("shader")};
    for (const QString &kindKey : wantedDirs) {
        ResourceCatalog::Kind kind = ResourceCatalog::Kind::Mod;
        if (!ResourceCatalog::kindFromCategoryKey(kindKey, &kind)) {
            continue;  // 页键认不回类别 = 这张表改了名，那一类这一趟不查（宁可少一类）
        }
        const QString dir = dirs.value(kindKey);
        if (dir.isEmpty()) {
            continue;
        }
        QDir folder(dir);
        const QFileInfoList entries =
            folder.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo &info : entries) {
            if (!selectedScope.isEmpty() && !selectedScope.contains(info.absoluteFilePath())) continue;
            // 禁用中的文件（`X.jar.disabled`）这一趟跳过 —— 不是因为认不出（后缀不改
            // 文件内容，sha1 照样能判），而是：那一行在屏幕上是被**划掉**的（名字一道
            // 删除线、开关关着，见 InstanceFilesPage::makeRow），游戏根本不加载它 ⇒ 替它
            // 问一趟"有没有新版本"是替一个**用不到的东西**问：真答了也只是让那一行多出
            // 一个 ↑，而用户按不下去（更新那一发是要**装进这个实例**的，装一个被禁用的
            // 文件没有意义）。
            // 顺带这一趟也小一截（少读几个 jar、少发几发反查）。
            //
            // NOTE: 判据就是盘上那个名字的后缀 —— 和文件页"这一条是不是禁用着"**逐字同一
            // 个**（`disabledSuffix()`，大小写不敏感，和 InstanceFilesPage 里那处一样）。
            // NOTE: **不是**用 `identityPath()` 去重 —— 被跳过的那一条压根不进这份名单。
            if (info.fileName().endsWith(InstanceFilesPage::disabledSuffix(),
                                         Qt::CaseInsensitive)) {
                continue;
            }
            paths.append(info.absoluteFilePath());
            pathKinds.insert(info.absoluteFilePath(), kind);
        }
    }
    if (paths.isEmpty()) {
        return;
    }

    m_updateCheck = UpdateCheckRun{};
    m_updateCheck.selectedScope = selectedScope;
    m_updateCheck.instanceId = instanceId;
    m_updateCheck.pathKinds = pathKinds;
    // 结果回来时要递给**同一页**（见 UpdateCheckRun::dialog 那段）。弱引用：收页了就不递。
    m_updateCheck.dialog = dialog;
    m_owner.appendLog(MainWindow::tr("正在检查「%1」的资源更新（%2 个文件）…")
                  .arg(m_owner.instanceDisplayName(instanceId))
                  .arg(paths.size()));

    InstanceIconLookup::Options options;
    // 三个口子和 ResourceStore / VersionManager / 文件管理页那三处**同一份设置**。
    options.curseForgeApiKey = m_owner.m_settings->curseForgeApiKey();
    options.modrinthApiBaseOverride = m_owner.m_settings->modrinthApiBaseOverride();
    options.curseForgeApiBaseOverride = m_owner.m_settings->curseForgeApiBaseOverride();
    options.cacheDir = AppPaths::instanceIconCacheDir();
    options.cacheKey = instanceId;
    options.requireVersionMetadata = true;

    // NOTE: 线程对象由 `finishUpdateCheck()` 自己收（见那一段 ★）—— 走到这里时
    // 上一趟要么已经跑完、要么被上面那道 `m_updateCheck.thread != nullptr` 挡回去了。
    m_updateCheck.thread = QThread::create([this, paths, options, instanceId]() {
        const InstanceIconLookup::Result result = InstanceIconLookup::lookup(paths, options);
        QMetaObject::invokeMethod(
            this,
            [this, result, instanceId]() { onUpdateCheckLookupFinished(result, instanceId); },
            Qt::QueuedConnection);
    });
    m_updateCheck.thread->setParent(this);
    m_updateCheck.thread->start();
}

void InstanceUpdateCheckController::onUpdateCheckLookupFinished(const InstanceIconLookup::Result &result,
                                            const QString &instanceId) {
    // 回来看的这一趟是不是**现在**要的那一趟（用户完全可能又点了一次检查、或者换了实例）。
    if (m_updateCheck.instanceId != instanceId) {
        return;
    }
    // 反查那一趟自己的毛病（读不动几个文件 / 整趟没跑完）——报告末尾照实说一句。
    // **不算失败**：认出来的那些照样判（那是真的）。
    if (!result.problem.isEmpty()) {
        m_updateCheck.lookupNote = result.problem;
    } else if (result.unreadable > 0) {
        m_updateCheck.lookupNote =
            MainWindow::tr("无法读取 %1 个文件，相关资源未完成检查。")
                .arg(result.unreadable);
    }

    const ModDependencies::Target target =
        m_owner.dependencyTargetFor(instanceId, ResourceCatalog::Source::Modrinth);

    // **一个文件只查一家**（缘由见 `UpdateCheckRun::fallbackItems` 那段）。
    // 先量一遍"哪几个文件 Modrinth 认出来了" —— 同一个文件在两家都认出来时，
    // 下面那两条记录（路径逐字同一个）只有**第一条**该往下走。
    // 必须**单独走一趟**：`result.found` 的顺序不定（两份回复都是哈希表），
    // 边遍历边攒的话，"CurseForge 那条"完全可能排在"Modrinth 那条"前面。
    QSet<QString> modrinthPaths;
    for (const InstanceIconLookup::Found &found : result.found) {
        if (found.hit.projectId.isEmpty()) {
            continue;
        }
        if (found.hit.source == ResourceCatalog::Source::Modrinth) {
            modrinthPaths.insert(found.path);
        }
    }

    for (const InstanceIconLookup::Found &found : result.found) {
        if (found.hit.projectId.isEmpty()) {
            continue;
        }
        ModDependencies::InstalledItem item;
        item.path = found.path;
        item.iconKey = found.hit.iconCacheId();
        item.title = found.hit.title;
        item.sha1 = found.sha1;
        item.currentVersion = found.currentVersion;
        item.projectId = found.hit.projectId;
        item.source = found.hit.source;
        item.loaderApplies = ModDependencies::kindUsesLoader(
            m_updateCheck.pathKinds.value(found.path, ResourceCatalog::Kind::Mod));
        // NOTE: 只有 Modrinth 有这个接口 —— CurseForge 那些照旧一项目一发（它们通常
        // 只占一小撮，几发不成气候；而"两家的号不是一套"这件事在下面那把钥匙上
        // 已经带着来源了）。
        // 这一条该带的加载器（`item.loaderApplies` 那一位的用法，
        // 见 `ModDependencies::kindUsesLoader()` 那段）。**每一处拼钥匙 / 发请求都得
        // 走它** —— 直接递 `target.loader` 的话，资源包那几类又带上「fabric」了。
        const QString itemLoader = item.loaderApplies ? target.loader : QString();
        if (found.hit.source == ResourceCatalog::Source::Modrinth && !found.sha1.isEmpty()) {
            // 批量那一发**按钥匙分组**（加载器不同的两拨不能合成一发 —— 见
            // `UpdateCheckRun::batchItems` 那段）。
            const QString batchKey =
                ModDependencies::latestVersionsKeyFor(QStringLiteral("update"), target.gameVersion,
                                                      itemLoader);
            m_updateCheck.batchItems[batchKey][found.sha1.toLower()].append(item);
            m_updateCheck.batchLoader.insert(batchKey, itemLoader);
            continue;
        }
        // 钥匙跟着**这一条自己的来源**走（两家的 id 空间不是一套，问错边问出来的是
        // 另一个项目）。来源在 hit 上，不在 target 上。
        const QString key = ModDependencies::versionsKeyFor(
            QStringLiteral("update"), found.hit.projectId, target.gameVersion, itemLoader,
            found.hit.source);
        // 同一个文件 Modrinth 那边也认出来了 ⇒ **押着**（它已经在 `batchItems` 里了，
        // 而两家的答案在这里同义）。等批量那一发回来看它有没有这个 sha1 的答案：有就
        // 到此为止（这一条已经被判过），没有才退回问 CurseForge。
        if (found.hit.source != ResourceCatalog::Source::Modrinth &&
            modrinthPaths.contains(found.path)) {
            m_updateCheck.fallbackItems[key].append(item);
            continue;
        }
        m_updateCheck.itemsByKey[key].append(item);
    }

    m_updateCheck.waitingKeys.clear();
    for (auto it = m_updateCheck.itemsByKey.constBegin();
         it != m_updateCheck.itemsByKey.constEnd(); ++it) {
        m_updateCheck.waitingKeys.insert(it.key());
    }
    // 批量那几发（Modrinth 那一拨）：**一把钥匙一发**（模组那拨带加载器、资源包那拨不带
    // —— 见 `UpdateCheckRun::batchItems` 那段）。空表就整个不发（"一个都不用问"也是走完了）。
    for (auto it = m_updateCheck.batchItems.constBegin();
         it != m_updateCheck.batchItems.constEnd(); ++it) {
        m_updateCheck.pendingBatchKeys.insert(it.key());
        m_owner.m_resourceStore->fetchLatestVersions(it.key(), it.value().keys(), target.gameVersion,
                                             m_updateCheck.batchLoader.value(it.key()));
    }
    if (m_updateCheck.waitingKeys.isEmpty() && m_updateCheck.pendingBatchKeys.isEmpty()) {
        // 一个都没认出来（新实例、或者全是自己改过的 jar）——**照样出一份报告**：
        // "一个都认不出来"正是用户需要知道的事，闷着不说是最坏的。
        finishUpdateCheck(instanceId);
        return;
    }

    // 逐个项目问版本列表。**一把钥匙一发**，互相不撞。
    for (auto it = m_updateCheck.itemsByKey.constBegin();
         it != m_updateCheck.itemsByKey.constEnd(); ++it) {
        requestUpdateCheckVersions(it.key());
    }
}

void InstanceUpdateCheckController::requestUpdateCheckVersions(const QString &requestKey) {
    const QList<ModDependencies::InstalledItem> items = m_updateCheck.itemsByKey.value(requestKey);
    if (items.isEmpty()) {
        return; // 这一把底下没人 = 不是这一趟的钥匙，不发
    }
    // 目标（游戏版本 / 加载器）每次现算：它读的是实例自己那几份文件，而这一趟的实例
    // 就在 `m_updateCheck.instanceId` 上。
    const ModDependencies::Target target =
        m_owner.dependencyTargetFor(m_updateCheck.instanceId, ResourceCatalog::Source::Modrinth);
    // 项目号从钥匙里读不出来（钥匙是拼出来的一串），所以从这一组里那一条自己的 `iconKey`
    // （`<来源>-<项目号>`）上取。
    const ModDependencies::InstalledItem &any = items.first();
    ResourceCatalog::Source source = ResourceCatalog::Source::Modrinth;
    const int dash = any.iconKey.indexOf(QLatin1Char('-'));
    if (dash > 0) {
        ResourceCatalog::sourceFromKey(any.iconKey.left(dash), &source);
    }
    const QString projectId = dash > 0 ? any.iconKey.mid(dash + 1) : any.iconKey;
    const QString itemLoader = any.loaderApplies ? target.loader : QString();
    // 先记账再发：答案回来时按 `waitingKeys` 认"是不是这一趟等的"，漏记就是"判了不算"。
    m_updateCheck.waitingKeys.insert(requestKey);
    m_owner.m_resourceStore->fetchVersions(requestKey, projectId, source, target.gameVersion, itemLoader);
}

void InstanceUpdateCheckController::onUpdateCheckLatestFinished(
    const QString &requestKey, const QHash<QString, ResourceCatalog::VersionEntry> &latest,
    const QString &problem) {
    // 不是这一趟等的那一发就丢掉（这一把钥匙只有更新检测在用，但"是不是我这一趟"这件事
    // 还是得问一句 —— 用户完全可能刚又点了一次检查）。
    if (!m_updateCheck.pendingBatchKeys.contains(requestKey)) {
        return;
    }
    m_updateCheck.pendingBatchKeys.remove(requestKey);
    for (auto it = latest.constBegin(); it != latest.constEnd(); ++it) {
        m_updateCheck.batchAnswered.insert(it.key());
    }

    QSet<QString> heldPaths;
    for (auto it = m_updateCheck.fallbackItems.constBegin();
         it != m_updateCheck.fallbackItems.constEnd(); ++it) {
        for (const ModDependencies::InstalledItem &item : it.value()) {
            heldPaths.insert(item.path);
        }
    }

    // 只判**这一发**问回来的那一拨（`batchItems` 现在按钥匙分层，别的钥匙底下那些不归它）。
    const QHash<QString, QList<ModDependencies::InstalledItem>> answeredItems =
        m_updateCheck.batchItems.value(requestKey);
    for (auto it = answeredItems.constBegin(); it != answeredItems.constEnd(); ++it) {
        const ResourceCatalog::VersionEntry found = latest.value(it.key());
        for (const ModDependencies::InstalledItem &item : it.value()) {
            if (found.id.isEmpty() && heldPaths.contains(item.path)) {
                continue; // 交给退回的那一发（上面那段）
            }
            const ModDependencies::UpdateVerdict verdict =
                ModDependencies::judgeUpdateAgainstLatest(item, found, problem);
            m_updateCheck.verdicts.append(verdict);
            // "这条可更新"的那一瞬间把它该换成的那一版留一份（和单发那条路同一个规矩，
            // 见 UpdateCheckRun::latestByPath）。
            if (verdict.state == ModDependencies::UpdateState::Outdated && !found.id.isEmpty()) {
                m_updateCheck.latestByPath.insert(item.path, found);
            }
        }
    }

    if (!m_updateCheck.pendingBatchKeys.isEmpty()) {
        return;
    }

    // NOTE: 判据是**这一趟攒下来的那一份**（`batchAnswered`，跨所有批量钥匙），
    // 不是这一发的回复 —— 批量现在分几发，只看这一发的话另一把钥匙答过的那些
    // 会被当成"没答"再问一遍（见上面那段）。
    //
    // 分流必须**逐条**做，不能"这一把钥匙底下只要有一条答过就整组跳过"。
    // 一把钥匙 = 一个项目，而同一个项目完全可能对应盘上**两个文件**（比如它发过一个
    // 本体包和一个附加包，用户两个都装了；或者同一个 jar 放过两份不同版本）。那种
    // 情况下 Modrinth 完全可能只答得出其中一个 sha1：整组跳过的写法会把另一个文件
    // **一条结论都不给** —— 它既不在 `itemsByKey`（不会被问、不会被判），也不在
    // `verdicts` 里 ⇒ 报告里凭空少一行、`checked` 数也比盘上的文件少。屏幕上完全
    // 看不出来（其余几条都正常），正是这一套最怕的那种"安静地少一条"。
    QStringList refire;
    for (auto it = m_updateCheck.fallbackItems.constBegin();
         it != m_updateCheck.fallbackItems.constEnd(); ++it) {
        QList<ModDependencies::InstalledItem> unanswered;
        for (const ModDependencies::InstalledItem &item : it.value()) {
            if (!item.sha1.isEmpty() && m_updateCheck.batchAnswered.contains(item.sha1)) {
                continue; // Modrinth 答过这一条，上面那个循环已经把它判过了
            }
            unanswered.append(item);
        }
        if (unanswered.isEmpty()) {
            continue;
        }
        // 挪进 `itemsByKey` —— **这一挪同时把它算进收尾判据**（那把钥匙发出去之后
        // `onUpdateCheckVersionsFinished()` 判的就是这儿这一组）。只挪**没答过的那几条**：
        // 答过的已经在上面判过了，再挪一遍就是同一个文件两条结论。
        m_updateCheck.itemsByKey[it.key()].append(unanswered);
        // 同一个项目的**另一条**（比如它底下还有一个只有 CurseForge 认出来的文件）如果
        // 已经发出去过了，就不再发第二发 —— 答案本来就是同一份（版本列表是按项目问的），
        // 而 `onUpdateCheckVersionsFinished()` 是按**答案回来时**那一组来判的，照样判得到。
        if (!m_updateCheck.waitingKeys.contains(it.key())) {
            refire.append(it.key());
        }
    }
    // 先清空再发：`requestUpdateCheckVersions()` 会往 `waitingKeys` 里加，而这两件事的顺序
    // 反过来就是"刚记上的账又被这一句抹掉"（也免得一边遍历 `fallbackItems` 一边动它）。
    m_updateCheck.fallbackItems.clear();
    for (const QString &key : refire) {
        requestUpdateCheckVersions(key);
    }

    if (m_updateCheck.waitingKeys.isEmpty()) {
        finishUpdateCheck(m_updateCheck.instanceId);
    }
}

void InstanceUpdateCheckController::onUpdateCheckVersionsFinished(const QString &requestKey,
                                              const QList<ResourceCatalog::VersionEntry> &versions,
                                              const QString &problem) {
    if (!m_updateCheck.waitingKeys.contains(requestKey)) {
        return; // 不是这一趟等的（详情页那一发 / 前置那一趟 / 已经判过的那把）
    }
    m_updateCheck.waitingKeys.remove(requestKey);

    const QList<ResourceCatalog::VersionEntry> &answer =
        problem.isEmpty() ? versions : QList<ResourceCatalog::VersionEntry>();
    for (const ModDependencies::InstalledItem &item : m_updateCheck.itemsByKey.value(requestKey)) {
        const ModDependencies::UpdateVerdict verdict =
            ModDependencies::judgeUpdate(item, answer, problem);
        m_updateCheck.verdicts.append(verdict);
        if (verdict.state == ModDependencies::UpdateState::Outdated && !answer.isEmpty()) {
            m_updateCheck.latestByPath.insert(item.path, answer.first());
        }
    }
    // 这一节手上这两样是两条**互相独立**的线：`waitingKeys` 是"一项目一发"那一拨
    // （CurseForge 那些），`pendingBatchKeys` 是 Modrinth 那一拨的批量那几发。只看
    // `waitingKeys` 的话，一个**两家都有**的实例上（一部分走批量、一部分走单发）
    // 只要单发里**任意一发**先回来、而它正好是最后一把没还的钥匙，
    // 这一趟就当场收尾了 —— 批量那一发回来时它自己那一句闸（见上面）直接 return：
    // **批量那一拨一条都不会进报告**，退回问 CurseForge 的那一拨（`fallbackItems`）
    // 也一辈子不会发出去。屏幕上是一份"检查了 80 个资源"的正常报告，日志也干干净净
    // —— 用户看到的就是「无法正常获取更新」。
    // 收尾判据必须和 `checkInstanceUpdates()` / `onUpdateCheckLatestFinished()` 那两句
    // （"空表 + 没有批量"）**逐字同一条**。
    if (m_updateCheck.waitingKeys.isEmpty() && m_updateCheck.pendingBatchKeys.isEmpty()) {
        finishUpdateCheck(m_updateCheck.instanceId);
    }
}

void InstanceUpdateCheckController::finishUpdateCheck(QString instanceId) {
    if (m_updateCheck.instanceId != instanceId || !m_updateCheck.waitingKeys.isEmpty() ||
        !m_updateCheck.pendingBatchKeys.isEmpty()) return;
    const UpdateCheckRun run = m_updateCheck;
    if (run.thread != nullptr) {
        if (!run.thread->isFinished()) {
            run.thread->wait();
        }
        run.thread->deleteLater();
    }
    m_updateCheck = UpdateCheckRun{};
    if (!m_pendingChecks.isEmpty()) {
        const auto pending = m_pendingChecks;
        m_pendingChecks.clear();
        QTimer::singleShot(0, this, [this, pending] {
            for (auto it = pending.cbegin(); it != pending.cend(); ++it)
                if (it.value()) checkInstanceUpdates(it.key(), it.value(), {});
        });
    }

    QList<ModDependencies::UpdateVerdict> outdated;
    QList<ModDependencies::UpdateVerdict> unknown;
    QList<ModDependencies::UpdateVerdict> current;
    for (const ModDependencies::UpdateVerdict &verdict : run.verdicts) {
        switch (verdict.state) {
        case ModDependencies::UpdateState::Outdated:
            outdated.append(verdict);
            break;
        case ModDependencies::UpdateState::UpToDate:
            current.append(verdict);
            break;
        case ModDependencies::UpdateState::Unknown:
            unknown.append(verdict);
            break;
        }
    }

    // 这一趟查出来的"哪几条有新版本"**必须留下来**：文件管理那几页状态栏里那一格
    // 「可更新」要的就是它 —— 不留的话，那趟检测只在弹框里活了几秒钟，
    // "筛出可更新的那几条"一次都做不到。
    MainWindow::UpdateCheckResult raw;
    raw.verdicts = ModDependencies::mergeScopedUpdates(
        m_owner.m_instanceUpdateResults.value(instanceId).verdicts, run.verdicts, run.selectedScope);
    raw.lookupNote = run.lookupNote;
    m_owner.m_instanceUpdateResults.insert(instanceId, raw);
    auto targets = run.selectedScope.isEmpty()
        ? QHash<QString, ResourceCatalog::VersionEntry>() : m_owner.m_instanceUpdateTargets.value(instanceId);
    for (const auto &path : run.selectedScope) targets.remove(path);
    for (auto it = run.latestByPath.cbegin(); it != run.latestByPath.cend(); ++it)
        targets.insert(it.key(), it.value());
    m_owner.m_instanceUpdateTargets.insert(instanceId, targets);
    {
        QHash<QString, QString> sha1ByKey;
        const QString instanceDirForPolicy = AppPaths::versionDir(instanceId);
        for (const ModDependencies::UpdateVerdict &verdict : run.verdicts) {
            const QString key = UpdatePolicy::storeKey(
                instanceDirForPolicy, InstanceFilesPage::identityPath(verdict.item.path));
            if (!key.isEmpty()) {
                sha1ByKey.insert(key, verdict.item.sha1);
            }
        }
        // Missing keys in a partial scan are not evidence of removed files.
        if (run.selectedScope.isEmpty() && UpdatePolicy::dropStalePins(m_owner.m_updateCenter->policy(), instanceId, sha1ByKey) > 0) {
            m_owner.saveUpdatePolicy();
        }
    }
    // 名单由**唯一那一处**按现在这本账算（见 rebuildInstanceOutdatedPaths）。
    m_owner.rebuildInstanceOutdatedPaths(instanceId);
    // **先记账再递**：递出去的那一份是上面这一行写的（不是从这里临时拼的），于是"这一页
    // 现在筛得出来的"和"下一次开这一页筛得出来的"永远是同一份。
    m_owner.pushInstanceFileUpdates(run.dialog, instanceId);
    const auto byName = [](const ModDependencies::UpdateVerdict &a,
                           const ModDependencies::UpdateVerdict &b) {
        return a.item.title < b.item.title;
    };
    std::sort(outdated.begin(), outdated.end(), byName);
    std::sort(unknown.begin(), unknown.end(), byName);
    std::sort(current.begin(), current.end(), byName);

    const auto displayName = [](const ModDependencies::UpdateVerdict &verdict) {
        if (!verdict.item.title.isEmpty()) {
            return verdict.item.title;
        }
        // 名字取不到就用文件名 —— 那是**盘上那一条**的名字，用户能照它找到文件。
        return QFileInfo(verdict.item.path).fileName();
    };

    QStringList lines;
    for (const ModDependencies::UpdateVerdict &verdict : outdated) {
        lines.append(QStringLiteral("• %1 —— %2 %3")
                         .arg(displayName(verdict), ModDependencies::updateOutdatedText(),
                              verdict.latestVersionNumber));
    }
    for (const ModDependencies::UpdateVerdict &verdict : unknown) {
        lines.append(QCoreApplication::translate("InstanceUpdateCheckController", "• %1 —— %2：%3")
                         .arg(displayName(verdict), ModDependencies::updateUnknownText(),
                              verdict.reason));
    }
    for (const ModDependencies::UpdateVerdict &verdict : current) {
        lines.append(QStringLiteral("• %1 —— %2")
                         .arg(displayName(verdict), ModDependencies::updateUpToDateText()));
    }

    const int checked = run.verdicts.size();
    const QString summary = ModDependencies::updateSummaryText(checked, outdated.size());

    m_owner.appendLog(MainWindow::tr("更新检测（%1）：%2").arg(m_owner.instanceDisplayName(instanceId), summary));
    for (const QString &line : lines) {
        m_owner.appendLog(QStringLiteral("  ") + line);
    }
    if (!run.lookupNote.isEmpty() || !unknown.isEmpty()) {
        QStringList problems;
        if (!run.lookupNote.isEmpty()) problems.append(run.lookupNote);
        for (const auto &verdict : unknown)
            problems.append(QStringLiteral("%1：%2").arg(displayName(verdict), verdict.reason));
        problems.removeDuplicates();
        problems.sort();
        const QString details = problems.join(QLatin1Char('\n'));
        if (m_lastIncompleteReports.value(instanceId) == details) return;
        m_lastIncompleteReports.insert(instanceId, details);
        const auto task = m_owner.m_activityCenter->begin(Tasks::TaskKind::Other,
            MainWindow::tr("资源更新检查"), instanceId);
        m_owner.m_activityCenter->finish(task, false,
            MainWindow::tr("更新检查结果可能不完整") + QLatin1Char('\n') + summary + QLatin1Char('\n') + details, false);
    } else {
        m_lastIncompleteReports.remove(instanceId);
    }
}
