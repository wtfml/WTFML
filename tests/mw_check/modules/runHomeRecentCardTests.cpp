// Retired single-card test: keep the original disabled status.
#if 0
#include "Support.hpp"
void runHomeRecentCardTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "T36.0 临时数据根目录建出来了");
    if (!root.isValid()) {
        return;
    }

    // 数据根目录**两头都要改**（理由见 T19/T35）：MainWindow 构造里会拿盘上那个「当前游戏
    // 文件夹」再把 AppPaths 改一次；而那本账就落在**当前游戏文件夹**底下。
    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());
    // checkPreLaunch（排在 Java 那一步之前）要求这两样存在 —— 而且 `libraries` 还**不能是
    // 空目录**（它那道闸口的原话是「libraries 文件夹为空或不存在」），所以底下摆一个占位。
    QDir().mkpath(root.path() + QStringLiteral("/libraries/占位"));
    QDir().mkpath(root.path() + QStringLiteral("/assets"));

    // ---------- 夹具：三个实例 ----------
    const QString id = QStringLiteral("1.20.1-T36");
    const QString displayName = QStringLiteral("T36主页");
    // 下拉框里那一句（= 名字 + "-" + 目录名）：账本上写的就是它，不是裸目录名。
    const QString shownName = displayName + QStringLiteral("-1.20.1-T36");
    // 版本 json 里**没有**那两条 quick play 参数的那个（⑥ 那一档要它）。
    const QString noQuickId = QStringLiteral("1.20.1-T36旧");
    // 只为了验「降级那一档真的把下拉框切过去了」：一开始让它占着下拉框。
    const QString otherId = QStringLiteral("1.21-T36另");
    const QString instancePath = AppPaths::versionDir(id);
    const QString worldDir = instancePath + QStringLiteral("/saves/基地周目");
    const QString serverName = QStringLiteral("朋友的服");
    const QString serverAddress = QStringLiteral("play.t36.example:25566");
    // 每个夹具的 Java 都钉在一个不存在的路径上（理由见文件头那段）。
    const QString deadJava = QStringLiteral("C:/T36没有这个目录/java.exe");

    // writeInstance：一个"看起来已安装"的实例（installedVersions() 只认 <id>/<id>.jar）。
    const auto writeInstance = [](const QString &versionId, const QByteArray &json,
                                  const QString &name, const QString &javaPath) {
        const QString dir = AppPaths::versionDir(versionId);
        QDir().mkpath(dir);
        writeFileAt(dir + QLatin1Char('/') + versionId + QStringLiteral(".json"), json);
        writeFileAt(dir + QLatin1Char('/') + versionId + QStringLiteral(".jar"),
                    QByteArrayLiteral("T36-client-jar"));
        writeFileAt(dir + QStringLiteral("/wtfml.meta"),
                    QJsonDocument(QJsonObject{{QStringLiteral("name"), name},
                                              {QStringLiteral("javaPath"), javaPath}})
                        .toJson(QJsonDocument::Compact));
    };

    writeInstance(id,
                  QJsonDocument(QJsonObject{
                                    {QStringLiteral("id"), QStringLiteral("1.20.1")},
                                    {QStringLiteral("type"), QStringLiteral("release")},
                                    {QStringLiteral("mainClass"),
                                     QStringLiteral("net.minecraft.client.main.Main")},
                                    {QStringLiteral("libraries"), QJsonArray{}},
                                    {QStringLiteral("javaVersion"),
                                     QJsonObject{{QStringLiteral("majorVersion"), 17}}},
                                    // 判据是**json 里写没写这一条**，不是版本号（见
                                    // QuickPlay.hpp 文件头那一段）：这个夹具认它们。
                                    {QStringLiteral("arguments"),
                                     QJsonObject{{QStringLiteral("game"),
                                                  QJsonArray{QStringLiteral("--quickPlaySingleplayer"),
                                                             QStringLiteral("--quickPlayMultiplayer")}}}}})
                      .toJson(QJsonDocument::Compact),
                  displayName, deadJava);
    writeInstance(noQuickId,
                  QByteArrayLiteral("{\"id\":\"1.20.1\",\"mainClass\":\"net.minecraft.client.main.Main\","
                                    "\"libraries\":[],\"javaVersion\":{\"majorVersion\":17}}"),
                  QStringLiteral("T36老版本"), deadJava);
    writeInstance(otherId, QByteArrayLiteral("{\"id\":\"1.21\",\"libraries\":[]}"),
                  QStringLiteral("T36别的"), deadJava);

    writeFileAt(worldDir + QStringLiteral("/level.dat"), QByteArrayLiteral("LEVEL"));
    writeFileAt(AppPaths::versionDir(noQuickId) + QStringLiteral("/saves/老世界/level.dat"),
                QByteArrayLiteral("LEVEL"));
    QString serverProblem;
    check(ServerListFile::addEntry(ServerListFile::pathFor(id), serverName, serverAddress,
                                   &serverProblem),
          "T36.0 夹具：那一份实例的 servers.dat 里有「朋友的服」这一条", serverProblem);

    // 身份：离线账号（这一节一个请求都不发）。
    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    const QString accountName = QStringLiteral("T36User");
    QString offlineError;
    check(AccountStore::addOffline(accountName, &offlineError),
          "T36.0 建一个离线账号并设成当前身份", offlineError);

    // ---------- 几个读卡片的助手 ----------
    const auto onDisk = [] { return LaunchHistory::load(LaunchHistory::historyPath()); };
    const auto labelText = [](QWidget *window, const QString &name) {
        auto *label = window->findChild<QLabel *>(name);
        return label != nullptr ? label->text() : QString();
    };
    // 卡片上那行「什么时候」的期望值：今天 ⇒ `HH:mm`；万一这一跑的瞬间跨了午夜，同一句话
    // 就该是「昨天 HH:mm」（判据在 verify 的 ZR6，这里只是不给自己找跨午夜的假红）。
    const auto whenExpectation = [](qint64 ms) {
        const QString clock = QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm"));
        return QStringList{clock, QStringLiteral("昨天 ") + clock};
    };
    // 点一颗按钮，并把这一发弹出来的每一条提示按脚本答掉（返回依次弹了哪几条）。
    // NOTE: 产品是在**嵌套事件循环**里弹框的（`execPrompt`），所以那个自走的答话器
    // 正好在框摆出来的那一刻醒过来 —— 和 AJ 那一节同一套。
    const auto clickAndAnswer = [](QPushButton *button, const QList<int> &script) {
        QList<PromptAnswerer::Answered> answered;
        if (button == nullptr) {
            return answered;
        }
        PromptAnswerer answerer(script);
        answerer.start();
        button->click();
        QApplication::processEvents();
        answerer.stop();
        // 返回的是**答话器自己记下来的那一份**（它才是那个自走定时器里被 `append()` 的
        // 表）。写成上面那个局部空表的话，这几条判据**全是假绿/假红**：点下去真弹了、真被
        // 答掉了，这里读到的永远是 0 条 —— 这一节就这么红了一轮（症状是"三处
        // 都像产品压根没弹框"，而产品那边一个字都没错）。
        return answerer.answered;
    };
    const auto titlesOf = [](const QList<PromptAnswerer::Answered> &answered) {
        QStringList titles;
        for (const PromptAnswerer::Answered &one : answered) {
            // 正文一起带上：红的理由分两种（"该弹的没弹"和"弹的是另一条"），只看标题
            // 分不出是哪一种 —— 这一节红的那一条就只留了个「无法启动」，
            // 正文里那句"启动前检查没通过：…"才是它到底在说什么。
            titles.append(one.title + QStringLiteral("：") +
                          one.text.split(QLatin1Char('\n')).value(0));
        }
        return titles;
    };
    // 「卡片露着没有」问的是**它自己有没有被藏起来**（父链上有页面被切走时会答错，所以用
    // isVisibleTo 而不是 isVisible —— 主页本来就是第一个选项卡）。
    const auto cardShown = [](QFrame *card) {
        return card != nullptr && card->isVisibleTo(card->parentWidget());
    };

    {
        MainWindow w;
        w.show();
        QApplication::processEvents();

        auto *card = w.findChild<QFrame *>(MainWindowUI::homeRecentCardName());
        auto *combo = comboByObjectName(&w, MainWindowUI::installedVersionComboName());
        auto *continueButton =
            w.findChild<QPushButton *>(MainWindowUI::homeRecentContinueButtonName());
        auto *settingsButton =
            w.findChild<QPushButton *>(MainWindowUI::homeRecentSettingsButtonName());
        check(card != nullptr && continueButton != nullptr && settingsButton != nullptr,
              "T36.1 前提：那张卡和它那两颗按钮都在");

        // ---------- ① 一次都没启动过 ----------
        check(!cardShown(card),
              "★★ T36.1 一次都没启动过 ⇒ **整张卡收起来**（要的是一小块活动提示，"
              "不是一个空壳子占着主页的左栏）");

        // ---------- ② 普通启动一次：账上一条 → 卡片当场出来 ----------
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch() - 10 * 60 * 1000;
        w.seedLaunchRecordForTest(id, t0);

        QList<LaunchHistory::Record> records = onDisk();
        check(QFile::exists(root.path() + QStringLiteral("/wtfml.history")),
              "★★ T36.2 账落在**当前游戏文件夹**里（`wtfml.history`）—— 换一个游戏文件夹"
              "就是另一本账，这是「跟着数据走」的落点");
        check(records.size() == 1,
              "★ T36.2 摆进去一条 ⇒ 盘上就一条（摆的是产品那条写路，不是测试自己另写一份）",
              QString::number(records.size()));
        if (records.size() == 1) {
            const LaunchHistory::Record first = records.first();
            check(first.instanceId == id && first.instanceName == shownName &&
                      first.accountKey == accountName,
                  "★★ T36.2 记的是**启动那一刻那一句显示名**（下拉框里那一句，不是裸目录名）"
                  "+ 当时那个身份的名字",
                  first.instanceName + QStringLiteral("｜") + first.accountKey);
            check(first.result == LaunchHistory::Result::Running &&
                      first.target == LaunchHistory::Target::None,
                  "★ T36.2 刚记下来是「还在跑」；这一趟是普通启动 ⇒ 目标那一格是空的"
                  "（世界 / 服务器只有通过 Quick Play 启动时才记录，普通进主菜单不能猜）",
                  LaunchHistory::targetKey(first.target));
            check(first.startedAt.toMSecsSinceEpoch() == t0,
                  "★ T36.2 时间就是**启动那一刻**（产品记的是「进程真的起来了」那一秒，"
                  "不是点按钮那一秒）");
        }
        check(cardShown(card), "★★ T36.2 有记录了 ⇒ 卡片出来");
        check(labelText(&w, MainWindowUI::homeRecentNameLabelName()) == shownName,
              "★ T36.2 第一行 = 当初那句显示名",
              labelText(&w, MainWindowUI::homeRecentNameLabelName()));
        check(labelText(&w, MainWindowUI::homeRecentDetailLabelName()) ==
                  QStringLiteral("（还在玩）"),
              "★ T36.2 第二行只有一句结局注解（这一趟没有目标、也还没有时长）",
              labelText(&w, MainWindowUI::homeRecentDetailLabelName()));
        check(whenExpectation(t0).contains(
                  labelText(&w, MainWindowUI::homeRecentWhenLabelName())),
              "★ T36.2 第三行是「什么时候」（今天就是 `HH:mm`，昨天就带「昨天」）",
              labelText(&w, MainWindowUI::homeRecentWhenLabelName()));
        check(continueButton != nullptr &&
                  continueButton->text() == MainWindowUI::homeRecentContinueText(),
              "★★ T36.2 普通启动那一趟也是「继续游玩」（那张草图里那颗按钮的两个说法："
              "接着玩的动作就是**把这一份再开起来**；只有「这一份自己也开不起来」时才降级成"
              "「启动实例」）",
              continueButton != nullptr ? continueButton->text() : QString());
        check(settingsButton != nullptr &&
                  settingsButton->text() == MainWindowUI::homeRecentSettingsText(),
              "★ T36.2 第二颗永远是「实例设置」",
              settingsButton != nullptr ? settingsButton->text() : QString());

        // ---------- ③ 一局玩完：时长 + 结局那句话 ----------
        w.seedLaunchFinishForTest(t0, false, false);
        records = onDisk();
        check(records.size() == 1 &&
                  records.first().result == LaunchHistory::Result::NormalExit &&
                  records.first().durationSeconds == 600,
              "★★ T36.3 正常退出 ⇒ 盘上那一条被**改写**成结局 + 时长（不是又加一条："
              "一本账上一个号一条）",
              QStringLiteral("条数 %1 / 秒 %2")
                  .arg(records.size())
                  .arg(records.isEmpty() ? -1 : records.first().durationSeconds));
        check(labelText(&w, MainWindowUI::homeRecentDetailLabelName()) ==
                  QStringLiteral("玩了 10 分钟"),
              "★★ T36.3 第二行 =「玩了 10 分钟」（正常退出不加注解）",
              labelText(&w, MainWindowUI::homeRecentDetailLabelName()));

        // 异常退出那一档：结局那句话**紧贴**着时长（中间不加分隔符）—— 它是那半句的注解。
        // 必须**另起一条「还在跑」的记录**再把它判成异常退出：上一段那一条已经落在「正常
        // 退出」这个终态上了，而产品的 `LaunchHistoryController::recordFinish()` 有一条
        // 「终态只落一次」的
        // 规矩（见 LaunchHistory 那段实现）—— 它**故意**不把一条已经收尾的记录改写成另一个
        // 结局。拿老那一条去喂，量到的是"产品没改"这个**正确的行为**。
        const qint64 tCrash = t0 + 2 * 60 * 1000;  // 时长 = 8 分钟（秒数取整规则见 durationText）
        w.seedLaunchRecordForTest(id, tCrash);
        w.seedLaunchFinishForTest(tCrash, true, false);
        check(labelText(&w, MainWindowUI::homeRecentDetailLabelName()) ==
                  QStringLiteral("玩了 8 分钟（上次是异常退出）"),
              "★★ T36.3 异常退出 ⇒ 结尾多一句「（上次是异常退出）」，**紧贴**着前面那句话",
              labelText(&w, MainWindowUI::homeRecentDetailLabelName()));

        // ---------- ④ 「压根没玩成」那一条不算数（卡片退回上一条真的玩过的）----------
        // 「以成功创建游戏进程为准，不以点击启动按钮为准」的落点：进程起来就死 /
        // 没起来的那一趟不该把"上次玩的是哪一份"改掉。
        {
            const qint64 tFail = QDateTime::currentMSecsSinceEpoch();
            w.seedLaunchRecordForTest(id, tFail);
            w.seedLaunchFinishForTest(tFail, false, true);
            records = onDisk();
            check(records.size() == 3 &&
                      records.first().result == LaunchHistory::Result::StartFailed,
                  "T36.4 前提：最新那一条是「压根没玩成」（产品自己那一档）",
                  records.isEmpty() ? QString() : LaunchHistory::resultKey(records.first().result));
            check(cardShown(card) &&
                      labelText(&w, MainWindowUI::homeRecentDetailLabelName()) ==
                          QStringLiteral("玩了 8 分钟（上次是异常退出）"),
                  "★★ T36.4 「压根没玩成」那一条**卡片不认** ⇒ 退回上一条真的玩过的"
                  "（「上一次」以成功创建游戏进程为准）",
                  labelText(&w, MainWindowUI::homeRecentDetailLabelName()));
            check(whenExpectation(tCrash).contains(
                      labelText(&w, MainWindowUI::homeRecentWhenLabelName())),
                  "★ T36.4 退回之后那一行「什么时候」也跟着退回那一条的时间",
                  labelText(&w, MainWindowUI::homeRecentWhenLabelName()));
        }

        // ---------- ⑤ 快捷启动进世界：第二行写上那个世界，按钮变「继续游玩」----------
        const qint64 t1 = QDateTime::currentMSecsSinceEpoch();
        w.seedQuickPlayRecordForTest(id, QuickPlay::Kind::Singleplayer,
                                     QStringLiteral("基地周目"), t1);
        records = onDisk();
        check(records.first().target == LaunchHistory::Target::World &&
                  records.first().targetId == QStringLiteral("基地周目"),
              "★★ T36.5 通过 Quick Play 进的那个世界被记下来了（值就是命令行上发出去的那一串"
              "存档目录名，不另算一遍）",
              records.isEmpty() ? QString() : records.first().targetId);
        check(labelText(&w, MainWindowUI::homeRecentDetailLabelName()) ==
                  QStringLiteral("单人世界：基地周目（还在玩）"),
              "★★ T36.5 第二行 =「单人世界：基地周目（还在玩）」",
              labelText(&w, MainWindowUI::homeRecentDetailLabelName()));
        check(continueButton != nullptr &&
                  continueButton->text() == MainWindowUI::homeRecentContinueText(),
              "★★ T36.5 世界还在 ⇒ 按钮写「继续游玩」",
              continueButton != nullptr ? continueButton->text() : QString());

        // ---------- ⑥ 「实例设置」那一颗开的是**这一份**实例的页 ----------
        {
            QString pageTitle;
            settingsButton->click();
            QApplication::processEvents();
            closeInstanceSettingsPage(&pageTitle);
            check(pageTitle.contains(displayName),
                  "★★ T36.6 「实例设置」那一颗开的就是这个实例的设置页（页眉上写着它）",
                  pageTitle);
        }

        // ---------- ⑦ 世界被删掉之后：文字留着，按钮降级成「启动实例」----------
        // 卡片是**上一趟重画**时算出来的 ⇒ 这一下正好验产品那句「现算一遍，不拿旧结论
        // 办事」（见 onHomeRecentContinueClicked 那段 NOTE）。
        QDir(worldDir).removeRecursively();
        if (combo != nullptr) {
            combo->setCurrentIndex(combo->findData(otherId));
        }
        QApplication::processEvents();
        check(combo != nullptr && selectedId(combo) == otherId,
              "T36.7 前提：下拉框先切到**另一个**实例（这样才量得出「降级那一档切没切」）",
              combo != nullptr ? selectedId(combo) : QString());
        check(continueButton != nullptr &&
                  continueButton->text() == MainWindowUI::homeRecentContinueText(),
              "T36.7 前提：世界删掉之后**卡片上那颗按钮还写着「继续游玩」**（没人重画过它）",
              continueButton != nullptr ? continueButton->text() : QString());
        {
            // 这一发会一路走到启动那条路（Java 那一步）—— 那一问就是"完整入口真的被
            // 走到了"的证据（「快捷启动仍走完整校验、登录、Java 和重复运行提醒，
            // 不绕过现有启动入口」）。
            const QList<PromptAnswerer::Answered> answered = clickAndAnswer(continueButton, {0});
            check(answered.size() == 1 && answered.first().text.contains(QStringLiteral("跟随全局")),
                  "★★ T36.7 世界没了还在按 ⇒ 它降级成「把这一份开起来」，走的就是用户自己"
                  "在下拉框里选好实例再点「启动游戏」那条路（这一趟停在 Java 那一问上）",
                  titlesOf(answered).join(QLatin1Char('|')));
            check(combo != nullptr && selectedId(combo) == id,
                  "★★ T36.7 降级那一档真的**把下拉框切到了这个实例**（不是另一条捷径）",
                  combo != nullptr ? selectedId(combo) : QString());
            check(records.size() == onDisk().size(),
                  "★★ T36.7 没启动成 ⇒ 账上**一条都没多**（点按钮那一刻不算数）",
                  QString::number(onDisk().size()));
        }

        // ---------- ⑧ 这个版本不认那两条参数：按钮也降级，而且**不许**直接发 ----------
        // 这一条是"卡片和启动器同一把尺子"的证据：`startQuickPlay()` 自己有一道兜底
        // （认不了就弹「这个版本用不了」，见 QuickPlay::unsupportedText）—— 那一句一露，
        // 就说明卡片越过了自己的判据直接发了快捷启动。正确的行为是它**根本不发**：
        // 降级成普通启动，于是弹的是 Java 那一问。
        w.seedQuickPlayRecordForTest(noQuickId, QuickPlay::Kind::Singleplayer,
                                     QStringLiteral("老世界"),
                                     QDateTime::currentMSecsSinceEpoch());
        check(continueButton != nullptr &&
                  continueButton->text() == MainWindowUI::homeRecentLaunchText(),
              "★★ T36.8 版本 json 里没有那两条参数 ⇒ 按钮写「启动实例」"
              "（`instanceSupportsQuickPlay()` 就是那把尺子，和启动那条路同一个判据）",
              continueButton != nullptr ? continueButton->text() : QString());
        check(labelText(&w, MainWindowUI::homeRecentDetailLabelName()).startsWith(
                  QStringLiteral("单人世界：老世界")),
              "★ T36.8 但它仍然是**那一条记录**（第二行照旧写着那个世界 —— 账上是历史，"
              "卡片不因为「现在发不出去」就把历史抹掉）",
              labelText(&w, MainWindowUI::homeRecentDetailLabelName()));
        {
            const QList<PromptAnswerer::Answered> answered = clickAndAnswer(continueButton, {0});
            check(answered.size() == 1 &&
                      !answered.first().text.contains(QStringLiteral("--quickPlay")),
                  "★★ T36.8 点下去**没有**「这个版本用不了」那一句 —— 它压根没去发那条参数，"
                  "而是降级成普通启动（真发出去游戏会当场退出）",
                  answered.isEmpty() ? QString() : answered.first().text);
        }

        // ---------- ⑨ 上限那一截也要从产品这条路上走一遍 ----------
        {
            const qint64 base = QDateTime::currentMSecsSinceEpoch();
            for (int i = 0; i < 25; ++i) {
                w.seedLaunchRecordForTest(id, base + i * 60000);
            }
            records = onDisk();
            check(records.size() == LaunchHistory::historyLimit(),
                  "★ T36.9 从产品这条写路连记 25 条 ⇒ 盘上只留最近 20 条（只保存"
                  "最近 20 条）",
                  QString::number(records.size()));
            check(!records.isEmpty() &&
                      records.first().startedAt.toMSecsSinceEpoch() == base + 24 * 60000,
                  "★★ T36.9 留的是**最近**那 20 条，最新那条在最前面（卡片的「上一次」就靠它）",
                  records.isEmpty()
                      ? QString()
                      : QString::number(records.first().startedAt.toMSecsSinceEpoch()));
        }
    }

    // ---------- ⑩ 那一份实例自己也已经没了：说一句、不启动 ----------
    // 「目标已经被删除时保留历史文字，但继续游玩降级为启动实例」的另一半：连实例都
    // 不在了，卡片上剩的是**历史文字**，点下去只能是一句话 —— 硬塞一个下拉框里找不到
    // 的值，开起来的是现在选着的**另一个**实例（那比什么都不做更糟）。
    QDir(AppPaths::versionDir(id)).removeRecursively();
    {
        MainWindow w3;
        w3.show();
        QApplication::processEvents();

        auto *card = w3.findChild<QFrame *>(MainWindowUI::homeRecentCardName());
        auto *continueButton =
            w3.findChild<QPushButton *>(MainWindowUI::homeRecentContinueButtonName());
        auto *settingsButton =
            w3.findChild<QPushButton *>(MainWindowUI::homeRecentSettingsButtonName());
        check(cardShown(card),
              "★★ T36.10 实例没了 ⇒ 卡片**照旧露着**（账上是历史，历史不该跟着实例一起消失）");
        check(continueButton != nullptr &&
                  continueButton->text() == MainWindowUI::homeRecentLaunchText(),
              "★★ T36.10 按钮降级成「启动实例」",
              continueButton != nullptr ? continueButton->text() : QString());
        check(settingsButton != nullptr &&
                  settingsButton->text() == MainWindowUI::homeRecentSettingsText(),
              "★ T36.10 第二颗还是「实例设置」那一句");

        const int before = onDisk().size();
        {
            const QList<PromptAnswerer::Answered> answered = clickAndAnswer(continueButton, {0});
            check(answered.size() == 1 && answered.first().title == QStringLiteral("实例未找到"),
                  "★★ T36.10 点「启动实例」⇒ **说一句、不启动**（措辞和 startQuickPlayNow() "
                  "里那一处逐字相同：同一件事说两种话，用户会以为是两件事）",
                  answered.isEmpty() ? QString() : answered.first().title);
            check(!answered.isEmpty() &&
                      answered.first().text.contains(QStringLiteral("现在不在已安装列表里")),
                  "★ T36.10 那句人话里说清了是哪一份、为什么",
                  answered.isEmpty() ? QString() : answered.first().text);
        }
        {
            const QList<PromptAnswerer::Answered> answered = clickAndAnswer(settingsButton, {0});
            check(answered.size() == 1 && answered.first().title == QStringLiteral("实例未找到"),
                  "★★ T36.10 「实例设置」那一颗也一样（开它的设置页只会给出一个空壳）",
                  answered.isEmpty() ? QString() : answered.first().title);
        }
        auto *manager = w3.findChild<LauncherManager *>();
        check((manager == nullptr || manager->runningGames().isEmpty()) &&
                  onDisk().size() == before,
              "★★ T36.10 这两下**什么都没启动、账上一条都没多**",
              QString::number(onDisk().size()));
    }

    // ---------- 收摊：账号、设置键、数据根目录都还回去 ----------
    TokenStore::clear(TokenStore::microsoftKey());
    TokenStore::clear(TokenStore::minecraftKey());
    TokenStore::clear(TokenStore::accountsKey());
    TokenStore::clear(TokenStore::currentUuidKey());
    check(AccountStore::accounts().isEmpty() && AccountStore::activeUuid().isEmpty(),
          "T36.9 这一节加的账号清干净了");

    sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
    sandbox.sync();
    AppPaths::setMinecraftDir(originalDir);
    check(AppPaths::minecraftDir() == originalDir, "T36.9 数据根目录换回来了");
}
#endif

