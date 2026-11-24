#include "Support.hpp"
#include "ui_check/modules/UiInstanceFilesTestHelpers.hpp"

#include <algorithm>
void runDependencyAndBatchTests() {
    // 那两项设置在盘上的名字。和 T25 一个规矩：这一节量的是"盘上那个键"，
    // 而 `LauncherSettings` 里那两个常量在匿名命名空间里、外面看不见，所以字面量写这儿。
    const QString kAutoIntoKey = QStringLiteral("Download/AutoIntoInstance");
    const QString kWarnKey = QStringLiteral("Download/WarnMissingDependencies");

    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "T31.0 临时数据根目录建出来了");
    if (!root.isValid()) {
        return;
    }

    FakeLookupApi api;
    check(api.start(), "T31.0 假接口服务器起来了");

    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    const QVariant sourceBefore = sandbox.value(QStringLiteral("Download/Source"));
    const QVariant modrinthBefore = sandbox.value(QStringLiteral("Launcher/ModrinthApiBase"));
    const QVariant curseForgeBefore = sandbox.value(QStringLiteral("Launcher/CurseForgeApiBase"));
    const QVariant autoIntoBefore = sandbox.value(kAutoIntoKey);
    const QVariant warnBefore = sandbox.value(kWarnKey);
    const auto restoreSettings = [&] {
        sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
        sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
        sandbox.setValue(QStringLiteral("Download/Source"), sourceBefore);
        sandbox.setValue(QStringLiteral("Launcher/ModrinthApiBase"), modrinthBefore);
        sandbox.setValue(QStringLiteral("Launcher/CurseForgeApiBase"), curseForgeBefore);
        sandbox.setValue(kAutoIntoKey, autoIntoBefore);
        sandbox.setValue(kWarnKey, warnBefore);
        sandbox.sync();
        AppPaths::setMinecraftDir(originalDir);
    };
    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.setValue(QStringLiteral("Download/Source"),
                     LauncherSettings::downloadSourceKey(DownloadSource::Official));
    sandbox.setValue(QStringLiteral("Launcher/ModrinthApiBase"), api.modrinthBase());
    sandbox.setValue(QStringLiteral("Launcher/CurseForgeApiBase"), api.curseForgeBase());
    // 两个开关都**清掉**：这一节第一趟要的就是"从没配过的用户"看到的那一档
    // （「缺失前置提醒」默认**开着**，见 verify 的 I5；这里的第二趟再手动关掉它）。
    sandbox.remove(kAutoIntoKey);
    sandbox.remove(kWarnKey);
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());
    QDir().mkpath(root.path() + QStringLiteral("/libraries"));
    QDir().mkpath(root.path() + QStringLiteral("/assets"));

    // ---------- 夹具：一个实例（Fabric）+ 两个模组文件 ----------
    const QString idV = QStringLiteral("1.95.1");
    const QString alternateId = QStringLiteral("1.95.2");
    const QDir versionsDir(root.path() + QStringLiteral("/versions"));
    makeInstalledVersion(versionsDir, idV);
    makeVersionMeta(versionsDir, idV,
                    QByteArrayLiteral("{\"loaderName\":\"Fabric\",\"loaderVersion\":\"0.15.11\"}"));
    makeInstalledVersion(versionsDir, alternateId);
    makeVersionMeta(versionsDir, alternateId,
                    QByteArrayLiteral("{\"loaderName\":\"Fabric\",\"loaderVersion\":\"0.15.11\"}"));
    const QString dirV = AppPaths::versionDir(idV);

    const QString modJar = dirV + QStringLiteral("/mods/模组丙.jar");
    const QString plainJar = dirV + QStringLiteral("/mods/无关甲.jar");
    writeFileAt(modJar, QByteArrayLiteral("T31-MOD-C"));
    writeFileAt(plainJar, QByteArrayLiteral("T31-PLAIN-A"));
    const QString nestedModDir = dirV + QStringLiteral("/mods/内嵌的文件夹");
    QDir().mkpath(nestedModDir + QStringLiteral("/内层"));
    writeFileAt(nestedModDir + QStringLiteral("/内层/里面的.jar"), QByteArrayLiteral("T31-NESTED"));

    ModpackExport::FileHashes hMod;
    QString hashError;
    const bool hashed = ModpackExport::hashFile(modJar, true, &hMod, &hashError);
    check(hashed, "T31.0 夹具那个模组的 sha1 算出来了（详情按钮要靠它认出来）", hashError);
    if (!hashed) {
        restoreSettings();
        return;
    }

    const QString apiRoot = api.modrinthBase().chopped(3);  // 去掉尾巴上那个 `/v2`
    const QString modProject = QStringLiteral("MODPROJC");
    const QString depProject = QStringLiteral("DEPPROJB");
    const QString modTitle = QStringLiteral("模组丙");
    const QString depTitle = QStringLiteral("前置乙");
    // 两份要下的文件都指向**这台假服务器**：产品会真的发 GET 出去，而这一节要量的正是
    // "前置那一份有没有被排进这一批"（404 不要紧 —— 请求行照样记在 api.requests 里，
    // 而且 404 是立刻回来的，不会把测试吊在那儿等超时）。
    const QString modFileUrl = apiRoot + QStringLiteral("/mod-c.jar");
    const QString depFileUrl = apiRoot + QStringLiteral("/dep-b.jar");

    // `POST /v2/version_files`：哈希 → 项目号（只映射那一个**认得出来**的）。
    api.modrinthReply = QByteArrayLiteral("{\"") + hMod.sha1.toUtf8() +
                        QByteArrayLiteral("\":{\"project_id\":\"") + modProject.toUtf8() +
                        QByteArrayLiteral("\",\"version_number\":\"1.0\",\"files\":[{\"hashes\":{"
                                          "\"sha1\":\"") +
                        hMod.sha1.toUtf8() +
                        QByteArrayLiteral("\",\"sha512\":\"cafebabe\"},\"url\":\"https://cdn."
                                          "modrinth.com/data/mod-c.jar\",\"filename\":\"模组丙.jar\","
                                          "\"size\":10}]}}");
    // `GET /v2/projects?ids=…`：那一份项目对象。**两条都在里面** —— 一条是那个模组自己
    // （详情页顶上那行字要它），一条是它的前置（弹窗名单里那一行要它）。
    api.modrinthProjectsReply =
        QByteArrayLiteral("[{\"id\":\"") + modProject.toUtf8() +
        QByteArrayLiteral("\",\"title\":\"") + modTitle.toUtf8() +
        QByteArrayLiteral("\",\"slug\":\"mod-c\",\"description\":\"一个模组\","
                          "\"game_versions\":[\"1.95.1\"],\"loaders\":[\"fabric\"]},{\"id\":\"") +
        depProject.toUtf8() + QByteArrayLiteral("\",\"title\":\"") + depTitle.toUtf8() +
        QByteArrayLiteral("\",\"slug\":\"dep-b\",\"description\":\"一个前置模组\","
                          "\"game_versions\":[\"1.95.1\"],\"loaders\":[\"fabric\"]}]");
    // 版本列表那一发**按项目号分叉**（见 FakeLookupApi::modrinthVersionListByProject）：
    // 模组那一份里声明了一个必装前置，前置那一份里什么都没有（有的话就成了递归）。
    const auto versionJson = [](const QString &id, const QString &number, const QString &fileName,
                                const QString &url, const QByteArray &dependencies) {
        return QByteArrayLiteral("[{\"id\":\"") + id.toUtf8() +
               QByteArrayLiteral("\",\"version_number\":\"") + number.toUtf8() +
               QByteArrayLiteral("\",\"date_published\":\"2026-09-01T00:00:00Z\","
                                 "\"game_versions\":[\"1.95.1\"],\"loaders\":[\"fabric\"],"
                                 "\"dependencies\":") +
               dependencies +
               QByteArrayLiteral(",\"files\":[{\"hashes\":{\"sha1\":\"aa\",\"sha512\":\"bb\"},"
                                 "\"url\":\"") +
               url.toUtf8() + QByteArrayLiteral("\",\"filename\":\"") + fileName.toUtf8() +
               QByteArrayLiteral("\",\"size\":10,\"primary\":true}]}]");
    };
    api.modrinthVersionListByProject.insert(
        modProject,
        versionJson(QStringLiteral("V1"), QStringLiteral("1.0.0"), QStringLiteral("模组丙.jar"),
                    modFileUrl,
                    // ★ 同一条 required 前置**写了两遍**（R7）。接口允许作者这样写，而按条数
                    // 等回信的一方会永远差一发：产品必须按项目号去重，只问一发、只等一发。
                    // 这一整节后面的断言（弹窗正文逐字比、日志里「含 1 个前置」、两份文件
                    // 都在下）会一起把它钉住 —— 去重丢了的话那一趟根本走不到弹窗。
                    QByteArrayLiteral("[{\"project_id\":\"DEPPROJB\",\"version_id\":null,"
                                      "\"dependency_type\":\"required\"},"
                                      "{\"project_id\":\"DEPPROJB\",\"version_id\":null,"
                                      "\"dependency_type\":\"required\"}]")));
    api.modrinthVersionListByProject.insert(
        depProject,
        versionJson(QStringLiteral("D1"), QStringLiteral("2.0.0"), QStringLiteral("前置乙.jar"),
                    depFileUrl, QByteArrayLiteral("[]")));
    // 兜底：别的项目号来问版本就是空表（这一节只会问上面那两个，写这个是防"某一条走岔了
    // 却拿到别人的版本"那种最难查的夹具事故）。
    api.modrinthVersionListReply = QByteArrayLiteral("[]");
    // CurseForge 那一支也要**答得上话**（每一趟反查都会发一发）：空结果、没有未匹配的。
    api.curseForgeReply =
        QByteArrayLiteral("{\"data\":{\"exactMatches\":[],\"unmatchedFingerprints\":[]}}");

    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    const QString accountName = QStringLiteral("T31User");
    QString offlineError;
    check(AccountStore::addOffline(accountName, &offlineError),
          "T31.0 建一个离线账号并设成当前身份（这一节一个外网请求都不发）", offlineError);

    {
        MainWindow w;
        w.show();
        QApplication::processEvents();

        OverlayHost *overlay = w.ui() != nullptr ? w.ui()->overlay() : nullptr;
        check(overlay != nullptr, "T31.1 覆盖层拿得到（下面每一步都挂在它身上）");

        // 设置 → 下载 里**真的有那一项**（要求「并在设置里面可以关闭这个弹窗」）。
        // 两趟量的都是"这一格管不管事"，所以先钉住它自己：在、默认勾着、文字就是产品那句。
        {
            QCheckBox *box =
                w.ui() != nullptr ? w.ui()->getWarnMissingDependenciesCheck() : nullptr;
            check(box != nullptr && box->isChecked() &&
                      box->text() == MainWindowUI::warnMissingDependenciesCheckText(),
                  "★ T31.1 前提：设置里那一项**在，而且默认是勾着的**"
                  "（用户要的「灰字写可以在设置里面关掉弹窗」，得真有那一格）",
                  box == nullptr ? QStringLiteral("没找到那颗勾选框")
                                 : QStringLiteral("勾着=%1 文字=%2")
                                       .arg(box->isChecked() ? QStringLiteral("是")
                                                             : QStringLiteral("不是"),
                                            box->text()));
        }

        // 某一页上某一条文件的行尾那颗「详情」（按路径找行，不按行号）。
        const auto detailForPath = [](InstanceFilesPage *files,
                                      const QString &path) -> UiRowCellPtr {
            if (files == nullptr || files->list() == nullptr) {
                return nullptr;
            }
            for (int row = 0; row < files->list()->count(); ++row) {
                QListWidgetItem *item = files->list()->item(row);
                if (item != nullptr &&
                    item->data(InstanceFilesPage::pathRole()).toString() == path) {
                    return uiDetailAt(files, row);
                }
            }
            return nullptr;
        };

        // 一趟走完攒下来的读数。
        struct PassResult {
            bool settingsOpened = false;
            bool detailOpened = false;
            int versionRows = 0;
            bool rowClicked = false;
            QStringList promptTitles;      ///< 这一趟弹出来的那几条提示，按先后
            QString depPromptTitle;        ///< 第一条如果是前置弹窗，它说了什么
            QString depPromptText;
            QString depPromptNote;
            QStringList depPromptButtons;
            int promptCount = 0;
            QStringList newRequests;       ///< 这一趟里发出去的新请求行
            QStringList newLogs;           ///< 这一趟里新写出来的日志
            bool done = false;
        };

        int logCursor = messagesSnapshot().size();
        // 跑一趟：实例设置 → 模组 → 详情 → 点某一版 → 等提示 → 收页。
        //
        // @param tag 这一趟的标签（断言里那句人话的前缀）。
        // @param expectDepPrompt 这一趟期不期待那个前置弹窗（决定脚本点哪一颗按钮）。
        // @param expectedPrompts 这一趟一共该弹几条（等不到就超时，断言自己会红）。
        const auto runPass = [&](const QString &tag, bool expectDepPrompt, int expectedPrompts) {
            PassResult r;
            const QStringList requestsBefore = api.requests;
            // 上一趟收掉的那一页是 `deleteLater()` 走的（`DeferredDelete`），
            // `processEvents()` 不递它 —— 不先冲掉的话它会一直**停在覆盖层的停放区**里，
            // 下面 `findChild<InstanceSettingsDialog *>()` 找到的就是这一份已经收掉的旧页
            // （拿它去 `showPage()` / 读行，然后在下一个事件循环里被析构 ⇒ 悬空指针）。
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

            // ① 开实例设置 → 模组那一页 → 等那颗「详情」亮 → 点它。
            //
            // 这一页现在**不阻塞**（`showPage()` 摆上去就返回），所以开完页当场跑这一段；
            // 别做成 0 毫秒定时器 —— 那要等下面 `pumpUntil` 才醒，而 `settingsOpened`
            // 是在那之前读的。
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idV));
            {
                OverlayHost *host = currentPageOverlay();
                auto *page =
                    host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page != nullptr) {
                    r.settingsOpened = true;
                    page->showPage(QStringLiteral("mod"));
                    InstanceFilesPage *mods = page->filesPage(QStringLiteral("mod"));
                    if (mods != nullptr && mods->list() != nullptr) {
                        UiRowCellPtr detail;
                        pumpUntil(
                            [&] {
                                detail = detailForPath(mods, modJar);
                                return detail != nullptr && detail->isEnabled();
                            },
                            15000);
                        if (detail != nullptr) {
                            detail->click();
                        }
                    }
                }
            }
            check(r.settingsOpened, tag + QStringLiteral("：实例设置那一页开起来了"));

            // ② 详情页那一层。产品那一发"开新页"排在队列里（这一页走 `showPage()`，
            // 摆上去就返回），所以量它的动作自己排一发 0 毫秒定时器 —— 产品的定时器
            // 排在前面，等它开完页才轮到这一发。
            //
            // 确认下载目标后，「一起下载」那一趟的按钮脚本是 `{1, 0}`：第 1 条提示（前置）点右边那颗
            // 「一起下载」，第 2 条（下载失败 —— 这一节那两份文件都指向假服务器，
            // 404 是必然的）点「确定」。第二趟（开关关掉）只会弹后者。
            auto *installed = w.ui()->getInstalledVersionCombo();
            installed->setCurrentIndex(installed->findData(idV));
            PromptAnswerer answerer(expectDepPrompt ? QList<int>{1, 0} : QList<int>{0});
            QTimer pickerAnswerer;
            QObject::connect(&pickerAnswerer, &QTimer::timeout, [&] {
                auto *box = currentPrompt();
                if (box == nullptr) return;
                pickerAnswerer.stop();
                const bool isPicker = box->objectName() == MainWindowUI::resourceDownloadInstancePickerName();
                auto *search = box->findChild<QLineEdit *>(QStringLiteral("homePickerSearch"));
                auto *list = box->findChild<QListWidget *>(QStringLiteral("homePickerInstanceList"));
                check(isPicker && list != nullptr && list->currentItem() != nullptr
                          && list->currentItem()->data(Qt::UserRole).toString() == idV,
                      tag + QStringLiteral("：内置选择器默认选中详情页所属实例"));
                answerer.start();
                if (!isPicker) return;
                if (!expectDepPrompt && search != nullptr && list != nullptr) {
                    search->setText(alternateId);
                    check(list->count() == 1 && list->item(0)->data(Qt::UserRole).toString() == alternateId
                              && !box->buttonAt(0)->isEnabled(),
                          tag + QStringLiteral("：搜索能找到另一个实例，隐藏的原目标不能确认"));
                    list->setCurrentRow(0);
                }
                if (box->buttonAt(0)->isEnabled()) {
                    box->buttonAt(0)->click();
                } else {
                    box->reject();
                }
            });
            QTimer::singleShot(0, [&, overlay] {
                auto *detail = w.findChild<ResourceDetailPage *>();
                if (detail != nullptr && overlay->pageOpen()) {
                    r.detailOpened = true;
                    // 版本清单是开页前发出去的那一发，答复在这一层循环里才落地。
                    pumpUntil([detail] { return detail->versionRowCount() > 0; }, 8000);
                    r.versionRows = detail->versionRowCount();
                    QTreeWidget *list = detail->versionList();
                    QTreeWidgetItem *row = nullptr;
                    if (list != nullptr && list->topLevelItemCount() > 0) {
                        QTreeWidgetItem *group = list->topLevelItem(0);
                        if (group != nullptr && group->childCount() > 0) {
                            row = group->child(0);
                        }
                    }
                    if (list != nullptr && row != nullptr) {
                        ++g_modalHandlerGeneration;
                        pickerAnswerer.start(20);
                        list->scrollToItem(row);
                        clickDetailTreeRow(list, list->visualItemRect(row).center());
                        r.rowClicked = true;
                    }
                    pumpUntil([&] { return answerer.answered.size() >= expectedPrompts; }, 20000);
                    // 再给一小段静默期：那两条提示是前后脚来的（前置那条答完，产品才去下
                    // 文件，下失败才弹第二条），只等条数容易在第一发上就跑掉。
                    QElapsedTimer quiet;
                    quiet.start();
                    while (quiet.elapsed() < 1200) {
                        QApplication::processEvents(QEventLoop::AllEvents, 50);
                    }
                    // 收页：返回箭头一点，详情页当场收掉（`showPage()` 摆上去的那种，
                    // 收页是同步的），产品紧接着把实例设置**排到下一轮**再开起来
                    // （见 openFileResourcePage 里那句 `QTimer::singleShot`）——
                    // 所以「再点一下返回把它收掉」这一发必须等它真起来：没开就再排
                    // 自己一轮（和 T24.5 那一段同一个写法）。不等的话这一发会点空，
                    // 那一页开着没人收，整份 mw_check 就停在那层循环里了（一行日志
                    // 都不出）。
                    int backTries = 0;
                    std::function<void()> closeSettings;
                    closeSettings = [&, overlay] {
                        if (!overlay->pageOpen() && ++backTries <= 50) {
                            QTimer::singleShot(0, closeSettings);
                            return;
                        }
                        if (auto *back = overlay->findChild<QPushButton *>(
                                OverlayHost::overlayBackButtonName())) {
                            back->click();
                        }
                        r.done = true;
                    };
                    if (auto *back = overlay->findChild<QPushButton *>(
                            OverlayHost::overlayBackButtonName())) {
                        back->click();
                    }
                    QTimer::singleShot(0, closeSettings);
                } else {
                    r.done = true;
                }
            });
            pumpUntil([&] { return r.done; }, 40000);

            answerer.stop();
            r.promptCount = answerer.answered.size();
            for (const PromptAnswerer::Answered &one : answerer.answered) {
                r.promptTitles.append(one.title);
            }
            if (!answerer.answered.isEmpty()) {
                r.depPromptTitle = answerer.answered.first().title;
                r.depPromptText = answerer.answered.first().text;
                r.depPromptNote = answerer.answered.first().note;
                r.depPromptButtons = answerer.answered.first().buttons;
            }
            for (const QString &line : api.requests) {
                if (!requestsBefore.contains(line)) {
                    r.newRequests.append(line);
                }
            }
            r.newLogs = messagesSnapshot().mid(logCursor);
            logCursor = messagesSnapshot().size();
            return r;
        };

        // ================= 第一趟：开关开着（默认那一档）=================
        PassResult on = runPass(QStringLiteral("T31.1"), true, 2);
        check(on.settingsOpened && on.detailOpened && on.rowClicked,
              "T31.1 前提：实例设置 → 模组 → 详情 → 点某一版，这一整条路真的走到了"
              "（少任何一步，下面量的都是空气）",
              QStringLiteral("设置页=%1 详情页=%2 版本行=%3 点了=%4")
                  .arg(on.settingsOpened ? QStringLiteral("开") : QStringLiteral("没开"),
                       on.detailOpened ? QStringLiteral("开") : QStringLiteral("没开"))
                  .arg(on.versionRows)
                  .arg(on.rowClicked ? QStringLiteral("是") : QStringLiteral("不是")));
        // 这一条是用户那句话的落点：正文 = 判据层那一段（句头 + 一条一行的名单 + 问句）。
        check(on.promptCount >= 1 &&
                  on.depPromptTitle == ModDependencies::dependencyPromptTitle() &&
                  on.depPromptText ==
                      ModDependencies::dependencyPromptText(ResourceCatalog::Kind::Mod,
                                                            {depTitle}),
              "★★ T31.1 弹窗正文**逐字**就是判据层那一段（「此模组缺失必要的前置：」+ 一行一个"
              "名字 + 「要一起下载吗？」）—— 用户原话「弹出 此模组缺失必要的前置：（模组名称，"
              "一行一个」",
              QStringLiteral("标题=%1 正文=%2").arg(on.depPromptTitle, on.depPromptText));
        // 灰字：要求「弹窗里灰字写可以在设置里面关掉弹窗」。**逐字**比，不 contains。
        check(on.depPromptNote == ModDependencies::dependencyPromptNoteText() &&
                  !on.depPromptNote.isEmpty(),
              "★★ T31.1 弹窗里那行**灰字**就是「可以在设置里关掉」那句（唯一出处 "
              "ModDependencies::dependencyPromptNoteText()）",
              on.depPromptNote);
        // 两颗按钮：左「只下这个」（默认）右「一起下载」—— 顺序反了的话，误按回车
        // 就替用户多做了一件事。
        check(on.depPromptButtons.size() == 2 &&
                  on.depPromptButtons.at(0) == ModDependencies::dependencyOnlyThisButtonText() &&
                  on.depPromptButtons.at(1) == ModDependencies::dependencyAllButtonText(),
              "★ T31.1 两颗按钮是「只下这个」「一起下载」（顺序 = 左边那颗是「不」，默认落它上头）",
              on.depPromptButtons.join(QStringLiteral(" / ")));

        // 「一起下载」按下去之后：前置那一份**真的被排进了这一批**（它的地址出现在请求里）。
        bool askedDepProject = false;
        bool gotDepFile = false;
        bool gotModFile = false;
        for (const QString &line : on.newRequests) {
            if (line.contains(QStringLiteral("/v2/projects?")) && line.contains(depProject)) {
                askedDepProject = true;
            }
            if (line.contains(QStringLiteral("/dep-b.jar"))) {
                gotDepFile = true;
            }
            if (line.contains(QStringLiteral("/mod-c.jar"))) {
                gotModFile = true;
            }
        }
        check(askedDepProject,
              "★★ T31.1 点完那一版之后，产品**真的去问过**那条前置「你有哪些版本」"
              "（`GET /v2/projects?ids=DEPPROJB`）—— 这一发是「要不要一起下」那条路的第一步",
              on.newRequests.join(QStringLiteral(" | ")).left(200));
        check(gotModFile && gotDepFile,
              "★★ T31.1 按了「一起下载」⇒ 这一批里**两份都在下**（那个模组 + 它的前置）"
              "—— 前置那一份没排进去的话，用户看到的是「下好了」，而进游戏那一下就崩",
              QStringLiteral("模组那份=%1 前置那份=%2")
                  .arg(gotModFile ? QStringLiteral("在") : QStringLiteral("不在"),
                       gotDepFile ? QStringLiteral("在") : QStringLiteral("不在")));
        // 「落进**那一个**实例」：产品自己写的那行日志里带着这个实例的 mods 目录。
        const QString destNative = QDir::toNativeSeparators(AppPaths::versionDir(idV) +
                                                            QStringLiteral("/mods"));
        bool landedInInstance = false;
        for (const QString &line : on.newLogs) {
            if (line.contains(destNative) && line.contains(QStringLiteral("含 1 个前置"))) {
                landedInInstance = true;
            }
        }
        check(landedInInstance,
              "★★ T31.1 而且下进的是**这个实例的 mods/**（日志里那行带着它的绝对路径），"
              "前置也落在同一个目录",
              destNative);

        // ================= 第二趟：设置里关掉之后 =================
        //
        // 关的方式走**用户那条路**（那颗勾选框），不是直接往 QSettings 里写：产品那一份
        // `m_settings` 是构造时读进内存的，外面改盘它一个字都不知道 —— 直接写键的话，
        // 这一趟量到的还是"开着"的那一套，而红的是别处（最难查的那种）。
        QCheckBox *warnBox =
            w.ui() != nullptr ? w.ui()->getWarnMissingDependenciesCheck() : nullptr;
        if (warnBox != nullptr) {
            warnBox->setChecked(false);
            QApplication::processEvents();
        }
        {
            LauncherSettings reread;
            check(warnBox != nullptr && !warnBox->isChecked() && !reread.warnMissingDependencies(),
                  "★ T31.2 前提：在设置里把那一项**关掉了**，而且真的落了盘"
                  "（这一下走的是用户那条路：那颗勾选框）",
                  warnBox == nullptr ? QStringLiteral("没找到那颗勾选框")
                                     : QStringLiteral("勾着=%1 盘上=%2")
                                           .arg(warnBox->isChecked() ? QStringLiteral("是")
                                                                     : QStringLiteral("不是"),
                                                reread.warnMissingDependencies()
                                                    ? QStringLiteral("开着")
                                                    : QStringLiteral("关着")));
        }
        PassResult off = runPass(QStringLiteral("T31.2"), false, 1);
        const QString alternateMods =
            QDir::toNativeSeparators(AppPaths::versionDir(alternateId) + QStringLiteral("/mods"));
        check(std::any_of(off.newLogs.cbegin(), off.newLogs.cend(), [&](const QString &line) {
                  return line.contains(QStringLiteral("正在下载")) && line.contains(alternateMods);
              }) && w.ui()->getInstalledVersionCombo()->currentData().toString() == idV,
              "T31.2 改选只修改本次安装落点，主页当前实例不变");
        // 「关掉 = 什么都不装」：那一发问名字的请求**压根没发**。这是这条路上唯一一次
        // "要不要一起下"的问句，它没发出去就说明整段被跳过了（不是"问了但没下"）。
        bool askedDepProjectOff = false;
        bool gotDepFileOff = false;
        for (const QString &line : off.newRequests) {
            if (line.contains(QStringLiteral("/v2/projects?")) && line.contains(depProject)) {
                askedDepProjectOff = true;
            }
            if (line.contains(QStringLiteral("/dep-b.jar"))) {
                gotDepFileOff = true;
            }
        }
        check(!askedDepProjectOff,
              "★★ T31.2 设置里关掉之后，那一发前置查询**一条都不发**"
              "（用户确认过：关掉 = 只下他点的那一版）",
              off.newRequests.join(QStringLiteral(" | ")).left(200));
        check(!gotDepFileOff,
              "★★ T31.2 也就**没有装前置**（那一份文件根本没被要求下）");
        check(off.promptTitles.isEmpty() ||
                  !off.promptTitles.contains(ModDependencies::dependencyPromptTitle()),
              "★★ T31.2 而且那个前置弹窗**一次都不弹**（屏幕上只有「下失败了」那种）",
              off.promptTitles.join(QStringLiteral(" / ")));

        // ================= 第三趟：批量 禁用 / 启用 / 删除 真改盘 =================
        const QString disabledJar = modJar + InstanceFilesPage::disabledSuffix();
        const QString disabledPlain = plainJar + InstanceFilesPage::disabledSuffix();
        struct BatchProbe {
            bool ran = false;
            bool rowsFound = false;
            bool barShown = false;
            bool afterDisableRelisted = false;
            // 盘上的读数**必须在这一发里面记下来**：这三下是**前后串着**做的
            // （禁用 → 启用 → 删除），等整段跑完再到外面去读盘，读到的已经是
            // "启用回来之后 + 删掉一个"那个终态 —— 禁用那一条永远看不见自己的现场。
            bool disabledBoth = false;   ///< 点完「禁用」：两条都挂上了后缀
            bool reenabledBoth = false;  ///< 点完「启用」：又都改回原名了
            bool plainGone = false;      ///< 点完「删除」：选中的那一份没了
            bool modKept = false;        ///< 而没选中的那一份还在
            bool nestedKept = false;     ///< ★ 拿一个**目录**走「禁用」：盘上原样不动
            bool nestedSaid = false;     ///< ★ 而且当场说了一句人话（不是静默什么都不做）
        };
        BatchProbe batch;
        {
            // 删除那一颗会弹「真的要删吗」那个框（产品对整批**只问一次**）：点左边那颗「删除」。
            PromptAnswerer deleter(QList<int>{0});
            // 这一页现在**不阻塞**（`showPage()`）：开完页当场跑这一段。
            // 先把上一趟收页留下的 `DeferredDelete` 冲掉 —— 理由同 runPass 开头那一段：
            // 不然 `findChild()` 会先把那份已经收掉的旧页交回来，而它会在下一个事件循环
            // 里被析构（`mods` 跟着悬空）。
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            const auto runBatch = [&] {
                OverlayHost *host = currentPageOverlay();
                auto *page =
                    host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
                if (page == nullptr) {
                    return;
                }
                page->showPage(QStringLiteral("mod"));
                InstanceFilesPage *mods = page->filesPage(QStringLiteral("mod"));
                if (mods == nullptr || mods->list() == nullptr) {
                    return;
                }
                batch.rowsFound = pumpUntil([mods] { return mods->list()->count() >= 2; }, 8000);
                // 按路径选中（"选中"这件事在屏幕上就是那一条的选中态，产品按路径记账）。
                // 先把旧的选择**清干净**再选 —— 这个 helper 三个调用点的意思都是
                // 「把选择设成这几条」，而 `setSelected(true)` 只会**加**不会减。
                // 为什么这里必须先清：产品在拨开关时**不**取消选中 —— 那两条会
                // **留在选中**里；不清的话这一发等于"两条都选着"，删除会把 modJar 也
                // 带走，而这条断言的**本意**是"只删选中的那一条"。
                const auto selectByPaths = [&](const QStringList &paths) {
                    mods->list()->clearSelection();
                    for (int row = 0; row < mods->list()->count(); ++row) {
                        QListWidgetItem *item = mods->list()->item(row);
                        if (item != nullptr &&
                            paths.contains(
                                item->data(InstanceFilesPage::pathRole()).toString())) {
                            item->setSelected(true);
                        }
                    }
                    QApplication::processEvents();
                };
                selectByPaths({modJar, plainJar});
                QWidget *bar = mods->selectionBar();
                batch.barShown = bar != nullptr && !bar->isHidden();
                if (QPushButton *b = mods->selectionButtonAt(
                        static_cast<int>(InstanceFilesPage::SelectionAction::Disable))) {
                    b->click();
                }
                QApplication::processEvents();
                // 现场在**这一句**上：三下是串着做的，等整段跑完再读盘读到的已经是终态。
                batch.disabledBoth = QFile::exists(disabledJar) && QFile::exists(disabledPlain);
                // 改完名产品会**重列**那一页（`refreshFilePages()`）⇒ 路径已经带上后缀了。
                batch.afterDisableRelisted = mods->list()->count() >= 2;
                selectByPaths({disabledJar, disabledPlain});
                if (QPushButton *b = mods->selectionButtonAt(
                        static_cast<int>(InstanceFilesPage::SelectionAction::Enable))) {
                    b->click();
                }
                QApplication::processEvents();
                batch.reenabledBoth = QFile::exists(modJar) && QFile::exists(plainJar) &&
                                      !QFile::exists(disabledJar) &&
                                      !QFile::exists(disabledPlain);
                // 删掉那个"无关"的（留下认得出资源的那一个，前面的断言还认它）。
                deleter.start();
                selectByPaths({plainJar});
                if (QPushButton *b = mods->selectionButtonAt(
                        static_cast<int>(InstanceFilesPage::SelectionAction::Delete))) {
                    b->click();
                }
                pumpUntil([&] { return !deleter.answered.isEmpty(); }, 8000);
                deleter.stop();
                batch.plainGone = !QFile::exists(plainJar);
                batch.modKept = QFile::exists(modJar);
                // ---- ★★ ：**目录不许被"禁用"**（用户点名的那一幕）----
                //
                // 走的是悬浮条上那颗「禁用」真正走的那条路（`batchToggleRequested` →
                // 对话框转发 → `MainWindow::toggleInstanceFiles()` → 逐条
                // `toggleInstanceFile()`）。喂进去的是一个**目录** —— 目录不许被改名成
                // `.connector.disabled`（真机上发生过）。这里只该说一句人话，一个字节都
                // 不动盘。整批都不改的话 `toggleInstanceFiles()` 末尾那个「改动失败」框
                // 就是这一句话的出口。
                const int saidBefore = deleter.answered.size();
                if (InstanceFilesPage *dirPage = page->filesPage(QStringLiteral("mod"))) {
                    // 这一发**自己带一个 answerer** —— 上面那个 `deleter` 已经 `stop()`
                    // 过了（它只管"删除"那一问），而整批都没改成的话产品会在末尾弹一个
                    // 「改动失败」的框；没有 answerer 在跑的话那是个嵌套事件循环，测试就
                    // 挂在那儿等一个永远没人点的框。
                    PromptAnswerer nestedAnswerer({0});
                    nestedAnswerer.start();
                    dirPage->batchToggleRequested({nestedModDir}, true);
                    QApplication::processEvents();
                    for (auto &one : nestedAnswerer.answered) {
                        deleter.answered.append(one);
                    }
                    nestedAnswerer.stop();
                }
                batch.nestedKept =
                    QFileInfo(nestedModDir).isDir() &&
                    !QFileInfo(nestedModDir + InstanceFilesPage::disabledSuffix()).exists() &&
                    QFile::exists(nestedModDir + QStringLiteral("/内层/里面的.jar"));
                for (int i = saidBefore; i < deleter.answered.size(); ++i) {
                    // 「文件夹」那两个字是这一句话唯一的记号（产品那句是
                    // 「「…」是一个文件夹，不能启用 / 禁用。」）。
                    if (deleter.answered.at(i).text.contains(QStringLiteral("文件夹"))) {
                        batch.nestedSaid = true;
                    }
                }
                if (auto *back =
                        overlay->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();
                }
                batch.ran = true;
            };
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idV));
            runBatch();
        }
        check(batch.ran && batch.rowsFound && batch.afterDisableRelisted,
              "T31.3 前提：模组那一页上那两个文件都在，改完名字之后那一页重列了",
              QStringLiteral("跑了=%1 行=%2 重列=%3")
                  .arg(batch.ran ? QStringLiteral("是") : QStringLiteral("不是"),
                       batch.rowsFound ? QStringLiteral("有") : QStringLiteral("没有"),
                       batch.afterDisableRelisted ? QStringLiteral("是") : QStringLiteral("不是")));
        check(batch.barShown,
              "T31.3 前提：选中两条之后那条悬浮操作条露出来了（灰化规则在 ui_check 里量）");
        check(batch.disabledBoth,
              "★★ T31.3 点「禁用」⇒ 盘上那两条都挂上了 `.disabled`"
              "（用户原话「在选中任意数量模组时…显示几个按钮：更新 禁用 启用 全选 反选 取消 删除」）");
        check(batch.reenabledBoth,
              "★★ T31.3 再点「启用」⇒ 又改回来了（那一批里两条都按同一个方向走）");
        check(batch.plainGone && batch.modKept,
              "★★ T31.3 点「删除」⇒ 那一份真从盘上没了，而**没选中的那个一个字节都没动**"
              "（批量删就是批量删，不是把这一页清空）",
              QStringLiteral("选中的那份没了=%1 没选中的那份还在=%2")
                  .arg(batch.plainGone ? QStringLiteral("是") : QStringLiteral("**不是**"),
                       batch.modKept ? QStringLiteral("是") : QStringLiteral("**不是（被连坐了）**")));
        // 要求：「游戏模组文件夹里套的文件夹也会被列出来，而禁用对其的作用只是改个
        // 名」。真机上 `mods/.connector` 就是这么被改成 `.connector.disabled` 的 ——
        // 里面的 jar 一起从游戏眼里消失。现在这一发只说话不动手。
        check(batch.nestedKept && batch.nestedSaid,
              "★★ T31.3 拿一个**目录**走「禁用」：盘上**一个字节都没动**（名字没加后缀、"
              "里面的东西照旧），而且当场说了一句人话 —— 用户原话：「游戏模组文件夹里套的"
              "文件夹也会被列出来，而禁用对其的作用只是改个名」",
              QStringLiteral("目录原样还在=%1 说了那句话=%2")
                  .arg(batch.nestedKept ? QStringLiteral("是") : QStringLiteral("**不是**"),
                       batch.nestedSaid ? QStringLiteral("是") : QStringLiteral("**不是**")));
    }

    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
          "T31.4 这一节加的账号清干净了");

    restoreSettings();
    check(AppPaths::minecraftDir() == originalDir, "T31.4 数据根目录换回来了");
}

