#include "Support.hpp"
#include "ListRowDelegate.hpp"
int MwFolderSwitchingChecks(const QDir &root, MainWindow &window) {
    // ------------------------------------------------------------------
    // 这一节会改**进程级**的 AppPaths 覆盖（切文件夹就是改它），所以末尾必须把它和
    // 盘上那个键都还回默认 —— 后面的节（以及将来加在后面的节）全靠"当前根目录还是
    // 沙盒里那份 .minecraft"这个前提在扫盘。
    {
        // 另一份"完整的 .minecraft"：里面只放一个别的版本，用来看切过去之后实例列表
        // 真的换了一批，而不只是标签变了。
        const QString otherRoot = root.filePath(QStringLiteral("other-mc"));
        const QDir otherVersions(otherRoot + QStringLiteral("/versions"));
        otherVersions.mkpath(QStringLiteral("."));
        makeInstalledVersion(otherVersions, QStringLiteral("1.7.10"));

        // 两个路径是不是同一个目录：两边都过一遍 canonicalFilePath，免得盘符大小写、
        // 斜杠方向、`.` 这些写法差异把断言弄挂。
        const auto sameDir = [](const QString &a, const QString &b) {
            const QString ca = QFileInfo(a).canonicalFilePath();
            const QString cb = QFileInfo(b).canonicalFilePath();
            return !ca.isEmpty() && ca == cb;
        };
        // NOTE: 文件夹那一列挂出来的按钮都带 objectName，按名字找（那一列只剩「添加
        // 文件夹…」一个 —— 「移除文件夹…」在行尾齿轮里）。
        // 按文案认控件是这套验证里明确不要的做法 —— 文案会随产品改，改完的第一次跑
        // 会变成"找不到控件、后面全不跑"，而挂出来的样子看不出是文案变了。

        // 按**上面那行字**找行号 —— 不许写死行号。那一列的行里写什么会变（用户给某一份
        // 起了 customname 之后就不是目录名了），写死行号的话改一处就静默量错一行；而且
        // 行本身现在是 setItemWidget 塞进去的控件，`item->text()` 是空的，也没有第二个
        // 能用来认人的字段。和底栏那块面板里的 rowIndexOf 是同一个取法。
        //
        // 行是连续摆的（setFolderRows 按下标生成 objectName），所以找不着 = 到头了。
        // 返回 -1 让调用方照实断言"没找到"，而不是拿 0 去量错行。
        const auto folderRowIndexOf = [](QWidget *window, const QString &title) {
            // 先把"关掉但还没真删掉"的那一批控件催掉。列表重灌（setFolderRows 里那句
            // clear()）和菜单关掉（WA_DeleteOnClose）走的都是 deleteLater —— 不催的话
            // 下面那句 findChild 会捞到**上一轮**的行控件（它们还挂在 viewport 上），
            // 于是量到的是已经不在列表里的那一行。症状特别像"产品没刷新"。
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            for (int i = 0;; ++i) {
                QLabel *label = window->findChild<QLabel *>(MainWindowUI::folderRowTitleName(i));
                if (label == nullptr) {
                    return -1;
                }
                if (label->text() == title) {
                    return i;
                }
            }
        };

        // 打开某一行的齿轮菜单，返回那块框（找不到 = nullptr）。这一节有三处要用：
        // 自带那份（移不掉）、"用户加的那份可以移除"、以及 P+ 真点一次「移除」。
        //
        // NOTE: 点之前先 close 掉可能还开着的那一块。**不这么做的话这一下点是"收回"而不是
        // "弹出"**：openFolderMenu 认 anchor，同一颗齿轮上的第二次点击一律当收回
        // （用户点一下开、再点一下关，这是要的行为）—— 于是这里拿到 nullptr，而
        // 调用方那些断言全写成 `if (menu != nullptr)`，症状是"那一段静默地什么都不做、
        // 一条 FAIL 都不出现"。
        // NOTE: 关掉的那块是 WA_DeleteOnClose（close() 只排了个 deleteLater），这里催一下，
        // 免得下一次 findChild 捞到那块**已经关掉的**。
        const auto openGearMenuOfRow = [&folderRowIndexOf](QWidget *window,
                                                          const QString &title) -> QWidget * {
            QApplication::processEvents();
            const int row = folderRowIndexOf(window, title);
            if (row < 0) {
                return nullptr;
            }
            auto *gear = window->findChild<QPushButton *>(MainWindowUI::folderRowGearName(row));
            if (gear == nullptr) {
                return nullptr;
            }
            if (auto *stale = window->findChild<QWidget *>(MainWindowUI::folderMenuName())) {
                stale->close();
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            gear->click();
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return window->findChild<QWidget *>(MainWindowUI::folderMenuName());
        };
        // 点菜单里的某一项。**找不到、或者那一项是灰的就什么都不点**（返回 false）——
        // 让调用方照实断言，不在这里解空指针把整轮跑崩在别的地方。
        // NOTE: 点完那一下不一定就做完了：FolderMenu::dispatch() 是 close() 之后
        // `QTimer::singleShot(0, ...)` 把动作排到**下一轮事件循环**（它是个 Qt::Popup，
        // 按着鼠标键盘的 grab，模态窗开在它底下点不动）。所以这里要 processEvents()
        // 把这颗 0 毫秒的定时器放出来。
        const auto clickFolderMenuAction = [](QWidget *menu, int action) -> bool {
            if (menu == nullptr) {
                return false;
            }
            auto *entry =
                menu->findChild<QPushButton *>(MainWindowUI::folderMenuActionName(action));
            if (entry == nullptr || !entry->isEnabled()) {
                return false;
            }
            entry->click();
            QApplication::processEvents();
            return true;
        };

        // ---- 冷启动：自带那份永远在列表里、而且移不掉 ----
        {
            MainWindow w6;
            w6.show();
            QApplication::processEvents();
            QListWidget *folders = byObjectName<QListWidget>(&w6, MainWindowUI::folderListName());
            QTreeWidget *instances =
                byObjectName<QTreeWidget>(&w6, MainWindowUI::instanceListName());
            QLabel *folderLabel = w6.findChild<QLabel *>(MainWindowUI::instanceFolderLabelName());
            check(folders != nullptr && instances != nullptr && folderLabel != nullptr,
                  "第六个窗口的文件夹列表/实例列表/「当前文件夹」那句话都找得到");
            if (folders != nullptr && instances != nullptr && folderLabel != nullptr) {
                check(folders->count() == 1, "用户还没添加过，列表里只有自带那份",
                      QString::number(folders->count()));
                const QString shownPath = folders->currentItem() != nullptr
                                              ? folders->currentItem()->data(Qt::UserRole).toString()
                                              : QString();
                check(folders->currentItem() != nullptr &&
                          sameDir(shownPath, AppPaths::defaultMinecraftDir()),
                      "那一条就是启动器自带的那份 .minecraft",
                      shownPath + QStringLiteral(" / ") + AppPaths::defaultMinecraftDir());
                // 那一条上面写的是「当前文件夹」—— 单行列表时代那个「（自带）」后缀不写了。
                // 字在**行里那个子控件**上，所以 item->text() 是空的（和上面那条按 UserRole
                // 认人的断言不冲突）。
                const int assistantRow =
                    folderRowIndexOf(&w6, MainWindowUI::folderAssistantTitle());
                QLabel *assistantSubtitle =
                    assistantRow >= 0
                        ? w6.findChild<QLabel *>(MainWindowUI::folderRowSubtitleName(assistantRow))
                        : nullptr;
                check(assistantRow >= 0, "自带那份在上面那行写的是「当前文件夹」",
                      QString::number(assistantRow));
                check(assistantSubtitle != nullptr &&
                          assistantSubtitle->text() ==
                              QDir::toNativeSeparators(AppPaths::defaultMinecraftDir()),
                      "下面那行是它完整的路径（系统分隔符，别把 QDir 的 `/` 露给用户）",
                      assistantSubtitle != nullptr ? assistantSubtitle->text() : QString());
                // 「移除」只有行尾齿轮菜单里那一项（用户原话「下面那个移除文件夹也可以
                // 弃掉了」）—— 自带那份移不掉的判据就在菜单里量。
                {
                    QWidget *menu = openGearMenuOfRow(&w6, MainWindowUI::folderAssistantTitle());
                    auto *removeEntry = menu != nullptr
                                            ? menu->findChild<QPushButton *>(
                                                  MainWindowUI::folderMenuActionName(
                                                      MainWindowUI::FolderActionRemove))
                                            : nullptr;
                    check(removeEntry != nullptr && !removeEntry->isEnabled(),
                          "齿轮菜单里的「移除」是灰的 —— 自带那份移不掉",
                          removeEntry != nullptr ? QStringLiteral("enabled=1")
                                                 : QStringLiteral("那一项没找到"));
                    if (menu != nullptr) {
                        menu->close();
                        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    }
                }
                // 数的是**实例条目**（instanceItemsOf 跳过组头）。沙盒里本来没有
                // wtfml.folders，开这个窗的时候产品把那五个默认组种了进去
                // （用户要的那五个），于是树是**分组**的 —— 三个实例一条不少，
                // 只是各挂在「可安装mod」/「原版」底下。
                const QList<QTreeWidgetItem *> unsorted = instanceItemsOf(instances);
                check(unsorted.size() == 3,
                      "右边列出沙盒里的 3 个实例（一条不多一条不少）",
                      QString("%1 条 / 顶层 %2 个")
                          .arg(unsorted.size())
                          .arg(instances->topLevelItemCount()));
                int underHeaders = 0;
                for (int i = 0; i < instances->topLevelItemCount(); ++i) {
                    underHeaders += instances->topLevelItem(i)->childCount();
                }
                // 组头只画**有实例**的那些：三个实例分属「可安装mod」和「原版」，
                // 另外三个默认组和「未分组」都是空的（用户要的"空白的分组隐藏"）。
                check(underHeaders == 3 && groupHeadersOf(instances).size() == 2,
                      "三个实例全都挂在组头底下（画出来的是有实例的那两个组）",
                      groupHeadersOf(instances).join(QLatin1Char('|')));
                check(QFile::exists(AppPaths::instanceGroupFilePath()),
                      "开这一趟窗就把默认组种到盘上了");
                check(folderLabel->text().contains(QStringLiteral("3 个实例")) &&
                          folderLabel->text().contains(QStringLiteral(".minecraft")),
                      "「当前文件夹」那句话写清了是哪一份、有几个实例", folderLabel->text());
            }
        }

        // ---- 添加 + 切换：走生产类（界面那条路要开文件对话框，跑不了） ----
        {
            LauncherSettings seed;
            FolderManager folders(&seed);
            check(folders.addFolder(otherRoot), "加上第二个游戏文件夹");
            check(sameDir(folders.currentFolder(), AppPaths::defaultMinecraftDir()),
                  "光加上不等于切过去，当前还是自带那份");
            // 返回值是"这条路径现在在列表里"，不是"这次新加进去了" —— 所以重复加
            // 也算成功，但列表里**不能**多一条。界面那边靠这个语义区分"路径不存在"
            // （要弹窗）和"本来就加过"（什么都不用做）。
            check(folders.addFolder(otherRoot) && folders.folders().size() == 2,
                  "同一份再加一次算成功，但列表里没有重复项",
                  QString::number(folders.folders().size()));
            check(!folders.addFolder(root.filePath(QStringLiteral("没有这个目录"))),
                  "不存在的目录加不进去");
            check(folders.setCurrentFolder(otherRoot), "切到新加的那一份");
            check(!folders.setCurrentFolder(root.filePath(QStringLiteral("没有这个目录"))),
                  "不在列表里的目录切不过去");
            {
                // 另开一个读盘者当对照，别拿同一个对象自证。
                LauncherSettings probe;
                const QStringList stored = probe.folders();
                // 盘上只存**用户加的那些**：自带那份由 exe 位置算出来，存下来会在
                // 启动器挪位置之后变成一个指向旧地方的死路径。
                check(stored.size() == 1 && sameDir(stored.at(0), otherRoot),
                      "用户加的那份落了盘，而且只有它（自带那份不入库）",
                      stored.join(QLatin1Char('|')));
                check(sameDir(probe.currentFolder(), otherRoot), "当前那一份也落了盘",
                      probe.currentFolder());
            }
        }

        // ---- 冷启动读回来：路径、列表、实例一起换 ----
        {
            MainWindow w7;
            w7.show();
            QApplication::processEvents();
            QListWidget *folderListView =
                byObjectName<QListWidget>(&w7, MainWindowUI::folderListName());
            QTreeWidget *instances =
                byObjectName<QTreeWidget>(&w7, MainWindowUI::instanceListName());
            QLabel *folderLabel = w7.findChild<QLabel *>(MainWindowUI::instanceFolderLabelName());
            check(sameDir(AppPaths::minecraftDir(), otherRoot),
                  "开窗就把 AppPaths 切到记着的那一份（全局路径跟着走）",
                  AppPaths::minecraftDir());
            if (folderListView != nullptr && instances != nullptr && folderLabel != nullptr) {
                check(folderListView->count() == 2, "列表里是自带那份 + 用户加的那份",
                      QString::number(folderListView->count()));
                check(folderListView->currentItem() != nullptr &&
                          sameDir(folderListView->currentItem()->data(Qt::UserRole).toString(),
                                  otherRoot),
                      "选中的是当前在用的那一份");
                const QList<QTreeWidgetItem *> instanceItems = instanceItemsOf(instances);
                check(instanceItems.size() == 1, "右边的实例列表换成了那一份里的 1 个",
                      QString::number(instanceItems.size()));
                check(instanceItems.size() == 1 &&
                          instanceItems.first()->data(0, Qt::UserRole).toString() ==
                              QStringLiteral("1.7.10"),
                      "列的就是那一份里的 1.7.10");
                check(!instanceItems.isEmpty() &&
                          !instanceItems.first()->data(0, ListRowDelegate::SubtitleRole).toString()
                               .contains(QStringLiteral("游玩")),
                      "没有游玩记录的实例只显示原有副标题");
                auto *versions = w7.findChild<VersionManager *>();
                const QString stableId = versions ? versions->ensureInstanceId(QStringLiteral("1.7.10")) : QString();
                const qint64 playedMs = 75 * 60 * 1000;
                check(!stableId.isEmpty() && LaunchHistory::savePlayTime(LaunchHistory::historyPath(), stableId,
                          {playedMs, playedMs}), "为另一个文件夹里的实例保存一条游玩记录");
                check(folderLabel->text().contains(QStringLiteral("1 个实例")),
                      "「当前文件夹」那句话跟着换了", folderLabel->text());
                auto *manager = w7.findChild<FolderManager *>();
                auto *pill = w7.findChild<QPushButton *>(MainWindowUI::instanceFolderPillName());
                const bool moved = manager && manager->setCurrentFolder(AppPaths::defaultMinecraftDir());
                check(moved, "从文件夹管理器切回第一个目录");
                if (moved) {
                    w7.ui()->getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
                    auto *picker = w7.findChild<QAbstractButton *>(MainWindowUI::homeCurrentPickerName());
                    if (picker) picker->click();
                    QApplication::processEvents();
                    auto *list = w7.findChild<QListWidget *>(QStringLiteral("homePickerInstanceList"));
                    if (list) {
                        for (int row = 0; row < list->count(); ++row) {
                            if (list->item(row)->data(Qt::UserRole).toString() == QStringLiteral("1.7.10")) {
                                emit list->itemClicked(list->item(row));
                                break;
                            }
                        }
                    }
                    const auto rows = instanceItemsOf(instances);
                    check(list && pill && sameDir(manager->currentFolder(), otherRoot) &&
                              folderListView->currentItem() &&
                              sameDir(folderListView->currentItem()->data(Qt::UserRole).toString(), otherRoot) &&
                              pill->text() == QDir(otherRoot).dirName() && rows.size() == 1 &&
                              rows.first()->data(0, Qt::UserRole).toString() == QStringLiteral("1.7.10"),
                          "从主页选择其他目录的实例后，管理页的文件夹、胶囊和实例列表同步",
                          QStringLiteral("picker=%1 list=%2 root=%3 pill=%4 rows=%5")
                              .arg(picker != nullptr).arg(list != nullptr).arg(manager->currentFolder(),
                                  pill ? pill->text() : QString()).arg(rows.size()));
                    check(rows.size() == 1 && rows.first()->data(0, ListRowDelegate::SubtitleRole).toString()
                              .contains(QStringLiteral("游玩 %1").arg(LaunchHistory::playTimeText(playedMs))),
                          "有游玩记录的实例在副标题显示游玩时长");
                    if (!sameDir(manager->currentFolder(), otherRoot)) manager->setCurrentFolder(otherRoot);
                }
                // 同上：判据从"那个按钮亮了"搬到"齿轮里那一项亮了"（就是同一件事，
                // 只是入口换了地方）。
                {
                    QWidget *menu = openGearMenuOfRow(&w7, QDir(otherRoot).dirName());
                    auto *removeEntry = menu != nullptr
                                            ? menu->findChild<QPushButton *>(
                                                  MainWindowUI::folderMenuActionName(
                                                      MainWindowUI::FolderActionRemove))
                                            : nullptr;
                    check(removeEntry != nullptr && removeEntry->isEnabled(),
                          "用户加的那份可以移除 —— 齿轮里那一项是亮的");
                    if (menu != nullptr) {
                        menu->close();
                        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    }
                }
            }
        }

        // ---- 移除当前在用的那一份：退回自带那份 ----
        {
            LauncherSettings seed;
            FolderManager folders(&seed);
            check(sameDir(folders.currentFolder(), otherRoot), "冷启动读到的是上次切过去的那份");
            check(!folders.removeFolder(AppPaths::defaultMinecraftDir()), "自带那份移不掉");
            check(folders.removeFolder(otherRoot), "把自己加的那份移除掉");
            check(sameDir(folders.currentFolder(), AppPaths::defaultMinecraftDir()),
                  "删掉的正好是当前那份 → 退回自带那份（不指着一个已经不在列表里的目录）",
                  folders.currentFolder());
            check(!folders.removeFolder(otherRoot), "再移除一次就没有了");
        }

        // ---- 「移除」＝只动列表：盘上的文件一个字节都不能少 ----
        // 「移除」只写设置、从不碰盘（FolderManager::removeFolder）—— 但这里必须真点
        // 一次齿轮里那一项（确认框也真点「是」），然后看盘上："读代码觉得它对"和
        // "使用者敢按"是两件事。
        //
        // 这一节用 before/after 的写法：先在那一份目录里放两个"用户的东西"（一个在根上、
        // 一个在存档目录里），移除之后它们必须一个字节都没少。
        {
            const QString keepFile = otherRoot + QStringLiteral("/存档-别删我.txt");
            const QString keepNested = otherRoot + QStringLiteral("/saves/世界/level.dat");
            writeFileAt(keepFile, QByteArrayLiteral("移除只该动列表"));
            writeFileAt(keepNested, QByteArrayLiteral("世界存档"));
            {
                // 上面那节刚把它移除掉，这里加回列表并**切成当前在用的那一份** ——
                // "移除的正是在用的那一份"是这条路上最贵的一条（移除之后启动器得改用别的
                // 那一份、全局路径跟着换），要验就验它。
                LauncherSettings seed;
                FolderManager folders(&seed);
                check(folders.addFolder(otherRoot) && folders.setCurrentFolder(otherRoot),
                      "P+ 先把那一份加回列表、并切成当前在用的那一份");
            }
            {
                MainWindow w8;
                w8.show();
                QApplication::processEvents();
                QListWidget *folderListView =
                    byObjectName<QListWidget>(&w8, MainWindowUI::folderListName());
                // 「移除」的入口 从下面那个按钮换成了行尾齿轮菜单里那一项
                // （要求「下面那个移除文件夹也可以弃掉了」）—— 这一节要验的那件事
                // （只动列表、盘上一个字节不动）一个字没变，只是点的地方换了。
                QWidget *menu = openGearMenuOfRow(&w8, QDir(otherRoot).dirName());
                auto *removeEntry = menu != nullptr
                                        ? menu->findChild<QPushButton *>(
                                              MainWindowUI::folderMenuActionName(
                                                  MainWindowUI::FolderActionRemove))
                                        : nullptr;
                check(folderListView != nullptr && removeEntry != nullptr &&
                          removeEntry->isEnabled(),
                      "P+ 第八个窗口：列表找得到、齿轮里的「移除」是亮的");
                if (folderListView != nullptr && removeEntry != nullptr &&
                    removeEntry->isEnabled()) {
                    const int removalsBefore =
                        countOccurrences(messagesSnapshot(), QStringLiteral("已从列表里移除文件夹"));
                    // 先安排人点「是」。那个确认框是这一步**唯一**说清"不删文件"的地方，
                    // 不点它这一步走不完（点掉窗 = 取消，什么都没发生）。
                    answerNextPrompt(0);
                    // NOTE: 这一下点完之后动作**不在当前调用栈里**：FolderMenu::dispatch()
                    // 先 close() 再 `QTimer::singleShot(0, ...)` 把动作排到下一轮事件
                    // 循环（它是个 Qt::Popup，按着鼠标键盘的 grab，模态窗开在它底下
                    // 点不动）。answerNextModal 排的是 0ms + 300ms 两条一次性定时器，
                    // 所以整条路是：这一下 processEvents() 先放出那颗 0ms 定时器 →
                    // 它开确认框、exec() 把事件循环嵌进去 → 300ms 那一发在**里面**
                    // 点掉它 → exec() 返回、processEvents() 才回来。
                    removeEntry->click();
                    QApplication::processEvents();
                    check(g_modalTitle == QStringLiteral("从列表里移除文件夹"),
                          "P+ 点下去弹的是「从列表里移除文件夹」的确认框", g_modalTitle);
                    check(g_modalText.contains(QStringLiteral("从启动器列表中移除")) &&
                              g_modalText.contains(QStringLiteral("目录和其中的文件将保留")),
                          "P+ 确认框把「只动列表、盘上文件不动」写死了", g_modalText);
                    check(g_modalText.contains(QStringLiteral("将使用「")) &&
                              g_modalText.contains(QDir::toNativeSeparators(AppPaths::defaultMinecraftDir())),
                          "P+ 它正是在用的那一份 → 确认框说了移除之后改用哪一份", g_modalText);
                    check(!g_modalWasWarning, "P+ 这一步是提示框不是警告框（它不危险）");
                    check(folderListView->count() == 1, "P+ 列表里只剩自带那份了",
                          QString::number(folderListView->count()));
                    check(QFile::exists(keepFile) && QFile::exists(keepNested),
                          "P+ **盘上那两个文件都还在**（用户要的就是这一条）",
                          keepFile + QStringLiteral(" / ") + keepNested);
                    // NOTE: 假实例是 makeInstalledVersion() 造的，它写的是 jar（见那个
                    // 函数）—— 别按 json 去查，那条路径压根不存在。
                    check(QFileInfo(otherRoot).isDir() &&
                              QFile::exists(otherRoot + QStringLiteral("/versions/1.7.10/"
                                                                     "1.7.10.jar")),
                          "P+ 那一份目录本身、以及里面的实例文件也都还在", otherRoot);
                    check(countOccurrences(messagesSnapshot(), QStringLiteral("已从列表里移除文件夹")) ==
                              removalsBefore + 1,
                          "P+ 日志说的是「已从列表里移除文件夹」，不是「已删除文件夹」");
                    check(countOccurrences(messagesSnapshot(), QStringLiteral("已删除文件夹")) == 0,
                          "P+ 整条路上一次「已删除文件夹」都没写过");
                }
            }
            // 收尾：把这一节加的那份从设置里也拿掉。沙盒最后是整块删的，但中途还会开窗的节
            // 会照实把列表画出来 —— 留一份"盘上已经没有"的目录在列表里是给别的节下绊子。
            {
                LauncherSettings seed;
                FolderManager folders(&seed);
                // 上面那次点击已经把它从列表里拿掉了，所以这里**必须**移除失败 ——
                // 反着断言正好一并验了"那次点击真的写进了设置"（点它的是另一个
                // FolderManager 实例，不落盘这里就会移除成功）。
                check(!folders.removeFolder(otherRoot),
                      "P+ 收尾：它已经不在列表里了（上次点击就移除掉了，而且落了盘）");
                check(folders.setCurrentFolder(AppPaths::defaultMinecraftDir()),
                      "P+ 收尾：切回自带那份");
            }
        }

        // ---- 删除文件夹**不追链接**----
        // 这一节是数据层直驱（不经界面）：会碰盘的是 FolderManager::deleteFolder()。
        //
        // NOTE: 判据**不能**只写 isSymLink()。实测 mklink /J 造出来的 junction 在 Qt 里
        // isSymLink() 是 **false**、canonicalFilePath() 也**原样返回字面路径**（根本
        // 不解链接），只有 isJunction() 认得出它。所以 ① / ② 必须用**真 junction** 来验
        // —— 拿符号链接验的话，防不住的那一半照样绿。
        //
        // 而"防不住的那一半"就是符号链接（canonicalFilePath() 对它**是解**
        // 的），见下面 ③。只验 ① / ② 的结论是**假的全绿**：能造的那一类防住了，
        // 防不住的那一类没进过测试。
        {
            const QString realDir = root.filePath(QStringLiteral("junction-target"));
            const QString targetFile = realDir + QStringLiteral("/世界存档.txt");
            const QString linkAsFolder = root.filePath(QStringLiteral("junction-as-folder"));
            const QString holder = root.filePath(QStringLiteral("junction-holder"));
            const QString innerLink = holder + QStringLiteral("/inner-mods");
            QDir().mkpath(realDir);
            QDir().mkpath(holder);
            writeFileAt(targetFile, QByteArrayLiteral("链接那一头的东西一个字节都不许动"));

            const auto makeJunction = [](const QString &link, const QString &target,
                                         const QString &kind = QStringLiteral("/J")) {
                QProcess proc;
                proc.start(QStringLiteral("cmd"),
                           {QStringLiteral("/c"), QStringLiteral("mklink"), kind,
                            QDir::toNativeSeparators(link),
                            QDir::toNativeSeparators(target)});
                proc.waitForFinished(10000);
                // 认的是 isJunction()（不是 exists()）：link 不存在时 exists() 也是 false，
                // 那样这条会"绿着"跳过整节。
                return QFileInfo(link).isJunction() || QFileInfo(link).isSymLink();
            };
            const bool madeA = makeJunction(linkAsFolder, realDir);
            const bool madeB = makeJunction(innerLink, realDir);
            check(madeA && madeB,
                  "先造出两个真 junction（mklink /J，不要管理员权限的那一种）",
                  QStringLiteral("A=%1 B=%2").arg(madeA ? 1 : 0).arg(madeB ? 1 : 0));
            check(madeA && !QFileInfo(linkAsFolder).isSymLink(),
                  "★ 且它不是 isSymLink() 认的那种（只判 isSymLink 的写法对这一类完全没设防）",
                  QStringLiteral("isSymLink=%1 isJunction=%2")
                      .arg(QFileInfo(linkAsFolder).isSymLink() ? 1 : 0)
                      .arg(QFileInfo(linkAsFolder).isJunction() ? 1 : 0));

            {
                LauncherSettings seed;
                FolderManager folders(&seed);
                check(folders.addFolder(linkAsFolder),
                      "把那个 junction **当一份游戏文件夹**加进列表（它看起来就是个普通目录）");
            }
            {
                LauncherSettings seed;
                FolderManager folders(&seed);
                QString error;
                check(folders.deleteFolder(linkAsFolder, &error),
                      "删除这一份（它其实是个 junction）", error);
                check(!QFileInfo(linkAsFolder).exists(),
                      "① 链接本身摘掉了（列表里那一份确实不见了）");
                check(QFile::exists(targetFile),
                      "★ ① 链接那一头的东西**还在**（追进去删掉就是这一节防的那个洞）");
            }

            // ② 链接在目录**里面**：删父目录时不能顺手把 inner-mods 那一头也清了。
            // 这一种更接近真实现场 —— 用户正常加的是游戏文件夹本身，而它里面某一层
            // 挂着一个 junction（saves / mods 挂到别处）。
            writeFileAt(holder + QStringLiteral("/本层自己的文件.txt"),
                        QByteArrayLiteral("这一层的东西该跟着删"));
            {
                LauncherSettings seed;
                FolderManager folders(&seed);
                check(folders.addFolder(holder), "把装着 junction 的那个目录加进列表");
            }
            {
                LauncherSettings seed;
                FolderManager folders(&seed);
                QString error;
                check(folders.deleteFolder(holder, &error),
                      "删除这一份（它里面挂着一个 junction）", error);
                check(!QFileInfo(holder).exists(),
                      "② 父目录整棵删掉了（不能因为「有链接不敢删」就整个留下）");
                check(!QFileInfo(innerLink).exists(),
                      "② 里面那个链接本身也跟着摘了（它属于这一棵）");
                check(QFile::exists(targetFile),
                      "★ ② 链接那一头的东西还是**一个字节都没动**");
            }

            // ③ **真正的符号链接**（`mklink /D`）—— 补的那一格。
            //
            // ① / ② 只拿 junction 验，可 junction 恰好是"canonicalFilePath() 不解"的那一类
            // （见上面那条 NOTE， 用探针 linkprobe 又量了一遍，还是不解）。
            // 符号链接那一类**是解的** ⇒ 列表里存进去的就已经是链接那一头的路径，
            // "删除这一份"删的是那边 —— 上面那个 guard 一辈子看不到它。所以这一格验的是
            // `normalized()` 那条新分支：**加进来之后列表里还是用户加的那条路径**。
            const QString symLinkAsFolder = root.filePath(QStringLiteral("symlink-as-folder"));
            const bool madeSym = makeJunction(symLinkAsFolder, realDir, QStringLiteral("/D"));
            if (madeSym) {
                check(QFileInfo(symLinkAsFolder).isSymLink(),
                      "③ 造出来的确实是 isSymLink() 认的那一种（junction 那一类 isSymLink 是 0）",
                      QStringLiteral("isSymLink=%1 isJunction=%2")
                          .arg(QFileInfo(symLinkAsFolder).isSymLink() ? 1 : 0)
                          .arg(QFileInfo(symLinkAsFolder).isJunction() ? 1 : 0));
                {
                    LauncherSettings seed;
                    FolderManager folders(&seed);
                    check(folders.addFolder(symLinkAsFolder),
                          "把符号链接**当一份游戏文件夹**加进列表");
                    check(folders.folders().contains(symLinkAsFolder),
                          "★ ③ 列表里存的是**用户加的那条链接路径**（解成它指向的目录就是那个洞）",
                          folders.folders().join(QStringLiteral(" | ")));
                    check(!folders.folders().contains(QDir::cleanPath(realDir)),
                          "★ ③ 而且那一头那个真目录**没有**混进列表");
                }
                {
                    LauncherSettings seed;
                    FolderManager folders(&seed);
                    folders.addFolder(symLinkAsFolder);
                    QString error;
                    check(folders.deleteFolder(symLinkAsFolder, &error),
                          "删除这一份（它其实是个符号链接）", error);
                    check(!QFileInfo(symLinkAsFolder).exists(),
                          "③ 链接本身摘掉了（列表里那一份确实不见了）");
                    check(QFile::exists(targetFile),
                          "★ ③ 链接那一头的东西**还在**（追进去删掉就是这一节防的那个洞）");
                }
            } else {
                std::printf("     （本机没有创建符号链接的权限 —— ③ 那一格**没验**；"
                            "判据是 FolderManager::normalized() 里那条 isLinkToElsewhere() "
                            "分支）\n");
            }
        }

        // ---- 齿轮菜单：五项各点一遍 ----
        // 要求：「每个版本右侧有个齿轮，点击以后会有个类似右键菜单的页面（框），里面是
        // 刷新 重命名 打开（explorer.exe） 移除 删除（红色字）……齿轮是按钮（以免你不理解）」。
        //
        // 这一节点的全是**真按钮**（行尾那颗齿轮、菜单里那五项都是 QPushButton，不是自绘
        // 加事件过滤器）：item view 里自绘那套压不住 itemClicked，也点不出 hover / 按下 /
        // disabled 三态 —— 用户那句「齿轮是按钮」正是指这个，所以这里按 objectName 把它们
        // 一一捞回来真点。
        //
        // NOTE: 每做一次动作，上面捕下来的指针就全成野的了，必须重新找。删掉它们的有两处：
        // ①「刷新」重灌整个列表（setFolderRows 里那句 clear() 把行控件连同里面那两行字
        // 和齿轮一起删了）；②菜单点完自己 close()，而它是 WA_DeleteOnClose。
        //
        // NOTE: **不点「重命名」那一项**。它弹的是 QInputDialog::getText，模态框在 offscreen
        // 下驱动不了（一按测试就卡在没人点的窗上）—— 和 S 节实例改名那条路同一个理由，
        // 产品代码也是同一个拆法：改名字这件事的**纯逻辑那一半**（FolderManager::
        // setDisplayName / validateDisplayName、以及 MainWindow::folderTitle 那个
        // 优先级）在下面那一小段里直接驱动。
        {
            const QString vanished = root.filePath(QStringLiteral("vanished-mc"));
            const QString doomedRoot = root.filePath(QStringLiteral("doomed-mc"));
            const QString doomedFile = doomedRoot + QStringLiteral("/saves/存档-会没的.txt");
            QDir().mkpath(vanished + QStringLiteral("/versions"));
            QDir().mkpath(doomedRoot + QStringLiteral("/saves"));
            writeFileAt(doomedFile, QByteArrayLiteral("删掉这份文件夹时它得跟着没"));
            {
                LauncherSettings seed;
                FolderManager folders(&seed);
                check(folders.addFolder(otherRoot) && folders.addFolder(vanished) &&
                          folders.addFolder(doomedRoot),
                      "P2 先把这一节要用的三份目录都放进列表");
            }
            // 这一行的标题字是目录名（没起过 customname，前面几节也没起过）。
            const QString otherTitle = QDir(otherRoot).dirName();

            MainWindow w9;
            w9.show();
            // 翻到**「实例」页**再量。这一页默认不是当前那张（开窗停在主页），而隐藏着的
            // 控件不排布局 —— 那种状态下量行高、量齿轮贴没贴边量到的都是还没摆过的默认
            // 位置（downloadTabIndex() 那段 NOTE 讲的就是这件事）。
            if (auto *tabs = w9.findChild<QTabWidget *>(MainWindowUI::tabWidgetName())) {
                tabs->setCurrentIndex(1);  // 1 = 「实例」页（主页 0 / 下载 2 / 设置 3）
            }
            QApplication::processEvents();
            QListWidget *folderListView =
                byObjectName<QListWidget>(&w9, MainWindowUI::folderListName());
            check(folderListView != nullptr && folderListView->count() == 4,
                  "P2 列表里是自带那份 + 这一节新加的三份",
                  folderListView != nullptr ? QString::number(folderListView->count()) : QString());
            if (folderListView != nullptr) {
                // 每一次动作之后都用这两个重新找一遍（理由见上面那段 NOTE）。
                const auto gearOf = [&w9, &folderRowIndexOf](const QString &title) -> QPushButton * {
                    const int r = folderRowIndexOf(&w9, title);
                    return r < 0 ? nullptr
                                 : w9.findChild<QPushButton *>(MainWindowUI::folderRowGearName(r));
                };
                const auto openMenuOf = [&w9](QPushButton *gear) -> QWidget * {
                    if (gear == nullptr) {
                        return nullptr;
                    }
                    // 先把可能还开着的那块收掉。**不这么做的话这一下点是"收回"而不是
                    // "弹出"**：openFolderMenu 认 anchor，同一颗齿轮上的第二次点击一律
                    // 当收回（用户点一下开、再点一下关，这是要的行为），于是这里会拿到
                    // nullptr —— 上一段形状检查是"只看不点"，很容易就把一块开着的留在那儿，
                    // 而下面每一段的断言又都写成 if (menu != nullptr)，症状是"那一段静默地
                    // 什么都不做、一条 FAIL 都不出现"。
                    if (auto *stale = w9.findChild<QWidget *>(MainWindowUI::folderMenuName())) {
                        stale->close();
                    }
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    gear->click();
                    QApplication::processEvents();
                    // 关掉的那块是 WA_DeleteOnClose —— close() 只是排了个 deleteLater。
                    // 这里催一下，免得下一次 findChild 捞到那块**已经关掉的**（那样
                    // openFolderMenu 会以为"现在还开着一块"并按 sameAnchor 决定开不开）。
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    return w9.findChild<QWidget *>(MainWindowUI::folderMenuName());
                };
                // 点菜单里的某一项。**找不到就什么都不点**：上面那些形状断言会把"这一项
                // 没找着"照实挂出来，这里再解一次空指针等于把整轮跑崩在别的地方。
                const auto clickAction = [](QWidget *m, int action) -> bool {
                    if (m == nullptr) {
                        return false;
                    }
                    auto *entry = m->findChild<QPushButton *>(
                        MainWindowUI::folderMenuActionName(action));
                    if (entry == nullptr || !entry->isEnabled()) {
                        return false;
                    }
                    entry->click();
                    QApplication::processEvents();
                    return true;
                };
                // 一张图标里有没有「红」（要求「删除的垃圾桶也换成红色吧」）。
                // 判据要**连饱和度一起看**：灰色像素的色相也是 0，只看色相的话把图标画成灰的
                // 这条照样绿。量的是**按钮上挂着的那张图**，不是再调一次 folderActionIcon()
                // —— 后者颜色是调用方自己传的，跟菜单里真用的那张是两回事。
                // 取图要带上**按钮那一刻的 Mode**：图标现在走 QIconEngine，颜色是按
                // Normal / Disabled 现算的。灰按钮上画图时 QStyle 问的是 Disabled
                // （灰图标），不问 Normal —— 一律按 Normal 取，会把"灰按钮上的灰图标"
                // 读成"红的"，把这条本来就对的规矩判红。
                const auto hasDangerRed = [](const QIcon &icon,
                                             QIcon::Mode mode = QIcon::Normal) {
                    const QImage image = icon.pixmap(MainWindowUI::folderMenuIconSize(),
                                                     MainWindowUI::folderMenuIconSize(), mode)
                                             .toImage();
                    for (int y = 0; y < image.height(); ++y) {
                        for (int x = 0; x < image.width(); ++x) {
                            const QColor pixel = image.pixelColor(x, y);
                            if (pixel.alpha() > 128 && pixel.saturation() > 120 &&
                                (pixel.hue() <= 10 || pixel.hue() >= 350)) {
                                return true;
                            }
                        }
                    }
                    return false;
                };

                // ---- 菜单的形状：五项、字、图标、左对齐、「删除」是红的 ----
                QPushButton *gear = gearOf(otherTitle);
                check(gear != nullptr, "P2 行尾那颗齿轮是个真按钮");
                if (gear != nullptr) {
                    // 列表住在页头文件夹胶囊点开的浮层里：浮层没开过，行控件就没排过布局，
                    // 量到的是默认大小。先打开量，量完收起（后面那几段只点不量）。
                    auto *folderPill = w9.findChild<QPushButton *>(MainWindowUI::instanceFolderPillName());
                    if (folderPill != nullptr) folderPill->click();
                    QApplication::processEvents();
                    const int rowIndex = folderRowIndexOf(&w9, otherTitle);
                    QWidget *rowWidget =
                        w9.findChild<QWidget *>(MainWindowUI::folderRowName(rowIndex));
                    check(rowWidget != nullptr &&
                              rowWidget->height() == MainWindowUI::folderRowHeight(),
                          "P2 一行的高度就是 folderRowHeight()（用户要的「分栏做高一点」）",
                          rowWidget != nullptr ? QString::number(rowWidget->height()) : QString());
                    // 齿轮贴着**行尾**：几行的齿轮竖着对齐，而不是紧跟着路径（路径一行长
                    // 一行短，跟着的话几颗齿轮会错开）。
                    check(rowWidget != nullptr &&
                              gear->geometry().right() >= rowWidget->width() - 12,
                          "P2 齿轮贴在行尾（不是跟着路径的长短跑）",
                          rowWidget != nullptr
                              ? QString("%1 / %2").arg(gear->geometry().right()).arg(
                                    rowWidget->width())
                              : QString());
                    if (auto *popup = w9.findChild<QWidget *>(MainWindowUI::instanceFolderPopupName())) popup->hide();
                    QApplication::processEvents();
                }
                QWidget *menu = openMenuOf(gear);
                check(menu != nullptr && menu->isVisible(), "P2 点齿轮弹出一块框（不是右键菜单，"
                                                            "也不是先要选中什么）");
                if (menu != nullptr) {
                    for (int a = 0; a < MainWindowUI::folderActionCount(); ++a) {
                        auto *entry =
                            menu->findChild<QPushButton *>(MainWindowUI::folderMenuActionName(a));
                        check(entry != nullptr, QString("P2 菜单里第 %1 项按 objectName 找得到").arg(a));
                        if (entry == nullptr) {
                            continue;
                        }
                        check(entry->text() == MainWindowUI::folderActionText(a),
                              QString("P2 第 %1 项的字和 folderActionText() 是同一份").arg(a),
                              entry->text());
                        check(!entry->icon().isNull() &&
                                  entry->iconSize().width() == MainWindowUI::folderMenuIconSize(),
                              QString("P2 第 %1 项前面带着图标").arg(a),
                              QString::number(entry->iconSize().width()));
                        // 图标在文字**前面**是 QPushButton 自己的摆法，前提是没把 text-align
                        // 设成居中 —— 设了就成了"一排居中按钮"，看着不像菜单。
                        check(entry->styleSheet().contains(QStringLiteral("text-align: left")),
                              QString("P2 第 %1 项是从左边起排的").arg(a));
                        // 五项的字全是粗的（要求「这个字能不能用加粗的」）。
                        // 两条一起钉：一条看样式表里有没有写、一条看控件**身上**的字体是不是
                        // 真粗了 —— 只写样式表那句话在 QSS 没被应用时照样绿。
                        check(entry->styleSheet().contains(QStringLiteral("font-weight: bold")),
                              QString("P2 第 %1 项的字在样式表里是粗的").arg(a));
                        check(entry->font().bold(),
                              QString("P2 第 %1 项的字真的粗了（不是只写在样式表里）").arg(a));
                    }
                    const QString danger = MainWindowUI::folderDangerColor(menu->palette()).name();
                    auto *deleteEntry = menu->findChild<QPushButton *>(
                        MainWindowUI::folderMenuActionName(MainWindowUI::FolderActionDelete));
                    auto *refreshEntry = menu->findChild<QPushButton *>(
                        MainWindowUI::folderMenuActionName(MainWindowUI::FolderActionRefresh));
                    check(deleteEntry != nullptr &&
                              deleteEntry->styleSheet().contains(danger),
                          "P2 「删除」那一项的字色是 folderDangerColor()（用户点名要的红字）",
                          deleteEntry != nullptr ? deleteEntry->styleSheet() : QString());
                    check(refreshEntry != nullptr && !refreshEntry->styleSheet().contains(danger),
                          "P2 只有「删除」是红的（别的四项不是）");
                    check(!MainWindowUI::folderActionIcon(MainWindowUI::FolderActionDelete, 16,
                                                          Qt::black)
                               .toImage()
                               .isNull() &&
                              MainWindowUI::folderActionIcon(MainWindowUI::FolderActionDelete, 16,
                                                             Qt::black)
                                      .toImage() !=
                                  MainWindowUI::folderActionIcon(MainWindowUI::FolderActionRefresh, 16,
                                                                 Qt::black)
                                      .toImage(),
                          "P2 垃圾桶和刷新圈是两个不同的字形（画重了才会一样）");
                    // 垃圾桶跟着字一起红（要求「删除的垃圾桶也换成红色吧」）。
                    check(deleteEntry != nullptr && hasDangerRed(deleteEntry->icon()),
                          "P2 垃圾桶是红的（和那一项的字同一个色）");
                    check(refreshEntry != nullptr && !hasDangerRed(refreshEntry->icon()),
                          "P2 只有垃圾桶是红的（别的四项的图标还是正文色）");
                }
                // 五项就是用户点名的那五个字，而且两两不同（顺序真的对上了）。
                check(MainWindowUI::folderActionText(MainWindowUI::FolderActionRefresh) ==
                              QStringLiteral("刷新") &&
                          MainWindowUI::folderActionText(MainWindowUI::FolderActionRename) ==
                              QStringLiteral("重命名") &&
                          MainWindowUI::folderActionText(MainWindowUI::FolderActionOpen) ==
                              QStringLiteral("打开") &&
                          MainWindowUI::folderActionText(MainWindowUI::FolderActionRemove) ==
                              QStringLiteral("移除") &&
                          MainWindowUI::folderActionText(MainWindowUI::FolderActionDelete) ==
                              QStringLiteral("删除"),
                      "P2 五项就是用户说的「刷新 重命名 打开 移除 删除」");

                // ---- 「打开」：真调 QDesktopServices::openUrl（但截下来，见 UrlSpy）----
                // 不截的话这一下会在跑测试这台机器上**真弹出一个资源管理器窗口**。
                UrlSpy spy;
                QDesktopServices::setUrlHandler(QStringLiteral("file"), &spy, "handle");
                // 自检：这套拦法本身通不通。**先自检再断言产品** —— 不然下面那条失败读起来
                // 像是"产品没调 openUrl"，而真因可能是拦法没生效（那样无论怎么点都是 0 发）。
                // 拦法失效时这一发会落到系统手里：offscreen 平台连 services() 都没有
                // （QDesktopServices 里那句 "The platform plugin does not support services."），
                // 什么都不会发生，所以这个自检是安全的。
                const bool control = QDesktopServices::openUrl(QUrl::fromLocalFile(otherRoot));
                check(control && spy.urls.size() == 1, "P2 自检：setUrlHandler 拦得下来",
                      QString("返回值 %1、截到 %2 发").arg(control ? 1 : 0).arg(spy.urls.size()));
                spy.urls.clear();
                QWidget *openMenu = openMenuOf(gearOf(otherTitle));
                check(openMenu != nullptr, "P2 又弹出一块新的菜单（不是上一块还开着）");
                auto *openEntry = openMenu != nullptr
                                      ? openMenu->findChild<QPushButton *>(
                                            MainWindowUI::folderMenuActionName(
                                                MainWindowUI::FolderActionOpen))
                                      : nullptr;
                // 这一条**放在 if 外面**：没有它的话，menu 是空的时候整段一条断言都不跑，
                // 挂出来的样子是"这一段全绿"（见 openMenuOf 里那段 NOTE）。
                check(openEntry != nullptr, "P2 「打开」那一项找得到");
                if (openEntry != nullptr) {
                    openEntry->click();
                    QApplication::processEvents();
                }
                QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
                check(spy.urls.size() == 1 && sameDir(spy.urls.value(0), otherRoot),
                      "P2 「打开」请求打开的就是**这一行**那份目录（走的是系统那套，不是自拼命令行）",
                      QString("%1 发：%2").arg(spy.urls.size()).arg(spy.urls.join(QLatin1Char('|'))));
                check(currentPrompt() == nullptr,
                      "P2 它在盘上，所以不弹「现在不在盘上」那个警告");

                // ---- 「刷新」：重扫一遍这个文件夹 ----
                const int refreshedBefore =
                    countOccurrences(messagesSnapshot(), QStringLiteral("已刷新游戏目录"));
                QWidget *refreshMenu = openMenuOf(gearOf(otherTitle));
                clickAction(refreshMenu, MainWindowUI::FolderActionRefresh);
                check(countOccurrences(messagesSnapshot(), QStringLiteral("已刷新游戏目录")) ==
                          refreshedBefore + 1,
                      "P2 「刷新」写的是「已刷新游戏文件夹」");
                check(folderListView->count() == 4,
                      "P2 刷新不该让列表少一行多一行", QString::number(folderListView->count()));

                // ---- 自带那一份：菜单弹得出来，但「移除」「删除」是灰的 ----
                QWidget *ownMenu = openMenuOf(gearOf(MainWindowUI::folderAssistantTitle()));
                check(ownMenu != nullptr, "P2 自带那一行的齿轮也弹得出菜单");
                if (ownMenu != nullptr) {
                    auto *removeEntry = ownMenu->findChild<QPushButton *>(
                        MainWindowUI::folderMenuActionName(MainWindowUI::FolderActionRemove));
                    auto *deleteOwn = ownMenu->findChild<QPushButton *>(
                        MainWindowUI::folderMenuActionName(MainWindowUI::FolderActionDelete));
                    auto *refreshOwn = ownMenu->findChild<QPushButton *>(
                        MainWindowUI::folderMenuActionName(MainWindowUI::FolderActionRefresh));
                    check(removeEntry != nullptr && !removeEntry->isEnabled() &&
                              deleteOwn != nullptr && !deleteOwn->isEnabled(),
                          "P2 自带那一份的「移除」「删除」是灰的（它移不掉也删不掉）");
                    check(removeEntry != nullptr && !removeEntry->toolTip().isEmpty(),
                          "P2 灰掉的那一项配了一句为什么（灰着不说话，用户只会以为界面坏了）",
                          removeEntry != nullptr ? removeEntry->toolTip() : QString());
                    check(refreshOwn != nullptr && refreshOwn->isEnabled(),
                          "P2 同一块菜单里「刷新」还是亮的（没把整块一起灰掉）");
                    // 灰掉的「删除」连垃圾桶都不许是红的：红图标画在灰按钮上会被读成
                    // "这一项现在能点"，那正是灰掉它要避免的事。
                    check(deleteOwn != nullptr &&
                              !hasDangerRed(deleteOwn->icon(), QIcon::Disabled),
                          "P2 自带那一份的垃圾桶不是红的（灰着的按钮上画红，会读成能点）");
                    ownMenu->close();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                }

                // ---- 「移除」：只动列表，盘上一个字节都不动 ----
                answerNextPrompt(0);
                QWidget *removeMenu = openMenuOf(gearOf(otherTitle));
                clickAction(removeMenu, MainWindowUI::FolderActionRemove);
                check(g_modalTitle == QStringLiteral("从列表里移除文件夹"),
                      "P2 齿轮里那一项弹的就是「移除」那个确认框（不是悄悄就动了列表）",
                      g_modalTitle);
                check(folderListView->count() == 3, "P2 那一行从列表里没了",
                      QString::number(folderListView->count()));
                check(gearOf(otherTitle) == nullptr, "P2 按名字也再找不着那一行了");
                check(QFileInfo(otherRoot).isDir() &&
                          QFile::exists(otherRoot +
                                        QStringLiteral("/versions/1.7.10/1.7.10.jar")),
                      "P2 **盘上的目录一个字节都没少**（「移除」不是「删除」）", otherRoot);

                // ---- 目录不在盘上时：「刷新」照实说、「打开」弹一句为什么 ----
                QDir(vanished).removeRecursively();
                check(!QFileInfo(vanished).isDir(),
                      "P2 把那一份从盘上挪走（列表里那一条还在 —— U 盘没插就是这个样子）");
                const QString vanishedTitle = QDir(vanished).dirName();
                const int missingBefore =
                    countOccurrences(messagesSnapshot(), QStringLiteral("游戏目录不存在"));
                QWidget *goneMenu = openMenuOf(gearOf(vanishedTitle));
                clickAction(goneMenu, MainWindowUI::FolderActionRefresh);
                check(countOccurrences(messagesSnapshot(), QStringLiteral("游戏目录不存在")) ==
                          missingBefore + 1,
                      "P2 目录不在盘上时「刷新」说的是另一句");
                const int vanishedRow = folderRowIndexOf(&w9, vanishedTitle);
                QLabel *vanishedSubtitle =
                    vanishedRow >= 0 ? w9.findChild<QLabel *>(
                                           MainWindowUI::folderRowSubtitleName(vanishedRow))
                                     : nullptr;
                check(vanishedSubtitle != nullptr &&
                          vanishedSubtitle->text().endsWith(QStringLiteral("（目录不在了）")),
                      "P2 那一行下面缀上了「（目录不在了）」（光看路径看不出这一份扫不出东西）",
                      vanishedSubtitle != nullptr ? vanishedSubtitle->text() : QString());

                answerNextPrompt(0);
                QWidget *openGoneMenu = openMenuOf(gearOf(vanishedTitle));
                clickAction(openGoneMenu, MainWindowUI::FolderActionOpen);
                check(g_modalTitle == QStringLiteral("打开游戏目录") &&
                          g_modalText.contains(QStringLiteral("找不到")),
                      "P2 对着一个不在盘上的目录按「打开」：弹一句照实的，不静默什么都不做",
                      g_modalTitle + QStringLiteral(" / ") + g_modalText);

                // ---- 「删除」：这一次是真的删盘 ----
                answerNextPrompt(0);
                QWidget *deleteMenu = openMenuOf(gearOf(vanishedTitle));
                clickAction(deleteMenu, MainWindowUI::FolderActionDelete);
                check(g_modalTitle == QStringLiteral("删除游戏目录") &&
                          g_modalText.contains(QStringLiteral("回收站")) &&
                          g_modalText.contains(QStringLiteral("用「移除」")),
                      "P2 确认框把「会进回收站」和「想留着就用移除」都写死了",
                      g_modalTitle + QStringLiteral(" / ") + g_modalText);
                check(g_modalWasWarning == false, "P2 它不是警告框（要答的那一下才是重点）");
                check(folderListView->count() == 2,
                      "P2 目录本来就不在盘上，那一条也照样清掉了（不算失败）",
                      QString::number(folderListView->count()));

                // 盘上真在的那一份：按下去之后**目录得真没**。
                const int deletedBefore =
                    countOccurrences(messagesSnapshot(), QStringLiteral("已删除游戏目录"));
                const QString doomedTitle = QDir(doomedRoot).dirName();
                answerNextPrompt(0);
                QWidget *doomedMenu = openMenuOf(gearOf(doomedTitle));
                check(doomedMenu != nullptr, "P2 第三份的齿轮也弹得出菜单");
                clickAction(doomedMenu, MainWindowUI::FolderActionDelete);
                check(countOccurrences(messagesSnapshot(), QStringLiteral("已删除游戏目录")) ==
                          deletedBefore + 1,
                      "P2 日志说的是「已删除游戏文件夹」（和「已从列表里移除文件夹」是两句）");
                check(!QFileInfo(doomedRoot).exists() && !QFile::exists(doomedFile),
                      "P2 **盘上那个目录连同里面的文件真的没了**（这才是「删除」）", doomedRoot);
                check(folderListView->count() == 1, "P2 删完只剩自带那份了",
                      QString::number(folderListView->count()));
                check(!g_modalText.isEmpty(), "P2 收尾：这一路上那三个确认框都真的弹过");
            }
        }

        // ---- 重命名：改的是 customname，磁盘上的目录名一个字都不动 ----
        // 齿轮里「重命名」那一项弹的是 QInputDialog（见上面那段 NOTE，offscreen 下点不动），
        // 所以这里直接驱动它背后那一半：数据层落盘 + 新开一个窗口看那一行画的字。
        // 这条路正是用户那句「一个 customname，不是直接改，毕竟 .minecraft 改起来容易出事」
        // 要的效果：改的是**显示**，目录名不动。
        {
            const QString customName = QStringLiteral("我的整合包");
            const QString otherTitle = QDir(otherRoot).dirName();
            {
                LauncherSettings seed;
                FolderManager folders(&seed);
                check(folders.addFolder(otherRoot), "P2b 把那一份加回列表（下面要量它那一行的字）");
            }
            // 校验规则和数据层是**同一份**（界面绝不自己抄一遍）。
            check(FolderManager::validateDisplayName(QString()).isEmpty(),
                  "P2b 空名字算合法（= 清掉名字、恢复默认显示名）");
            check(FolderManager::validateDisplayName(
                      QString(FolderManager::maxDisplayNameLength(), QLatin1Char('x')))
                      .isEmpty(),
                  "P2b 刚好到上限的名字是合法的");
            check(!FolderManager::validateDisplayName(
                       QString(FolderManager::maxDisplayNameLength() + 1, QLatin1Char('x')))
                       .isEmpty(),
                  "P2b 超一个字就给一句人话", FolderManager::validateDisplayName(
                                                  QString(FolderManager::maxDisplayNameLength() + 1,
                                                          QLatin1Char('x'))));
            check(!FolderManager::validateDisplayName(QStringLiteral("带\t制表符的名字")).isEmpty(),
                  "P2b 名字里不许有制表符 —— 它是落盘的字段分隔符，放进去等于把名字劈成两半");
            {
                LauncherSettings seed;
                FolderManager folders(&seed);
                check(folders.displayName(otherRoot).isEmpty(),
                      "P2b 还没起过名字时是空串（空串不是可显示的字，交给界面给默认名）");
                check(folders.setDisplayName(otherRoot, customName), "P2b 起个名字");
                check(folders.displayName(otherRoot) == customName, "P2b 读回来就是它");
            }
            {
                // 落了盘才算数：换一个实例（= 下一次启动）读得到。
                LauncherSettings reread;
                const QStringList stored = reread.folderNames();
                const int tab = stored.size() == 1 ? stored.first().indexOf(QLatin1Char('\t')) : -1;
                check(tab > 0 && stored.first().mid(tab + 1) == customName &&
                          sameDir(stored.first().left(tab), otherRoot),
                      "P2b 名字落了盘（`路径<TAB>名字` 那一项）", stored.join(QLatin1Char('|')));
            }
            {
                MainWindow w10;
                w10.show();
                QApplication::processEvents();
                const int row = folderRowIndexOf(&w10, customName);
                QLabel *subtitle =
                    row >= 0 ? w10.findChild<QLabel *>(MainWindowUI::folderRowSubtitleName(row))
                             : nullptr;
                check(row >= 0, "P2b 冷启动之后那一行上面写的是用户起的名字（不是目录名）",
                      QString::number(row));
                check(folderRowIndexOf(&w10, otherTitle) < 0,
                      "P2b 目录名那个默认显示名已经不在列表里了（同一个目录不会占两行）");
                check(subtitle != nullptr &&
                          subtitle->text() == QDir::toNativeSeparators(otherRoot),
                      "P2b 下面那行还是**真路径**（名字只是显示用的，没把路径也改掉）",
                      subtitle != nullptr ? subtitle->text() : QString());
            }
            check(QFileInfo(otherRoot).isDir() &&
                      QFileInfo(otherRoot + QStringLiteral("/versions/1.7.10")).isDir() &&
                      QDir(otherRoot).dirName() == otherTitle,
                  "P2b **磁盘上的目录名一个字都没动**（用户那句「毕竟 .minecraft 改起来容易出事」）",
                  QDir(otherRoot).dirName());
            {
                // 清掉名字 = 恢复默认显示名。
                LauncherSettings seed;
                FolderManager folders(&seed);
                check(folders.setDisplayName(otherRoot, QString()), "P2b 清掉名字");
                check(folders.displayName(otherRoot).isEmpty(), "P2b 清掉之后读回来是空串");
                check(LauncherSettings().folderNames().isEmpty(), "P2b 盘上那一项也跟着没了");
            }
            {
                MainWindow w11;
                w11.show();
                QApplication::processEvents();
                check(folderRowIndexOf(&w11, otherTitle) >= 0,
                      "P2b 清掉名字之后又回到目录名那个默认显示名", otherTitle);
            }
            {
                // 收尾：把这一份从列表里拿掉（沙盒最后整块删掉，但中途还会开窗的节会照实
                // 把列表画出来 —— 留一份"盘上已经没有"的目录在列表里是给别的节下绊子）。
                LauncherSettings seed;
                FolderManager folders(&seed);
                check(folders.removeFolder(otherRoot), "P2b 收尾：把那一份从列表里拿掉");
            }
        }

        // ---- 记着的那份不在盘上（U 盘没插 / 目录被挪走）----
        {
            const QString missing = root.filePath(QStringLiteral("not-there-mc"));
            LauncherSettings seed;
            seed.setCurrentFolder(missing);
            FolderManager folders(&seed);
            check(sameDir(folders.currentFolder(), AppPaths::defaultMinecraftDir()),
                  "记着的那份不在盘上 → 这次先用自带那份");
            check(seed.currentFolder() == missing,
                  "**不**改盘上那个值：那块盘下次插上还该接着用它", seed.currentFolder());
            check(countOccurrences(messagesSnapshot(), QStringLiteral("不在盘上")) > 0,
                  "这件事写了日志（不是静默换了一份 .minecraft）");
        }

        // ---- 恢复：盘上的当前目录写回"空串"（= 自带那份），全局路径也还回默认 ----
        {
            LauncherSettings reset;
            FolderManager folders(&reset);
            check(folders.setCurrentFolder(AppPaths::defaultMinecraftDir()),
                  "切回自带那份");
            check(reset.currentFolder().isEmpty(),
                  "自带那份在盘上存成空串，不钉死绝对路径（启动器挪个位置它得跟着走）",
                  reset.currentFolder());
        }
        AppPaths::setMinecraftDir(QString());
        check(sameDir(AppPaths::minecraftDir(), AppPaths::defaultMinecraftDir()),
              "全局路径回到沙盒里那份 .minecraft（后面的节按它扫盘）",
              AppPaths::minecraftDir());

        QDir(otherRoot).removeRecursively();
    }
    return 0;
}


