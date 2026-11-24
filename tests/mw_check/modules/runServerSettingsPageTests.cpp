#include "Support.hpp"
#include "ListRowDelegate.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/ServerSettingsController.hpp"
#include "controllers/ServerWorkspaceController.hpp"
void runServerSettingsPageTests() {
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir root;
    check(root.isValid(), "T42 前提：拿得到一个临时的数据根目录");
    // 数据根目录**两头都要改**（理由见 T39/T40）：`MainWindow` 构造里会拿盘上那个「当前
    // 游戏文件夹」再把 AppPaths 改一次。收尾**逐字还原**那两个键。
    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());

    // ---------- 盘上摆一个服务端（判据层直接摆，不经过界面）----------
    const QString serverId = QStringLiteral("我的服务端");
    ServerModel::Info info;
    info.name = serverId;
    info.flavor = ServerInstall::Flavor::Vanilla;
    info.flavorKnown = true;
    info.gameVersion = QStringLiteral("1.20.1");
    info.javaPath = QCoreApplication::applicationFilePath();
    QString problem;
    check(ServerRegistry::createServer(serverId, info, &problem),
          "T42 前提：盘上摆了一个服务端", problem);
    const QString serverDir = AppPaths::serverDir(serverId);
    {
        QFile jar(serverDir + QStringLiteral("/server.jar"));
        if (jar.open(QIODevice::WriteOnly)) {
            jar.close();
        }
    }
    // 世界目录（备份那一段要它真在盘上）+ 一份带注释的 server.properties。
    QDir().mkpath(serverDir + QStringLiteral("/world"));
    {
        QFile levelDat(serverDir + QStringLiteral("/world/level.dat"));
        if (levelDat.open(QIODevice::WriteOnly)) {
            levelDat.write(QByteArrayLiteral("LEVEL-DATA"));
            levelDat.close();
        }
    }
    const QString originalProps = QStringLiteral("#Minecraft server properties\n"
                                                "motd=欢迎来到我的服务端\n"
                                                "server-port=25565\n"
                                                "level-name=world\n"
                                                "view-distance=10\n");
    {
        QFile props(ServerRegistry::propertiesPath(serverId));
        if (props.open(QIODevice::WriteOnly)) {
            props.write(originalProps.toUtf8());
            props.close();
        }
    }
    const auto propsTextOnDisk = [&serverId]() {
        QFile file(ServerRegistry::propertiesPath(serverId));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    };
    {
        ServerSettingsController controller;
        const auto before = controller.readRaw(serverId);
        const auto external = before + QStringLiteral("#external-change\n");
        QFile file(ServerRegistry::propertiesPath(serverId));
        check(file.open(QIODevice::WriteOnly) && file.write(external.toUtf8()) == external.toUtf8().size(), "T42 external properties fixture is written");
        file.close();
        const auto refused = controller.save(serverId, before, {{"motd", "should-not-overwrite"}});
        check(!refused.ok && propsTextOnDisk() == external && refused.problem.contains(QStringLiteral("其它程序")),
            "T42 stale properties editor refuses to overwrite external changes");
        check(file.open(QIODevice::WriteOnly) && file.write(originalProps.toUtf8()) == originalProps.toUtf8().size(), "T42 original properties restored for subsequent UI checks");
    }

    MainWindow w;
    w.show();
    QApplication::processEvents();

    bool pageSeen = false;
    QTimer::singleShot(0, [&pageSeen, &serverId, &originalProps, &propsTextOnDisk, &w] {
        OverlayHost *host = currentPageOverlay();
        if (host == nullptr) {
            return;
        }
        auto *page = host->findChild<ServerSettingsPage *>();
        if (page == nullptr) {
            return;
        }
        pageSeen = true;

        auto *title = page->findChild<QLabel *>(MainWindowUI::serverSettingsTitleName());
        auto *sections =
            page->findChild<QTreeWidget *>(MainWindowUI::serverSettingsSectionListName());
        auto *stack = page->findChild<QStackedWidget *>(MainWindowUI::serverSettingsStackName());
        auto *save = page->findChild<QPushButton *>(MainWindowUI::serverSettingsSaveButtonName());
        auto *saveNote = page->findChild<QLabel *>(MainWindowUI::serverSettingsSaveNoteName());
        auto *backupNow =
            page->findChild<QPushButton *>(MainWindowUI::serverSettingsBackupNowButtonName());
        auto *backupList =
            page->findChild<QTreeWidget *>(MainWindowUI::serverSettingsBackupListName());
        auto *checkSummary =
            page->findChild<QLabel *>(MainWindowUI::serverSettingsCheckSummaryName());
        auto *checkList =
            page->findChild<QTreeWidget *>(MainWindowUI::serverSettingsCheckListName());
        auto *rawEdit =
            page->findChild<QPlainTextEdit *>(MainWindowUI::serverSettingsRawEditName());
        const auto textFieldOf = [page](const QString &key) {
            return page->findChild<QLineEdit *>(MainWindowUI::serverSettingsFieldName(key));
        };
        const auto numberFieldOf = [page](const QString &key) {
            return page->findChild<QSpinBox *>(MainWindowUI::serverSettingsFieldName(key));
        };
        const bool wired = title != nullptr && sections != nullptr && stack != nullptr &&
                           save != nullptr && saveNote != nullptr && backupNow != nullptr &&
                           backupList != nullptr && checkSummary != nullptr &&
                           checkList != nullptr && rawEdit != nullptr
                           && textFieldOf(QStringLiteral("motd")) != nullptr
                           && numberFieldOf(QStringLiteral("server-port")) != nullptr;
        check(wired, "★ T42.0 前提：那一页整页开出来了，表单 / 原始文本 / 备份 / 体检四样都找得到");
        if (!wired) {
            if (auto *back =
                    host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
                back->click();
            }
            return;
        }

        // ---------- ① 顶上说的是谁 + 表单起手摆的是盘上那一份 ----------
        check(title->text() == serverId &&
                  sections->topLevelItemCount() == MainWindowUI::serverSettingsSectionCount() &&
                  stack->count() == MainWindowUI::serverSettingsSectionCount(),
              "★ T42.1 顶上那行字是**用户认得的那一个名字**，左栏四页摆齐了",
              title->text());
        check(textFieldOf(QStringLiteral("motd"))->text() == QStringLiteral("欢迎来到我的服务端")
                  && numberFieldOf(QStringLiteral("server-port"))->value() == 25565,
              "★★ T42.1 表单起手摆的就是盘上那份 `server.properties` 里写的（不是表的缺省值）",
              QStringLiteral("motd=「%1」 port=%2")
                  .arg(textFieldOf(QStringLiteral("motd"))->text())
                  .arg(numberFieldOf(QStringLiteral("server-port"))->value()));
        check(!save->isEnabled() &&
                  saveNote->text() == MainWindowUI::serverSettingsCleanNote(),
              "★ T42.1 刚开页 = 什么都没改 ⇒ 「保存」点不动、那行小字是「没有改动」",
              saveNote->text());

        // ---------- ② 表单改一格 → 保存带亮 → 保存 → 盘上只动了那一行 ----------
        textFieldOf(QStringLiteral("motd"))->setText(QStringLiteral("新的说明"));
        check(save->isEnabled() &&
                  saveNote->text() == MainWindowUI::serverSettingsDirtyFieldsNote(1),
              "★★ T42.2 改了一格 ⇒ 「保存」亮了、小字说的是「已修改 1 项」（那几句话是产品"
              "措辞，只有翻译点那一个出处）",
              saveNote->text());
        save->click();
        QApplication::processEvents();
        const QString afterSave = propsTextOnDisk();
        // 写盘那一侧按 Java Properties 的规矩把非 ASCII 转成大写四位的 `\uXXXX`
        // （R9：服务端按 ISO-8859-1 读这份文件，原样写 UTF-8 在游戏里是乱码），
        // 所以这里量的是**转义后**的那一行。
        check(afterSave.contains(QStringLiteral("motd=\\u65B0\\u7684\\u8BF4\\u660E")) &&
                  afterSave.contains(QStringLiteral("#Minecraft server properties")) &&
                  afterSave.contains(QStringLiteral("view-distance=10")) &&
                  !afterSave.contains(QStringLiteral("欢迎来到我的服务端")),
              "★★★ T42.2 保存之后盘上那一份**只动了 motd 那一行**：注释、别的键、键的顺序"
              "都照旧（「用户那份 + 他改的那几项」，不是我们重新生成的一份；非 ASCII 按 "
              "Java Properties 转义成正体的 `\\uXXXX`）",
              afterSave);
        check(saveNote->text() == MainWindowUI::serverSettingsSavedNote() &&
                  !save->isEnabled(),
              "★★ T42.2 存完那一行小字换成「已保存」、按钮又灭了（按过就没了）",
              saveNote->text());

        // ---------- ③ 切到「原始文本」：表单那笔改动**已经合进文本**了 ----------
        // 这一步量的是"两页看到的不是两份东西"（见 `mergeServerSettingsEdits`）。
        sections->setCurrentItem(sections->topLevelItem(static_cast<int>(ServerSettingsPage::Section::RawText)));
        QApplication::processEvents();
        check(stack->currentIndex() == static_cast<int>(ServerSettingsPage::Section::RawText) &&
                  rawEdit->toPlainText() == afterSave,
              "★★★ T42.3 切到「原始文本」⇒ 文本框里就是**刚存下去的那一份**（表单那笔改动"
              "早合进来了 —— 用户马上要看的正是「我改完之后这份文件长什么样」）",
              rawEdit->toPlainText());

        // ---------- ④ 在文本框里改一行 → 保存 → 整份写回去 ----------
        rawEdit->setPlainText(rawEdit->toPlainText() + QStringLiteral("pvp=false\n"));
        check(save->isEnabled() &&
                  saveNote->text() == MainWindowUI::serverSettingsDirtyRawNote(),
              "★★ T42.4 动了文本框 ⇒ 保存带说的是「原始文本动过」（不是「改过 N 项」）",
              saveNote->text());
        save->click();
        QApplication::processEvents();
        const QString afterRaw = propsTextOnDisk();
        check(afterRaw.contains(QStringLiteral("pvp=false")) &&
                  afterRaw.contains(QStringLiteral("motd=\\u65B0\\u7684\\u8BF4\\u660E")),
              "★★★ T42.4 文本框里加的那一行真的写下去了（保存写回去的就是**这一份原文**；"
              "中文以 `\\uXXXX` 的样子放在原文里 —— 游戏按 ISO-8859-1 读）",
              afterRaw);

        // ---------- ⑤ 切回表单：按新那份重摆，基线也跟着换 ----------
        sections->setCurrentItem(sections->topLevelItem(static_cast<int>(ServerSettingsPage::Section::Common)));
        QApplication::processEvents();
        check(stack->currentIndex() == static_cast<int>(ServerSettingsPage::Section::Common) &&
                  textFieldOf(QStringLiteral("motd"))->text() == QStringLiteral("新的说明") &&
                  !save->isEnabled(),
              "★★ T42.5 切回表单 ⇒ 按盘上那份重摆、而且**基线也重设到它**（不是「改过」）",
              textFieldOf(QStringLiteral("motd"))->text());

        // ---------- ⑥ 「立即备份」→ 盘上真多一个 zip → 这一列跟着多一行 ----------
        const QString backupDir = BackupFiles::backupDir(BackupFiles::Kind::World);
        const auto zipCount = [&backupDir]() {
            return QDir(backupDir).entryList(QStringList{QStringLiteral("*.zip")}, QDir::Files).size();
        };
        const int zipsBefore = zipCount();
        PromptAnswerer answerer({0});
        answerer.start();
        backupNow->click();
        const bool wrote = pumpUntil(
            [&zipCount, &zipsBefore, backupList]() {
                return zipCount() > zipsBefore && backupList->topLevelItemCount() > 0;
            },
            60000);
        // 收尾那一下的账（那一页重摆过了）已经在上面那两条 `pumpUntil` 里做完了，这里只把
        // 定时器停掉，别让它接着往后点别的框。
        answerer.stop();
        check(wrote, "★★★ T42.6 按「立即备份」⇒ 备份目录里真多一个 zip，而且这一列当场多了"
                     "一行（收尾那一下会把这一页重摆一遍）",
              QStringLiteral("zip %1 → %2 / 列 %3 行")
                  .arg(zipsBefore)
                  .arg(zipCount())
                  .arg(backupList->topLevelItemCount()));
        check(answerer.answered.size() == 1
                  && answerer.answered.first().title.contains(QStringLiteral("备份")),
              "★★ T42.6 打完那一下**当面说了一句**（一张阻塞的「备份完成」卡片 —— 后台那一趟"
              "跑完时从嵌套循环里弹出来的）",
              QStringLiteral("%1 条：%2")
                  .arg(answerer.answered.size())
                  .arg(answerer.answered.isEmpty() ? QString()
                                                   : answerer.answered.first().title));
        if (wrote) {
            check(backupList->topLevelItem(0)->text(0).contains(QStringLiteral("-")) &&
                      !backupList->topLevelItem(0)->data(0, Qt::UserRole).toString().isEmpty(),
                  "★ T42.6 那一行的身份（userData）是盘上那个 zip 的路径 —— 认路径不认那行字",
                  backupList->topLevelItem(0)->data(0, Qt::UserRole).toString());
        }

        // ---------- ⑦ 体检单是 `ServerCheck` 现算的 ----------
        check(!checkSummary->text().isEmpty() && checkList->topLevelItemCount() >= 1,
              "★★ T42.7 体检那一页有总括那句话、单子上也有条目（台词和条目都是 `ServerCheck` "
              "按盘上那些事实现算的）",
              QStringLiteral("总括「%1」/ %2 条")
                  .arg(checkSummary->text())
                  .arg(checkList->topLevelItemCount()));
        // 备份完再来一趟：世界备份那一格不再是 0 —— 体检的原料确实跟着盘走。
        check(checkSummary->text().contains(QStringLiteral("项")),
              "★ T42.7 总括那句是 `summaryText()` 的形态（N 项要处理 · N 项提醒 · N 项说明）",
              checkSummary->text());

        // ---------- ⑧ 玩家名单：四份文件、加一个 / 拿掉一个都真写盘 ----------
        {
            auto *playerNote =
                page->findChild<QLabel *>(MainWindowUI::serverSettingsPlayerNoteName());
            auto *playerCombo =
                sections->topLevelItem(static_cast<int>(ServerSettingsPage::Section::Players));
            auto *playerTree =
                page->findChild<QTreeWidget *>(MainWindowUI::serverSettingsPlayerTreeName());
            auto *playerAdd = page->findChild<QPushButton *>(
                MainWindowUI::serverSettingsPlayerAddButtonName());
            auto *playerRemove = page->findChild<QPushButton *>(
                MainWindowUI::serverSettingsPlayerRemoveButtonName());
            const bool playersWired =
                playerNote != nullptr && playerCombo != nullptr && playerTree != nullptr
                && playerAdd != nullptr && playerRemove != nullptr;
            check(playersWired,
                  "★ T42.10 玩家名单那一块整块找得到（下拉框 / 名单 / 两颗按钮 / 那行小字）");
            if (playersWired) {
                sections->setCurrentItem(sections->topLevelItem(static_cast<int>(ServerSettingsPage::Section::Players)));
                QApplication::processEvents();
                check(stack->currentIndex()
                              == static_cast<int>(ServerSettingsPage::Section::Players)
                          && playerCombo->childCount()
                                 == MainWindowUI::serverSettingsPlayerListCount(),
                      "★ T42.10 切到玩家那一页：四份名单都在那个下拉框里"
                      "（`serverSettingsPlayerListCount()` 是上限）",
                      QString::number(playerCombo->childCount()));
                bool comboOk = true;
                QStringList comboTitles;
                for (int i = 0; i < playerCombo->childCount(); ++i) {
                    comboTitles << playerCombo->child(i)->text(0);
                    if (playerCombo->child(i)->text(0)
                        != MainWindowUI::serverSettingsPlayerListTitle(i)) {
                        comboOk = false;
                    }
                }
                check(comboOk,
                      "★ T42.10 下拉框里那几格的字就是唯一出处里那几个名字（**那个下标**"
                      "就是 `ServerPlayers::List` 的下标）",
                      comboTitles.join(QStringLiteral(" / ")));
                check(playerTree->topLevelItemCount() == 0 && !playerRemove->isEnabled()
                          && playerAdd->isEnabled() && !playerNote->text().isEmpty(),
                      "★ T42.10 夹具里白名单还是空的 ⇒ 一棵 0 行的树、「移除…」灰着、"
                      "「加一个人…」亮着、那行小字说着这是哪份文件",
                      playerNote->text());
                const QDir serverDir(ServerRegistry::describe(serverId).dirPath);
                const auto readList = [&serverDir](ServerPlayers::List list) {
                    QFile file(serverDir.filePath(ServerPlayers::fileName(list)));
                    if (!file.open(QIODevice::ReadOnly)) {
                        return ServerPlayers::Content{};
                    }
                    return ServerPlayers::parse(list, file.readAll());
                };
                // 加一个人：那一问是**阻塞**的（`InlineMessage::getText` 自己转一层嵌套
                // 事件循环），所以答它那一发只能摆在点按钮**之前**（同 T27.6 那支笔）。
                QTimer::singleShot(0, [] {
                    InlineMessageBox *input = currentPrompt();
                    if (input == nullptr) {
                        return;
                    }
                    if (auto *edit =
                            input->findChild<QLineEdit *>(InlineMessageBox::lineEditName())) {
                        edit->setText(QStringLiteral("alex"));
                    }
                    if (auto *ok =
                            input->findChild<QPushButton *>(InlineMessageBox::buttonName(0))) {
                        ok->click();
                    } else {
                        input->close();
                    }
                });
                playerAdd->click();
                QApplication::processEvents();
                const ServerPlayers::Content afterAdd =
                    readList(ServerPlayers::List::Whitelist);
                check(afterAdd.ok && afterAdd.entries.size() == 1
                          && afterAdd.entries.first().name == QStringLiteral("alex")
                          && afterAdd.entries.first().uuid
                                 == ServerPlayers::offlineUuid(QStringLiteral("alex")),
                      "★★★ T42.10 「加一个人」⇒ 盘上 `whitelist.json` 真多一条，uuid 就是"
                      "离线 UUID（离线服务端直接认得出这人）",
                      afterAdd.entries.isEmpty() ? QStringLiteral("(空)")
                                                 : afterAdd.entries.first().uuid);
                check(playerTree->topLevelItemCount() == 1
                          && playerTree->topLevelItem(0)->text(0).contains(
                              QStringLiteral("alex")),
                      "★ T42.10 这一列当场多了一行（收尾那一下把这一页重摆了）",
                      QString::number(playerTree->topLevelItemCount()));
                playerTree->setCurrentItem(playerTree->topLevelItem(0));
                QApplication::processEvents();
                check(playerRemove->isEnabled(),
                      "★ T42.10 选中一行 ⇒ 「移除…」亮起来（一颗都没选的时候它点不动）");
                QTimer::singleShot(0, [] {
                    InlineMessageBox *box = currentPrompt();
                    if (box == nullptr) {
                        return;
                    }
                    if (auto *yes =
                            box->findChild<QPushButton *>(InlineMessageBox::buttonName(0))) {
                        yes->click();
                    } else {
                        box->close();
                    }
                });
                playerRemove->click();
                QApplication::processEvents();
                const ServerPlayers::Content afterRemove =
                    readList(ServerPlayers::List::Whitelist);
                check(afterRemove.ok && afterRemove.entries.isEmpty()
                          && playerTree->topLevelItemCount() == 0,
                      "★★★ T42.10 「移除」⇒ 盘上那一条真没了、这一列也回到 0 行"
                      "（问过一句才动手）",
                      QString::number(afterRemove.entries.size()));
            }
        }

        // ---------- ⑨ 服务端起 / 停：正开着的这一页跟着翻状态与只读闸 ----------
        {
            auto *statusLabel =
                page->findChild<QLabel *>(MainWindowUI::serverSettingsStatusName());
            auto *workspace = w.findChild<ServerWorkspaceController *>();
            check(statusLabel != nullptr && workspace != nullptr,
                  "T42.11 前提：顶上那行状态和服务端工作区都找得到");
            if (statusLabel != nullptr && workspace != nullptr) {
                sections->setCurrentItem(sections->topLevelItem(static_cast<int>(ServerSettingsPage::Section::Common)));
                QApplication::processEvents();
                // 改一格、**不保存**：下面要证明"只翻状态"不会把没保存的改动冲掉。
                textFieldOf(QStringLiteral("motd"))->setText(QStringLiteral("还没保存的说明"));
                ServerRun::ServerProcess *process = workspace->ensureProcess(serverId);
                QTemporaryDir serverRunDir;
                process->start(serverId, serverRunDir.path(),
                               QCoreApplication::applicationFilePath(),
                               {QStringLiteral("--fake-server"), QStringLiteral("normal")}, 4000);
                const bool running = pumpUntil(
                    [process] { return process->status() == ServerModel::Status::Running; }, 20000);
                check(running, "T42.11 前提：假服务端起来了（没起来后面那几条没有意义）",
                      ServerModel::statusName(process->status()));
                check(statusLabel->text() == ServerModel::statusName(ServerModel::Status::Running),
                      "★★★ T42.11 服务端一起来，正开着这一页顶上的状态**当场**翻成「运行中」"
                      "（以前要等重开这一页才看得见）",
                      statusLabel->text());
                check(!textFieldOf(QStringLiteral("motd"))->isEnabled() && !save->isEnabled(),
                      "★★ T42.11 在跑 ⇒ 整页只读、保存点不动（它退出时会把自己内存里那份"
                      "`server.properties` 写回盘上）");
                check(textFieldOf(QStringLiteral("motd"))->text()
                          == QStringLiteral("还没保存的说明"),
                      "★★★ T42.11 只翻状态、不重摆表单 ⇒ 用户还没保存的那一格原样还在");
                process->requestStop();
                const bool stopped =
                    pumpUntil([process] { return !process->isRunning(); }, 20000);
                check(stopped, "T42.11 前提：假服务端停下来了",
                      ServerModel::statusName(process->status()));
                check(statusLabel->text() == ServerModel::statusName(process->status()),
                      "★★ T42.11 停下来之后状态那行也跟着翻回去", statusLabel->text());
                check(textFieldOf(QStringLiteral("motd"))->isEnabled(),
                      "★★ T42.11 停了 ⇒ 这一页又能改");
            }
        }

        // ---------- ⑩ 收页 ----------
        if (auto *back = host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
            back->click();  // 返回箭头 = reject（见 OverlayHost::showPage）
        }
    });
    QMetaObject::invokeMethod(&w, "openServerSettingsPage", Qt::DirectConnection,
                              Q_ARG(QString, serverId));
    QApplication::processEvents();

    check(pageSeen, "★★ T42.0 点出来的是那一页（`openServerSettingsPage` 经元对象系统驱动）");
    check(currentOverlay() == nullptr, "★ T42.8 按返回箭头 ⇒ 那一页收掉了（没卡在嵌套循环里）");
    // 收页之后盘上那份文件还在（这一节写的是真盘）。
    check(propsTextOnDisk().contains(QStringLiteral("pvp=false")),
          "★ T42.8 收页之后盘上那份 `server.properties` 还是改过的那一份");

    // 用服务端列表的实际启动入口，让假 Java 等到测试发来就绪信号。
    auto *tree = w.findChild<QTreeWidget *>(MainWindowUI::serverListName());
    auto *delegate = tree ? qobject_cast<ListRowDelegate *>(tree->itemDelegate()) : nullptr;
    auto *workspace = w.findChild<ServerWorkspaceController *>();
    auto *activity = w.findChild<ActivityCenterController *>();
    const QModelIndex row = tree ? tree->model()->index(0, 0) : QModelIndex();
    const bool startFixtureReady = delegate && workspace && activity &&
        row.data(Qt::UserRole).toString() == serverId && ServerRegistry::acceptEula(serverId);
    check(startFixtureReady, "T42.12 服务端列表有一份已同意 EULA 的启动夹具");
    if (startFixtureReady) {
        const auto toggleServer = [&] { delegate->actionTriggered(row, ListRowDelegate::Action::Launch); };
        auto *tasks = activity->tasks();
        const auto activeLaunch = [&] {
            for (const auto &task : tasks->active()) {
                if (task.kind == Tasks::TaskKind::ServerLaunch && task.targetId == serverId) {
                    return task.id;
                }
            }
            return Tasks::invalidTaskId();
        };
        toggleServer();
        auto *process = workspace->process(serverId);
        const auto readyTask = activeLaunch();
        check(readyTask != Tasks::invalidTaskId() && process &&
                  process->status() == ServerModel::Status::Starting &&
                  tasks->snapshot(readyTask).detail.contains(QStringLiteral("就绪")),
              "T42.12 服务端尚未就绪时，任务中心显示启动任务");
        if (process && process->isRunning()) {
            check(process->sendCommand(QStringLiteral("ready")) && pumpUntil([&] {
                      return tasks->snapshot(readyTask).state == Tasks::TaskState::Succeeded;
                  }, 5000), "T42.12 收到服务端就绪信号才标记成功");
            toggleServer();
            check(pumpUntil([&] { return !process->isRunning(); }, 5000) &&
                      tasks->snapshot(readyTask).state == Tasks::TaskState::Succeeded,
                  "T42.12 停服后启动任务继续保留成功记录");
        }
        if (process && !process->isRunning()) {
            toggleServer();
            const auto cancelledTask = activeLaunch();
            toggleServer();
            check(pumpUntil([&] {
                      return !process->isRunning() &&
                             tasks->snapshot(cancelledTask).state == Tasks::TaskState::Cancelled;
                  }, 5000), "T42.12 启动期间主动停止，任务记为已取消");
        }
        if (process && !process->isRunning()) {
            toggleServer();
            const auto failedTask = activeLaunch();
            QTimer dismiss;
            QObject::connect(&dismiss, &QTimer::timeout, [] {
                if (auto *prompt = currentPrompt()) {
                    if (auto *button =
                            prompt->findChild<QPushButton *>(InlineMessageBox::buttonName(0))) {
                        button->click();
                    }
                }
            });
            dismiss.start(25);
            check(process->sendCommand(QStringLiteral("exit-before-ready")) && pumpUntil([&] {
                      const auto task = tasks->snapshot(failedTask);
                      return task.state == Tasks::TaskState::Failed &&
                             task.errorSummary.contains(QStringLiteral("退出码 0"));
                  }, 5000), "T42.12 未就绪便退出，退出码为 0 也记为启动失败");
        }
    }

    w.close();
    QApplication::processEvents();
    sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
    sandbox.sync();
    AppPaths::setMinecraftDir(originalDir);
    check(AppPaths::minecraftDir() == originalDir, "T42.9 数据根目录换回来了");
}

