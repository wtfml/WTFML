#include "Support.hpp"
void runInstanceUpdateFilterTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "T28.0 临时数据根目录建出来了");
    if (!root.isValid()) {
        return;
    }

    FakeLookupApi api;
    check(api.start(), "T28.0 假接口服务器起来了");

    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    const QVariant sourceBefore = sandbox.value(QStringLiteral("Download/Source"));
    const QVariant modrinthBefore = sandbox.value(QStringLiteral("Launcher/ModrinthApiBase"));
    const QVariant curseForgeBefore = sandbox.value(QStringLiteral("Launcher/CurseForgeApiBase"));
    // 「上次启动的版本」也要记着还（T19/T20 同一条规矩）：MainWindow 一起窗就会把盘上那一格
    // 写回去，留着的话后一节开窗选中的就不是它自己的夹具了。
    const QString lastVersionBefore = LauncherSettings().lastLaunchedVersion();
    const auto restoreSettings = [&] {
        sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
        sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
        sandbox.setValue(QStringLiteral("Download/Source"), sourceBefore);
        sandbox.setValue(QStringLiteral("Launcher/ModrinthApiBase"), modrinthBefore);
        sandbox.setValue(QStringLiteral("Launcher/CurseForgeApiBase"), curseForgeBefore);
        sandbox.sync();
        LauncherSettings().setLastLaunchedVersion(lastVersionBefore);
        AppPaths::setMinecraftDir(originalDir);
    };
    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.setValue(QStringLiteral("Download/Source"),
                     LauncherSettings::downloadSourceKey(DownloadSource::Official));
    sandbox.setValue(QStringLiteral("Launcher/ModrinthApiBase"), api.modrinthBase());
    sandbox.setValue(QStringLiteral("Launcher/CurseForgeApiBase"), api.curseForgeBase());
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());
    QDir().mkpath(root.path() + QStringLiteral("/libraries"));
    QDir().mkpath(root.path() + QStringLiteral("/assets"));

    // 一个报得出游戏版本号和加载器的实例：那一趟版本列表就是按这两样问的（和 T24 同一个
    // 手法）。报不出东西的实例量不出来 —— 问出去的 URL 里那两段是空的，而"筛子空了"和
    // "接线断了"在屏幕上是一个样子。
    const QString idV = QStringLiteral("1.96.3");
    const QDir versionsDir(root.path() + QStringLiteral("/versions"));
    makeInstalledVersion(versionsDir, idV);
    makeVersionMeta(versionsDir, idV,
                    QByteArrayLiteral("{\"loaderName\":\"Fabric\",\"loaderVersion\":\"0.15.11\"}"));
    const QString dirV = AppPaths::versionDir(idV);

    // 两个文件：① 认得出的模组（⇒ 可更新），② 认不出来的那个（⇒ **不许**出现在可更新里 ——
    // "说不清"和"有新版本"是两件事，见 ModDependencies::judgeUpdate）。
    const QString modJar = dirV + QStringLiteral("/mods/模组乙.jar");
    const QString unknownJar = dirV + QStringLiteral("/mods/没认出来的.jar");
    writeFileAt(modJar, QByteArrayLiteral("T28-MOD-B"));
    writeFileAt(unknownJar, QByteArrayLiteral("T28-NOBODY"));
    // ③ 一条**禁用着**的（★★ ）：内容和第①条**逐字节一样** ⇒ 同一个 sha1 ⇒
    // 同一个项目 ⇒ 真去查的话它照样判得出"可更新"。它的用处只有一个：量"这一趟**整个
    // 跳过**了它"（要求「检查更新时跳过它」）。
    // 拿**一样的字节**是有意的：这样"没查它"和"查了、没认出来"才分得开 —— 认不出来的
    // 那一条（第②个文件）即便查了也不会进名单，拿它当判据是**假绿**。
    const QString disabledJar = dirV + QStringLiteral("/mods/模组丁.jar.disabled");
    writeFileAt(disabledJar, QByteArrayLiteral("T28-MOD-B"));

    ModpackExport::FileHashes hMod;
    QString hashError;
    const bool hashed = ModpackExport::hashFile(modJar, true, &hMod, &hashError);
    check(hashed, "T28.0 那个模组的 sha1 算出来了（用产品自己那份实现，这一节验的不是哈希）",
          hashError);
    if (!hashed) {
        restoreSettings();
        return;
    }

    const QString project = QStringLiteral("UPDPROJ");
    const QString modTitle = QStringLiteral("模组乙");
    // `POST /v2/version_files`：哈希 → 项目号。**只映射认得出的那一个**（第二个文件不在里面
    // —— 它就是"认不出来"那一档）。
    api.modrinthReply = QByteArrayLiteral("{\"") + hMod.sha1.toUtf8() +
                        QByteArrayLiteral("\":{\"project_id\":\"") + project.toUtf8() +
                        QByteArrayLiteral("\",\"version_number\":\"1.0\",\"files\":[{\"hashes\":{"
                                          "\"sha1\":\"") +
                        hMod.sha1.toUtf8() +
                        QByteArrayLiteral("\",\"sha512\":\"cafebabe\"},\"url\":\"https://cdn."
                                          "modrinth.com/data/mod-b.jar\",\"filename\":"
                                          "\"模组乙.jar\",\"size\":10}]}}");
    api.modrinthProjectsReply =
        QByteArrayLiteral("[{\"id\":\"") + project.toUtf8() +
        QByteArrayLiteral("\",\"title\":\"") + modTitle.toUtf8() +
        QByteArrayLiteral("\",\"slug\":\"mod-b\",\"description\":\"一个模组\","
                          "\"game_versions\":[\"1.96.3\"],\"loaders\":[\"fabric\"]}]");
    // `GET /v2/project/<项目号>/version`：**两条**版本，缺一条都判不出"可更新"。
    //
    // `judgeUpdate()` 的判法是"盘上这一份的 sha1 在列表里排第几"：**排在 0 才是最新的**，
    // 排在后面 = 有更新。所以这里必须：
    // · 第一条（最新那条）的文件 sha1 和盘上那个**不一样**（它才是"新的那一版"）；
    // · 第二条的 sha1 **正好是盘上那个**（= judgeUpdate 认得出盘上这份是 1.0）。
    // 只有第一条 ⇒ 判出来是"说不清"（这一版不在列表里）；只有第二条 ⇒ 判出来是"已是最新"。
    // 两种都**不会有**"可更新"，而屏幕上的样子和"功能没做"一模一样。
    api.modrinthVersionListReply = QByteArrayLiteral(
        "[{\"id\":\"VNEW\",\"version_number\":\"2.0\",\"date_published\":\"2026-09-01T00:00:00Z\","
        "\"game_versions\":[\"1.96.3\"],\"loaders\":[\"fabric\"],\"files\":[{\"hashes\":{"
        "\"sha1\":\"1111111111111111111111111111111111111111\",\"sha512\":\"aa\"},"
        "\"url\":\"https://cdn.modrinth.com/data/mod-b-2.0.jar\",\"filename\":"
        "\"模组乙-2.0.jar\",\"size\":10,\"primary\":true}]},"
        "{\"id\":\"VOLD\",\"version_number\":\"1.0\",\"date_published\":\"2025-01-01T00:00:00Z\","
        "\"game_versions\":[\"1.96.3\"],\"loaders\":[\"fabric\"],\"files\":[{\"hashes\":{"
        "\"sha1\":\"") +
        hMod.sha1.toUtf8() +
        QByteArrayLiteral("\",\"sha512\":\"bb\"},\"url\":\"https://cdn.modrinth.com/data/"
                          "mod-b.jar\",\"filename\":\"模组乙.jar\",\"size\":10}]}]");
    api.modrinthVersionFilesUpdateReply =
        QByteArrayLiteral("{\"") + hMod.sha1.toUtf8() +
        QByteArrayLiteral("\":{\"id\":\"VNEW\",\"version_number\":\"2.0\","
                          "\"date_published\":\"2026-09-01T00:00:00Z\","
                          "\"game_versions\":[\"1.96.3\"],\"loaders\":[\"fabric\"],"
                          "\"files\":[{\"hashes\":{"
                          "\"sha1\":\"1111111111111111111111111111111111111111\","
                          "\"sha512\":\"aa\"},"
                          "\"url\":\"https://cdn.modrinth.com/data/mod-b-2.0.jar\","
                          "\"filename\":\"模组乙-2.0.jar\",\"size\":10,\"primary\":true}]}}");
    // CurseForge 那一支也要答得上话（那一趟反查每次都会发指纹那一发）：空结果、没有未匹配的。
    api.curseForgeReply =
        QByteArrayLiteral("{\"data\":{\"exactMatches\":[],\"unmatchedFingerprints\":[]}}");

    // T28.5 要真下一次新版：地址换成本机替身。先让它给错内容（sha1 对不上 = 更新失败）。
    const QByteArray newJar = QByteArrayLiteral("T28-MOD-B-2.0");
    QByteArray served = QByteArrayLiteral("T28-BROKEN!!!");
    QTcpServer cdn;
    check(cdn.listen(QHostAddress::LocalHost, 0), "T28.0 下载替身起来了");
    QObject::connect(&cdn, &QTcpServer::newConnection, &cdn, [&cdn, &served] {
        while (QTcpSocket *socket = cdn.nextPendingConnection()) {
            socket->setParent(&cdn);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &served] {
                socket->readAll();
                socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " +
                              QByteArray::number(served.size()) + "\r\n\r\n" + served);
                socket->disconnectFromHost();
            });
        }
    });
    api.modrinthVersionFilesUpdateReply
        .replace("https://cdn.modrinth.com/data/mod-b-2.0.jar",
                 QStringLiteral("http://127.0.0.1:%1/b.jar").arg(cdn.serverPort()).toUtf8())
        .replace("1111111111111111111111111111111111111111",
                 QCryptographicHash::hash(newJar, QCryptographicHash::Sha1).toHex())
        .replace("\"size\":10", "\"size\":" + QByteArray::number(newJar.size()));

    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    const QString accountName = QStringLiteral("T28User");
    QString offlineError;
    check(AccountStore::addOffline(accountName, &offlineError),
          "T28.0 建一个离线账号并设成当前身份（这一节一个外网请求都不发）", offlineError);

    {
        MainWindow w;
        w.show();
        QApplication::processEvents();
        OverlayHost *overlay = w.ui() != nullptr ? w.ui()->overlay() : nullptr;
        check(overlay != nullptr, "T28.1 覆盖层拿得到（那几页都开在它上面）");
        if (overlay == nullptr) {
            restoreSettings();
            return;
        }

        // 这一趟的结论**不再弹一段文本**，而是开成一整页（「更新
        // 中心」）—— 和 T32 那一节同一个改动。那一页开出来的时候，**实例设置那一页会让位**
        // （`runWhenOverlayFree()` 静默收掉它，见 MainWindow 那段），所以"查完之后这一页
        // 长什么样"必须在**重新开一次**实例设置里量（下面第二趟）。
        bool checkedFound = false;
        int comboCount = -1;
        int updateRows = -1;
        QString updateRowPath;
        QString updateArrowGlyph;
        bool updateChipHiddenBefore = false;
        QString updateChipTextBefore;
        bool updateChipHiddenAfter = true;
        // 「这一条有新版本」那颗 ↑ 的账（★★ ）：按路径记"名字那一格的尾巴上有没有
        // 它"、那一格的**字号**，以及名字划没划掉。**它就是名字那一格字符串的尾巴**
        // （见 InstanceFilesPage::nameTextWithUpdateMark()），所以"和名字一个字号"、
        // "紧挨着名字"这两条在这一层是结构保证的 —— 这里量的是"有没有"。
        QHash<QString, bool> arrowOnPath;
        QHash<QString, int> namePointSize;
        QSet<QString> struckPaths;

        // 第一趟：开实例设置 → 量"还没查过"那一档 → 点「检查更新」→ **立刻收掉实例设置**
        // → 回到底下那一层 pump 到更新中心那一页开出来。
        //
        // 顺序是这一节的形状，不是随手的：那一趟结果回来时要开的新页**只能在覆盖层空着
        // 的时候开**（`runWhenOverlayFree()`），而实例设置页现在**不阻塞**（`showPage()`）——
        // 点完按钮马上按返回箭头把它收掉，让"什么时候覆盖层空了"这件事在**外面**这一层
        // 数得清。反过来在这一页还开着的时候等新页，等到的只会是
        // `runWhenOverlayFree()` 轮数用完之后弹的「先关掉现在这一页」。
        // 只认「更新中心」那一页（见 `PageCloser::setOnlyTitle()`）：不拦着的话计时器会先
        // 把**实例设置**记一条、顺手点掉（它比新页先开出来）。
        int centerPageCount = -1;
        QString centerPageTitle;
        PageCloser closer;
        closer.setOnlyTitle(UpdateCenterPage::windowTitle());
        closer.start();
        QTimer::singleShot(0, [&] {
            OverlayHost *host = currentPageOverlay();
            auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
            if (page == nullptr) {
                return;
            }
            // ① 先量"还没查过"那一档。
            // 收尾：还没查过 ⇒ 那一颗**整颗收着**，不露、不写条数、点它也不顺手查一趟 ——
            // "哪一条有更新"由**那一行自己的 ↑** 说（见下面 ③ 里那几条）。
            InstanceFilesPage *mods = page->filesPage(QStringLiteral("mod"));
            page->showPage(QStringLiteral("mod"));
            QPushButton *state = mods != nullptr
                                     ? mods->stateButtonAt(InstanceFilesPage::StateFilter::Update)
                                     : nullptr;
            if (mods != nullptr && state != nullptr) {
                updateChipHiddenBefore = state->isHidden();
                updateChipTextBefore = state->text();
            }
            // ② 点那颗按钮。**它自己不会阻塞** —— 反查在后台线程上、版本列表那几发是异步的，
            // 结果要等下面那个 pumpUntil 转着事件循环才回来。
            page->showPage(QStringLiteral("mod"));
            checkedFound = true;
            mods = page->filesPage(QStringLiteral("mod"));
            check(pumpUntil([mods] { return mods && mods->updateChecked(); }, 20000),
                  "background update check finishes without navigation");
            check(currentPageOverlay() == host && host->findChild<InstanceSettingsDialog *>(),
                  "background result preserves the active instance page");
            QMetaObject::invokeMethod(page, "updateCenterRequested", Qt::DirectConnection);

            // 收页（返回箭头 = reject）。**必须立刻收**（理由见上面那段）：这一层不退出去，
            // 结果回来时那一页开不出来。
            if (auto *back =
                    host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                back->click();
            } else {
                host->closeOverlay();
            }
        });
        QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                  Q_ARG(QString, idV));
        QApplication::processEvents();
        // 这一层现在空着 ⇒ 那一趟结果回来时自己开出「更新中心」，`closer` 记一条再收掉它。
        pumpUntil([&closer] { return !closer.seen.isEmpty(); }, 20000);
        closer.stop();
        centerPageCount = closer.seen.size();
        centerPageTitle = closer.seen.isEmpty() ? QString() : closer.seen.first().title;
        // 收完之后覆盖层该整个空下来。
        pumpUntil([] { return currentOverlay() == nullptr; }, 8000);

        // 第二趟：重新开一次实例设置，量"查完之后这一页长什么样"。
        //
        // 这一趟之所以量得到，是因为那一趟的结论**留在数据里**（不是活在那一个框里）——
        // 这本来就是本节 T28.4 那条判据的立足点，现在只是**多关了一次**。
        QTimer::singleShot(0, [&] {
            OverlayHost *host = currentPageOverlay();
            auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
            if (page == nullptr) {
                return;
            }
            InstanceFilesPage *mods = page->filesPage(QStringLiteral("mod"));
            page->showPage(QStringLiteral("mod"));
            QPushButton *state = mods != nullptr
                                     ? mods->stateButtonAt(InstanceFilesPage::StateFilter::Update)
                                     : nullptr;
            if (mods != nullptr && state != nullptr) {
                comboCount = mods->canFilterUpdates() ? 4 : 3;
                updateChipHiddenAfter = state->isHidden();
                // ③-a 先按「全部」把这一页三行都量一遍（那一颗筛子还没点下去，列表是整份的）：
                // 哪一行有 ↑、它的字号和名字比怎么样、哪一行的名字被划掉了。
                if (mods->list()->count() == 3) {
                    for (int row = 0; row < mods->list()->count(); ++row) {
                        const QString rowPath =
                            mods->list()
                                ->item(row)
                                ->data(InstanceFilesPage::pathRole())
                                .toString();
                        // 行是代理画的：名字在条目的 AccessibleTextRole 上，字号就是列表的字号
                        // （代理拿列表字体画名字），关着的那条划删除线（判据是 disabledRole）。
                        QListWidgetItem *item = mods->list()->item(row);
                        const QString name = item->data(Qt::AccessibleTextRole).toString();
                        arrowOnPath.insert(
                            rowPath, name.endsWith(InstanceFilesPage::updateArrowText()));
                        namePointSize.insert(rowPath, mods->list()->font().pointSize());
                        if (item->data(InstanceFilesPage::disabledRole()).toBool()) {
                            struckPaths.insert(rowPath);
                        }
                    }
                }
                // ③-b 点它那一发留在这儿：查完之后那一颗**带上条数**（见上面 ① 那段），点下去
                // 列表**只剩有新版本的那几条** —— 下面 T28.2 量的正是这个。
                if (!state->isHidden()) {
                    state->click();
                }
                updateRows = mods->list()->count();
                if (updateRows > 0) {
                    updateRowPath = mods->list()
                                        ->item(0)
                                        ->data(InstanceFilesPage::pathRole())
                                        .toString();
                }
                // 这一刻列表停在「可更新」上 ⇒ 只剩那一行，它的名字后面该缀着那个 ↑。
                if (QListWidgetItem *first = mods->list()->item(0)) {
                    updateArrowGlyph = first->data(Qt::AccessibleTextRole).toString();
                }
            }
            // 收页（返回箭头 = reject）。
            if (auto *back =
                    host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                back->click();
            } else {
                host->closeOverlay();
            }
        });
        QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                  Q_ARG(QString, idV));
        QApplication::processEvents();

        check(checkedFound, "T28.1 进入模组页自动检查更新");
        check(updateChipHiddenBefore,
              "★★ T28.2 还没查过的时候，「可更新」那一颗**整颗收着**（原话："
              "「没有检查的时候先隐藏可更新按钮」"
              "）",
              QStringLiteral("那一颗藏没藏：%1").arg(int(updateChipHiddenBefore)));
        // ⚠ 藏起来的那一颗**照样写着条数**（`stateFilterButtonText()` = 那三个字 + `(0)`）：
        // 条数在 fillStateButtons() 里是**一句话一种写法**，有没有依据这一层归显隐那条
        // 判据管（没依据的时候整颗不摆，屏幕上也就读不到这个 0）。这一条钉的是"别在这儿
        // 再开第二种写法"—— 上午那一版就是在这儿多写了一个分支。
        check(updateChipTextBefore ==
                  InstanceFilesPage::stateFilterButtonText(
                      InstanceFilesPage::StateFilter::Update, 0),
              "★ T28.2 还没查过的时候那一颗写的仍是「可更新(0)」（唯一出处 "
              "stateFilterButtonText()）—— 它是**收着的**，屏幕上读不到这个数；"
              "「别写条数」那件事归显隐那条判据，不在这儿再开一个分支",
              QStringLiteral("那一颗写的字：%1（该是 %2）")
                  .arg(updateChipTextBefore,
                       InstanceFilesPage::stateFilterButtonText(
                           InstanceFilesPage::StateFilter::Update, 0)));
        check(centerPageCount == 1 && centerPageTitle == UpdateCenterPage::windowTitle(),
              "★★ T28.2 那一趟真的跑完了（反查 + **批量那一发** + judgeUpdate 全接上了 ⇒ "
              "正好开出一页「更新中心」）",
              QStringLiteral("开了 %1 页：%2").arg(centerPageCount).arg(centerPageTitle));
        check(comboCount == 4,
              "★ T28.2 模组那一页有「可更新」这一档（下载页里真有那一栏的四页之一）—— "
              "★ 量的是 `canFilterUpdates()`（那一档在不在），不是「露着几颗」",
              QStringLiteral("%1 档").arg(comboCount));
        // 这一条就是这一节的正题：那一趟查出来"有新版本"的那一条**必须已经递到这一页上**。
        // 只弹报告、不递结果的话，这里筛出来是空的（而屏幕上就是"这个实例干净得很"）。
        check(updateRows == 1 && QDir::toNativeSeparators(updateRowPath) ==
                                     QDir::toNativeSeparators(modJar),
              "★★ T28.2 那一趟查出来「有新版本」的那一条**递到这一页上了**：选「可更新」"
              "正好剩下那个模组（认不出来的那一个不在里面 —— 「说不清」不是「可更新」）",
              QStringLiteral("%1 条：%2").arg(updateRows).arg(updateRowPath));
        {
            // 那一行写的整串 —— 唯一出处 `nameTextWithUpdateMark()`（盘上那个名字就叫
            // 「模组乙.jar」，禁用/启用没动过它）。
            const QString expected = InstanceFilesPage::nameTextWithUpdateMark(
                QStringLiteral("模组乙.jar"), true);
            check(updateArrowGlyph == expected,
                  "★★ T28.2 查出来有新版本的那一行，名字后面**长出了那个 ↑**"
                  "（走的是唯一出处 `nameTextWithUpdateMark()`）",
                  updateArrowGlyph.isEmpty() ? QStringLiteral("<没有这一格>") : updateArrowGlyph);
        }
        check(arrowOnPath.value(modJar, false) && !arrowOnPath.value(unknownJar, true) &&
                  !arrowOnPath.value(disabledJar, true),
              "★★ T28.2 那个 ↑ **只长在查出来有新版本的那一条上**：认不出来的那一条没有"
              "（「说不清」不是「可更新」），禁用着的那一条也没有",
              QStringLiteral("模组乙=%1 / 没认出来的=%2 / 模组丁(禁用)=%3")
                  .arg(int(arrowOnPath.value(modJar, false)))
                  .arg(int(arrowOnPath.value(unknownJar, false)))
                  .arg(int(arrowOnPath.value(disabledJar, false))));
        check(namePointSize.value(modJar, -1) > 0 &&
                  InstanceFilesPage::nameTextWithUpdateMark(QStringLiteral("x"), false) ==
                      QStringLiteral("x"),
              "★★ T28.2 那个 ↑ **和名字一个字号**（用户点名的一条）—— 它是名字那一格字符串的"
              "**尾巴**（同一个控件、同一个字体），所以「一样」是结构保证的；没有新版本的行"
              "原样就是那个名字、一个字都不多",
              QStringLiteral("那一格 %1 pt").arg(namePointSize.value(modJar, -1)));
        // （要求：「禁用的模组名字标灰，删除线覆盖」）：名字那一格的字体
        // 带着删除线，而且**只有禁用着的那一条**带着（别的两条不许被划）。
        check(struckPaths.contains(disabledJar) && !struckPaths.contains(modJar) &&
                  !struckPaths.contains(unknownJar),
              "★★ T28.2 被禁用那一条的名字**划了一道删除线**，另外两条没有（灰 + 删除线是"
              "用户点名要的那一层；「（已禁用）」那四个字照旧挂在那儿）",
              QStringLiteral("划掉的有 %1 条").arg(struckPaths.size()));
        check(!arrowOnPath.value(disabledJar, false) &&
                  QDir::toNativeSeparators(updateRowPath) !=
                      QDir::toNativeSeparators(disabledJar),
              "★★ T28.2 禁用着的那一条**整个没进这一趟**（它和模组乙逐字节一样，真查了"
              "一定判得出「可更新」）—— 用一样的字节就是不让「没查它」和「查了没认出来」"
              "混成一件事",
              QDir::toNativeSeparators(disabledJar));

        // ---------- ②' 走的是**批量那一发**，不是一个项目一发----------
        {
            const auto countMatching = [&api](const QString &needle) {
                int n = 0;
                for (const QString &line : api.requests) {
                    if (line.contains(needle)) {
                        ++n;
                    }
                }
                return n;
            };
            check(countMatching(QStringLiteral("/version_files/update")) == 1,
                  "★★ T28.3 Modrinth 那一拨走的是**批量那一发**（`POST /version_files/update`，"
                  "一次一串 sha1）—— 一项目一发是 623 发请求、整批 429 的那条路",
                  QStringLiteral("%1 发").arg(countMatching(QStringLiteral("/version_files/update"))));
            // 这一条和上一条是**两件事的两面**：只加上批量那一发、单发那条路照样跑，
            // 请求数一发都不少（用户下次还是撞限流）。
            check(countMatching(QStringLiteral("/project/UPDPROJ/version")) == 0,
                  "★★ T28.3 那个项目**一个单发都没发**（`/v2/project/<项目号>/version`）"
                  "—— 走批量那一发之后，同一个文件不该再单独问一遍",
                  QStringLiteral("%1 发").arg(countMatching(QStringLiteral("/project/UPDPROJ/version"))));
            QString batchBody;
            for (int i = 0; i < api.requests.size(); ++i) {
                if (api.requests.at(i).contains(QStringLiteral("/version_files/update"))) {
                    batchBody = QString::fromUtf8(api.bodies.value(i));
                }
            }
            check(batchBody.contains(hMod.sha1) &&
                      batchBody.contains(QStringLiteral("\"algorithm\":\"sha1\"")),
                  "★★ T28.3 那一发的正文里带着盘上那个文件的 sha1、而且写着 algorithm=sha1",
                  batchBody);
        }

        // ---------- ③ 关掉这一页再进来：那一格还得筛得出来 ----------
        //
        // 判据是"**离开这一页之后**还记不记得" —— 这就是"那一趟结果只活在那一个框里"和
        // "真的留下来了"的分界。不留下来的话，屏幕上那句"还没有查过更新"会重新挂上去，
        // 而用户明明刚刚才查完。
        int reopenRows = -1;
        bool reopenNotice = false;
        QTimer::singleShot(0, [&] {
            OverlayHost *host = currentPageOverlay();
            InstanceSettingsDialog *page = nullptr;  // 只认没藏起来的那一页（理由见 ④ 的 openSettings）
            if (host != nullptr)
                for (auto *dialog : host->findChildren<InstanceSettingsDialog *>())
                    if (!dialog->isHidden()) page = dialog;
            if (page == nullptr) {
                return;
            }
            InstanceFilesPage *mods = page->filesPage(QStringLiteral("mod"));
            page->showPage(QStringLiteral("mod"));
            QPushButton *state = mods != nullptr
                                     ? mods->stateButtonAt(InstanceFilesPage::StateFilter::Update)
                                     : nullptr;

            if (mods != nullptr && state != nullptr) {
                state->click();
                reopenRows = mods->list()->count();
                reopenNotice = mods->noticeLabel()->text() ==
                               InstanceFilesPage::updateNotCheckedText();
            }
            if (auto *back =
                    host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                back->click();
            } else {
                host->closeOverlay();
            }
        });
        QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                  Q_ARG(QString, idV));
        QApplication::processEvents();
        check(reopenRows == 1 && !reopenNotice,
              "★★ T28.4 **关掉这一页再进来，「可更新」还是筛得出那一条** —— 那一趟的结果"
              "留下来了（只在那个框里活了几秒钟的话，这里会重新挂着「还没有查过更新」）",
              QStringLiteral("%1 条 / 又见到「还没查过」=%2")
                  .arg(reopenRows)
                  .arg(int(reopenNotice)));

        // ---------- ④ 选着「可更新」就地更新：开着的这一页当场跟上（#143） ----------
        // 收掉的实例页只是藏起来、等主人 deleteLater()（OverlayHost::releaseContent 那段），删掉之前仍挂在
        // 这一层底下。测试代码在顶层直接收的页，processEvents() 不会替它跑 DeferredDelete，findChild 就会
        // 拿到上一页（早已停在「模组」上，切页不会重查）。所以先清，再只认没藏起来的那一页。
        const auto openSettings = [&w, &idV]() -> InstanceSettingsDialog * {
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QMetaObject::invokeMethod(&w, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, idV));
            QApplication::processEvents();
            OverlayHost *open = currentPageOverlay();
            if (open == nullptr) return nullptr;
            for (auto *dialog : open->findChildren<InstanceSettingsDialog *>())
                if (!dialog->isHidden()) return dialog;
            return nullptr;
        };
        // 「那一趟检查跑完了」的信号：finishUpdateCheck() 把结果递到页上之后写的那行日志。新页一开就带着上一趟的
        // 名单（「可更新(1)」立刻露着），光看那一颗分不出这一趟有没有收尾；不等它收尾就点更新，检查和批量更新会撞在一起。
        const auto checksLogged = [] { return countOccurrences(messagesSnapshot(), QStringLiteral("更新检测（")); };
        const int checksBefore = checksLogged();
        QPointer<InstanceSettingsDialog> page = openSettings();
        OverlayHost *host = currentPageOverlay();
        InstanceFilesPage *mods = nullptr;
        if (page != nullptr) {
            page->showPage(QStringLiteral("mod"));  // 新页从「概览」切进来 = 自动再查一遍
            mods = page->filesPage(QStringLiteral("mod"));
        }
        QPushButton *state =
            mods != nullptr ? mods->stateButtonAt(InstanceFilesPage::StateFilter::Update) : nullptr;
        const bool settled =
            state != nullptr && pumpUntil(
                                    [&] {
                                        return checksLogged() > checksBefore && !state->isHidden() &&
                                               state->text() == InstanceFilesPage::stateFilterButtonText(
                                                                    InstanceFilesPage::StateFilter::Update, 1);
                                    },
                                    20000);
        check(settled, "T28.5 新开的模组页那一趟自动检查跑完，「可更新(1)」露着",
              state != nullptr ? state->text() : QString());
        check(state != nullptr && !state->isHidden(), "T28.5 更新前「可更新」那一颗露着");
        if (state != nullptr) {
            state->click();
            // 页键给空串：跳过模组页那个差异预览框，下面走的仍是同一条下载 + 收尾。
            const auto update = [&] {
                emit page->fileBatchUpdateRequested(QString(), {mods->list()->item(0)
                                                        ->data(InstanceFilesPage::pathRole())
                                                        .toString()});
            };
            PromptAnswerer failure({0});
            failure.start();
            update();
            pumpUntil([&failure] { return !failure.answered.isEmpty(); }, 20000);
            failure.stop();
            QApplication::processEvents();
            check(!failure.answered.isEmpty() &&
                      mods->stateFilter() == InstanceFilesPage::StateFilter::Update &&
                      mods->list()->count() == 1 &&
                      state->text() == InstanceFilesPage::stateFilterButtonText(
                                           InstanceFilesPage::StateFilter::Update, 1),
                  "T28.5 更新失败：那一条留在「可更新」，数量仍是 1", state->text());

            served = newJar;
            update();
            const bool collapsed = pumpUntil([state] { return state->isHidden(); }, 20000);
            check(collapsed && mods->stateFilter() == InstanceFilesPage::StateFilter::All &&
                      mods->list()->count() == 3,
                  "★★ T28.5 全部更新成功：开着的模组页收起「可更新」并切回「全部」（#143）",
                  QStringLiteral("%1 / 列表 %2 条").arg(state->text()).arg(mods->list()->count()));
        }
        if (host != nullptr) {
            host->closeOverlay();
        }
        // 失败、成功两批收尾各排了一页「更新中心」：等两页都开出来收掉，别留给下一段。
        PageCloser after;
        after.start();
        pumpUntil([&after] { return after.seen.size() >= 2; }, 8000);
        pumpUntil([] { return currentOverlay() == nullptr; }, 8000);
        after.stop();

        // ---------- ⑤ 换成功的结论记「已是最新」：忽略别的不带回它，撤销才带回（#143） ----------
        const auto closePage = [] {
            if (OverlayHost *open = currentPageOverlay()) {
                if (auto *back =
                        open->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                    back->click();
                } else {
                    open->closeOverlay();
                }
            }
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QApplication::processEvents();
        };
        const auto chipText = [](int count) {
            return InstanceFilesPage::stateFilterButtonText(InstanceFilesPage::StateFilter::Update,
                                                            count);
        };
        const auto centre = [&w]() -> UpdateCenterPage * {
            auto *found = w.findChild<UpdateCenterPage *>(MainWindowUI::updateCenterPageName());
            return found != nullptr && found->isVisible() ? found : nullptr;
        };

        // 第二个可更新的模组：模组乙换回旧内容，再添一个模组丙，两条一起重查。
        check(readBytesAt(modJar) == newJar, "T28.6 前提：T28.5 之后模组乙在盘上是新版内容",
              QString::fromLatin1(readBytesAt(modJar)));
        writeFileAt(modJar, QByteArrayLiteral("T28-MOD-B"));
        const QString otherJar = dirV + QStringLiteral("/mods/模组丙.jar");
        writeFileAt(otherJar, QByteArrayLiteral("T28-MOD-C"));
        check(readBytesAt(modJar) == QByteArrayLiteral("T28-MOD-B") &&
                  readBytesAt(otherJar) == QByteArrayLiteral("T28-MOD-C"),
              "T28.6 前提：模组乙换回旧内容、模组丙写进去了",
              QStringLiteral("乙=%1 丙=%2").arg(QString::fromLatin1(readBytesAt(modJar)),
                                               QString::fromLatin1(readBytesAt(otherJar))));
        // 失败时一眼看出缺的是哪条：每一行的文件名、盘上内容是哪一版、名字后有没有 ↑，再附上最近那一趟检测的逐条结论。
        const int checksBefore6 = checksLogged();
        const auto updateDiag = [&]() {
            QStringList parts;
            if (mods != nullptr) {
                for (int row = 0; row < mods->list()->count(); ++row) {
                    const QListWidgetItem *item = mods->list()->item(row);
                    const QString path = item->data(InstanceFilesPage::pathRole()).toString();
                    const QByteArray bytes = readBytesAt(path);
                    const QString content = bytes == QByteArrayLiteral("T28-MOD-B")   ? QStringLiteral("乙旧版")
                                            : bytes == newJar                        ? QStringLiteral("乙新版")
                                            : bytes == QByteArrayLiteral("T28-MOD-C") ? QStringLiteral("丙")
                                                                                     : QStringLiteral("其它");
                    const bool marked = item->data(Qt::AccessibleTextRole).toString().endsWith(
                        InstanceFilesPage::updateArrowText());
                    parts.append(QStringLiteral("%1[%2%3]").arg(QFileInfo(path).fileName(), content,
                                                                marked ? QStringLiteral(" ↑") : QString()));
                }
            }
            const QStringList log = messagesSnapshot();
            int last = -1;
            for (int i = 0; i < log.size(); ++i)
                if (log.at(i).contains(QStringLiteral("更新检测（"))) last = i;
            QStringList verdicts;
            for (int i = last; i >= 0 && i < log.size() && verdicts.size() < 8; ++i)
                if (i == last || log.at(i).contains(QStringLiteral("•"))) verdicts.append(log.at(i).trimmed());
            return QStringLiteral("%1%2 | 本趟检测%3收尾 | 行：%4 | 最近一趟检测：%5")
                .arg(state != nullptr ? state->text() : QString(),
                     state != nullptr && state->isHidden() ? QStringLiteral("（收着）") : QString(),
                     checksLogged() > checksBefore6 ? QStringLiteral("已") : QStringLiteral("未"),
                     parts.join(QStringLiteral("，")),
                     verdicts.isEmpty() ? QStringLiteral("（没有检测日志）") : verdicts.join(QStringLiteral("；")));
        };
        const QString otherSha1 = QString::fromLatin1(
            QCryptographicHash::hash(QByteArrayLiteral("T28-MOD-C"), QCryptographicHash::Sha1)
                .toHex());
        // 两份答复都是「sha1 → 版本对象」：把模组乙那一项照抄一份挂到丙的 sha1 底下。
        for (QByteArray *reply : {&api.modrinthReply, &api.modrinthVersionFilesUpdateReply}) {
            QJsonObject byHash = QJsonDocument::fromJson(*reply).object();
            QJsonObject entry = byHash.value(hMod.sha1).toObject();
            if (reply == &api.modrinthReply) {  // 这一份认的是「盘上这个文件」，sha1 得换成丙的
                QJsonArray files = entry.value(QStringLiteral("files")).toArray();
                QJsonObject file = files.at(0).toObject();
                // 只换 sha1、留着 sha512：反查那一层只收两样哈希都有的文件（parseModrinthVersionFiles，
                // 真接口两样总是一起给）。整个换成只有 sha1 的对象，丙在反查这一步就被跳过、根本进不了判定。
                QJsonObject hashes = file.value(QStringLiteral("hashes")).toObject();
                hashes.insert(QStringLiteral("sha1"), otherSha1);
                file.insert(QStringLiteral("hashes"), hashes);
                files.replace(0, file);
                entry.insert(QStringLiteral("files"), files);
            }
            byHash.insert(otherSha1, entry);
            *reply = QJsonDocument(byHash).toJson(QJsonDocument::Compact);
        }

        page = openSettings();
        check(page != nullptr, "T28.6 实例设置开得出来");
        if (page != nullptr) {
            page->showPage(QStringLiteral("mod"));  // 切进模组页 = 自动再查一遍
            mods = page->filesPage(QStringLiteral("mod"));
            state = mods->stateButtonAt(InstanceFilesPage::StateFilter::Update);
            const bool two = pumpUntil(
                [&] { return !state->isHidden() && state->text() == chipText(2); }, 20000);
            bool askedForOther = false;  // 反查那一发（POST …/version_files）的正文里有没有丙的 sha1
            for (int i = 0; i < api.requests.size() && i < api.bodies.size(); ++i)
                if (api.requests.at(i).endsWith(QStringLiteral("/version_files HTTP/1.1")) &&
                    api.bodies.at(i).contains(otherSha1.toUtf8()))
                    askedForOther = true;
            check(askedForOther, "T28.6 重查时反查那一发带上了模组丙的 sha1");
            check(two, "T28.6 重查之后两条可更新", updateDiag());
            if (two) {
                state->click();
                QString modPath;
                QString otherPath;
                for (int row = 0; row < mods->list()->count(); ++row) {
                    const QString path =
                        mods->list()->item(row)->data(InstanceFilesPage::pathRole()).toString();
                    (path.endsWith(QStringLiteral("模组乙.jar")) ? modPath : otherPath) = path;
                }
                emit page->fileBatchUpdateRequested(QString(), {modPath});
                check(pumpUntil([&] { return state->text() == chipText(1); }, 20000),
                      "T28.6 只更新模组乙：还剩丙一条", state->text());
                closePage();  // 收掉实例页，排着的「更新中心」开出来

                const bool centreShown = pumpUntil([&] { return centre() != nullptr; }, 8000);
                check(centreShown, "T28.6 收尾那一页「更新中心」开出来了");
                if (centreShown) {
                    emit centre()->ignoreToggled(otherPath, true);
                    closePage();
                    // 只建页、不切页（`filesPage()`）：切进去会重查，把要量的结论盖掉。
                    page = openSettings();
                    mods = page->filesPage(QStringLiteral("mod"));
                    state = mods->stateButtonAt(InstanceFilesPage::StateFilter::Update);
                    check(state->isHidden(),
                          "★★ T28.6 更新成功后忽略另一条：已更新的那条不回到「可更新」（#143）",
                          state->text());

                    emit page->updateCenterRequested();
                    QPushButton *undo = nullptr;
                    pumpUntil(
                        [&] {
                            OverlayHost *open = currentPageOverlay();
                            if (centre() == nullptr || open == nullptr) return false;
                            undo = newestByObjectName<QPushButton>(
                                open, MainWindowUI::updateCenterUndoName());
                            return undo != nullptr && !undo->isHidden();
                        },
                        8000);
                    check(undo != nullptr && !undo->isHidden(), "T28.7 「撤销上一次更新」露着");
                    if (undo != nullptr && !undo->isHidden()) {
                        PromptAnswerer confirm({0});
                        confirm.start();
                        undo->click();
                        confirm.stop();
                        closePage();
                        page = openSettings();
                        mods = page->filesPage(QStringLiteral("mod"));
                        state = mods->stateButtonAt(InstanceFilesPage::StateFilter::Update);
                        const bool shown = !state->isHidden() && state->text() == chipText(1);
                        if (shown) state->click();
                        check(shown && mods->list()->count() == 1 &&
                                  mods->list()
                                          ->item(0)
                                          ->data(InstanceFilesPage::pathRole())
                                          .toString() == modPath &&
                                  readBytesAt(modJar) == QByteArrayLiteral("T28-MOD-B"),
                              "★★ T28.7 撤销之后模组乙回到「可更新」（丙仍忽略着）（#143）",
                              state->text());
                    }
                }
            }
            closePage();
        }
    }

    restoreSettings();
    check(AppPaths::minecraftDir() == originalDir, "T28.9 数据根目录换回来了");
}

