#include "Support.hpp"
void runDownloadFilterTests() {
    FakeResourceApi api;
    check(api.start(), "ZI0 本地假接口起来了");
    // 正文里**故意不给图标**（没有 logo 那一项）：给了的话 fillResourceHitRows 会顺手
    // requestIcon()，而那条地址在 media.forgecdn.net 上 —— 那就成了真外网。没有可用地址时
    // store 发的是一个空图标、一个请求都不发（见 ResourceStore::requestIcon 最后那一段）。
    api.searchBody = QByteArrayLiteral(
        "{\"data\":[{\"id\":1546263,\"name\":\"Arizona\",\"slug\":\"arizona\","
        "\"summary\":\"一句简介\",\"downloadCount\":642,"
        "\"authors\":[{\"name\":\"birdmuncher\"}],\"categories\":[{\"name\":\"Technology\"}],"
        "\"latestFilesIndexes\":[{\"gameVersion\":\"1.21.1\",\"modLoader\":4}]}],"
        "\"pagination\":{\"index\":0,\"pageSize\":20,\"resultCount\":1,\"totalCount\":1}}");

    // 接口根地址是 MainWindow **构造那一刻**从 QSettings 读进去的（LauncherSettings 构造 +
    // 构造里那句 syncCurseForgeSettings），所以必须"先写 ini、再造窗口"（和 AA10 同一个做法）。
    const QString kBaseKey = QStringLiteral("Launcher/CurseForgeApiBase");
    {
        QSettings sandbox;
        sandbox.setValue(kBaseKey, api.baseUrl());
        sandbox.sync();
    }
    // 身份：一个**新加的离线账号**（外观走本地，不发请求）。
    const QString accountName = QStringLiteral("Filters");
    {
        QString offlineError;
        check(AccountStore::addOffline(accountName, &offlineError),
              "ZI0 当前身份换成一个离线账号（这一节一个外网请求都不发）", offlineError);
    }

    {
        MainWindow w;
        w.show();
        QApplication::processEvents();

        MainWindowUI *ui = w.ui();
        const QString key = ResourceCatalog::kindKey(ResourceCatalog::Kind::Mod);
        QComboBox *sourceCombo = ui == nullptr ? nullptr : ui->getDownloadFilterSourceCombo(key);
        QComboBox *versionCombo = ui == nullptr ? nullptr : ui->getDownloadFilterVersionCombo(key);
        QComboBox *loaderCombo = ui == nullptr ? nullptr : ui->getDownloadFilterLoaderCombo(key);
        QComboBox *typeCombo = ui == nullptr ? nullptr : ui->getDownloadFilterTypeCombo(key);
        QLineEdit *nameEdit = ui == nullptr ? nullptr : ui->getDownloadSearchEdit(key);
        QPushButton *searchButton =
            ui == nullptr ? nullptr : ui->getDownloadFilterSearchButton(key);
        QPushButton *resetButton = ui == nullptr ? nullptr : ui->getDownloadFilterResetButton(key);
        QTreeWidget *tree = ui == nullptr ? nullptr : ui->getDownloadResourceTree(key);
        const bool haveAll = sourceCombo != nullptr && versionCombo != nullptr &&
                             loaderCombo != nullptr && typeCombo != nullptr &&
                             nameEdit != nullptr && searchButton != nullptr &&
                             resetButton != nullptr && tree != nullptr;
        check(haveAll, "ZI1 模组那一页上八件控件都在（名称/版本/加载器/来源/类型/搜索/清空/结果树）");
        if (!haveAll) {
            // 控件都不在，下面每一条都无从谈起（而且会当场解空指针）。直接把这一节收掉。
            QSettings sandbox;
            sandbox.remove(kBaseKey);
            sandbox.sync();
            return;
        }

        // 「这一下搜了几发」**只有日志数得准**：`startResourceSearch()` 每发一次就写一行
        // （「正在搜索「x」…」/「正在获取最热门的…」，见 MainWindow 里那两句 tr）。
        // 按**请求条数**数是不行的：同一页的第二发会把在飞的第一发 abort 掉
        // （ResourceStore::search 开头那句），假接口上那一发可能压根没记上 —— 于是"搜了两发"
        // 在请求表里看起来和"搜了一发"一模一样。这一段好几条要判的正是"只搜一发"。
        const auto searchLogs = [] {
            int count = 0;
            for (const QString &message : messagesSnapshot()) {
                if (message.contains(QStringLiteral("正在搜索")) ||
                    message.contains(QStringLiteral("正在获取最热门的"))) {
                    ++count;
                }
            }
            return count;
        };

        // ---- ZI1. 灌进去的选项：来源两项，且默认停在 Modrinth ----
        check(sourceCombo->count() == ResourceCatalog::allSources().size(),
              "ZI1 「来源」框里就是 allSources() 那几项（没有「全部」—— 一个页面上只搜一家）",
              QString::number(sourceCombo->count()));
        check(sourceCombo->currentData().toString() ==
                  ResourceCatalog::sourceKey(ResourceCatalog::Source::Modrinth),
              "ZI1 默认停在 Modrinth（加这一行之前那一趟的场景，行为不变）",
              sourceCombo->currentData().toString());
        check(typeCombo->count() > 0 && typeCombo->itemData(0).toString().isEmpty() &&
                  typeCombo->currentIndex() == 0,
              "ZI1 「类型」框的第一项是「全部」（键空串 ⇒ 不筛），而且已经选中它",
              QStringLiteral("count=%1 第一项键=%2")
                  .arg(typeCombo->count())
                  .arg(typeCombo->itemData(0).toString()));
        check(loaderCombo->count() > 0 && loaderCombo->itemData(0).toString().isEmpty() &&
                  loaderCombo->currentIndex() == 0,
              "ZI1 「模组加载器」框也一样（第一项「全部」）");

        // ---- ZI2. 换来源 = 换一份选项，而且**不搜索**（程序改的那一下不算"用户挑的"）----
        const int cfIndex = sourceCombo->findData(
            ResourceCatalog::sourceKey(ResourceCatalog::Source::CurseForge));
        check(cfIndex >= 0, "ZI2 来源框里找得到 CurseForge 那一项");
        api.requests.clear();
        const int searchesBeforeSource = searchLogs();
        sourceCombo->setCurrentIndex(cfIndex);
        QApplication::processEvents();
        const QList<ResourceCatalog::FilterOption> cfLoaders =
            ResourceCatalog::loaderOptions(ResourceCatalog::Source::CurseForge);
        const QList<ResourceCatalog::FilterOption> cfTypes =
            ResourceCatalog::categoryOptions(ResourceCatalog::Source::CurseForge,
                                             ResourceCatalog::Kind::Mod);
        bool loadersSwapped = loaderCombo->count() == cfLoaders.size();
        for (int i = 0; loadersSwapped && i < cfLoaders.size(); ++i) {
            loadersSwapped = loaderCombo->itemData(i).toString() == cfLoaders.at(i).key;
        }
        check(loadersSwapped,
              "ZI2 换成 CurseForge 之后「模组加载器」那几项的键换成 CF 那套（数字，不是 fabric）",
              QStringLiteral("框里 %1 项").arg(loaderCombo->count()));
        bool typesSwapped = typeCombo->count() == cfTypes.size();
        for (int i = 0; typesSwapped && i < cfTypes.size(); ++i) {
            typesSwapped = typeCombo->itemData(i).toString() == cfTypes.at(i).key;
        }
        check(typesSwapped, "ZI2 「类型」那几项也跟着换（Modrinth 是 slug、CF 是类目号）",
              QStringLiteral("框里 %1 项").arg(typeCombo->count()));
        check(loaderCombo->currentIndex() == 0 && typeCombo->currentIndex() == 0,
              "ZI2 两项都退回「全部」（拿 A 家的号去问 B 家会搜出个空的，而且不报错）");
        // NOTE: 这一下是**程序**改的（`setCurrentIndex`），不是用户在框里挑的。用户真挑一项时
        // Qt 还会再发一发 `activated`，而那一发**要搜**（见 ZI10）——
        // 所以这一条量的是"中间态不搜"，不是"换来源不搜"。
        check(api.requests.isEmpty() && searchLogs() == searchesBeforeSource,
              "ZI2 程序改（setCurrentIndex）**不发请求** —— 换选项这类「改到一半」的中间态不搜",
              QStringLiteral("搜了 %1 发：%2")
                  .arg(searchLogs() - searchesBeforeSource)
                  .arg(api.requests.join(QStringLiteral("|"))));

        // ---- ZI3. 五个条件都填上，按「搜索」键 ----
        const int loaderPick = cfLoaders.size() > 1 ? 1 : 0;  // 跳过第一项那一个「全部」
        const int typePick = cfTypes.size() > 1 ? 1 : 0;
        nameEdit->setText(QStringLiteral("jei"));
        versionCombo->setCurrentText(QStringLiteral("1.21.1"));
        loaderCombo->setCurrentIndex(loaderPick);
        typeCombo->setCurrentIndex(typePick);
        api.requests.clear();
        searchButton->click();
        const bool answered = pumpUntil([&api] { return !api.requests.isEmpty(); }, 10000);
        check(answered, "ZI3 按「搜索」键之后那一发真的发出去了", api.requests.join(QStringLiteral("|")));
        check(api.lastRequest().contains(QStringLiteral("classId=6")),
              "ZI3 带上「模组」那个类目号（这一类是靠它分出来的）", api.lastRequest());
        check(api.lastRequest().contains(QStringLiteral("searchFilter=jei")),
              "ZI3 名称框里那个词进了 searchFilter=（这一家的文本筛选叫这个名字；"
              "发 `search=` 它一个字都不筛、也不报错）",
              api.lastRequest());
        check(api.lastRequest().contains(QStringLiteral("gameVersion=1.21.1")),
              "ZI3 版本框里那串字进了 gameVersion=（那是用户**自己打的**，列表里根本没有它）",
              api.lastRequest());
        check(api.lastRequest().contains(
                  QStringLiteral("modLoaderType=") + cfLoaders.at(loaderPick).key),
              "ZI3 加载器框里选的那一项变成了 modLoaderType=", api.lastRequest());
        check(api.lastRequest().contains(
                  QStringLiteral("categoryId=") + cfTypes.at(typePick).key),
              "ZI3 类型框里选的那一项变成了 categoryId=", api.lastRequest());

        // ---- ZI4. 结果真的摆上了这一页（整条路走完，不是只发了一发） ----
        // NOTE: 判据不能只看「树里有 1 行」—— 按下那一刻 MainWindow 会先 `tree->clear()`
        // 再摆一行灰的「正在搜索「xx」…」（见 searchResourceKind 开头那段），而
        // **占位行也是一个顶层条目**（MainWindow 自己那句注释：「占位行（「正在搜索…」
        // 那几行）没有这一位」）。结果还没回来时那一行就是树里的唯一一行 ⇒ 只判"树里有
        // 1 行"会在"结果还没到"的时候立刻成立，紧接着那条读来源角色的断言就读到一个
        // 空串 —— 假接口答复快就绿、慢就红（实测同一条命令跑 5 次红 1 次）。
        // 真正要等的是**结果行**：它带着 source 角色，占位行不带。
        const auto isResultRow = [tree] {
            if (tree->topLevelItemCount() != 1) {
                return false;
            }
            return !tree->topLevelItem(0)
                        ->data(MainWindowUI::versionTreeKeyColumn(),
                               MainWindowUI::resourceSourceRole())
                        .toString()
                        .isEmpty();
        };
        const bool filled = pumpUntil(isResultRow, 10000);
        check(filled, "ZI4 结果摆进了这一页那棵树",
              QString::number(tree->topLevelItemCount()));
        if (tree->topLevelItemCount() > 0) {
            check(tree->topLevelItem(0)
                      ->data(MainWindowUI::versionTreeKeyColumn(),
                             MainWindowUI::resourceSourceRole())
                      .toString() == ResourceCatalog::sourceKey(ResourceCatalog::Source::CurseForge),
                  "ZI4 这一行的来源角色记的是 curseforge（点开第二层时靠它问对那一侧）",
                  tree->topLevelItem(0)
                      ->data(MainWindowUI::versionTreeKeyColumn(),
                             MainWindowUI::resourceSourceRole())
                      .toString());
        }

        // ---- ZI5. 「清空条件」：框都回默认，**并且立刻重搜一发** ----
        // 要求：「修改任何内容（不论是来源网站还是…）都刷新一下」。
        // 清完必须再搜一发：清完不动的话，界面和列表对不上 —— 六个框全空着，底下还摆着
        // 上一批按老条件筛出来的结果 —— 而那正是用户点这颗键要摆脱的东西。
        const int searchesBeforeReset = searchLogs();
        api.requests.clear();
        resetButton->click();
        const bool resetAnswered = pumpUntil([&api] { return !api.requests.isEmpty(); }, 10000);
        check(searchLogs() - searchesBeforeReset == 1,
              "ZI5 点「清空条件」**重搜一发**（清完的界面必须和列表对上）—— 而且只搜这一发",
              QStringLiteral("搜了 %1 发：%2")
                  .arg(searchLogs() - searchesBeforeReset)
                  .arg(api.requests.join(QStringLiteral("|"))));
        check(resetAnswered, "ZI5 那一发真的发出去了", api.requests.join(QStringLiteral("|")));
        check(nameEdit->text().isEmpty(), "ZI5 名称框清了", nameEdit->text());
        check(versionCombo->currentText().isEmpty(),
              "ZI5 版本框也清了（它是可输入的 ⇒ 读的是框里那串字，清不掉就等于没清）",
              versionCombo->currentText());
        check(loaderCombo->currentIndex() == 0 && typeCombo->currentIndex() == 0,
              "ZI5 加载器和类型都回到「全部」",
              QStringLiteral("加载器 %1 / 类型 %2")
                  .arg(loaderCombo->currentIndex())
                  .arg(typeCombo->currentIndex()));
        check(sourceCombo->currentData().toString() ==
                  ResourceCatalog::sourceKey(ResourceCatalog::Source::CurseForge),
              "ZI5 「来源」**不清**（它不是「条件」，是「去哪儿问」——清一下等于替用户换了个来源）",
              sourceCombo->currentData().toString());
        // 上面这一发（清空之后自动发的那一发）里的参数必须是**干净的**：空串 = 这一项压根
        // 不出现在地址里（读法只有 downloadFilterXxx 那四个访问器一处）。
        check(!api.lastRequest().contains(QStringLiteral("gameVersion=")) &&
                  !api.lastRequest().contains(QStringLiteral("modLoaderType=")) &&
                  !api.lastRequest().contains(QStringLiteral("categoryId=")),
              "ZI5 清空之后那一发里那三段参数一个都不在（空串 = 这一项不出现在地址里）",
              api.lastRequest());
        check(api.lastRequest().contains(QStringLiteral("searchFilter=")),
              "ZI5 searchFilter= 那一段照旧在（空词 = 这一类里最热门的，和第一次进页面同一个规矩）",
              api.lastRequest());

        // ---- ZI6. 两个输入框里按回车 == 按「搜索」键 ----
        // 版本那一格：**自己打一个列表里没有的版本号**，这正是它可输入的理由。
        QLineEdit *versionEdit = versionCombo->lineEdit();
        check(versionEdit != nullptr,
              "ZI6 版本那一格是可输入的（没有 lineEdit 就没有「可自行输入」这回事）");
        if (versionEdit != nullptr) {
            api.requests.clear();
            versionCombo->setCurrentText(QStringLiteral("1.20.1"));
            QMetaObject::invokeMethod(versionEdit, "returnPressed");
            const bool sent = pumpUntil([&api] { return !api.requests.isEmpty(); }, 10000);
            check(sent && api.lastRequest().contains(QStringLiteral("gameVersion=1.20.1")),
                  "ZI6 在版本框里按回车 == 按「搜索」键（那串手打的版本号真的发出去了）",
                  api.lastRequest());
        }
        api.requests.clear();
        nameEdit->setText(QStringLiteral("sodium"));
        QMetaObject::invokeMethod(nameEdit, "returnPressed");
        const bool nameSent = pumpUntil([&api] { return !api.requests.isEmpty(); }, 10000);
        check(nameSent && api.lastRequest().contains(QStringLiteral("searchFilter=sodium")),
              "ZI6 名称框里按回车也一样", api.lastRequest());

        // ---- ZI7. 数据包那一页：Modrinth 没有分类可选 ⇒ 整个「类型」那一栏藏起来 ----
        const QString packKey = ResourceCatalog::kindKey(ResourceCatalog::Kind::DataPack);
        QWidget *packTypeBox = ui->getDownloadFilterTypeBox(packKey);
        QComboBox *packSource = ui->getDownloadFilterSourceCombo(packKey);
        check(packTypeBox != nullptr && packSource != nullptr, "ZI7 数据包那一页也有筛选行");
        if (packTypeBox != nullptr && packSource != nullptr) {
            check(ResourceCatalog::categoryOptions(ResourceCatalog::Source::Modrinth,
                                                   ResourceCatalog::Kind::DataPack)
                      .isEmpty(),
                  "ZI7 前提：Modrinth 那边这一类压根没有分类可选（categoryOptions 回空表）");
            check(packTypeBox->isHidden(),
                  "ZI7 回空表 ⇒ 整个「类型」那一栏藏起来（摆一个只有「全部」的框等于骗人）");
            packSource->setCurrentIndex(packSource->findData(
                ResourceCatalog::sourceKey(ResourceCatalog::Source::CurseForge)));
            QApplication::processEvents();
            check(!packTypeBox->isHidden(),
                  "ZI7 换成 CurseForge 之后它又有分类可选了 ⇒ 那一栏回来");
        }

        // ---- ZI8. 整合包那一页那颗「导入整合包…」和实例页那颗一个待遇（忙态一起灰） ----
        QPushButton *packImport =
            ui->getDownloadFilterImportModpackButton();
        check(packImport != nullptr && packImport->isEnabled(),
              "ZI8 下载页那颗「导入整合包…」在，而且是亮的");
        QMetaObject::invokeMethod(&w, "setDownloadUiBusy", Qt::DirectConnection,
                                  Q_ARG(bool, true));
        check(packImport != nullptr && !packImport->isEnabled(),
              "ZI8 置忙时它跟着灰（同一个槽、同一条被顶回来的路，亮着那颗点了也白走一趟）");
        QMetaObject::invokeMethod(&w, "setDownloadUiBusy", Qt::DirectConnection,
                                  Q_ARG(bool, false));
        check(packImport != nullptr && packImport->isEnabled(), "ZI8 忙完又亮回来");

        // ---- ZI10. 「下拉即搜，文字回车才搜」----
        //
        // 要求：「修改任何内容（不论是来源网站还是…）都刷新一下」+「下拉即搜，文字回车才搜」。
        // 于是分工是：**在筛子里挑一项 = 立刻搜**；**框里打的字 = 只有回车那一下搜**。
        //
        // 「用户在下拉里挑了一项」这件事只有 `activated` 说得清，而 Qt 发它的地方是
        // itemSelected（鼠标/键盘挑）和 editingFinished（框里的字正好是清单里某一项）——
        // 所以下面这个小工具**照 Qt 的次序**模拟一次挑选：先 setCurrentIndex、再发 activated
        // （Qt 6.10 的 QComboBoxPrivate::itemSelected 就是这两句）。真实交互和它一一对应。
        //
        // NOTE: 每一条之间都要跑一次事件循环 —— 版本那一格有一道"同一回合只认第一发"的闸
        // （MainWindow 里 m_versionEnterSearches 那段），那笔账由 0 毫秒的单次定时器
        // 抹掉，不跑事件循环它就一直挂着、把下一条的那一发吃掉。
        const auto pickFromCombo = [](QComboBox *combo, int index) {
            combo->setCurrentIndex(index);
            combo->activated(index);
        };
        // 版本那一格这会儿还是空的（这一趟没去取在线清单），替"清单到了"补两项 ——
        // 「挑一项」这件事得有东西可挑才量得出来（灌清单那条路在 ZI1 之外由
        // fillDownloadFilterVersionOptions 管，这里只借它造个场面）。
        if (versionCombo->count() == 0) {
            versionCombo->addItem(QStringLiteral("1.21.1"));
            versionCombo->addItem(QStringLiteral("1.20.1"));
        }
        QApplication::processEvents();

        // ① 名称框里**打字**：一发都不搜（一个词一次请求，而这个接口按分钟限流 ——
        // 边打边搜等于把额度花在"打到一半的那些词"上）。
        int searchesBefore = searchLogs();
        nameEdit->setText(QStringLiteral("jei"));
        nameEdit->textEdited(QStringLiteral("jei"));
        QApplication::processEvents();
        check(searchLogs() == searchesBefore,
              "ZI10 名称框里打字**不发请求**（回车才算一发）",
              QStringLiteral("搜了 %1 发").arg(searchLogs() - searchesBefore));

        // ② 版本框里也一样 —— 而且这里那串字**正好是清单里的一项**（最容易漏的一侧：
        // 框里字一匹配，Qt 自己就会去动 currentIndex）。
        searchesBefore = searchLogs();
        versionEdit->setText(QStringLiteral("1.20.1"));
        versionEdit->textEdited(QStringLiteral("1.20.1"));
        QApplication::processEvents();
        check(searchLogs() == searchesBefore,
              "ZI10 版本框里打字**也不发**（哪怕框里那串字正好是清单里的一项）",
              QStringLiteral("搜了 %1 发").arg(searchLogs() - searchesBefore));

        // ③ 版本是在下拉里挑的 ⇒ 挑完就搜，而且只有一发。
        const int versionPick = versionCombo->findText(QStringLiteral("1.20.1"));
        searchesBefore = searchLogs();
        api.requests.clear();
        pickFromCombo(versionCombo, versionPick);
        const bool versionPicked = pumpUntil([&api] { return !api.requests.isEmpty(); }, 10000);
        check(versionPicked && searchLogs() - searchesBefore == 1 &&
                  api.lastRequest().contains(QStringLiteral("gameVersion=1.20.1")),
              "ZI10 在下拉里挑一个版本**立刻搜一发**（而且只搜这一发）",
              QStringLiteral("搜了 %1 发：%2")
                  .arg(searchLogs() - searchesBefore)
                  .arg(api.requests.join(QStringLiteral("|"))));
        QApplication::processEvents();

        // ④ 下面这一条是**这一节的重点**：框里的字正好是清单里某一项时按回车，Qt 会连发两发
        // —— `returnPressed`（我们接的那一发）之后紧跟 `editingFinished`，而 QComboBox
        // 自己接了后者、并在 findText 命中时补一次 setCurrentIndex + activated。
        // 没有那道闸的话，用户按一次回车会搜两次、烧两发限流额度（而界面上完全看不出来：
        // 第二发只是把第一发的结果又冲掉一次）。这里走的就是 Qt 真实的那一对信号。
        searchesBefore = searchLogs();
        api.requests.clear();
        versionEdit->setText(QStringLiteral("1.21.1"));  // 清单里**有**这一项
        QMetaObject::invokeMethod(versionEdit, "returnPressed");
        QMetaObject::invokeMethod(versionEdit, "editingFinished");
        const bool enterAnswered = pumpUntil([&api] { return !api.requests.isEmpty(); }, 10000);
        check(enterAnswered && searchLogs() - searchesBefore == 1 &&
                  api.lastRequest().contains(QStringLiteral("gameVersion=1.21.1")),
              "ZI10 版本框里按回车**只搜一发**（Qt 紧跟着那发 editingFinished 被那道闸拦住了）",
              QStringLiteral("搜了 %1 发：%2")
                  .arg(searchLogs() - searchesBefore)
                  .arg(api.requests.join(QStringLiteral("|"))));
        QApplication::processEvents();

        // ⑤ 手打的版本号不在清单里时：照旧搜（这正是这一格可输入的理由），而且**不往清单里
        // 追项**（Qt 默认的 InsertAtBottom 会在回车那一发把框里的字追加进去，追一次多一条，
        // 而追进去的那一项永远也搜不出东西来）。
        const int itemsBeforeEnter = versionCombo->count();
        searchesBefore = searchLogs();
        api.requests.clear();
        versionEdit->setText(QStringLiteral("26.9"));
        QMetaObject::invokeMethod(versionEdit, "returnPressed");
        QMetaObject::invokeMethod(versionEdit, "editingFinished");
        const bool typedAnswered = pumpUntil([&api] { return !api.requests.isEmpty(); }, 10000);
        check(typedAnswered && searchLogs() - searchesBefore == 1 &&
                  api.lastRequest().contains(QStringLiteral("gameVersion=26.9")),
              "ZI10 手打一个清单里没有的版本号 + 回车 ⇒ 照搜一发",
              QStringLiteral("搜了 %1 发：%2")
                  .arg(searchLogs() - searchesBefore)
                  .arg(api.requests.join(QStringLiteral("|"))));
        check(versionCombo->count() == itemsBeforeEnter,
              "ZI10 而且**没往清单里追项**（不然每按一次回车多一条，还都是搜不出东西的项）",
              QStringLiteral("回车前 %1 项、回车后 %2 项")
                  .arg(itemsBeforeEnter)
                  .arg(versionCombo->count()));
        QApplication::processEvents();

        // ⑥ 加载器 / 类型 / 来源：挑一项都立刻搜一发，而且那一项真的进了请求。
        searchesBefore = searchLogs();
        api.requests.clear();
        pickFromCombo(loaderCombo, loaderPick);
        const bool loaderPicked = pumpUntil([&api] { return !api.requests.isEmpty(); }, 10000);
        check(loaderPicked && searchLogs() - searchesBefore == 1 &&
                  api.lastRequest().contains(QStringLiteral("modLoaderType=") +
                                             cfLoaders.at(loaderPick).key),
              "ZI10 挑一项「模组加载器」立刻搜一发（就一发），带的就是那一项",
              QStringLiteral("搜了 %1 发：%2")
                  .arg(searchLogs() - searchesBefore)
                  .arg(api.requests.join(QStringLiteral("|"))));

        searchesBefore = searchLogs();
        api.requests.clear();
        pickFromCombo(typeCombo, typePick);
        const bool typePicked = pumpUntil([&api] { return !api.requests.isEmpty(); }, 10000);
        check(typePicked && searchLogs() - searchesBefore == 1 &&
                  api.lastRequest().contains(QStringLiteral("categoryId=") +
                                             cfTypes.at(typePick).key),
              "ZI10 挑一项「类型」也一样",
              QStringLiteral("搜了 %1 发：%2")
                  .arg(searchLogs() - searchesBefore)
                  .arg(api.requests.join(QStringLiteral("|"))));

        // 来源这一格：挑**当前**那一项（CurseForge）—— 换到 Modrinth 那一项的地址是写死的真
        // 外网，这一趟是离线量。用户在下拉里点已经选中的那一项，Qt 照样发 activated。
        searchesBefore = searchLogs();
        api.requests.clear();
        pickFromCombo(sourceCombo, cfIndex);
        const bool sourcePicked = pumpUntil([&api] { return !api.requests.isEmpty(); }, 10000);
        check(sourcePicked && searchLogs() - searchesBefore == 1,
              "ZI10 挑一项「来源」也立刻搜一发（挑的还是当前那一项时也算）",
              QStringLiteral("搜了 %1 发：%2")
                  .arg(searchLogs() - searchesBefore)
                  .arg(api.requests.join(QStringLiteral("|"))));
        QApplication::processEvents();
    }

    // 还回去：产品自己从不写这个键，验证工程也不能给它留在沙盒的 ini 里
    // （下一个跑的人会捡到一个指向本机端口的地址）。
    {
        QSettings sandbox;
        sandbox.remove(kBaseKey);
        sandbox.sync();
        check(!QSettings().contains(kBaseKey), "ZI9 那个口子键还回去了（没留在沙盒里）");
    }
}

