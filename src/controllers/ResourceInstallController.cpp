#include "controllers/ResourceInstallController.hpp"
#include <QCoreApplication>

#include <QDir>
#include <QFileDialog>
#include <QScopedValueRollback>
#include <QSet>
#include <QStringList>
#include <algorithm>

#include "MainWindow.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/DownloadTaskLedgerController.hpp"
#include "controllers/MainWindowViewData.hpp"
#include "controllers/ResourcePageController.hpp"
#include "download/ModDependencies.hpp"
#include "download/ResourceStore.hpp"
#include "manage/InstanceFiles.hpp"
#include "manage/ModpackImport.hpp"
#include "manage/VersionManager.hpp"
#include "settings/LauncherSettings.hpp"
#include "tasks/TaskManager.hpp"
#include "InlineMessage.hpp"
#include "MainWindowUI.hpp"
#include "util/AppPaths.hpp"

using MainWindowViewData::loaderGroupKey;

ResourceInstallController::ResourceInstallController(MainWindow *owner) : QObject(owner), m_owner(*owner) {}

std::optional<ResourceInstallController::InstallTarget>
ResourceInstallController::selectTarget(ResourceCatalog::Kind kind,
                                        const ResourceCatalog::VersionEntry &version) {
    MainWindow &owner = m_owner;
    if (kind == ResourceCatalog::Kind::Modpack) {
        return InstallTarget{AppPaths::modpackCacheDir(), {}, {}};
    }

    const QString folder = ResourceCatalog::folderForKind(kind);
    if (folder.isEmpty()) {
        return std::nullopt;
    }
    const QString defaultId = owner.resourceDownloadDefaultInstanceId();
    const QDir versionsDir(AppPaths::versionsDir());
    if (owner.m_settings->downloadAutoIntoInstance()) {
        auto rows = owner.currentFolderInstancePickerRows(defaultId);
        for (auto &row : rows) {
            const QString game = owner.m_versionManager->instanceGameVersion(row.id);
            if (!game.isEmpty() && !version.gameVersions.isEmpty() &&
                !version.gameVersions.contains(game)) {
                row.subtitle += QStringLiteral(" · ") + MainWindow::tr("版本不符");
            }
            if (kind == ResourceCatalog::Kind::Mod && !version.loaders.isEmpty()) {
                const bool compatible = std::any_of(version.loaders.cbegin(), version.loaders.cend(),
                    [&row](const QString &loader) { return loaderGroupKey(loader) == row.loaderKey; });
                if (!compatible) row.subtitle += QStringLiteral(" · ") + MainWindow::tr("加载器不符");
            }
        }
        bool otherFolder = false;
        const QString selectedId = owner.m_ui->chooseResourceDownloadInstance(rows, &otherFolder);
        if (!otherFolder) {
            if (selectedId.isEmpty()) return std::nullopt;
            const QString instanceDir = versionsDir.filePath(selectedId);
            return InstallTarget{QDir(instanceDir).filePath(folder), selectedId, instanceDir};
        }
    }

    const QString instanceDir = defaultId.isEmpty() ? QString() : versionsDir.filePath(defaultId);
    const QString defaultDir = instanceDir.isEmpty() ? AppPaths::minecraftDir()
                                                     : QDir(instanceDir).filePath(folder);
    const QString chosen = QFileDialog::getExistingDirectory(
        &owner, MainWindow::tr("下载到哪个文件夹"), defaultDir);
    if (chosen.isEmpty()) {
        return std::nullopt;
    }
    if (!instanceDir.isEmpty() && QDir(chosen) == QDir(defaultDir)) {
        return InstallTarget{chosen, defaultId, instanceDir};
    }
    return InstallTarget{chosen, {}, {}};
}

bool ResourceInstallController::targetStillAvailable(const InstallTarget &target) {
    if (target.instanceId.isEmpty()
        || (QDir(AppPaths::versionDir(target.instanceId)) == QDir(target.instanceDirectory)
            && QDir(target.instanceDirectory).exists())) {
        return true;
    }
    InlineMessage::warning(m_owner.m_ui->overlay(), MainWindow::tr("目标实例已不可用"),
                           MainWindow::tr("游戏目录已切换或该实例已移除，请重新选择下载目标。"));
    return false;
}

void ResourceInstallController::install(ResourceCatalog::Kind kind, QString displayTitle,
                                        ResourceCatalog::VersionEntry version,
                                        ResourceCatalog::Source source) {
    if (m_installing || m_check.active) {
        return;
    }
    const QScopedValueRollback installing(m_installing, true);
    MainWindow &owner = m_owner;
    const auto destination = selectTarget(kind, version);
    if (!destination || !targetStillAvailable(*destination)) {
        return;
    }
    const QString &instanceId = destination->instanceId;
    // 判据：这一版声明的必装前置里，**目标实例还缺哪几个**（`ModDependencies` 那边算，
    // 纯函数）。一条不缺就走下面那一句 —— 那时候**一个请求都不多发**。
    //
    // NOTE: 资源包 / 数据包**不看**：它们那一版列的是 `minecraft` / `datapack`（游戏本体），
    // 那和"前置"不是一回事；整合包更不用 —— 它的依赖在清单里，本来就是整个包一起装。
    if ((kind == ResourceCatalog::Kind::Mod || kind == ResourceCatalog::Kind::Shader) &&
        !instanceId.isEmpty() && owner.m_settings->warnMissingDependencies()) {
        // 光影的**豁免**：这个实例已经能跑光影了（加载器是 OptiFine，或者
        // mods/ 里已经有 iris / oculus / optifine）⇒ 整段跳过。
        // 理由：Modrinth 上光影项目声明的前置就是 `iris`（OptiFine 那一支还有的写
        // `optifine`），而**装了 OptiFine 的人不需要 iris** —— 不豁免的话，他每下一次光影
        // 都被劝着装一个用不上的前置。判据直接借现成的 `instanceShaderUsable()`
        // （整段词匹配那一条也在它里面，不在这儿再写一遍）。
        const bool shaderAlreadyUsable =
            kind == ResourceCatalog::Kind::Shader && owner.instanceShaderUsable(instanceId);
        if (!shaderAlreadyUsable) {
            const ModDependencies::Target target = owner.dependencyTargetFor(instanceId, source);
            bool installedKnown = false;
            const QSet<QString> installed = owner.installedModKeys(instanceId, &installedKnown);
            const QList<ModDependencies::Lookup> missing =
                ModDependencies::missingRequiredDependencies(version, installed, target);
            if (!missing.isEmpty()) {
                startDependencyCheck(kind, displayTitle, version, *destination, target, missing, installedKnown);
                return;
            }
        }
        // 设置里关着 / 不知道装进哪个实例 / 光影已经能用 / 没有缺的：交给下面那一句，
        // 它的判断和话术一个字都不变（关掉 = 只下他点的那一版）。
    }
    installNow(kind, displayTitle, version, {}, *destination);
}

void ResourceInstallController::startDependencyCheck(ResourceCatalog::Kind kind,
                                                     const QString &displayTitle,
                                                     const ResourceCatalog::VersionEntry &version,
                                                     const InstallTarget &destination,
                                                     const ModDependencies::Target &target,
                                                     const QList<ModDependencies::Lookup> &missing,
                                                     bool installedKnown) {
    MainWindow &owner = m_owner;
    m_check = DependencyCheck{};
    m_check.active = true;
    m_check.kind = kind;
    m_check.title = displayTitle;
    m_check.version = version;
    m_check.destination = destination;
    m_check.target = target;
    m_check.missing = missing;
    // "这个实例的 mods 到底认过没有"（见 installedModKeys 的 known 出参）—— 它只决定
    // 弹框那句话怎么写（"这几个还没装" vs "无法确认你装了没有"），所以跟着这一趟存着。
    m_check.installedKnown = installedKnown;

    QStringList ids;
    ids.reserve(missing.size());
    for (const ModDependencies::Lookup &lookup : missing) {
        if (!ids.contains(lookup.dependency.projectId)) {
            ids.append(lookup.dependency.projectId);
        }
    }
    // 消费方名字（`dependency`）**必须跟详情页那一趟（`detail`）分开**：两处问的完全可能
    // 是同一批号，而 `fetchProjects()` 是按钥匙去重的（同一把钥匙再来一发把前一发掐掉）。
    // 撞上的症状是"先发的那一趟永远等不到回信"，屏幕上一点异常都没有。
    m_check.titleRequestKey = ModDependencies::projectsKeyFor(
        QStringLiteral("dependency"), ids, m_check.target.source);
    owner.appendLog(ModDependencies::dependencyLookupLogText(missing.size()));
    owner.m_resourceStore->fetchProjects(m_check.titleRequestKey, ids, m_check.target.source);
}

void ResourceInstallController::onProjectsFinished(const QString &requestKey,
                                                   const QList<ResourceCatalog::Hit> &hits,
                                                   const QString &problem) {
    MainWindow &owner = m_owner;
    if (!m_check.active || requestKey != m_check.titleRequestKey) {
        return; // 不是这一趟等的（用户中途又点了别的下载）
    }
    // 名字取不到**不算错**：那一条退成项目号显示（见 ModDependencies::buildPlan 那段），
    // 而"没有名字"挡不住"要不要一起下"这个决定。所以这里只记一句日志。
    if (!problem.isEmpty()) {
        owner.appendLog(MainWindow::tr("前置模组的名字没取到：%1").arg(problem));
    }
    // 名字进**会话级**那个缓存（详情页那一行读的是同一份）。取不到名字的那几条**不写**：
    // 写一个空串进去等于把"还没问过"说成"问过了、它没名字"。
    owner.rememberProjectTitles(hits, m_check.target.source);

    // 逐条去问"它有哪些版本"。**同一批发出去**（一把钥匙一个项目，互相不撞）——
    // 串起来问的话，N 个前置就是 N 个往返，而它们本来就互不相干。
    //
    // NOTE: 同一把钥匙**只问一发、只等一发**（R7）。`missing` 已经按项目去重过
    // （见 `ModDependencies::missingRequiredDependencies()`），这里再按钥匙收一道：
    // `ResourceStore` 那边同一把钥匙的第二发会把第一发掐掉、**而且不回调**，发两发就
    // 少一发回信，这一趟永远收不了尾。
    m_check.answers.clear();
    m_check.pendingKeys.clear();
    for (const ModDependencies::Lookup &lookup : m_check.missing) {
        if (m_check.pendingKeys.contains(lookup.requestKey)) {
            continue;
        }
        m_check.pendingKeys.insert(lookup.requestKey);
        owner.m_resourceStore->fetchVersions(lookup.requestKey, lookup.dependency.projectId,
                                             m_check.target.source, m_check.target.gameVersion,
                                             m_check.target.loader);
    }
    if (m_check.pendingKeys.isEmpty()) {
        finishDependencyCheck(); // 一条都不缺（走不到：missing 非空才进这一趟）
    }
}

void ResourceInstallController::onVersionsFinished(
    const QString &requestKey, const QList<ResourceCatalog::VersionEntry> &versions,
    const QString &problem) {
    if (!m_check.active) {
        return;
    }
    // 认这一发：钥匙还在"待回"集合里才是这一趟等的。同一条钥匙的重复 / 迟到回信在这里
    // 被挡掉（R7：同一项目在清单里出现两次时，少减一次，这一趟就永远收不了尾）；
    // 详情页那一发（或者别的页面）的回复钥匙不在集合里，同样挡掉。
    if (!m_check.pendingKeys.remove(requestKey)) {
        return;
    }
    // 失败时**不往 answers 里放空表**：`buildPlan` 那边"钥匙缺席"和"回来了但是空的"
    // 是两句不同的话（"没问到"和"它就是没有版本"）—— 放空表等于把前者说成后者。
    if (problem.isEmpty()) {
        m_check.answers.insert(requestKey, versions);
    }
    if (m_check.pendingKeys.isEmpty()) {
        finishDependencyCheck();
    }
}

void ResourceInstallController::finishDependencyCheck() {
    MainWindow &owner = m_owner;
    const QScopedValueRollback installing(m_installing, true);
    DependencyCheck check = std::move(m_check);
    m_check.active = false;
    if (!targetStillAvailable(check.destination)) {
        return;
    }

    const bool installedKnown = check.installedKnown;

    // 名字从会话缓存里查（详情页那一行读的是同一份）。缺的那几条**退成项目号**——
    // 列出来总比"有 2 个前置没找到"强（见 ModDependencies::buildPlan 那段），
    // 而"为什么没名字"已经写进日志了（见 onProjectsFinished）。
    QHash<QString, QString> titles;
    for (const ModDependencies::Lookup &lookup : check.missing) {
        const QString name = owner.m_projectTitles.value(
            owner.projectTitleKey(check.target.source, lookup.dependency.projectId));
        if (!name.isEmpty()) {
            titles.insert(lookup.dependency.projectId, name);
        }
    }

    const ModDependencies::Plan plan =
        ModDependencies::buildPlan(check.missing, check.answers, titles);

    QStringList names;
    names.reserve(check.missing.size());
    for (const ModDependencies::Lookup &lookup : check.missing) {
        const QString name = titles.value(lookup.dependency.projectId);
        names.append(name.isEmpty() ? lookup.dependency.projectId : name);
    }

    const QString note = ModDependencies::unresolvedNoteText(plan.unresolved);
    QList<ModDependencies::Resolved> extras;
    if (plan.downloads.isEmpty()) {
        // 一条都没解开：**照样把这个模组装上**，但得如实说一句"前置一条都没找到"。
        // 不说的话，用户点完下载看到的就是"下好了"，而进游戏那一下就崩。
        //
        // NOTE: 这一档走 `ask()` 而不是 `warning()`：只有 `ask()` 带得了那行**灰字**
        // （那行灰字写的是"可以在设置里关掉"）。一颗「知道了」按下去就走。
        QString text = ModDependencies::dependencyPromptText(check.kind, names);
        if (!note.isEmpty()) {
            text += QStringLiteral("\n\n") + note;
        }
        InlineMessage::ask(owner.m_ui->overlay(), MessageKind::Warning,
                           ModDependencies::dependencyPromptTitle(), text,
                           {MainWindow::tr("知道了")}, 0, -1, false,
                           ModDependencies::dependencyPromptNoteText());
    } else {
        QString text = installedKnown
                           ? ModDependencies::dependencyPromptText(check.kind, names)
                           : ModDependencies::dependencyPromptUnknownText(check.kind, names);
        if (!note.isEmpty()) {
            text += QStringLiteral("\n\n") + note;
        }
        // 两颗按钮：左「只下这个」（**默认那颗** —— 误按回车不该替用户多做一件事），
        // 右「一起下载」。
        // NOTE: 必须用 `ask()`：`question()` 带不了那行灰字（没有 note 这一格）。
        const int choice = InlineMessage::ask(
            owner.m_ui->overlay(), MessageKind::Question,
            ModDependencies::dependencyPromptTitle(), text,
            {ModDependencies::dependencyOnlyThisButtonText(),
             ModDependencies::dependencyAllButtonText()},
            0, -1, false, ModDependencies::dependencyPromptNoteText());
        if (choice == 1) {
            extras = plan.downloads;
        }
    }

    installNow(check.kind, check.title, check.version, extras, check.destination);
}

void ResourceInstallController::installNow(ResourceCatalog::Kind kind, const QString &displayTitle,
                                          const ResourceCatalog::VersionEntry &version,
                                          const QList<ModDependencies::Resolved> &extras,
                                          const InstallTarget &target) {
    MainWindow &owner = m_owner;
    const bool isModpack = kind == ResourceCatalog::Kind::Modpack;
    const QString &instanceId = target.instanceId;
    const QString &destinationDir = target.directory;
    if (isModpack) {
        // 整合包一律自动先缓存到 modpackCacheDir() 再直接安装进当前游戏目录，不再弹“下载到哪个文件夹”。
        QDir().mkpath(destinationDir);
    }

    const ResourceCatalog::FileEntry *file = version.primaryFile();
    if (file == nullptr || file->url.isEmpty()) {
        // 「接口没给地址」有两种，**不是同一句话**：一种是这一版真的没有文件；
        // 另一种是作者在项目设置里关掉了第三方分发（CurseForge 那条路，`downloadUrl`
        // 给的就是 null，见 FileEntry::unavailableNote）。后者要是也说成"没有可下载的文件"，
        // 用户会当成启动器坏了，然后一遍遍重试 —— 那句话在那儿就写好了，用它。
        const QString note = file == nullptr ? QString() : file->unavailableNote;
        InlineMessage::warning(owner.m_ui->overlay(), MainWindow::tr("下载失败"),
                               note.isEmpty() ? MainWindow::tr("这一版没有可下载的文件。") : note);
        return;
    }

    // 文件名是**接口给的那一串**，下面要拿它拼两种路径：落点（见 downloadResourceFiles）
    // 和整合包那条 `modpackPath`。先过一遍同一套规矩（`InstanceFiles::validateFileName()`）
    // —— `downloadResourceFiles()` 里还有一道（最后一道，别的调用方也走那儿），这里这一道
    // 是为了**早**拦住：用户当场看到一句"这个文件名没法用"，而不是等一批文件下到一半。
    const QString fileNameProblem = InstanceFiles::validateFileName(file->filename);
    if (!fileNameProblem.isEmpty()) {
        InlineMessage::warning(owner.m_ui->overlay(), MainWindow::tr("下载失败"),
                               MainWindow::tr("版本文件名无效，已取消下载（%1）：%2")
                                   .arg(file->filename, fileNameProblem));
        return;
    }

    // 加载器对不对得上。**这一层没有按加载器筛过**（见 onResourceItemActivated 里那段
    // NOTE），所以在这儿补一道闸：给一个 Fabric 实例装一个 Forge 的模组，症状是"游戏进去就
    // 崩"，而那一刻没人会想到是刚才那个模组。
    //
    // NOTE: 这道闸**只管模组**（非模组时不拦）。理由：`VersionEntry::loaders`
    // 不是只有模组加载器 —— 资源包那一版列的是 `minecraft`、数据包列的是 `datapack`、
    // 光影列的是 `iris/optifine`，它们跟"这个实例装的是什么模组加载器"根本没有关系。
    // 不收的话，给一个 Forge 实例装资源包/数据包会**每次都弹一次**「加载器对不上」，
    // 而那句话（"这一版是给 minecraft 用的"）本身就说不通 —— 那种框弹多了，用户就
    // 再也不看这类提醒了，真正该拦的那一次也跟着一起被忽略。
    const bool loaderMatters = kind == ResourceCatalog::Kind::Mod && !instanceId.isEmpty();
    // 判据刻意做得**窄**（只拦"知道实例是什么加载器、且这一版只列了另一种"这一种）：
    // · 不是模组（资源包 / 数据包 / 光影 / 整合包）→ 放行；
    // · 认不出实例的加载器（纯原版，或者 json 里没写）→ 放行；
    // · 这一版列了不止一种加载器（Quilt 之类兼容好几种）→ 放行；
    // · 纯原版实例 + 一个带加载器的模组 → 也放行（认不出来的和纯原版在这里分不开，
    // 宁可漏拦也不要拦住一个本来装得上的）。
    // 拦下来的是**问一句**，不是禁止 —— 坚持"就要这么装"就照装。
    // 加载器名走 instanceLoaderName()，**不是** `parts.loaderName`：后者只有我们自己的
    // 安装器会写，PCL 那类合并实例读出来是空的 —— 于是这道闸
    // 对它们**恒不放行**，"加载器对不上"那句话一次都弹不出来（它最该拦的那一对恰好就
    // 在这些实例上）。理由同 instanceModsUsable() 那段。
    QString instanceLoaderVersion;
    const QString instanceLoader =
        loaderMatters ? MainWindowViewData::instanceLoaderName(
                            owner.m_versionManager->displayParts(instanceId),
                            owner.m_versionManager, &instanceLoaderVersion)
                      : QString();
    if (!instanceLoader.isEmpty() && version.loaders.size() == 1
        && version.loaders.first().compare(instanceLoader, Qt::CaseInsensitive) != 0) {
        const auto answer = InlineMessage::question(owner.m_ui->overlay(),
            MainWindow::tr("加载器不匹配"),
            MainWindow::tr("当前实例为 %1 %2，所选文件适用于 %3。\n\n安装不兼容的文件可能导致游戏无法启动。确认安装？")
                .arg(instanceLoader, instanceLoaderVersion, version.loaders.first()),
            MainWindow::tr("是"), MainWindow::tr("否"), false);
        if (!answer) {
            return;
        }
    }

    QList<VersionManager::ResourceFile> files;
    VersionManager::ResourceFile resourceFile;
    resourceFile.url = file->url;
    resourceFile.filename = file->filename;
    resourceFile.sha1 = file->sha1;
    resourceFile.size = file->size;
    files.append(resourceFile);

    // 前置**永远是模组**（光影那一类的必装前置是 iris 之类）—— 落点必须按模组算，
    // 不能跟着主文件那个目录走：光影的主文件进 `shaderpacks/`，前置跟着进去的话游戏
    // 根本不加载它，光影还是不可用，而界面上已经说"装好了"。
    //
    // 所以每一条前置自己带 `dir`（`ResourceFile::dir` 那条路本来就是给"一批里跨目录"
    // 准备的，更新中心那一批在用）。只有实例里才有 mods 这个落点；整合包那一类不查
    // 前置（见 install()），这里的空串走不到。
    if (!targetStillAvailable(target)) {
        return;
    }
    const QString dependencyDir =
        extras.isEmpty() ? QString() : QDir(target.instanceDirectory).filePath(QStringLiteral("mods"));
    for (const ModDependencies::Resolved &extra : extras) {
        if (extra.file.url.isEmpty() || extra.file.filename.isEmpty()) {
            continue; // 走不到（resolveDependency 已经挡过一道），挡第二道不费什么
        }
        VersionManager::ResourceFile extraFile;
        extraFile.url = extra.file.url;
        extraFile.filename = extra.file.filename;
        extraFile.sha1 = extra.file.sha1;
        extraFile.size = extra.file.size;
        extraFile.dir = dependencyDir;
        files.append(extraFile);
    }

    QString label = QStringLiteral("%1 %2").arg(displayTitle, version.versionNumber);
    if (!extras.isEmpty()) {
        // 日志和"下好了"那句话上写清楚这一批里**还有前置**（不然用户看到下了好几个文件、
        // 而屏幕上只说了一个模组的名字，会以为下错了）。
        label += MainWindow::tr("（含 %1 个前置）").arg(extras.size());
    }
    // 下完接着走导入那条路（那边会问实例名、挑 Java，然后解包、下清单里那几百个文件）。
    // 路径按**接口给的文件名**算 —— 落盘用的就是它（见 downloadResourceFiles），这里
    // 不另编一套命名。
    const QString modpackPath =
        isModpack ? destinationDir + QLatin1Char('/') + file->filename : QString();
    owner.appendLog(
        MainWindow::tr("正在下载 %1（%2）。").arg(label, QDir::toNativeSeparators(destinationDir)));
    if (!extras.isEmpty() && !dependencyDir.isEmpty()) {
        // 主文件去的目录和前置去的目录**不是同一个**，日志上要说清 —— 不然用户看到
        // shaderpacks/ 里多了一个 zip、mods/ 里多了一个 jar，会以为是两回事。
        owner.appendLog(MainWindow::tr("%1 个前置按模组装进 %2。")
                            .arg(extras.size())
                            .arg(QDir::toNativeSeparators(dependencyDir)));
    }
    // instanceId 传空的有两路：整合包下到缓存里（盘上没有任何实例在被写），以及用户
    // 自己挑了实例之外的目录那一路 —— 两路都不该锁住谁（见上面算落点那一段）。
    //
    // 这一批也要在任务中心里**看得见**（状态、进度、取消、失败后的重试）——
    // 不记任务的话，下几百 MB 的整合包时底栏那格一直空着，失败之后也没有任何地方
    // 能点「重试」（失败清单是有的，可没有一行任务认领它）。
    //
    // 号必须在 `downloadResourceFiles()` **之前**建：所有文件盘上都已经有的时候，那一批
    // 会**同步**收尾（当场发 `resourceDownloadFinished`），先建号才认领得到那一条终态。
    // 反过来，"忙"的那几种拒绝也会同步发一条失败终态 —— 所以先自己把忙态问一遍，
    // 被拒的白问不该在账本上留一行红字（判据和 updateInstanceFiles() 那道闸同一个）。
    //
    // `m_downloadInFlight` 必须一起问。版本安装取 version.json 的那
    // 一段是元数据作业（不计进 `isDownloading()`），而那一趟的号已经在
    // `DownloadTaskLedgerController::task()` 里了 —— 只问下面那几个谓词的话，这一句会放行，
    // 紧接着 `setTask(beginTask(...))` 把安装那一件顶掉：安装收尾时收的是**这一单**，
    // 它自己永远停在"进行中"（而且下载器那边还会以"已有下载任务"拒掉安装）。
    const auto isBatchBusy = [&owner]() {
        return owner.m_downloadInFlight || owner.m_versionManager->isDownloading()
               || owner.m_versionManager->isLoaderInstalling()
               || owner.m_versionManager->isModpackImporting()
               || owner.m_versionManager->isNamedInstanceInstalling()
               || (owner.m_serverInstall != nullptr && owner.m_serverInstall->ownsDownloadBatch());
    };
    if (isBatchBusy()) {
        InlineMessage::information(
            owner.m_ui->overlay(), MainWindow::tr("暂时无法操作"),
            MainWindow::tr("已有下载或安装任务正在进行，请结束后再下载这个。"));
        return;
    }

    QString confirmedName;
    if (isModpack) {
        const QString titleCopy = displayTitle;
        const ModpackImport::OneClickInstanceName suggested =
            ModpackImport::suggestOneClickInstanceName(titleCopy);
        confirmedName = suggested.name;
        while (true) {
            // 通过 checkImportTargetName 统一校验合法性后再判是否续装，避免非法路径名误查盘。
            const bool resumePrompt =
                InstanceFiles::checkImportTargetName(confirmedName, nullptr, QString())
                == InstanceFiles::ImportNameVerdict::OkResume;
            const QString promptText =
                resumePrompt
                    ? MainWindow::tr("「%1」上次未安装完成，将继续安装，已下载的文件不再重复下载。")
                          .arg(confirmedName)
                    : MainWindow::tr("整合包「%1」将安装到当前游戏目录，请确认实例名称。")
                          .arg(titleCopy);
            bool ok = false;
            const QString input = InlineMessage::getText(
                owner.m_ui->overlay(), MainWindow::tr("安装整合包"), promptText, confirmedName, &ok);
            if (!ok) {
                return;
            }
            confirmedName = input.trimmed();
            QString problem;
            // 下载前尚未拿到压缩包清单指纹，传空指纹放行未完成导入以便后续续装或分流。
            if (InstanceFiles::checkImportTargetName(confirmedName, &problem, QString())
                != InstanceFiles::ImportNameVerdict::Taken) {
                break;
            }
            InlineMessage::warning(owner.m_ui->overlay(), MainWindow::tr("换个名字"), problem);
        }
        if (isBatchBusy()) {
            InlineMessage::information(
                owner.m_ui->overlay(), MainWindow::tr("暂时无法操作"),
                MainWindow::tr("已有下载或安装任务正在进行，请结束后再下载这个。"));
            return;
        }
    }

    const Tasks::TaskId task =
        isModpack ? owner.beginTask(Tasks::TaskKind::Import,
                                    MainWindow::tr("安装整合包 %1").arg(confirmedName),
                                    confirmedName, MainWindowViewData::modpackOneClickSteps())
                  : owner.beginTask(Tasks::TaskKind::Download, label);
    owner.m_downloadTaskLedger->setResourceLabel(label);
    owner.m_downloadTaskLedger->setTask(task);
    // 这一单的登记**必须摆在 `downloadResourceFiles()` 之前**：这批文件**盘上都已经有**时
    // （上次下完了但导入失败 / 被取消，`.mrpack` 还躺在缓存里），`downloadResourceFiles()`
    // 会在它**返回之前**就同步发完 `resourceDownloadFinished(label, true, …)` ——
    // 那时这一句还没执行，没有人接得到这一单，导入永远不开始；界面却照样弹
    // 「已经装进实例里了」。而失败那一支**不** take 记录，只有成功那一支才取
    // （见 `onResourceDownloadFinished()` 里那段 ★），所以提前登记不会漏。
    if (!modpackPath.isEmpty()) {
        owner.m_pendingModpackImports.insert(label, {modpackPath, confirmedName, task});
    }
    const bool started =
        owner.m_versionManager->downloadResourceFiles(destinationDir, instanceId, label, files);
    if (started) {
        // 只有真开起来了才算"这一批的主人"（被拒那一发或已命中缓存的同步完成可能在
        // `downloadResourceFiles()` 返回前已经收过尾并清空 Ledger，故仅在仍是当前任务时更新）。
        if (owner.m_downloadTaskLedger->task() == task) {
            owner.m_downloadTaskLedger->setOwner(task);
            owner.m_activityCenter->tasks()->setProgress(task, 0, 0);
        }
    } else {
        if (!modpackPath.isEmpty()) {
            // 没开起来（撞上上面那道忙闸）：刚登记的那一单作废，留着就是一条永远等不到
            // 下载的记录。
            owner.m_pendingModpackImports.remove(label);
        }
        if (owner.m_downloadTaskLedger->task() == task) {
            owner.m_downloadTaskLedger->clearTask();
            owner.m_downloadTaskLedger->setResourceLabel(QString());
            owner.finishTask(task, false, MainWindow::tr("无法开始下载。"));
        }
    }
}
