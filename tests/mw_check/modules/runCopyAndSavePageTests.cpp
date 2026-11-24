#include "Support.hpp"
#include "util/DirectorySessionLock.hpp"
#include <QLockFile>
#include "ui_check/modules/UiInstanceFilesTestHelpers.hpp"
void runCopyAndSavePageTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    QTemporaryDir elsewhere;
    QTemporaryDir worldSource;
    // 第四个是给 T27.3 那条 junction 用的：它**最后声明 ⇒ 最先析构** —— 那个链接指着源实例
    // 目录，而 QTemporaryDir 收尾时用的 QDir::removeRecursively() 会**追进** junction（见
    // 本文件后面删除那一段量的同一件事）。链接要是没删干净，让它自己的窝先收，别等到
    // `elsewhere` 那一趟把源实例里的东西顺手删掉（后面几节还要用那个目录）。
    QTemporaryDir aliasHome;
    check(root.isValid() && elsewhere.isValid() && worldSource.isValid() && aliasHome.isValid(),
          "T27.0 四个临时目录建出来了");
    if (!root.isValid() || !elsewhere.isValid() || !worldSource.isValid() || !aliasHome.isValid()) {
        return;
    }

    // 沙盒设置：数据根目录指到这个临时目录（产品代码里那一堆 AppPaths 全按它算）。
    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    const auto restoreSettings = [&] {
        sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
        sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
        sandbox.sync();
        AppPaths::setMinecraftDir(originalDir);
    };

    // ---------- 夹具：一份"装了点东西"的实例 ----------
    const QString id = QStringLiteral("T27-源实例");
    const QString versionDir = root.path() + QStringLiteral("/versions/") + id;
    const auto writeAt = [](const QString &path, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(bytes);
            file.close();
        }
    };
    writeAt(versionDir + QStringLiteral("/") + id + QStringLiteral(".jar"), "jar-bytes");
    writeAt(versionDir + QStringLiteral("/") + id + QStringLiteral(".json"),
            R"({"id":"1.20.1"})");
    writeAt(versionDir + QStringLiteral("/wtfml.meta"),
            R"({"name":"T27-源实例","instanceId":"t27-source-id"})");
    writeAt(versionDir + QStringLiteral("/mods/sodium.jar"), "mod");
    writeAt(versionDir + QStringLiteral("/saves/我的世界/level.dat"), QByteArray(4096, 'x'));
    QDir().mkpath(versionDir + QStringLiteral("/saves"));

    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());

    MainWindow window;
    window.show();
    QApplication::processEvents();

    // NOTE: 这一节里所有"回答一个问句"的动作都只排**一发 0 毫秒**、**不补 300 毫秒兜底**
    // （和 dismissNextPrompt() 那两发的差别就在这儿）：下面每一处 emit / invoke 都是
    // **同步**把框摆出来的（`ask()` / `getText()` 先建框、后进循环），所以那一发一定
    // 找得到它。补一发 300 毫秒的反而危险 —— 它会在**后面某一问**上才醒过来，把那一问
    // 随手点掉，而症状是"后面那条断言莫名其妙地没弹框"（见 g_modalHandlerGeneration
    // 那条 NOTE 记的那个真实案例）。每一发都先 ++代数，让更早排下的兜底作废。

    header("T27.1 复制与删除归入实例操作，不在概览重复展示");
    // NOTE: 这一小节**自己建一个对话框**、不走 openInstanceSettings()：那一条会卡在嵌套
    // 事件循环里（见 T25 那条 NOTE），而这里要量的全是「那一页上摆了什么东西」，
    // 和主窗口那一半没有关系 —— 真正接线的那一半在 T27.6。
    {
        InstanceSettingsDialog dialog(id, id, InstanceSettingsDialog::Override(),
                                      InstanceSettingsDialog::GlobalValues(), QStringList(),
                                      true);
        dialog.resize(1080, 720);
        dialog.show();
        dialog.showPage(InstanceSettingsDialog::overviewCategoryKey());
        QApplication::processEvents();
        auto *button = dialog.findChild<QPushButton *>(InstanceSettingsDialog::copyButtonName());
        auto *remove = dialog.findChild<QPushButton *>(InstanceSettingsDialog::deleteButtonName());
        check(button && remove && !button->isVisible() && !remove->isVisible(),
              "T27.1 概览不再重复展示复制与删除入口");
        dialog.showPage(QStringLiteral("operationsmenu"));
        QApplication::processEvents();
        check(button != nullptr, "★ T27.1 实例操作页找得到「复制实例」（保留原 objectName）");
        if (button != nullptr) {
            check(button->text() == InstanceSettingsDialog::copyText(),
                  "T27.1 按钮上那几个字来自唯一出处", button->text());
            check(button->isVisible(), "T27.1 实例操作页开着时复制入口可见");
            // tooltip 里必须说清**落到哪儿**：那一颗问的是「复制到哪个游戏文件夹」，
            // 而副本落在**那个文件夹**的 versions/ 底下 —— 不说的话用户会在自己挑的
            // 那一层里找它，找不到就以为复制没成。
            check(button->toolTip().contains(QStringLiteral("目标游戏目录")),
                  "★★ T27.1 tooltip 说清了副本落在「<那个文件夹>\\versions\\<新名字>」"
                  "（问的是游戏文件夹，不是「就拷到你挑的这一层」）",
                  button->toolTip());
            // 副本拿到的是一个**新实例 id**：所以它是一件独立的东西，不是一个链接。
            check(InstanceSettingsDialog::copyHint().contains(QStringLiteral("独立")) &&
                      button->toolTip().contains(QStringLiteral("保留原实例")),
                  "T27.1 tooltip 也说清了副本是独立的（拿到一个新的实例 id）",
                  button->toolTip());
        }

        // ---- 同一页上那颗「删除实例」----
        //
        // 要求：「顺便把删除实例功能做了，按钮放在概览里」。这一颗**不是新功能**：真去删
        // 的那条路（`confirmDeleteInstance()`）就是实例行行尾那颗垃圾桶走的同一条 —— 这一页
        // 上多出来的只是**第二个入口**。接线那一半在 T27.8。
        check(remove != nullptr, "★ T27.1 实例操作页找得到「删除实例」（保留原 objectName）");
        if (remove != nullptr) {
            check(remove->text() == InstanceSettingsDialog::deleteText(),
                  "T27.1 按钮上那几个字来自唯一出处", remove->text());
            check(remove->isVisible(), "T27.1 实例操作页开着时删除入口可见");
            // 它是**红的**（`ButtonKind::Danger`）：用户给的规矩是「危险操作（如修改存档，
            // 删除实例）做成红色」，而这一页上它是唯一一颗真会丢东西的按钮。
            check(remove->property("wtfmlButtonKind").toString() ==
                      Theme::buttonKindKey(Theme::ButtonKind::Danger),
                  "★★ T27.1 那一颗是「危险」那一档（红的）—— 同一页上紧挨着的「复制实例」"
                  "是绿的",
                  remove->property("wtfmlButtonKind").toString());
            // 字里必须说清"进回收站"：颜色只是提醒，真正让人停下来的是字；而删掉的东西
            // 其实能还原，不能写成"找不回来"。
            check(InstanceSettingsDialog::deleteHint().contains(QStringLiteral("回收站")),
                  "★★ T27.1 解释里写着「删掉的东西会进回收站」（删除走回收站，别写成永久删）",
                  InstanceSettingsDialog::deleteHint());
            check(remove->toolTip().contains(QStringLiteral("回收站")),
                  "★ T27.1 tooltip 也标了「回收站」", remove->toolTip().left(40));
        }
        dialog.hide();
    }

    header("T27.2 复制实例：动手的那一半（真拷一份到另一个游戏文件夹）");
    {
        const QString destFolder = elsewhere.path();
        bool done = false;
        dismissNextPrompt();  // 成了之后那一句「已经复制好了」
        const bool invoked = QMetaObject::invokeMethod(
            &window, "copyInstanceTo", Qt::DirectConnection, Q_RETURN_ARG(bool, done),
            Q_ARG(QString, id), Q_ARG(QString, destFolder),
            Q_ARG(QString, QStringLiteral("副本A")));
        QApplication::processEvents();
        check(invoked, "T27.2 invokeMethod 认得这三个参数（不认得就是静默 false，下面全假绿）");
        check(done, "★ T27.2 复制成功（返回值）");
        const QString copyDir = destFolder + QStringLiteral("/versions/副本A");
        const bool jarOk = QFileInfo::exists(copyDir + QStringLiteral("/副本A.jar"));
        const bool modOk = QFileInfo::exists(copyDir + QStringLiteral("/mods/sodium.jar"));
        check(jarOk && modOk, "T27.2 jar 跟着改了名，mods/ 底下那一份也带过来了",
              QStringLiteral("副本A.jar=%1 mods/sodium.jar=%2")
                  .arg(jarOk ? 1 : 0)
                  .arg(modOk ? 1 : 0));
        check(!QFileInfo::exists(copyDir + QLatin1Char('/') + id + QStringLiteral(".jar")),
              "T27.2 老名字那一份没留下");
        check(QFileInfo::exists(versionDir + QLatin1Char('/') + id + QStringLiteral(".jar")),
              "★ T27.2 源那一份一个字节都没动");
        check(g_modalTitle == QStringLiteral("复制实例"),
              "T27.2 收尾报了一句「复制实例」", g_modalTitle);
        // 那一份落在**别的**游戏文件夹里 ⇒ 它不在当前列表上，话里必须说清落在哪儿。
        check(g_modalText.contains(QStringLiteral("游戏目录")),
              "★★ T27.2 而且话里说明了它在另一个游戏文件夹里、不在当前列表上", g_modalText);
        check(!g_modalWasWarning, "T27.2 这是报信不是报错（标题不该是警告色）");
    }

    header("T27.3 复制实例：被挡住的那几条（一个字节都不许动）");
    {
        bool ok = true;
        dismissNextPrompt();
        QMetaObject::invokeMethod(&window, "copyInstanceTo", Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, ok), Q_ARG(QString, id),
                                  Q_ARG(QString, elsewhere.path()),
                                  Q_ARG(QString, QStringLiteral("副本A")));
        check(!ok, "T27.3 目标重名 → 拒绝");
        // 重名是**数据层**报上来的（copyInstance 那句"已经有一个叫…"），走的是 critical；
        // 而名字不合法那两条是这一层自己判的，走 warning。两条路的"严重程度"在这里是分开
        // 断言的 —— 标题颜色跟着 kind 走，混起来用户就分不清"我输错了"和"盘上出事了"。
        check(g_modalTitle == QStringLiteral("复制实例失败"),
              "T27.3 重名是按「复制实例失败」报的（数据层那一整句原样端出来）", g_modalTitle);
        check(g_modalText.contains(QStringLiteral("已存在名为")), "T27.3 原因说清了重名",
              g_modalText);
        QFile f(elsewhere.path() + QStringLiteral("/versions/副本A/副本A.jar"));
        QByteArray got;
        if (f.open(QIODevice::ReadOnly)) {
            got = f.readAll();
        }
        check(got == QByteArray("jar-bytes"), "★ T27.3 原来那一份没被动过");

        dismissNextPrompt();
        QMetaObject::invokeMethod(&window, "copyInstanceTo", Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, ok), Q_ARG(QString, id),
                                  Q_ARG(QString, elsewhere.path()),
                                  Q_ARG(QString, QStringLiteral("a/b")));
        check(!ok, "T27.3 名字里带分隔符 → 拒绝");
        check(g_modalWasWarning && g_modalTitle == QStringLiteral("复制实例"),
              "T27.3 名字不合法是这一层判的（warning + 标题就是「复制实例」）", g_modalTitle);
        check(g_modalText.contains(QStringLiteral("不能有")), "T27.3 原因里说的是字符那一条",
              g_modalText);
        check(!QFileInfo::exists(elsewhere.path() + QStringLiteral("/versions/a")),
              "★ T27.3 而且一个目录都没建出来");
        // 空名字在**这一层**就拦住了（copyInstance() 那边也会拦，但那是数据层的口吻）
        dismissNextPrompt();
        QMetaObject::invokeMethod(&window, "copyInstanceTo", Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, ok), Q_ARG(QString, id),
                                  Q_ARG(QString, elsewhere.path()), Q_ARG(QString, QString()));
        check(!ok, "T27.3 空名字 → 拒绝");
        check(g_modalText.contains(QStringLiteral("副本得有个名字")), "T27.3 空名字那句话直接明了",
              g_modalText);

        // ---- ★ 目标落在**源实例目录自己里面** → 拒绝（ 补的第四道闸）----
        //
        // 这一条量的是**一次能把盘写爆**的路：挑游戏文件夹那一步是个普通目录选择框，用户
        // 一路点进 `versions/<这一份自己>` 再按确定就是这种输入。原来三道闸（名字 / 正在跑 /
        // 正在装）一条都拦不住，数据层那两道（QDir 比"是不是同一个目录"、exists() 比"有没有
        // 重名"）也拦不住 —— 然后复制是"先 mkpath 目标、再列源目录"，列出来的东西里包含它
        // 刚造出来的那一层 ⇒ **自己抄自己、每层再深一层**，一声不响地写到盘满。
        //
        // 摆法：目标直接喂**源实例目录**（不是游戏文件夹）。判据红了就说明这一层没拦住 ——
        // 下面那条"一个目录都没建出来"是关键：它证明**盘上真的一个字节都没多**（不只是
        // 弹了个框）。
        dismissNextPrompt();
        QMetaObject::invokeMethod(&window, "copyInstanceTo", Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, ok), Q_ARG(QString, id),
                                  Q_ARG(QString, versionDir), Q_ARG(QString, QStringLiteral("副本乙")));
        check(!ok, "★★ T27.3 目标就是**这一份实例自己的目录** → 拒绝（不然它会自己抄自己）");
        check(g_modalWasWarning && g_modalTitle == QStringLiteral("复制实例"),
              "T27.3 而且是这一层用一句 warning 拦的", g_modalTitle);
        check(g_modalText.contains(QStringLiteral("不能复制到其自身或子目录")) &&
                  g_modalText.contains(QDir::toNativeSeparators(versionDir)),
              "★ T27.3 话里点名了**用户刚挑的那个目录**（这一层存在的意义就是这句话）",
              g_modalText);
        check(!QFileInfo::exists(versionDir + QStringLiteral("/versions")),
              "★★ T27.3 拒绝之后**盘上一个目录都没多**（多出一个 versions/，"
              "那就是递归的开始）");
        // 源目录自己及其内容照旧。
        check(QFileInfo::exists(versionDir + QStringLiteral("/mods/sodium.jar")) &&
                  QFileInfo::exists(versionDir + QStringLiteral("/saves/我的世界/level.dat")),
              "★ T27.3 源实例一个字节都没动");

        // 子目录同理（用户点进 saves/ 再按确定也是这种输入）。
        dismissNextPrompt();
        QMetaObject::invokeMethod(&window, "copyInstanceTo", Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, ok), Q_ARG(QString, id),
                                  Q_ARG(QString, versionDir + QStringLiteral("/saves")),
                                  Q_ARG(QString, QStringLiteral("副本丙")));
        check(!ok && !QFileInfo::exists(versionDir + QStringLiteral("/saves/versions")),
              "★★ T27.3 挑的是它**底下的**一个子目录也一样拒（判的是「是不是在里面」，"
              "不是「是不是就是它」）");

        // ---- 别名挡得住吗：拿一个**junction** 指着源实例目录当游戏文件夹 ----
        //
        // 挑文件夹那一步带的是 `QDir::DontResolveSymlinks`，拿回来的路径不规范化，
        // **只比字面量挡不住别名** —— 这一条就是为那个写的。
        //
        // 实测的坑：`canonicalFilePath()` **不解 junction**（Windows 上它走
        // `slowCanonicalized()`，逐段判的是 `isSymLink()`，而 junction 是 `JunctionType`）
        // ⇒ 光靠它这一条照样绕得过去。更阴的是：那一版跑起来"`!ok` 是绿的" —— 因为复制
        // 真的跑下去了、一路自己抄自己、最后撞上路径长度上限才失败，而盘上已经多出一层
        // `versions/`（下面那条断言就是把它逮住的那一条）。所以这里**同时**钉住"报的是
        // 哪一句"：真正的拒绝走的是界面上那句 warning，不是数据层的「复制实例失败」。
        const QString aliasDir = aliasHome.path() + QStringLiteral("/指到源实例的链接");
        const bool madeAlias =
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                               QDir::toNativeSeparators(aliasDir),
                               QDir::toNativeSeparators(versionDir)}) == 0 &&
            QFileInfo(aliasDir).isJunction();
        check(madeAlias, "★ T27.3 先造出一个真 junction 指着源实例目录（mklink /J）");
        if (madeAlias) {
            dismissNextPrompt();
            QMetaObject::invokeMethod(&window, "copyInstanceTo", Qt::DirectConnection,
                                      Q_RETURN_ARG(bool, ok), Q_ARG(QString, id),
                                      Q_ARG(QString, aliasDir),
                                      Q_ARG(QString, QStringLiteral("副本丁")));
            check(!ok, "★★ T27.3 目标是一个**指着源实例目录的 junction** 也拒 —— "
                       "判据比的是规范化之后的真路径，不是字面量",
                  g_modalText);
            // 光看 !ok 会是个**假绿**：复制跑下去撞路径上限也是 !ok。必须是界面上那句
            // warning（标题「复制实例」+ 那句话），不是数据层那句「复制实例失败」。
            check(g_modalTitle == QStringLiteral("复制实例")
                      && g_modalText.contains(QStringLiteral("不能复制到其自身或子目录")),
                  "「复制跑下去撞了墙才失败」那种假绿",
                  g_modalTitle + QStringLiteral(" / ") + g_modalText);
            check(!QFileInfo::exists(versionDir + QStringLiteral("/versions")),
                  "★ T27.3 同样是「一个目录都没建出来」");
            // 收尾：把这个链接删掉（junction 本身用 rmdir 删，不要动它那一头）。
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("rmdir"),
                               QDir::toNativeSeparators(aliasDir)});
        }
    }

    header("T27.3a 后台复制期间源实例算「正在搬文件」（设置页拒绝打开）");
    // `copyInstanceInBackground()` 是界面上那颗「复制实例」真正走的路 ——
    // 拷树落在后台线程。它开头**同步**把源实例压进 `FileOperationOccupancy`（两趟压同一个实例时引用计数各算各的），
    // 收尾（成功或失败）第一件事就是摘掉。不记的话，拷树跑到一半用户照样能启动这一份
    // （游戏一边写存档、拷树一边读 ⇒ 副本里是半截世界），也能开设置页去改同一个目录。
    // 判据看得见的是 `InstanceDialogController::open()` 那道闸 —— 它会回一句
    // 「正在搬文件」，而不是把那一页开出来。
    {
        pumpUntil([] { return currentOverlay() == nullptr; }, 3000);
        const QString destFolder = elsewhere.path();
        const QString copyName = QStringLiteral("后台副本");
        const QString copyDir = destFolder + QStringLiteral("/versions/") + copyName;

        PromptAnswerer answerer({0});
        answerer.start();
        const bool invoked = QMetaObject::invokeMethod(
            &window, "copyInstanceToInBackground", Qt::DirectConnection,
            Q_ARG(QString, id), Q_ARG(QString, destFolder), Q_ARG(QString, copyName));
        check(invoked,
              "T27.3a invokeMethod 认得后台复制那一发（不认得就是静默 false，下面全假绿）");

        // 复制**不支持取消**，但账本必须说清为什么（详情页那一行小字）。
        // 这一发是**同步**读完的：`beginTask` 在 `upgrade/拷贝` 真正起线程之前就记好了。
        bool sawCopyTask = false, copyCancelExposed = true;
        QString copyBlockedReason;
        for (const TaskCenter::Item &item : window.taskCenterItemsForTest()) {
            if (!item.title.contains(copyName)) continue;
            if (item.state != TaskCenter::State::Running
                && item.state != TaskCenter::State::Queued) continue;
            sawCopyTask = true;
            copyCancelExposed = item.capabilities.cancel;
            copyBlockedReason = item.cancelBlockedReason;
        }
        check(sawCopyTask && !copyCancelExposed && !copyBlockedReason.isEmpty(),
              "★ T27.3a 后台复制这一项在账本里明写「为什么不给取消」"
              "（只把按钮藏起来，用户分不清是做完还是停不得）");

        // 忙标记是**同步**记上的（甚至可能还没进后台线程），所以这一发一定有东西挡着。
        QMetaObject::invokeMethod(&window, "openInstanceSettings", Qt::DirectConnection,
                                  Q_ARG(QString, id));
        bool busyRefused = false;
        for (const PromptAnswerer::Answered &one : answerer.answered) {
            if (one.title == QStringLiteral("正在搬文件")
                || one.text.contains(QStringLiteral("正在后台复制"))) {
                busyRefused = true;
                break;
            }
        }
        check(busyRefused,
              "★★ T27.3a 拷树还在后台跑的时候开设置页 ⇒ 被「正在搬文件」挡下来"
              "（不挡的话用户就能在拷贝期间改同一个目录）");
        check(currentPageOverlay() == nullptr,
              "★★ T27.3a 而且那一页**没有**被开出来（挡在 controller 开页那一道闸上，"
              "不是先开出来再报一句）");

        // 判"这一趟真的收尾了"看两件事：副本落在盘上、忙标记摘掉了。
        //
        // NOTE: **不能**拿"收尾那一句报出来了"当完成判据：复制很快时它会和上面那条
        // 「正在搬文件」撞在一起，而 OverlayHost 不叠提示（第二条只留一行日志）。
        // 那是一条真实的（罕见）产品路径，但拿它当测试的完成信号就是一条会随机
        // 红/绿的判据 —— 这里改成直接读忙名单。
        const bool copied = pumpUntil(
            [&] {
                return !window.instanceBusyForTest(id)
                       && QFileInfo::exists(copyDir + QLatin1Char('/')
                                            + AppPaths::versionMetaFileName())
                       && currentOverlay() == nullptr;
            },
            15000);
        check(copied,
              "T27.3a 后台复制跑完了、忙标记也摘干净了（副本落在另一个游戏文件夹）");
        answerer.stop();

        // 收尾之后忙标记必须摘干净：同一份实例的设置页要能正常打开。
        bool pageOpened = false;
        QTimer::singleShot(0, [&] {
            OverlayHost *host = currentPageOverlay();
            pageOpened = host != nullptr
                         && host->findChild<InstanceSettingsDialog *>() != nullptr;
            if (host != nullptr) {
                if (auto *back = host->findChild<QPushButton *>(
                        OverlayHost::overlayBackButtonName())) {
                    back->click();
                } else {
                    host->closeOverlay();
                }
            }
        });
        QMetaObject::invokeMethod(&window, "openInstanceSettings", Qt::DirectConnection,
                                  Q_ARG(QString, id));
        QApplication::processEvents();
        check(pageOpened,
              "★★ T27.3a 复制收了尾 ⇒ 同一份实例的设置页又能开了（忙标记摘干净了；"
              "一直红 = finished 里漏了 remove）");
    }

    header("T27.3d 目标文件夹是当前游戏文件夹的 junction 别名 ⇒ 目标名字照样算「文件操作中」");
    {
        const QString link = aliasHome.path() + QStringLiteral("/game-link");
        QProcess::execute(QStringLiteral("cmd"),
                          {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                           QDir::toNativeSeparators(link), QDir::toNativeSeparators(root.path())});
        const bool madeLink = QFileInfo(link).isJunction();
        check(madeLink, "★ T27.3d 先造出一个真 junction 指着**当前游戏文件夹**（mklink /J）");
        if (madeLink) {
            pumpUntil([] { return currentOverlay() == nullptr; }, 3000);
            // 复制收尾会弹一句「已经复制好了」。那句提示是在 pumpUntil 的
            // processEvents 里弹出来的（nested execPrompt），没人点就一直卡在里层循环
            // ——14 楼的 15 秒超时压在**外层**，进不去。T27.3a 也有同一句，它配了
            // PromptAnswerer；这里照着配一个。
            PromptAnswerer answerer({0});
            answerer.start();
            const QString aliasCopyName = QStringLiteral("T27-别名副本");
            const QString aliasCopyDir =
                root.path() + QStringLiteral("/versions/") + aliasCopyName;
            const bool invoked = QMetaObject::invokeMethod(
                &window, "copyInstanceToInBackground", Qt::DirectConnection,
                Q_ARG(QString, id), Q_ARG(QString, link), Q_ARG(QString, aliasCopyName));
            check(invoked, "T27.3d invokeMethod 认得后台复制那一发");
            // 忙标记是**同步**记上的（在起后台线程之前），所以这一句一定问得到。
            check(window.instanceBusyForTest(aliasCopyName),
                  "★★ T27.3d 目标名字**当场**记进「文件操作中」（字面量比较在这里是 false）");
            const bool finished = pumpUntil(
                [&] {
                    return !window.instanceBusyForTest(id)
                           && !window.instanceBusyForTest(aliasCopyName)
                           && QFileInfo::exists(aliasCopyDir + QLatin1Char('/')
                                                + AppPaths::versionMetaFileName())
                           && currentOverlay() == nullptr;
                },
                15000);
            check(finished, "T27.3d 复制跑完、两边的忙标记都摘干净（副本落在别名底下）");
            answerer.stop();
            QDir(aliasCopyDir).removeRecursively();
            check(!QFileInfo::exists(aliasCopyDir), "T27.3d 收尾：把这一份别名副本删掉");
            // 收尾：junction 只能 rmdir（删链接本体，不跟着删它那一头）。
            QProcess::execute(QStringLiteral("cmd"),
                              {QStringLiteral("/c"), QStringLiteral("rmdir"),
                               QDir::toNativeSeparators(link)});
            check(!QFileInfo(link).isJunction(), "T27.3d 收尾：链接本体删掉（那一头一个字节没动）");
        }
    }

    header("T27.3b 升级副本：拿「用户挑的目标版本」当底，不是当前实例的克隆");
    // 升级副本后台化那一版一开始把 `copyInstance()` 的源写成了 `sourceId`
    // （当前实例），于是"升级"造出来的是一份**当前实例的克隆** —— 版本号根本没变，
    // 用户以为升了级、其实没升，而且没有任何报错。这一条钉的就是"底"是哪一个：
    // 目标版本的主 jar 带一段认得出的内容，副本里必须是那一段。
    {
        pumpUntil([] { return currentOverlay() == nullptr; }, 3000);
        const QString targetId = QStringLiteral("T27-新版本");
        const QString targetDir = root.path() + QStringLiteral("/versions/") + targetId;
        writeAt(targetDir + QLatin1Char('/') + targetId + QStringLiteral(".jar"),
                "target-jar-bytes");
        writeAt(targetDir + QLatin1Char('/') + targetId + QStringLiteral(".json"),
                R"({"id":"1.20.4"})");
        writeAt(targetDir + QStringLiteral("/wtfml.meta"),
                R"({"name":"T27-新版本","instanceId":"t27-target-id"})");

        // `getText()` 预填的就是这句建议名，而 PromptAnswerer 只点按钮、不改输入框 ⇒
        // 这里拿到的名字就是建议名。
        const QString newName = QStringLiteral("T27-源实例 升级副本");
        const QString newDir = root.path() + QStringLiteral("/versions/") + newName;

        bool emitted = false;
        bool closed = false;
        PromptAnswerer answerer({0, 0, 0, 0});
        answerer.start();
        // 操作收尾之后（四次问句都答完、提示都收了）才点返回箭头 —— 提前收页会让
        // `finished` 那条线上的 lambda 落在一个已经收掉页的会话上。
        QTimer closer;
        closer.setInterval(30);
        QObject::connect(&closer, &QTimer::timeout, [&] {
            if (closed || answerer.answered.size() < 4 || currentPrompt() != nullptr) {
                return;
            }
            closed = true;
            closer.stop();
            if (OverlayHost *host = currentPageOverlay()) {
                if (auto *back = host->findChild<QPushButton *>(
                        OverlayHost::overlayBackButtonName())) {
                    back->click();
                } else {
                    host->closeOverlay();
                }
            }
        });
        closer.start();
        // 兜底（栈上的 QTimer ⇒ 出了这一节一定不会再响）：失败路径可能只弹 1~3 条，
        // 别让整节挂死在一个没人应答的问句上。
        QTimer fallback;
        fallback.setSingleShot(true);
        QObject::connect(&fallback, &QTimer::timeout, [&] {
            if (closed) {
                return;
            }
            closed = true;
            closer.stop();
            if (InlineMessageBox *box = currentPrompt()) {
                box->close();
            }
            if (OverlayHost *host = currentPageOverlay()) {
                if (auto *back = host->findChild<QPushButton *>(
                        OverlayHost::overlayBackButtonName())) {
                    back->click();
                } else {
                    host->closeOverlay();
                }
            }
        });
        fallback.start(20000);

        // 那一页现在**不阻塞**（`showPage()`）：开完页当场发信号，别做成 0ms 定时器——
        // 定时器要等 `processEvents()` 才醒，而那时下面几行已经把问句应答器停掉了，
        // 四句问句就没人答、兜底也停了 ⇒ 整节挂死。
        QMetaObject::invokeMethod(&window, "openInstanceSettings", Qt::DirectConnection,
                                  Q_ARG(QString, id));
        {
            OverlayHost *host = currentPageOverlay();
            auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
            if (page != nullptr) {
                emitted = true;
                // 和「创建升级副本…」那一颗**发出的东西逐字相同**（只发意图，不写盘）：
                // `saves` 是夹具里唯一一份默认勾着的范围。
                page->transferApplyRequested(QStringLiteral("upgrade"), targetId,
                                             QStringList{QStringLiteral("saves")});
            }
        }
        // 等这一整趟收尾：前两问同步答掉之后，搬文件在后台线程里跑，进度提示那一档
        // `pageOpen()` 是**假**的（所以不能拿"覆盖层空了"当判据）—— 结果 / 快速启动
        // 那两问要等 `finished` 回来才弹。正常路径由 closer 在四问答完、提示收干净之后
        // 点返回箭头；失败路径由 20 秒兜底关页。两者都会把 `closed` 置真。
        pumpUntil([&] { return closed; }, 25000);
        fallback.stop();
        closer.stop();
        answerer.stop();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();

        check(emitted, "T27.3b 「升级」页开出来了、信号也真的发出去了（这条红了下面都是假绿）");
        check(answerer.answered.size() >= 4,
              "★★ T27.3b 一路问下来是四句：确认 → 新名字 → 结果 → 快速启动",
              QStringLiteral("收到 %1 句").arg(answerer.answered.size()));

        QString jarBytes;
        {
            QFile f(newDir + QLatin1Char('/') + newName + QStringLiteral(".jar"));
            if (f.open(QIODevice::ReadOnly)) {
                jarBytes = QString::fromUtf8(f.readAll());
            }
        }
        check(jarBytes == QStringLiteral("target-jar-bytes"),
              "★★ T27.3b 新副本的主 jar 是**目标版本**那一份（不是当前实例的克隆）",
              QStringLiteral("读到：%1（读到 jar-bytes 就是错抄了 sourceId）").arg(jarBytes));
        QString jsonText;
        {
            QFile f(newDir + QLatin1Char('/') + newName + QStringLiteral(".json"));
            if (f.open(QIODevice::ReadOnly)) {
                jsonText = QString::fromUtf8(f.readAll());
            }
        }
        check(jsonText.contains(QStringLiteral("1.20.4")),
              "★★ T27.3b 版本 json 也是目标那一份（1.20.4）", jsonText);
        check(QFileInfo::exists(newDir + QStringLiteral("/saves/我的世界/level.dat")),
              "★ T27.3b 勾着的存档搬进了**新副本**里");
        QFile srcJar(versionDir + QLatin1Char('/') + id + QStringLiteral(".jar"));
        QByteArray srcBytes;
        if (srcJar.open(QIODevice::ReadOnly)) {
            srcBytes = srcJar.readAll();
        }
        check(srcBytes == QByteArray("jar-bytes"), "★ T27.3b 源实例一个字节都没动");
    }

    header("T27.3e 存档页批量删除：目录锁拿不到（deleteWorldInBackground 同步拒绝）时不挂死");
    {
        // 回归：第一发同步回调 whenDone(false) 时 loop.quit() 早于 loop.exec()，exec() 永不返回。
        const QString world = versionDir + QStringLiteral("/saves/我的世界");
        QMetaObject::invokeMethod(&window, "openInstanceSettings", Qt::DirectConnection,
                                  Q_ARG(QString, id));
        OverlayHost *host = currentPageOverlay();
        auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
        check(page != nullptr, "T27.3e 设置页开出来了");
        if (page != nullptr) {
            const bool lockWas = DirectorySessionLock::enabled;
            DirectorySessionLock::enabled = true;
            QLockFile foreign(DirectorySessionLock::lockFilePath(DirectorySessionLock::keyFor(root.path())));
            check(foreign.tryLock(0), "T27.3e 前提：这个数据目录的锁已被「另一份启动器」占着");
            PromptAnswerer answerer({0});  // 确认框点「删除」，失败提示点第一颗
            answerer.start();
            bool returned = false;
            emit page->lifecycleBatchRequested(QStringLiteral("save"), QStringList{world},
                                               InstanceSettingsDialog::LifecycleBatchAction::Remove);
            returned = true;
            answerer.stop();
            DirectorySessionLock::enabled = lockWas;
            foreign.unlock();
            check(returned, "★★ T27.3e 同步拒绝时批量删除也能返回（不卡在 loop.exec()）");
            check(QFileInfo::exists(world + QStringLiteral("/level.dat")),
                  "T27.3e 被拒绝的那个世界一个字节没动");
            check(answerer.answered.size() >= 2,
                  "T27.3e 先问「删除世界」确认，再弹失败原因",
                  QStringLiteral("收到 %1 句").arg(answerer.answered.size()));
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (OverlayHost *open = currentPageOverlay()) {
                if (auto *back = open->findChild<QPushButton *>(OverlayHost::overlayBackButtonName()))
                    back->click();
                else
                    open->closeOverlay();
            }
            QApplication::processEvents();
        }
    }

    header("T27.3c 「正在搬文件」是引用计数：叠两趟、收掉一趟，标记还在");
    {
        // 用一个不存在的 id：它只进这一份名单，不和别的判据互相干扰。
        const QString busyId = QStringLiteral("T27-计数夹具");
        check(!window.instanceBusyForTest(busyId), "T27.3c 前提：这一号本来不在名单上");

        window.markFileOperationBusyForTest(busyId);
        window.markFileOperationBusyForTest(busyId);
        check(window.instanceBusyForTest(busyId), "T27.3c 压两趟 ⇒ 在名单上");

        window.releaseFileOperationBusyForTest(busyId);
        check(window.instanceBusyForTest(busyId),
              "★★ T27.3c 收掉**一趟**之后标记必须还在（拿集合记的写法在这里会变红）");

        window.releaseFileOperationBusyForTest(busyId);
        check(!window.instanceBusyForTest(busyId),
              "★★ T27.3c 两趟都收掉 ⇒ 干干净净摘掉（实例又能启动 / 删除了）");

        // 多摘一次必须是空操作：配对写错时不许把计数拖成负数，也不许动别的一趟。
        window.markFileOperationBusyForTest(busyId);
        window.releaseFileOperationBusyForTest(busyId);
        window.releaseFileOperationBusyForTest(busyId);
        check(!window.instanceBusyForTest(busyId),
              "T27.3c 多摘一次是空操作（计数不会变成负数、也不会留在名单上）");
    }

    header("T27.4 导入世界：动手的那一半（真拷进 saves/）");
    {
        QDir().mkpath(worldSource.path() + QStringLiteral("/region"));
        writeAt(worldSource.path() + QStringLiteral("/level.dat"), "nbt");
        writeAt(worldSource.path() + QStringLiteral("/region/r.0.mca"), "region");
        bool ok = false;
        const bool invoked = QMetaObject::invokeMethod(
            &window, "importWorldInto", Qt::DirectConnection, Q_RETURN_ARG(bool, ok),
            Q_ARG(QString, id), Q_ARG(QString, worldSource.path()),
            Q_ARG(QString, QStringLiteral("导进来的")));
        check(invoked, "T27.4 invokeMethod 认得这三个参数");
        check(ok, "★ T27.4 导入成功");
        check(QFileInfo::exists(versionDir + QStringLiteral("/saves/导进来的/level.dat")) &&
                  QFileInfo::exists(versionDir + QStringLiteral("/saves/导进来的/region/r.0.mca")),
              "T27.4 落在**这个实例的** saves/ 底下，level.dat 和 region 都过去了");
        // 源一个字节都不动（导入是**拷**，不是搬）。
        check(QFileInfo::exists(worldSource.path() + QStringLiteral("/level.dat")),
              "★ T27.4 源那一份没被动过（是导入，不是搬走）");
        // 成功这条路**只写日志、不弹框**：用户按完就看见那一页多了一行，不需要再确认一次。
        check(currentPrompt() == nullptr, "T27.4 成功时**不弹框**（那一页自己会多出一行）");

        // 源是**一个真的目录、但里面没有 level.dat**（用户最容易犯的错：选到了世界文件夹
        // 的**上一层**）。这一条要的是"原因里点名 level.dat"，所以不能拿一个不存在的路径
        // 去撞 —— 那一条会被更前面的"不是一个目录"接住，报的是另一句话。
        const QString notAWorld = root.path() + QStringLiteral("/不是世界");
        QDir().mkpath(notAWorld);
        writeAt(notAWorld + QStringLiteral("/region/r.0.mca"), "region");
        dismissNextPrompt();
        const bool failed = QMetaObject::invokeMethod(
            &window, "importWorldInto", Qt::DirectConnection, Q_RETURN_ARG(bool, ok),
            Q_ARG(QString, id), Q_ARG(QString, notAWorld), Q_ARG(QString, QStringLiteral("试试")));
        check(failed && !ok, "T27.4 源不是世界 → 拒绝");
        check(g_modalTitle == QStringLiteral("导入世界失败"), "T27.4 报的是「导入世界失败」",
              g_modalTitle);
        check(g_modalText.contains(QStringLiteral("level.dat")), "T27.4 原因里点名了 level.dat",
              g_modalText);
        check(!QFileInfo::exists(versionDir + QStringLiteral("/saves/试试")),
              "T27.4 而且没在 saves 里留下一个空壳");
    }

    header("T27.5 世界改名：动手的那一半");
    {
        bool ok = false;
        const bool invoked = QMetaObject::invokeMethod(
            &window, "renameInstanceWorld", Qt::DirectConnection, Q_RETURN_ARG(bool, ok),
            Q_ARG(QString, id),
            Q_ARG(QString, versionDir + QStringLiteral("/saves/我的世界")),
            Q_ARG(QString, QStringLiteral("改过名的")));
        check(invoked, "T27.5 invokeMethod 认得这三个参数");
        check(ok, "★ T27.5 改名成功");
        check(!QFileInfo::exists(versionDir + QStringLiteral("/saves/我的世界")) &&
                  QFileInfo::exists(versionDir + QStringLiteral("/saves/改过名的/level.dat")),
              "T27.5 老目录没了、新目录在、里面的东西跟着过去了");

        dismissNextPrompt();
        QMetaObject::invokeMethod(&window, "renameInstanceWorld", Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, ok), Q_ARG(QString, id),
                                  Q_ARG(QString, versionDir + QStringLiteral("/saves/改过名的")),
                                  Q_ARG(QString, QStringLiteral("导进来的")));
        check(!ok, "T27.5 和另一个世界重名 → 拒绝");
        check(g_modalText.contains(QStringLiteral("已经有一个叫")), "T27.5 原因说清了重名",
              g_modalText);
        check(QFileInfo::exists(versionDir + QStringLiteral("/saves/改过名的/level.dat")),
              "★ T27.5 被挡住时那一个原样还在");
    }

    header("T27.6 存档那一页发出来的三发信号真的接到 MainWindow 上了");
    // 量法：开实例设置 → 拿到那一页 → **自己发那一发信号** → 看屏幕上真的出现了什么。
    // 三发里只有两发能这么量（见文件头那条 NOTE：「导入世界」点了会卡在原生选择框上）。
    {
        bool probed = false;
        QString pageTitle;
        // 点那支笔那一发里**不该**有任何提示框（ 把"打开之前那一问"删掉了）。
        QList<PromptAnswerer::Answered> penAsks;
        QString batchText;
        QString batchTitle;
        bool penFound = false;
        bool pageOpened = false;
        QString rowPath;
        QString prefilled;
        QTimer::singleShot(0, [&] {
            OverlayHost *host = currentPageOverlay();
            auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
            if (page == nullptr) {
                return;
            }
            InstanceFilesPage *saves = page->filesPage(QStringLiteral("save"));
            if (saves == nullptr) {
                return;
            }
            probed = true;
            page->showPage(QStringLiteral("save"));
            const auto pen = uiEditAt(saves, 0);
            penFound = pen != nullptr;
            if (pen != nullptr && saves->list() != nullptr && saves->list()->item(0) != nullptr) {
                rowPath =
                    saves->list()->item(0)->data(InstanceFilesPage::pathRole()).toString();
            }

            // ---- ① 那支笔：**直接进「世界设置」那一页**，中间一句话都不问----
            //
            // 要求：「世界设置换成点开是一个额外的页面」+「点击笔的时候不要弹备份
            // 提示了」。所以这一下量的是**反过来的那件事**：点下去屏幕上**不许有**任何提示
            // 框（原来这里会垫一问「打开（只读）/ 先备份一份 / 取消」），人直接站到那一页上。
            //
            // 点的是**那支真的笔**（不是直接 emit editRequested）：这样"那一页自己把
            // clicked 接成 editRequested"这一段也顺带量到了。
            //
            // NOTE: 这一行的 `level.dat` 是 T27.4 导进来的那 3 个字节（`nbt`），解不出来 ⇒
            // 开的是**"读不出来"**那一档（标题「这个世界可能坏了：<目录>」、正文整块
            // 收着、只留 重命名 / 打开文件夹 / 删除世界 / 备份一份）。
            //
            // NOTE: 那一发点击还是**包在 PromptAnswerer 里**的：万一哪天那一问又回来了，
            // 框会当场被点掉（不会让这一节挂在"等一个没人答的框"上），而 `penAsks`
            // 会非空 —— 下面那条断言红在"点笔不该弹任何东西"上，方向是对的。
            QList<PromptAnswerer::Answered> penAsks;
            if (pen != nullptr) {
                PromptAnswerer answerer({0});
                answerer.start();
                pen->click();
                QApplication::processEvents();
                answerer.stop();
                penAsks = answerer.answered;
            }

            // ---- ② 页面上那颗「重命名…」：整条路（输入框预填 → 真改盘上的目录名）----
            //
            // 这一颗原来在菜单里，现在在页面上（要求「改文件的设置上方是重命名
            // 打开文件夹 删除世界」）。两问仍然是**串**在一条线上的：`getText()` 那一问
            // 要等它自己那层嵌套循环 —— 所以答它那一发**只能在点那颗按钮之前排**，
            // 点下去之后同步就进了那一层。
            {
                OverlayHost *openedHost = currentPageOverlay();
                auto *opened = openedHost != nullptr
                                   ? openedHost->findChild<InstanceSettingsDialog *>()
                                   : nullptr;
                WorldSettingsPage *worldPage =
                    opened != nullptr ? opened->worldSettingsPage() : nullptr;
                pageOpened = worldPage != nullptr && !worldPage->isHidden();
                // 顺手记下这一页顶上那颗标题 —— 下面要拿它量"开出来的确实是**这一行**那个
                // 世界的页"，而那一行 `level.dat` 解不出来，所以是坏档那一档的说法。
                if (worldPage != nullptr) {
                    pageTitle = worldPage->breadcrumbTitle();
                }
                QTimer::singleShot(0, [&] {
                    InlineMessageBox *input = currentPrompt();
                    if (input == nullptr) {
                        return;
                    }
                    auto *edit = input->findChild<QLineEdit *>(InlineMessageBox::lineEditName());
                    if (edit != nullptr) {
                        prefilled = edit->text();
                        edit->setText(QStringLiteral("笔改的"));
                    }
                    if (auto *ok =
                            input->findChild<QPushButton *>(InlineMessageBox::buttonName(0))) {
                        ok->click();
                    } else {
                        input->close();
                    }
                });
                if (worldPage != nullptr) {
                    worldPage->renameButton()->click();
                }
            }

            // ---- ③ 「批量编辑」：今天是个占位 ----
            dismissNextPrompt();
            emit saves->batchEditRequested();
            batchTitle = g_modalTitle;
            batchText = g_modalText;

            // =============================================================
            // T27.7 「世界设置…」整条路：真的读一份存档、改规则、写回去
            // =============================================================
            //
            // 上面 T27.6 量的是"那支笔接上了没有"，这一段量的是**那一页本身**：盘上一份
            // 真的 gzip `level.dat` → 点那支笔 → 页面上的值就是盘上那些 → 改三格 →
            // 「保存」→ 再读回盘上那一份核对（顺带看备份留没留、没碰过的地方动没动）。
            //
            // 「打开之前弹出坏档警告并提供备份按钮」那半句**改了** —— 用户
            // 原话「点击笔的时候不要弹备份提示了」。坏档**照样判**（读不动 ⇒ 直接开
            // 「读不出来」那一档，原因写在页里），备份**照样有**（页里那颗「备份一份」），
            // 只是**打开之前不再垫一问**。A 那半量"好档直接进页"，B 那半量"坏档也直接进页
            // + 在页里点备份那一颗"。
            //
            // 为什么非要在这儿量：`build/verify` 的 ZL 那一节量的是**纯函数 + 真文件**
            // （读得对不对 / 写得对不对），ui_check 的 Z16 量的是**页面上摆得对不对**
            // （它一个字节都不碰盘）。两头中间那一段 —— MainWindow 的翻译
            // （`worldViewOf` / `applyWorldViewTo`）、"读不动就摆成读不出来那一档"、
            // 保存那几道闸 —— **只有这里量得到**。
            //
            // NOTE: 这一段里"接住问句"用的是 `PromptAnswerer`（自走的定时器 + 脚本），不是
            // T27.6 那种"在 handler 里再排一发"的写法：这里的问句是**一串**
            // （保存之前那几道闸、备份那一颗的回话…），而两条之间那条路是同步跑完的
            // —— 排发那套要么套三层，要么被代数顶掉（见 g_modalHandlerGeneration 那条
            // NOTE）。脚本最后一位顶着"多出来的问句"，所以**真多弹一个也不会卡死**，
            // 那一位会被下面的断言抓住（而不是让测试挂在一个没人答的框上）。
            // 点笔那一发也包在里面：按用户点名的要求，它**一个框都不该弹** ——
            // 真弹了的话框会被当场点掉，断言红在"不该弹东西"上。
            {
                const QString worldDir = versionDir + QStringLiteral("/saves/笔改的");
                const QString brokenDir = versionDir + QStringLiteral("/saves/坏掉的");
                const QByteArray brokenBytes("not-a-level-dat-at-all");
                // 盘上那个世界名故意和目录名**不一样**：页面上那一格要是显示的是目录名，
                // 下面「值是从 level.dat 里来的」那一条就红。
                const QString diskLevelName = QStringLiteral("盘上写的世界");

                const auto askTitle = [](const QList<PromptAnswerer::Answered> &list, int index) {
                    return index < list.size() ? list.at(index).title : QString();
                };
                const auto askText = [](const QList<PromptAnswerer::Answered> &list, int index) {
                    return index < list.size() ? list.at(index).text
                                               : QStringLiteral("<这一问没弹出来>");
                };
                const auto askNote = [](const QList<PromptAnswerer::Answered> &list, int index) {
                    return index < list.size() ? list.at(index).note : QString();
                };
                const auto askButtons = [](const QList<PromptAnswerer::Answered> &list, int index) {
                    return index < list.size() ? list.at(index).buttons : QStringList();
                };
                const auto samePath = [](const QString &a, const QString &b) {
                    const QString left = QFileInfo(a).canonicalFilePath();
                    const QString right = QFileInfo(b).canonicalFilePath();
                    return !left.isEmpty() && left.compare(right, Qt::CaseInsensitive) == 0;
                };
                const auto rowOfWorld = [&](const QString &dir) {
                    QListWidget *list = saves->list();
                    for (int i = 0; list != nullptr && i < list->count(); ++i) {
                        QListWidgetItem *item = list->item(i);
                        if (item != nullptr &&
                            samePath(item->data(InstanceFilesPage::pathRole()).toString(), dir)) {
                            return i;
                        }
                    }
                    return -1;
                };

                // ---------- 夹具 ----------
                //
                // 盘上换成一份**真的** `level.dat`：gzip 包着的 NBT。三条规则正好把三档
                // 控件都摆上（开关 / 只能填数字的框 / 随便填的框），其中 `moddedRule` 那条
                // 对应的就是"有模组会往 GameRules 里塞任意字符串"那一档
                // （`NbtTree::ValueKind::Text`）。
                {
                    const auto field = [](const QString &name, NbtTree::Node node) {
                        NbtTree::Field item;
                        item.first = name;
                        item.second = std::move(node);
                        return item;
                    };
                    const auto number = [](NbtTree::Type type, qint64 value) {
                        NbtTree::Node node;
                        node.type = type;
                        node.number = value;
                        return node;
                    };
                    const auto string = [](const QString &value) {
                        NbtTree::Node node;
                        node.type = NbtTree::Type::String;
                        node.text = value;
                        return node;
                    };
                    const auto compound = [](NbtTree::FieldList fields) {
                        NbtTree::Node node;
                        node.type = NbtTree::Type::Compound;
                        node.fields = std::move(fields);
                        return node;
                    };

                    NbtTree::Document document;
                    document.root.type = NbtTree::Type::Compound;
                    document.root.fields = NbtTree::FieldList{field(
                        QStringLiteral("Data"),
                        compound(NbtTree::FieldList{
                            field(QStringLiteral("LevelName"), string(diskLevelName)),
                            field(QStringLiteral("LastPlayed"),
                                  number(NbtTree::Type::Long, 1758960000000LL)),
                            field(QStringLiteral("DataVersion"),
                                  number(NbtTree::Type::Int, 3465)),
                            field(QStringLiteral("Difficulty"),
                                  number(NbtTree::Type::Byte, 2)),
                            field(QStringLiteral("allowCommands"),
                                  number(NbtTree::Type::Byte, 1)),
                            field(QStringLiteral("hardcore"), number(NbtTree::Type::Byte, 0)),
                            field(QStringLiteral("Version"),
                                  compound(NbtTree::FieldList{
                                      field(QStringLiteral("Name"),
                                            string(QStringLiteral("1.20.1"))),
                                      field(QStringLiteral("Id"),
                                            number(NbtTree::Type::Int, 3465))})),
                            // 这三条就是页面上那三行（顺序 = 盘上的顺序，界面不重排）。
                            field(QStringLiteral("GameRules"),
                                  compound(NbtTree::FieldList{
                                      field(QStringLiteral("keepInventory"),
                                            string(QStringLiteral("true"))),
                                      field(QStringLiteral("randomTickSpeed"),
                                            string(QStringLiteral("3"))),
                                      field(QStringLiteral("moddedRule"),
                                            string(QStringLiteral("whatever")))})),
                            // 我们没碰过的那一格：存完之后它必须**原样还在** ——
                            // "只改用户点名的那几格"这条保证就靠它量。
                            field(QStringLiteral("某个模组写的"), string(QStringLiteral("别动我"))),
                        }))};

                    const QString rawPath = root.path() + QStringLiteral("/t27-level.raw");
                    writeAt(rawPath, NbtTree::serialize(document));
                    QString gzWhy;
                    const bool gzipped =
                        ZipUtil::gzipFrom(rawPath, WorldFiles::levelDataPath(worldDir), &gzWhy);
                    QFile::remove(rawPath);
                    check(gzipped, "★ T27.7 盘上摆出来的那份 level.dat 是真的（gzip 过的 NBT）",
                          gzWhy);
                }
                // 这一份字节读完**必须把句柄关掉**（所以套一层花括号）：下面保存那一下是
                // `QSaveFile::commit()` → `MoveFileEx(REPLACE_EXISTING)` 换掉 level.dat，
                // 而那一步要求目标**没有别人开着**。测试自己攥着一个读句柄的话，换名会
                // 以"拒绝访问"失败 —— 症状是产品弹「保存失败」、盘上一个字没变，而这一节
                // 每一条断言都红得像是"翻译错了 / 没写盘"（测试自己攥着一个活到本节末尾的
                // `QFile` 就会踩到）。产品里没有这一档（没人攥着 level.dat），
                // 是测试自己的手。
                QByteArray beforeBytes;
                {
                    QFile freshFile(WorldFiles::levelDataPath(worldDir));
                    if (freshFile.open(QIODevice::ReadOnly)) {
                        beforeBytes = freshFile.readAll();
                    }
                }
                // 这一份字节后面有两处要拿它比：备份里那份是不是**原件**（逐字节相同，
                // 不是我们解了又压回去的那一份）。
                check(beforeBytes.startsWith(QByteArray("\x1f\x8b", 2)),
                      "★ T27.7 那一份的头两个字节是 gzip 的魔数（拿它当「备份是原件」的基准）");
                {
                    WorldFiles::LevelData probe;
                    QString probeWhy;
                    const bool readable = WorldFiles::readLevelData(worldDir, &probe, &probeWhy);
                    check(readable, "★★ T27.7 产品自己读得动这一份（这条红了下面全是假绿）",
                          probeWhy);
                    check(probe.info.hasLevelName && probe.info.levelName == diskLevelName,
                          "★ T27.7 世界名是从 level.dat 里读出来的（不是目录名）",
                          probe.info.levelName);
                    check(probe.info.hasRules && probe.info.rules.size() == 3,
                          "★ T27.7 盘上那三条规则都读到了",
                          QString::number(probe.info.rules.size()));
                }

                // 坏档那一个世界：`level.dat` 在（所以列表里列得出来），但内容不是 NBT。
                writeAt(brokenDir + QStringLiteral("/level.dat"), brokenBytes);
                page->refreshFilePages();

                // ---------- A：好档 ----------
                const int rowHealthy = rowOfWorld(worldDir);
                check(rowHealthy >= 0, "★★ T27.7 存档那一页里找得到「笔改的」这一行");
                QList<PromptAnswerer::Answered> openAsks;
                {
                    // 那支笔点下去**一句话都不问**（要求「点击笔的时候不要
                    // 弹备份提示了」）。原来这里垫着一问「打开 / 先备份一份 / 取消」，现在
                    // 它整个删掉了 —— 点笔 = 直接进页。
                    // （外面还是包着 PromptAnswerer：万一那一问又回来了，框当场被点掉、
                    // 下面那条断言红在"不该弹东西"上，而不是让这一节挂住。）
                    PromptAnswerer answerer({0});
                    answerer.start();
                    if (const auto pen = rowHealthy >= 0 ? uiEditAt(saves, rowHealthy) : nullptr) {
                        pen->click();
                    }
                    answerer.stop();
                    openAsks = answerer.answered;
                }
                check(openAsks.isEmpty(),
                      "★★ T27.7 点那支笔**不弹任何提示框**（打开之前那一问已经没有了）",
                      openAsks.isEmpty() ? QString() : openAsks.first().title);

                WorldSettingsPage *world = page->worldSettingsPage();
                check(world != nullptr && !world->isHidden(),
                      "★★ T27.7 那一格世界设置真的切出来了（不是还停在世界列表上）");
                const auto ruleRowOf = [world](const QString &name) {
                    for (int i = 0; world != nullptr && i < world->ruleList()->count(); ++i) {
                        if (world->ruleList()->item(i)
                                ->data(WorldSettingsPage::ruleNameRole())
                                .toString() == name) {
                            return i;
                        }
                    }
                    return -1;
                };
                if (world != nullptr) {
                    check(world->breadcrumbTitle() == QStringLiteral("世界设置：笔改的"),
                          "T27.7 页面上那颗标题写的是哪一个世界", world->breadcrumbTitle());
                    if (auto *crumbHost = currentPageOverlay()) {
                        check(crumbHost->pageTitle() == QStringLiteral("世界设置：笔改的") &&
                                  world->findChild<QPushButton *>(
                                      QStringLiteral("worldSettingsBackButton")) == nullptr,
                              "★★ #74 页眉面包屑写「世界设置：<世界名>」，页里不再有第二个「返回」",
                              crumbHost->pageTitle());
                    }
                    check(!world->isUnreadable(),
                          "T27.7 这一档是好的，不该摆成「读不出来」那一档");
                    check(world->nameEdit()->text() == diskLevelName,
                          "★★ T27.7 世界名那一格里的值是**盘上那份 level.dat 里的**",
                          world->nameEdit()->text());
                    QStringList names;
                    QHash<QString, int> rowOfName;
                    for (int i = 0; i < world->ruleList()->count(); ++i) {
                        const QString name = world->ruleList()
                                                 ->item(i)
                                                 ->data(WorldSettingsPage::ruleNameRole())
                                                 .toString();
                        names << name;
                        rowOfName.insert(name, i);
                    }
                    // 这一列现在是**并过全集的**（要求「下面的世界设置不要
                    // 要求进了世界才显示而是把所有的都列出来」）—— 盘上只有三条，而屏幕上
                    // 是"启动器认识的那几十条 + 盘上多出来的 `moddedRule`"，按名字排序。
                    //
                    // 这一条量的正是 MainWindow 那一段翻译（`worldViewOf()` 有没有真去问
                    // `WorldFiles::allGameRules()`）：只摆盘上那三条的话它就是红的。
                    check(names.size() >= 40 && rowOfName.contains(QStringLiteral("keepInventory")) &&
                              rowOfName.contains(QStringLiteral("randomTickSpeed")) &&
                              rowOfName.contains(QStringLiteral("moddedRule")),
                          "★★ T27.7 那一列是**全集**（盘上那三条 + 启动器认识的那几十条），"
                          "不是「盘上有几条就摆几条」",
                          QStringLiteral("%1 条").arg(names.size()));
                    // 排序是**大小写不敏感**的（`allGameRules()` 那一头排的），所以
                    // `moddedRule`（m）落在 `keepInventory`（k）和 `randomTickSpeed`（r）
                    // 中间 —— 它是**盘上有、全集里没有**的那一条（从盘上捡回来的）。
                    // 不排序的实现会把它甩到所有小写名字后面（大写在 ASCII 里排在小写
                    // 前面），那正是这一条要钉的。
                    check(rowOfName.value(QStringLiteral("keepInventory")) <
                              rowOfName.value(QStringLiteral("moddedRule")) &&
                              rowOfName.value(QStringLiteral("moddedRule")) <
                                  rowOfName.value(QStringLiteral("randomTickSpeed")),
                          "★ T27.7 名字是**排序过的**（`moddedRule` 落在 m 那一段里，"
                          "不是被甩到所有小写名字后面）",
                          QStringLiteral("k=%1 m=%2 r=%3")
                              .arg(rowOfName.value(QStringLiteral("keepInventory")))
                              .arg(rowOfName.value(QStringLiteral("moddedRule")))
                              .arg(rowOfName.value(QStringLiteral("randomTickSpeed"))));
                    bool sortedOk = true;
                    for (int i = 1; i < names.size(); ++i) {
                        if (QString::compare(names.at(i - 1), names.at(i), Qt::CaseInsensitive) >
                            0) {
                            sortedOk = false;
                            break;
                        }
                    }
                    check(sortedOk, "★ T27.7 那一列整份都是排好序的（大小写不敏感）",
                          sortedOk ? QString() : names.join(QLatin1Char('/')));
                    check(rowOfName.contains(QStringLiteral("doDaylightCycle")),
                          "★ T27.7 盘上**没有**的那一条也在（`doDaylightCycle` —— 一个刚建好、"
                          "还没被游戏写过档的世界就是这样，原来那一页遇到这种档只写一句"
                          "「进游戏玩一次就会写进去」）");
                    // 刚摆好的时候**必须是干净的**：这一条量的是 MainWindow 那一段翻译
                    // （`worldViewOf()` 摆进去的那一份）和页自己那本台账对得上 ——
                    // 对不上的话，用户一进来什么都没改、按一下返回就被问"改过还没存"。
                    // ui_check 的 Z16a 拿手搓的 WorldView 量过同一件事，量不到这一段翻译。
                    check(!world->isDirty(),
                          "★★ T27.7 刚打开的时候这一页是干净的（什么都没改不该被问一句）");
                }

                // 三档控件是不是各就各位（这一条量的正是 MainWindow 那一段翻译）。
                const int rowBool = ruleRowOf(QStringLiteral("keepInventory"));
                const int rowInt = ruleRowOf(QStringLiteral("randomTickSpeed"));
                const int rowText = ruleRowOf(QStringLiteral("moddedRule"));
                QAbstractButton *boolToggle =
                    world != nullptr && rowBool >= 0 ? world->ruleToggleAt(rowBool) : nullptr;
                QLineEdit *intEditor =
                    world != nullptr && rowInt >= 0 ? world->ruleEditorAt(rowInt) : nullptr;
                QLineEdit *textEditor =
                    world != nullptr && rowText >= 0 ? world->ruleEditorAt(rowText) : nullptr;
                check(boolToggle != nullptr && boolToggle->isChecked(),
                      "★ T27.7 值写着 true 的那一条摆的是**开关**，而且是拨上去的");
                check(intEditor != nullptr && intEditor->text() == QStringLiteral("3"),
                      "★ T27.7 值是个整数的摆的是输入框，里面是盘上那个 3",
                      intEditor != nullptr ? intEditor->text() : QString());
                check(textEditor != nullptr && textEditor->text() == QStringLiteral("whatever"),
                      "★ T27.7 认不出形状的那一条摆的是随便填的框（值原样端出来）",
                      textEditor != nullptr ? textEditor->text() : QString());

                const auto hasDefaultMark = [world, &ruleRowOf](const QString &name) {
                    const int row = ruleRowOf(name);
                    QWidget *host =
                        world == nullptr || row < 0 ? nullptr : world->ruleList()->itemWidget(
                                                                  world->ruleList()->item(row));
                    const auto *mark =
                        host == nullptr
                            ? nullptr
                            : host->findChild<QLabel *>(WorldSettingsPage::ruleDefaultMarkName());
                    return mark != nullptr && !mark->text().isEmpty();
                };
                check(!hasDefaultMark(QStringLiteral("keepInventory")) &&
                          !hasDefaultMark(QStringLiteral("moddedRule")) &&
                          hasDefaultMark(QStringLiteral("doDaylightCycle")),
                      "★★ T27.7 盘上有的那几条**不挂**「默认」，盘上没有的那条挂上 —— 混在"
                      "一列里不标出来，用户会以为「屏幕上摆的就是这个世界本来的样子」");

                // ---------- ：「修改存档文件」那个折叠框 ----------
                check(world != nullptr && !world->fileEditingOpen() &&
                          world->fileBody()->isHidden(),
                      "★★ T27.7 「修改存档文件」那个折叠框**默认收起**（世界名 / 允许作弊 / "
                      "极限模式 / 难度这四格一个都不该一进来就摊着）");
                QList<PromptAnswerer::Answered> boxAsks;
                {
                    // 一问：展开那一句备份警告（点第 3 颗「继续修改」）。
                    PromptAnswerer answerer({2});
                    answerer.start();
                    if (world != nullptr) {
                        world->fileToggle()->click();
                    }
                    answerer.stop();
                    boxAsks = answerer.answered;
                }
                check(boxAsks.size() == 1,
                      "★★ T27.7 展开那个框**一共一问**（就是那句备份警告）",
                      QString::number(boxAsks.size()));
                check(askTitle(boxAsks, 0) == QStringLiteral("改动存档文件"),
                      "★★ T27.7 展开弹的是那一句备份警告", askTitle(boxAsks, 0));
                check(askButtons(boxAsks, 0) ==
                          QStringList({QStringLiteral("取消"), QStringLiteral("先创建备份"),
                                       QStringLiteral("继续修改")}),
                      "★★ T27.7 那三颗是「取消 / 先创建备份 / 继续修改」（用户要的那颗备份就在"
                      "这一问里，写进存档的「继续修改」排最后）",
                      askButtons(boxAsks, 0).join(QLatin1Char('/')));
                check(world != nullptr && world->fileEditingOpen() &&
                          !world->fileBody()->isHidden(),
                      "★★ T27.7 答「继续修改」之后框**真的开着**（那四格露出来了）");
                {
                    // 收起来那一下**不该弹任何东西**（页面发的是 `fileEditingRequested(false)`，
                    // MainWindow 那一头直接返回）。
                    PromptAnswerer answerer({0});
                    answerer.start();
                    if (world != nullptr) {
                        world->fileToggle()->click();
                    }
                    answerer.stop();
                    check(answerer.answered.isEmpty() && world != nullptr &&
                              world->fileBody()->isHidden(),
                          "★ T27.7 用户自己把框收起来时**不弹那一问**（那不是「想改文件了」，"
                          "是「不改了」）",
                          QString::number(answerer.answered.size()));
                }
                {
                    // 再展开一次，这次答「取消」⇒ 框**收回去**（不收回来的话屏幕上那个框
                    // 开着、而用户什么也没答应，看着像那句警告白弹了）。
                    PromptAnswerer answerer({0});
                    answerer.start();
                    if (world != nullptr) {
                        world->fileToggle()->click();
                    }
                    answerer.stop();
                    check(answerer.answered.size() == 1 && world != nullptr &&
                              !world->fileEditingOpen() && world->fileBody()->isHidden() &&
                              world->fileToggle()->findChild<QLabel *>() != nullptr &&
                              world->fileToggle()->findChild<QLabel *>()->text() ==
                                  QStringLiteral("展开"),
                          "★★ T27.7 那一问答「取消」之后框**收回去**（不许留一个开了的框在"
                          "屏幕上，用户什么都没答应）");
                }

                // 改四格：世界名、那条开关（true → false）、那个数字（3 → 7），以及
                // **盘上根本没有的那一条**（`doDaylightCycle`：true → false）。
                // `moddedRule` 故意**一个字都不动** —— 它是下面"没动的那条不许变"的对照。
                //
                // 最后那一格是 加的，盯的是 `MissingField::Create` 那一路：
                // 盘上没有这一条的时候不能以"改不了"收场（用户刚在屏幕上拨了一下、界面
                // 也答应了，然后告诉他存不下去，那是最坏的一种界面）。
                QAbstractButton *defaultToggle = nullptr;
                if (world != nullptr) {
                    const int rowDefault = ruleRowOf(QStringLiteral("doDaylightCycle"));
                    defaultToggle = rowDefault >= 0 ? world->ruleToggleAt(rowDefault) : nullptr;
                }
                if (world != nullptr) {
                    world->nameEdit()->setText(QStringLiteral("改过名的世界"));
                    if (boolToggle != nullptr) {
                        boolToggle->click();
                    }
                    if (intEditor != nullptr) {
                        intEditor->setText(QStringLiteral("7"));
                    }
                    if (defaultToggle != nullptr) {
                        defaultToggle->click();  // 默认是 true ⇒ 拨下去变 false
                    }
                }

                // 改过还没存的时候按「返回」**必须先问一句**（和「配置文件」那一页同一条
                // 规矩）。判据是页自己那本台账（`WorldSettingsPage::isDirty()`）——
                // 那条路的另一半（"存过之后**不**该再问"）就在下面几步之后。
                // 这一点**先量、后存**：反过来的话，页面上已经没有"改过还没存"这一档了。
                QList<PromptAnswerer::Answered> dirtyAsks;
                {
                    // 脚本点第 2 颗「留下来继续改」—— 问完人还得留在这一页上（下面那几格
                    // 里改的东西**不许**被这一下丢掉）。
                    PromptAnswerer answerer({1});
                    answerer.start();
                    if (world != nullptr) {
                        if (auto *overlayHost = currentPageOverlay()) {
                            if (auto *headerBack = overlayHost->findChild<QPushButton *>(
                                    OverlayHost::overlayBackButtonName())) {
                                headerBack->click();
                            }
                        }
                    }
                    answerer.stop();
                    dirtyAsks = answerer.answered;
                }
                check(dirtyAsks.size() == 1 &&
                          askTitle(dirtyAsks, 0) == QStringLiteral("有未保存的更改"),
                      "★★ T27.7 改过还没存就按返回 ⇒ 必须问一句（不许悄悄丢掉）",
                      dirtyAsks.isEmpty() ? QStringLiteral("<一句都没问>") : askTitle(dirtyAsks, 0));
                check(askButtons(dirtyAsks, 0) ==
                          QStringList({QStringLiteral("返回并丢掉"),
                                       QStringLiteral("留下来继续改")}),
                      "★ T27.7 那两颗是「返回并丢掉 / 留下来继续改」（默认那颗是留下）",
                      askButtons(dirtyAsks, 0).join(QLatin1Char('/')));
                check(world == nullptr || !world->isHidden(),
                      "★★ T27.7 答「留下来继续改」之后人还在这一页上（没被赶回列表）");
                check(world != nullptr && world->nameEdit() != nullptr &&
                          world->nameEdit()->text() == QStringLiteral("改过名的世界"),
                      "★ T27.7 那一问不吃掉已经改好的那一格");

                QList<PromptAnswerer::Answered> saveAsks;
                {
                    // 存这一下**不该弹任何框**：四道闸全过（没在跑游戏、盘上没被人动过、
                    // 读得动、备份留得下）。真弹了一个就被这一发收掉，下面那条断言会红 ——
                    // 不设备份那一位的话，测试会挂在一个没人答的框上。
                    PromptAnswerer answerer({1});
                    answerer.start();
                    if (world != nullptr) {
                        world->saveButton()->click();
                    }
                    answerer.stop();
                    saveAsks = answerer.answered;
                }
                check(saveAsks.isEmpty(), "★★ T27.7 保存那一下一个问题都不该问（四道闸全过）",
                      saveAsks.isEmpty() ? QString() : askTitle(saveAsks, 0));
                check(world != nullptr &&
                          world->noteLabel()->text().contains(QStringLiteral("已保存")),
                      "★ T27.7 存完之后底下那行字改口说「已保存」",
                      world != nullptr ? world->noteLabel()->text() : QString());
                check(world != nullptr &&
                          world->noteLabel()->text().contains(QStringLiteral("已有 level.dat 备份")),
                      "★ T27.7 而且它知道备份已经有了（存完那一刻 MainWindow 告诉了它）");

                // ---------- 读回来核对 ----------
                {
                    WorldFiles::LevelData after;
                    QString afterWhy;
                    const bool readable = WorldFiles::readLevelData(worldDir, &after, &afterWhy);
                    check(readable, "★★ T27.7 存完之后这一份还读得动（写坏了的话下面全红）",
                          afterWhy);
                    check(after.info.levelName == QStringLiteral("改过名的世界"),
                          "★★ T27.7 页面上改的世界名真的写进盘了", after.info.levelName);
                    QString keep;
                    QString tick;
                    QString modded;
                    for (const NbtTree::GameRule &rule : after.info.rules) {
                        if (rule.name == QStringLiteral("keepInventory")) {
                            keep = rule.value;
                        } else if (rule.name == QStringLiteral("randomTickSpeed")) {
                            tick = rule.value;
                        } else if (rule.name == QStringLiteral("moddedRule")) {
                            modded = rule.value;
                        }
                    }
                    check(keep == QStringLiteral("false"),
                          "★★ T27.7 拨下去的那条开关写成了 false（Bool 那一档）", keep);
                    check(tick == QStringLiteral("7"),
                          "★★ T27.7 填进去的那个数字写进盘了（Int 那一档）", tick);
                    check(modded == QStringLiteral("whatever"),
                          "★★ T27.7 页面上没动的那一条一个字都没变（Text 那一档）", modded);
                    QString daylight;
                    bool daylightFound = false;
                    for (const NbtTree::GameRule &rule : after.info.rules) {
                        if (rule.name == QStringLiteral("doDaylightCycle")) {
                            daylightFound = true;
                            daylight = rule.value;
                        }
                    }
                    check(daylightFound && daylight == QStringLiteral("false"),
                          "★★ T27.7 盘上**没有**的那一条规则，拨过之后被建出来了（值也写对了）"
                          "—— 老档连 `Data/GameRules` 那一格都没有的时候也走得通",
                          QStringLiteral("%1 / %2").arg(daylightFound ? QStringLiteral("在")
                                                                      : QStringLiteral("不在"),
                                                        daylight));
                    check(after.info.rules.size() == 4,
                          "★★ T27.7 只写进去**动过的那一条**：盘上原来三条 + 新建的一条 = 四条。"
                          "屏幕上那几十条默认值一条都没被塞进存档（这正是 `deltaFromOpened()` "
                          "那份差集存在的理由）",
                          QStringLiteral("%1 条").arg(after.info.rules.size()));
                    check(after.info.hasDifficulty && after.info.difficulty == 2 &&
                              after.info.hasVersionName &&
                              after.info.versionName == QStringLiteral("1.20.1"),
                          "★★ T27.7 页面上**没动**的那几项（难度 / 游戏版本）原样留着 —— 难度"
                          "现在也摆在那个折叠框里了（默认收起），用户没碰它就不许写",
                          QStringLiteral("%1 / %2").arg(after.info.difficulty).arg(
                              after.info.versionName));
                    const NbtTree::Node *extra = NbtTree::descend(
                        after.document.root, QStringLiteral("Data/某个模组写的"));
                    check(extra != nullptr && extra->type == NbtTree::Type::String &&
                              extra->text == QStringLiteral("别动我"),
                          "★★ T27.7 摸都没摸过的那一格（别的模组写的）原样还在 —— 这一条就是"
                          "「只改用户点名的那几格」");
                }

                // ---------- 备份 ----------
                {
                    // 格子名是「实例 id__世界名」（见 InstanceMaintenanceController.cpp 的
                    // worldBackupSlot()）—— 两个实例都有叫 `world` 的存档时不能再撞在一起。
                    // 格子名里最后那段路径哈希测试算不出来，按前缀找那一格。
                    const QString backupRoot = AppPaths::worldBackupDir();
                    QString dir;
                    for (const QString &slot :
                         QDir(backupRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                        if (slot.startsWith(QStringLiteral("T27-源实例__笔改的-"))) {
                            dir = backupRoot + QLatin1Char('/') + slot;
                            break;
                        }
                    }
                    check(!dir.isEmpty(),
                          "★★ T27.7 找得到「T27-源实例__笔改的」那一格备份", dir);
                    const QStringList gens =
                        QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    check(gens.size() == 1,
                          "★★ T27.7 写下去之前真的留了一份原来的 level.dat（一代）",
                          QString::number(gens.size()));
                    QFile backup(gens.isEmpty()
                                     ? QString()
                                     : dir + QLatin1Char('/') + gens.first() +
                                           QStringLiteral("/level.dat"));
                    const QByteArray backupBytes =
                        backup.open(QIODevice::ReadOnly) ? backup.readAll() : QByteArray();
                    check(!backupBytes.isEmpty() && backupBytes == beforeBytes,
                          "★★ T27.7 备份里那份是**改之前盘上的原件**（逐字节相同，不是我们解了"
                          "又压回去的那一份）");
                }

                // ---------- A 的收尾：按返回回列表 ----------
                QList<PromptAnswerer::Answered> backAsks;
                {
                    PromptAnswerer answerer({1, 0});
                    answerer.start();
                    if (world != nullptr) {
                        if (auto *overlayHost = currentPageOverlay()) {
                            if (auto *headerBack = overlayHost->findChild<QPushButton *>(
                                    OverlayHost::overlayBackButtonName())) {
                                headerBack->click();
                            }
                        }
                    }
                    answerer.stop();
                    backAsks = answerer.answered;
                }
                check(backAsks.isEmpty(),
                      "★★ T27.7 存过之后按返回不该再问「改过的东西还没存」（基线跟着往前挪了）",
                      backAsks.isEmpty() ? QString() : askTitle(backAsks, 0));
                check(world == nullptr || world->isHidden(),
                      "★ T27.7 返回之后回到的是世界列表那一格");
                if (auto *crumbHost = currentPageOverlay()) {
                    check(crumbHost->pageOpen() && crumbHost->pageTitle() == QStringLiteral("实例设置"),
                          "★★ #74 点面包屑的「返回」回到存档列表（整页没关），页眉改回「实例设置」",
                          crumbHost->pageTitle());
                }

                // ---------- #70 Esc 统一返回：世界设置页有改动先确认 → 返回存档列表 → 顶层 Esc 关闭 ----------
                const int rowEsc = rowOfWorld(worldDir);
                if (const auto penEsc = rowEsc >= 0 ? uiEditAt(saves, rowEsc) : nullptr) {
                    penEsc->click();
                    QApplication::processEvents();
                }
                if (world != nullptr && !world->isHidden()) {
                    {
                        PromptAnswerer expandAnswerer({2});
                        expandAnswerer.start();
                        world->fileToggle()->click();
                        expandAnswerer.stop();
                    }
                    // 下拉打开时按 Esc 只关下拉，不返回存档列表。
                    if (QComboBox *diff = world->difficultyCombo()) {
                        diff->showPopup();
                        QApplication::processEvents();
                        if (QWidget *popup = QApplication::activePopupWidget()) {
                            QKeyEvent escPopup(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                            QApplication::sendEvent(popup, &escPopup);
                            QApplication::processEvents();
                        }
                        check(QApplication::activePopupWidget() == nullptr && !world->isHidden(),
                              "★★ #70 世界设置页下拉打开时按 Esc 只关下拉，不退出世界设置");
                    }
                    // 有未保存改动时按 Esc 与点「返回」一样先弹确认框；在确认框里按 Esc 只关确认框。
                    world->nameEdit()->setText(QStringLiteral("Esc临时改动"));
                    QList<PromptAnswerer::Answered> escDirtyAsks;
                    {
                        PromptAnswerer answerer({-1});  // -1 = 确认框上按 Esc / 取消
                        answerer.start();
                        QKeyEvent escDirty(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                        QApplication::sendEvent(world->nameEdit(), &escDirty);
                        answerer.stop();
                        escDirtyAsks = answerer.answered;
                    }
                    check(escDirtyAsks.size() == 1 &&
                              askTitle(escDirtyAsks, 0) == QStringLiteral("有未保存的更改") &&
                              !world->isHidden(),
                          "★★ #70 世界设置页有未保存改动时按 Esc 先弹确认框，取消后仍留在世界设置页");
                    // 确认丢弃后按 Esc 回到存档列表，不关掉整个实例设置页。
                    {
                        PromptAnswerer answerer({0});  // 0 = 「返回并丢掉」
                        answerer.start();
                        QKeyEvent escLeave(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                        QApplication::sendEvent(world->nameEdit(), &escLeave);
                        answerer.stop();
                    }
                    QApplication::processEvents();
                    check(world->isHidden() && currentPageOverlay() != nullptr &&
                              currentPageOverlay()->pageOpen() &&
                              currentPageOverlay()->pageTitle() == QStringLiteral("实例设置"),
                          "★★ #70 世界设置页按 Esc 回到存档列表，整页未关、页眉恢复「实例设置」");
                }

                // ---------- B：坏档 ----------
                //
                // 读不动的那一份**也走同一个入口**（点笔 → 直接开页），只是开出来的是"读
                // 不出来"那一档 —— 所以这一段分两步量：
                // ① 点笔：**一句话都不问**（ 要求「点击笔的时候不要弹备份
                // 提示了」—— 原来这里弹的是「打开（只读）/ 先备份一份 / 取消」那一问）；
                // 人直接站在那一页上，而那一页是"读不出来"那一档。
                // ② 在这一档上点页里那颗「备份一份」⇒ 报「备份好了」。
                //
                // 备份这件能力**一件都没少**：它从"打开之前那一问里的第 2 颗"换成了页里
                // 那一颗（原来那一问的那颗按钮本来就是调同一个 `backupRequested()`）。
                const int rowBroken = rowOfWorld(brokenDir);
                check(rowBroken >= 0, "★ T27.7 坏掉的那个世界在列表里有一行");
                QList<PromptAnswerer::Answered> brokenAsks;
                {
                    PromptAnswerer answerer({0});
                    answerer.start();
                    if (const auto pen = rowBroken >= 0 ? uiEditAt(saves, rowBroken) : nullptr) {
                        pen->click();
                    }
                    answerer.stop();
                    brokenAsks = answerer.answered;
                }
                check(brokenAsks.isEmpty(),
                      "★★ T27.7 坏档那一份点笔也**不弹任何提示框**（读不出来的原因改在页里说）",
                      brokenAsks.isEmpty() ? QString() : askTitle(brokenAsks, 0));
                check(world != nullptr && !world->isHidden(),
                      "★★ T27.7 坏档也是**直接开页**（不是像原来那样先弹一问、点「先备份」就"
                      "不开页）");
                check(world != nullptr && world->isUnreadable(),
                      "★★ T27.7 而且开出来的是**「读不出来」那一档**（我们不往读不出来的存档"
                      "上写东西）");
                if (world != nullptr && world->isUnreadable()) {
                    check(world->breadcrumbTitle() ==
                              QStringLiteral("存档读取失败：坏掉的"),
                          "★★ T27.7 标题上写着「这个世界可能坏了」、还带着是哪一个世界",
                          world->breadcrumbTitle());
                    // 那一句小字 = **原因原文** + "只能看和备份"（`refreshNote()` 那一档）。
                    // 量的是"原因真的搬到页里来了"（原来它在打开之前那一问的正文里）。
                    check(world->noteLabel()->text().contains(QStringLiteral("标签类型 110")) &&
                              world->noteLabel()->text().contains(QStringLiteral("备份")),
                          "★ T27.7 读不动的原因摆在页里（用户拿去搜、拿去问人都要它），"
                          "而且说明了这一档只能看和备份",
                          world->noteLabel()->text());
                    // 判据是 `isHidden()` 而不是 `isVisible()`（offscreen 上后者恒假，
                    // 见上面 A 那一段那条 NOTE）。
                    check(world->body()->isHidden(),
                          "★★ T27.7 这一档整块正文收着（那一堆值一个都不作数）");
                    check(world->fileBody()->isHidden(),
                          "★ T27.7 连带「修改存档文件」那个折叠框也收着（它是正文的一部分）");
                    check(!world->saveButton()->isVisibleTo(world),
                          "★ T27.7 这一档不给「保存」（没有东西可存）");
                    check(!world->fullBackupButton()->isHidden(),
                          "★★ T27.7 但「立即备份」还在 —— 读不动的那一份尤其该留");
                }
                // 页里没有只备份 level.dat 的按钮了：备份走顶上那颗「立即备份」（有自己的检查）。
            }

            // ---------- 收尾：关掉整个「实例设置」 ----------
            //
            // 这一关**也得接住**：`InstanceSettingsDialog::reject()` 上挂着那两道闸
            // （「配置文件」/「世界设置」改过还没存），存过之后它们**都不该响** —— 响了就是
            // 用户按了一下返回箭头被问一句"要丢掉吗"。这一发必须接：一次保存失败就会让
            // 它挂在「改过还没保存」那个框上（屏幕上什么都没发生、测试整个卡死）——
            // 脚本那一位顶着"真多问了一句"，下面那条断言会红，而不会让测试挂住。
            QList<PromptAnswerer::Answered> closeAsks;
            {
                PromptAnswerer answerer({0});  // 0 = 「丢掉改动」（真问了就认下这一句）
                answerer.start();
                // 世界设置页开着时，面包屑上的「返回」先退回存档列表，再点一次才关整页。
                for (int hop = 0; hop < 2; ++hop) {
                    if (auto *back =
                            host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName());
                        back != nullptr && host->pageOpen()) {
                        back->click();
                        QApplication::processEvents();
                    }
                }
                if (host->pageOpen()) {
                    host->dismiss();
                }
                answerer.stop();
                closeAsks = answerer.answered;
            }
            check(closeAsks.isEmpty(),
                  "★★ T27.7 存过之后关掉整页也不该再问「改过还没保存」（两道闸都该是静的）",
                  closeAsks.isEmpty() ? QString() : closeAsks.first().title);
        });
        QMetaObject::invokeMethod(&window, "openInstanceSettings", Qt::DirectConnection,
                                  Q_ARG(QString, id));
        QApplication::processEvents();
        check(probed, "★★ T27.6 那一页开出来了（这条红了下面全是假绿）");
        check(penFound, "★ T27.6 存档页第一行行尾那支笔真的拿到手了");
        const QString rowName = QFileInfo(rowPath).fileName();
        // 那支笔点下去**一句话都不问**（要求「点击笔的时候不要弹备份
        // 提示了」）。原来这里垫着一问「打开（只读）/ 先备份一份 / 取消」，现在它整个没了
        // —— 所以这一条量的是"没弹东西"，而不是"弹的那一问长什么样"。
        check(penAsks.isEmpty(),
              "★★ T27.6 点那支笔**不弹任何提示框**（那一问已经没有了）",
              penAsks.isEmpty() ? QString() : penAsks.first().title);
        check(pageOpened, "★★ T27.6 点完那支笔人真的站在「世界设置」那一页上"
                          "（不是还停在世界列表上）");
        // 这一行的 `level.dat` 是 T27.4 导进来的那 3 个字节 ⇒ 开出来的是**坏档**那一档的
        // 标题，而且里面带着**这一行**的世界名（点哪一行说哪一行）。
        check(!rowName.isEmpty() &&
                  pageTitle == QStringLiteral("存档读取失败：%1").arg(rowName),
              "★ T27.6 顶上那句带着**这一行**的世界名，而且是坏档那一档的说法", pageTitle);
        check(!prefilled.isEmpty() && prefilled == rowName,
              "★ T27.6 页里「重命名…」那一问的输框预填的就是**现在这个名字**（改名不用重打"
              "一遍）",
              prefilled);
        // 「重命名…」那条路真的改了盘（那一问之后）。
        const QString parent = QFileInfo(rowPath).absolutePath();
        check(QFileInfo::exists(parent + QStringLiteral("/笔改的/level.dat")) &&
                  !QFileInfo::exists(rowPath),
              "★★ T27.6 页里那颗「重命名…」整条路走通了（盘上的目录名真的换了）",
              QStringLiteral("%1 → 笔改的").arg(rowPath));
        check(batchTitle == QStringLiteral("批量编辑"),
              "★ T27.6 顶上「批量编辑」那一颗接上了（弹的是「批量编辑」）", batchTitle);
        check(batchText.contains(QStringLiteral("暂不支持")),
              "★★ T27.6 而且明说「暂不支持」—— 占位不许装成能用", batchText);
    }

    header("T27.8 概览页那颗「删除实例」：接到 MainWindow，删完把页收掉");
    // 要求：「顺便把删除实例功能做了，按钮放在概览里」。这一节量的是**接线**：那一页发
    // 出来的 `deleteInstanceRequested` 真的走到了 `confirmDeleteInstance()`（两道闸 + 问一句
    // + 真删目录），以及**删完之后那一页自己收掉了没有**。
    //
    // 为什么"收页"值得单量：不收的话用户会停在一页对着已经被删掉的目录的设置上，随便动
    // 一格都会把 wtfml.meta 写回那个目录（`QDir().mkpath()` 会把它重新建出来）—— 盘上就
    // 多出一个只有 meta、没有 json 的空壳实例。判据是 `pageOpen()`（T27 这一段用的都是它）。
    //
    // NOTE: 这一节**排在最后**：它真把 `id` 那个目录删掉，后面的 T27.7 还要用它。
    {
        bool probed = false;
        bool keptOnCancel = false;
        bool askedOnCancel = false;
        bool dirUntouchedAfterCancel = false;
        bool productDismissed = false;
        QString askedTitle;
        QString askedText;
        QString secondTitle;

        QTimer::singleShot(0, [&] {
            OverlayHost *host = currentPageOverlay();
            auto *page = host != nullptr ? host->findChild<InstanceSettingsDialog *>() : nullptr;
            auto *button = page != nullptr
                               ? page->findChild<QPushButton *>(
                                     InstanceSettingsDialog::deleteButtonName())
                               : nullptr;
            if (button == nullptr) {
                return;
            }
            probed = true;

            // ① 先走**没删成**的那一条：问一句点「取消」⇒ 那一页必须还开着。
            //
            // 点那一颗之前先排好"答一问"：`ask()` 是同步的，点下去就会进它那层嵌套循环
            // （理由见 T27.6 那条 NOTE）。**取消是第二颗**（`question()` 的按钮表是
            // `{要问的那件事, 否}`，而删除这一问的默认按钮就是"否"）。
            {
                const int gen = ++g_modalHandlerGeneration;
                QTimer::singleShot(0, [&, gen] {
                    if (gen != g_modalHandlerGeneration) {
                        return;
                    }
                    InlineMessageBox *box = currentPrompt();
                    askedOnCancel = box != nullptr;
                    recordPrompt(box);
                    askedTitle = g_modalTitle;
                    askedText = g_modalText;
                    clickPromptButton(1);  // 「取消」
                });
                button->click();
                // 回到这儿说明那一问已经有结果了（`ask()` 是同步的）。
                keptOnCancel = host->pageOpen() &&
                               host->findChild<InstanceSettingsDialog *>() != nullptr;
                // "盘上一个字节都没动"**只能量在这儿**：下面第 ② 趟是**真要删**的，
                // 等整段跑完再问就已经晚了（那时量到的是删完之后的盘）。
                dirUntouchedAfterCancel = QFileInfo::exists(versionDir);
            }

            // ② 同一页上再来一次，这回点「删除」（第一颗）⇒ 页面收掉 + 目录真的没了。
            //
            // 「产品自己收页了没有」直接接那一发 `backPressed()`：它响了 = 产品真的喊了
            // "收页"（不管后面还有没有人接着收）。
            auto dismissConnection = QObject::connect(
                host, &OverlayHost::backPressed, host,
                [&productDismissed]() { productDismissed = true; });
            {
                const int gen = ++g_modalHandlerGeneration;
                QTimer::singleShot(0, [&, gen] {
                    if (gen != g_modalHandlerGeneration) {
                        return;
                    }
                    recordPrompt(currentPrompt());
                    secondTitle = g_modalTitle;
                    clickPromptButton(0);  // 「删除」
                });
                button->click();
                // NOTE: 收页是**同步**的（`dismiss()` → `backPressed()` → 那一页 `reject()`），
                // 但这一页收掉的最终样子（页对象 `deleteLater` 之后）要到下面那几发事件
                // 处理之后才量得到。
            }

            QObject::disconnect(dismissConnection);
            // 兜底：产品那一半要是没收到页，这一句把这一页收掉，好让下面那几条**报红**而不是
            // 把整个测试挂在 `invokeMethod` 那一行上（挂住的话一行信息都拿不到）。
            if (!productDismissed) {
                host->dismiss();
                QApplication::processEvents();
            }
        });
        QMetaObject::invokeMethod(&window, "openInstanceSettings", Qt::DirectConnection,
                                  Q_ARG(QString, id));
        // `processEvents()` 不递 `DeferredDelete`（`deleteLater()` 走的就是它）——
        // 页收掉之后那一份要等这一发才真的析构，下面 `findChild()` 才量得准。
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(probed, "★★ T27.8 那一页开出来了、那颗按钮也拿到手了（这条红了下面全是假绿）");
        check(askedOnCancel, "★★ T27.8 点下去**真的问了一句**才动手（不是直接就删）");
        check(askedTitle == QStringLiteral("删除实例"),
              "★★ T27.8 问的是「删除实例」那一问", askedTitle);
        check(askedText.contains(QDir::toNativeSeparators(versionDir)),
              "★★ T27.8 正文里把**要删的那个目录的完整路径**写出来了（用户要能看清删的是哪一个）",
              askedText);
        check(keptOnCancel,
              "★★ T27.8 点了「取消」⇒ 那一页**留在原地**（实例还在，用户正该停在这页上）");
        check(dirUntouchedAfterCancel, "★★ T27.8 而且盘上一个字节都没动");
        check(secondTitle == QStringLiteral("删除实例"),
              "★ T27.8 第二次点的还是「删除实例」那一问（不是别的东西弹出来了）", secondTitle);
        check(productDismissed,
              "★★ T27.8 删成之后**产品自己喊了收页**（不收的话下一格改动会把 wtfml.meta "
              "写回一个已经不存在的目录里，盘上多出一个空壳实例）");
        check(!QFileInfo::exists(versionDir), "★★ T27.8 那个实例目录真的删掉了",
              QDir::toNativeSeparators(versionDir));
        check(!window.findChild<InstanceSettingsDialog *>(),
              "★ T27.8 覆盖层里也不再挂着那一页了");
    }

    restoreSettings();
    check(AppPaths::minecraftDir() == originalDir, "T27.9 数据根目录换回来了");
}


