#include "Support.hpp"
#include "ActivityPopover.hpp"
int MwTaskSummaryChecks(MainWindow &window) {
    // ------------------------------------------------------------------
    // 底栏中间那格不归谁所有 —— 每一件活（下载 / 安装 / 导入 / 启动）都是账本里的一行，
    // 那一格画的是 `Tasks::collapsedSummary()` 现算出来的那一行。所以"还回来"不是
    // 一段要写对的代码，判据是：那一格任何时候都等于账本投影。
    //
    // 这一节量的是**那条链**：
    // setDownloadUiBusy / onVersionDownloadStats → 账本 → 底栏那格 + 面板
    //
    // NOTE: 怎么把「在下载」摆出来 —— 这个态只由 setDownloadUiBusy(true) 置上，而那两处
    // 真实调用点（装加载器 / 导入整合包）都要联网。所以那个函数带 Q_INVOKABLE
    // （见 MainWindow.hpp 里那段 NOTE），这里直接把它调起来：这就是真的那条路，
    // 只是省掉了前面联网那几步。
    {
        QPushButton *abCell = window.findChild<QPushButton *>(MainWindowUI::taskSummaryButtonName());
        QLabel *abStatus = window.findChild<QLabel *>(MainWindowUI::statusLabelName());
        QProgressBar *abBar = window.findChild<QProgressBar *>(MainWindowUI::progressBarName());
        QLabel *abDot = window.findChild<QLabel *>(MainWindowUI::taskSummaryDotName());
        check(abCell != nullptr && abStatus != nullptr && abBar != nullptr && abDot != nullptr,
              "AB0 找得到底栏中间那格（那颗按钮 / 那行字 / 那根条 / 那颗红点）");

        if (abCell != nullptr && abStatus != nullptr && abBar != nullptr && abDot != nullptr) {
            // ---- ① 起一批下载：账本里多一件，那一格写的是**它的标题** ----
            (void)QMetaObject::invokeMethod(&window, "setDownloadUiBusy", Qt::DirectConnection,
                                            Q_ARG(bool, true));
            QApplication::processEvents();
            // 这一批在界面上没有对应的实例（下面那三个 id 都是空的），走的是兜底那一支
            // —— 标题「下载游戏文件」（见 DownloadTaskLedgerController::begin()）。
            check(abStatus->text() == QStringLiteral("下载游戏文件"),
                  "AB1 起一批下载：中间那格写的是**这件任务的标题**（不再是一根空条）",
                  abStatus->text());
            check(abCell->toolTip().contains(QStringLiteral("下载游戏文件"))
                      && abBar->toolTip() == abCell->toolTip(),
                  "AB1 悬停提示随账本摘要更新到进度环和摘要按钮",
                  abBar->toolTip());
            // 还没报过数 ⇒ 数量不可测 ⇒ 条走不确定态（range 0..0）。**绝不编一个 0% 出来**。
            check(abBar->isVisible() && abBar->minimum() == 0 && abBar->maximum() == 0,
                  "AB1 还没报过总数：条是不确定态（不编 0%）",
                  QStringLiteral("%1..%2").arg(abBar->minimum()).arg(abBar->maximum()));

            // ---- ①b 底下**真**摆一批下载（不摆的话下面那条「取消」本来就不该露）----
            // 账本里那位 `canCancel` 只说了"这一类活**允许**取消"；露不露还要过
            // `MainWindow::cancelWouldWork()` —— 它问的是"**这会儿真掐得动吗**"，也就是
            // `VersionManager::canCancelDownload()`（下载器忙不忙）。光把界面置忙（上面那句
            // Q_INVOKABLE）**不会**让下载器忙起来，所以得真排一个作业：一个只 listen、
            // 既不 accept 也不回一个字节的本地端口 ⇒ 那个作业一直吊着 ⇒ `isBusy()` 保持 true。
            // NOTE: **必须排在 AB1 那两条之后**：作业一登记就泵一发进度
            // （`emitBatchProgress` → `versionDownloadStats` → 账本那位数被改成 0/1），
            // 排在前头会把 AB1 那条"还没报过总数"顶成确定态。
            // NOTE: 收尾（AB5）里 `cancelBatch()` 放掉它，免得后面的节捡到一个"永远在下"的下载器。
            QTcpServer abStall;
            abStall.listen(QHostAddress::LocalHost, 0);
            VersionManager *abVm = window.findChild<VersionManager *>();
            const QString abSlowPath =
                QDir(QDir::tempPath()).filePath(QStringLiteral("wtfml-ab-slow.jar"));
            QFile::remove(abSlowPath);  // 上一趟留下的进程内残留（存在就会被当成"已下好"跳过）
            if (abVm != nullptr && abStall.isListening()) {
                abVm->downloads()->downloadFile(
                    QStringLiteral("http://127.0.0.1:%1/slow.jar").arg(abStall.serverPort()),
                    abSlowPath, QString(), -1, VerifyMode::ExistsOnly);
                QApplication::processEvents();
            }
            check(abVm != nullptr && abVm->canCancelDownload(),
                  "AB1b 底下真有一批在跑（判据就是下载器忙不忙）—— 下面那条「取消」的正当性全在它身上");

            // ---- ② 报一次数：进度落进账本，底栏 + 面板都看得见 ----
            // NOTE: 等过那个 200 ms 的节流窗口再报数。onVersionDownloadStats() 是**前缘
            // 节流**（DownloadTaskLedgerController::takeProgressTick() 里那句 elapsed() < 200），
            // 而节流的起点是"上一发真报数"—— 这一节之前有没有别人报过（别的节留下的
            // 后台下载、真网络那几条探针），就决定了这一发会不会被**吞掉**。被吞掉时
            // 报出来的是「0 / 0」，看着像产品坏了，其实是这条断言在跟节流器抢时间。
            // 等一段（比窗口长）再发，这一条就与"上一发什么时候"无关了。
            QThread::msleep(250);
            (void)QMetaObject::invokeMethod(&window, "onVersionDownloadStats",
                                            Qt::DirectConnection, Q_ARG(int, 400),
                                            Q_ARG(int, 1000), Q_ARG(int, 0),
                                            Q_ARG(QString, QStringLiteral("objects/ab.jar")));
            QApplication::processEvents();
            check(abBar->minimum() == 0 && abBar->maximum() == 100,
                  "AB2 报过数之后底栏那根条换成**确定态**（0..100 的百分比条）",
                  QStringLiteral("%1..%2").arg(abBar->minimum()).arg(abBar->maximum()));
            // NOTE: 底栏那根条的 value 此刻**不一定**是 40 —— 百分比那一档走
            // `Anim::smoothProgress()` 是追值动画（一两拍 processEvents 推不到终点）。
            // 真正该量的是"账本里记的那个数"，它在**面板那一行**上（面板不走动画、
            // 直接 setValue）。点开面板读它。
            abCell->click();
            ActivityPopover::openAllIn(&window);  // 摘要先弹浮动面板，从「全部任务」进整页
            QApplication::processEvents();
            QWidget *abPanel = window.findChild<QWidget *>(MainWindowUI::taskCenterPanelName());
            check(abPanel != nullptr && abPanel->isVisible(), "AB2 点中间那格弹出了任务中心面板");
            if (abPanel != nullptr) {
                QLabel *pct = abPanel->findChild<QLabel *>(
                    MainWindowUI::taskCenterPanelRowPercentName(0));
                QLabel *detail = abPanel->findChild<QLabel *>(
                    MainWindowUI::taskCenterPanelRowDetailName(0));
                auto *rowBar =
                    abPanel->findChild<QProgressBar *>(MainWindowUI::taskCenterPanelRowBarName(0));
                check(pct != nullptr && pct->text() == MainWindowUI::taskCenterPercentText(40),
                      "AB2 面板里那行写着 40%（400 / 1000 落进账本的那一个数）",
                      pct != nullptr ? pct->text() : QStringLiteral("<没找到>"));
                check(rowBar != nullptr && rowBar->value() == 40,
                      "AB2 面板里那根条也停在 40（面板不走追值动画）",
                      rowBar != nullptr ? QString::number(rowBar->value())
                                        : QStringLiteral("<没找到>"));
                check(detail != nullptr && detail->text().contains(QStringLiteral("400 / 1000")),
                      "AB2 那一行的小字写着「已下载 400 / 1000」",
                      detail != nullptr ? detail->text() : QStringLiteral("<没找到>"));

                // ---- ②b 行尾那颗「取消」（行内快捷动作）----
                // 这一件是**在跑的下载** ⇒ 账本那一位 `canCancel` 该是 true（见
                // `MainWindow::defaultTaskCapabilities()`：只有下载 / 安装 / 导入那三档给
                // 它）。面板只照着那一位摆按钮，不自己按状态推一遍。
                // NOTE: 按**标题**认这一行而不是"第 0 行" —— 这一节之前别的节可能往账本里
                // 留过东西（它们都是终态），行号不是我说了算的。
                const auto findRowByTitle = [abPanel](const QString &title,
                                                      int *outIndex) -> QWidget * {
                    for (int i = 0; i < 20; ++i) {
                        QWidget *row = abPanel->findChild<QWidget *>(
                            MainWindowUI::taskCenterPanelRowName(i));
                        if (row == nullptr) {
                            break;
                        }
                        auto *text = row->findChild<QLabel *>(
                            MainWindowUI::taskCenterPanelRowTitleName(i));
                        if (text != nullptr && text->text() == title) {
                            if (outIndex != nullptr) {
                                *outIndex = i;
                            }
                            return row;
                        }
                    }
                    return nullptr;
                };
                const QString abTaskTitle = QStringLiteral("下载游戏文件");
                int abRowIndex = -1;
                QWidget *abRow = findRowByTitle(abTaskTitle, &abRowIndex);
                check(abRow != nullptr, "AB2 面板里按标题找得到这一件（账本那一行）");
                auto *abRowCancel = abRowIndex < 0
                                        ? nullptr
                                        : abPanel->findChild<QPushButton *>(
                                              MainWindowUI::taskCenterPanelRowCancelName(
                                                  abRowIndex));
                check(abRowCancel != nullptr,
                      "AB2 在跑的下载：行尾那颗「取消」在（允许 ∧ 还活着 ∧ 现在真掐得动，三样都要）",
                      abRowCancel != nullptr ? abRowCancel->text() : QStringLiteral("<没找到>"));

                // ---- ②c 点这一行 ⇒ 同一个工作区右栏显示详情（宽屏三列形态）----
                // 详情不再另开第二层 Overlay；窄屏也只是让详情在当前工作区占满正文。
                OverlayHost *abOverlay = window.ui() == nullptr ? nullptr : window.ui()->overlay();
                QString rdKey;
                QString rdState;
                QString rdTitle;
                bool rdCancel = false;
                bool rdRetry = false;
                if (abRow != nullptr && abOverlay != nullptr) {
                    const QPoint abLocal(4, 4);
                    const QPoint abGlobal = abRow->mapToGlobal(abLocal);
                    QMouseEvent press(QEvent::MouseButtonPress, QPointF(abLocal),
                                      QPointF(abGlobal), Qt::LeftButton, Qt::LeftButton,
                                      Qt::NoModifier);
                    QApplication::sendEvent(abRow, &press);
                    QApplication::processEvents();
                    QWidget *detailContent = abPanel->findChild<QWidget *>(
                        QStringLiteral("taskCenterDetailContent"));
                    rdKey = detailContent == nullptr
                                ? QString()
                                : detailContent->property("taskKey").toString();
                    auto *state = detailContent == nullptr
                                      ? nullptr
                                      : detailContent->findChild<QLabel *>(
                                            MainWindowUI::taskDetailStateName());
                    rdState = state == nullptr ? QString() : state->text();
                    auto *title = detailContent == nullptr
                                      ? nullptr
                                      : detailContent->findChild<QLabel *>(
                                            MainWindowUI::taskDetailTitleName());
                    rdTitle = title == nullptr ? QString() : title->text();
                    auto *rc = detailContent == nullptr
                                   ? nullptr
                                   : abPanel->findChild<QPushButton *>(
                                         MainWindowUI::taskDetailCancelName());
                    auto *rt = detailContent == nullptr
                                   ? nullptr
                                   : abPanel->findChild<QPushButton *>(
                                         MainWindowUI::taskDetailRetryName());
                    rdCancel = rc != nullptr && rc->isVisible();
                    rdRetry = rt != nullptr && rt->isVisible();
                    const QString wantKey =
                        abRow->property(MainWindowUI::taskCenterPanelRowKeyProperty()).toString();
                    check(!rdKey.isEmpty() && rdKey == wantKey,
                          "AB2 点这一行后右栏详情认的正是**这一行**的钥匙（不是隔壁那一行）",
                          QStringLiteral("页上 %1 / 行上 %2").arg(rdKey, wantKey));
                    check(rdState == MainWindowUI::taskDetailStateText(TaskCenter::State::Running),
                          "AB2 这一页的状态那一句 =「正在进行」（和账本那一刻同一个词）", rdState);
                    check(rdTitle == abTaskTitle,
                          "AB2 详情页标题原样摆的（和底栏那格读的是同一份账本）", rdTitle);
                    check(rdCancel && !rdRetry,
                          "AB2 在跑的那一项：详情页上能取消、不能重试（面板和详情页读同一份能力位）",
                          QStringLiteral("cancel=%1 retry=%2").arg(rdCancel).arg(rdRetry));
                    check(abOverlay->pageOpen(),
                          "AB2 查看详情仍留在同一个任务中心工作区（没有叠第二层覆盖页）");
                }
                // 任务中心是 OverlayHost 当前那一页；只 close 子控件会留下一个“页面仍开着、
                // 内容已经没了”的半开状态，下一次点底栏只会先执行 evict()，也就不会走
                // openTaskCenterPanel() 里的 markAllViewed()。按产品的页面出口完整收掉。
                if (abOverlay != nullptr && abOverlay->pageOpen()) {
                    abOverlay->evict();
                }
                QApplication::processEvents();
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }

            // ---- ③ 这一批结束但没有终态信号：走兜底那一支，按**失败**收 ----
            // 「正常情况」是数据层先发一条终态（onVersionDownloadFinished），这里故意不
            // 发那一条 —— 量的是 setDownloadUiBusy(false) 里那句兜底：账本上留下的会是
            // 一件**失败**的任务（终态只落一次，见 TaskManager::finish），并挂一颗
            // 「用户还没看过」的红点。
            (void)QMetaObject::invokeMethod(&window, "setDownloadUiBusy", Qt::DirectConnection,
                                            Q_ARG(bool, false));
            QApplication::processEvents();
            check(abStatus->text() ==
                      QStringLiteral("下载游戏文件") + MainWindowUI::taskSummaryFailedSuffix(),
                  "AB3 没有终态信号就置闲：那一件按**失败**收尾，那句话缀「（失败）」",
                  abStatus->text());
            check(abDot->isVisible(),
                  "AB3 还没看过的失败 ⇒ 那一格前面挂上红点（唯一撤掉它的动作是打开任务中心）");

            // ---- ④ 多画几遍底栏：还是同一句话（那一格是账本的投影，「忘了还」不存在） ----
            repaintBottomBar(&window);
            repaintBottomBar(&window);
            check(abStatus->text() ==
                      QStringLiteral("下载游戏文件") + MainWindowUI::taskSummaryFailedSuffix(),
                  "AB4 反复重画底栏，那句话一个字都不变（不再有借 / 还这回事）",
                  abStatus->text());

            // ---- ④b 导航收起：标题那颗跟着字藏了，红点改挂到进度环右上角（两颗互斥） ----
            QLabel *abBadge = window.findChild<QLabel *>(MainWindowUI::taskSummaryBadgeName());
            TitleBar *abRail = window.ui()->titleBar();
            const auto badgeShown = [abBadge] { return abBadge != nullptr && abBadge->isVisible(); };
            check(abBadge != nullptr && abBadge->parentWidget() == abBar, "AB5 进度环上有那颗角标（环的子控件）");
            abRail->setCollapsed(true);
            QApplication::processEvents();
            check(abRail->isCollapsed() && badgeShown() && !abDot->isVisible(),
                  "★ AB5 收起 + 未读失败：环的右上角挂红点角标，标题那颗不露");
            check(abBadge != nullptr && abBar->rect().contains(abBadge->geometry()) &&
                      !abBadge->pixmap().isNull() && abRail->width() == TitleBar::collapsedRailWidth(),
                  "AB5 角标落在环那 28×28 之内（裁不掉）、画的是那颗点，图标轨宽度不变",
                  QStringLiteral("轨宽 %1").arg(abRail->width()));
            abRail->setCollapsed(false);
            QApplication::processEvents();
            check(!badgeShown() && abDot->isVisible(), "★ AB5 点 W 展开：角标收掉、标题前那颗红点回来");
            const QSize abWide = window.size();
            window.resize(1000, abWide.height());
            QApplication::processEvents();
            check(abRail->isCollapsed() && badgeShown() && !abDot->isVisible(),
                  "★ AB5 窗宽缩过 1024 自动收起：角标顶上，标题那颗不露");
            window.resize(abWide);
            QApplication::processEvents();
            check(!abRail->isCollapsed() && !badgeShown() && abDot->isVisible(),
                  "AB5 窗宽回到 1024 以上：换回标题那颗");
            abRail->setCollapsed(true);
            QApplication::processEvents();
            check(badgeShown() && !abDot->isVisible(), "AB5 再收起：互斥依旧（角标在、标题那颗不露）");

            // ---- ⑤ 打开任务中心：那次失败被「看过」了，红点灭掉；顺手把已完成清掉 ----
            abCell->click();
            ActivityPopover::openAllIn(&window);  // 摘要先弹浮动面板，从「全部任务」进整页
            QApplication::processEvents();
            check(!abDot->isVisible(),
                  "AB4 打开过任务中心之后那颗红点灭掉（看过了，就不是「没看过」）",
                  abDot->isVisible() ? QStringLiteral("还亮着") : QStringLiteral("灭了"));
            check(!badgeShown(), "★ AB5 收起时打开任务中心：角标跟着灭（同一个「看过」）");
            abRail->setCollapsed(false);  // 后面几节量的是展开态
            QApplication::processEvents();
            check(!badgeShown() && !abDot->isVisible(), "AB5 看过之后展开：两颗都不亮");
            QWidget *abPanel2 = window.findChild<QWidget *>(MainWindowUI::taskCenterPanelName());
            if (abPanel2 != nullptr) {
                // ---- 收了尾的那一件：行尾那颗「取消」该**收起来**了 ----
                // 「取消」是 `canCancel ∧ 还是活的` 两样都成立才露（`MainWindow::taskCenterItem()`
                // 里那一句 `&& Tasks::isActiveState(...)`）—— 这一条量的是后半句：同一份能力位
                // 到了终态上必须变脸。漏了它，界面上就会留一颗点了没反应的「取消」。
                int abDoneIndex = -1;
                QWidget *abDoneRow = nullptr;
                for (int i = 0; i < 20; ++i) {
                    QWidget *row = abPanel2->findChild<QWidget *>(
                        MainWindowUI::taskCenterPanelRowName(i));
                    if (row == nullptr) {
                        break;
                    }
                    auto *text =
                        row->findChild<QLabel *>(MainWindowUI::taskCenterPanelRowTitleName(i));
                    if (text != nullptr && text->text() == QStringLiteral("下载游戏文件")) {
                        abDoneIndex = i;
                        abDoneRow = row;
                        break;
                    }
                }
                QPushButton *abDoneCancel =
                    abDoneIndex < 0
                        ? nullptr
                        : abPanel2->findChild<QPushButton *>(
                              MainWindowUI::taskCenterPanelRowCancelName(abDoneIndex));
                check(abDoneRow != nullptr && abDoneCancel == nullptr,
                      "AB4 收了尾（失败）那一件：行尾不再长那颗「取消」（终态上它没意义）",
                      abDoneCancel != nullptr ? abDoneCancel->text() : QStringLiteral("<没找到>"));
                // 那颗「清除已结束记录」现在挂在**页标题行**：面板把自己的工具栏
                // （`TaskCenterPanel::headerActions()`）交给 `OverlayHost::setPageHeaderWidget()`，
                // 打开这一页时它已不是面板的子树 ⇒ 从窗口这一层找（收页后会自动还回面板）。
                auto *clearBtn = window.findChild<QPushButton *>(
                    MainWindowUI::taskCenterPanelClearButtonName());
                check(clearBtn != nullptr,
                      "AB4 有已完成的那件 ⇒ 面板标题行里有那颗「清除已完成」");
                if (clearBtn != nullptr) {
                    clearBtn->click();
                    QApplication::processEvents();
                }
                // 收页走产品的出口（`OverlayHost::evict()` → backPressed），不能只
                // `abPanel2->close()`：面板是**嵌在覆盖页里**的，close() 只把这一块藏
                // 起来 —— 覆盖层那一位还占着、面板对象也还活着（等 deleteLater），
                // 下面的 AB6 再点底栏那格时会被当成"同一块还开着"。
                if (OverlayHost *abOverlay2 =
                        window.ui() == nullptr ? nullptr : window.ui()->overlay()) {
                    abOverlay2->evict();
                } else {
                    abPanel2->close();
                }
                QApplication::processEvents();
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            // 清完账本上没有已完成的了 ⇒ 那一格回到占位话。这一条同时是**给后面几节留一块
            // 干净的底栏**：不清的话，那件失败的任务会在接下来 8 秒里一直占着中间那格
            // （`taskSummaryRecentWindowMs()`），后面哪一节读它都会读到这一句。
            check(abStatus->text() == MainWindowUI::taskSummaryNoTaskText() && !abBar->isVisible(),
                  "AB4 清掉已完成之后那一格回到占位话（底栏留干净）", abStatus->text());

            // ---- ⑤b 放掉 AB1b 那一批吊着的下载 ----
            // 留着的下巴：`isBusy()` 会一直是真的（后面的节谁再问下载器忙不忙都会读到"忙"），
            // 那个 `.part` 也会一直占着临时目录。`cancelBatch()` 之后补一发 begin/end，
            // 把那一批的**计数**也清零（那几个计数器只有 `beginBatch()` 会清）。
            if (abVm != nullptr) {
                abVm->downloads()->cancelBatch();
                abVm->downloads()->beginBatch();
                abVm->downloads()->endBatch();
            }
            QFile::remove(abSlowPath);
            QApplication::processEvents();
            check(abVm == nullptr || !abVm->canCancelDownload(),
                  "AB5 放掉之后下载器不忙了（这一节不在环境里留一批永远下不完的活）",
                  abVm == nullptr ? QStringLiteral("<没拿到 vm>") : QStringLiteral("还忙着"));
        }
    }
    // ---- AB7. 主页启动卡片（#65）的数据就是这一项启动任务：阶段名、件数、失败原因 ----
    {
        using Phase = MainWindowUI::LaunchCard::Phase;
        Tasks::TaskSnapshot t;
        t.id = 42;
        t.kind = Tasks::TaskKind::Launch;
        t.state = Tasks::TaskState::Running;
        t.targetId = QStringLiteral("spring");
        for (const char *name : {"校验登录状态", "检查启动文件", "准备 Java 运行环境", "等待游戏启动", "完成"})
            t.steps.append(Tasks::TaskStep{QString::fromUtf8(name), Tasks::TaskStepState::Pending, QString()});
        const auto doneUpTo = [&t](int n) {
            for (int i = 0; i < t.steps.size(); ++i)
                t.steps[i].state = i < n ? Tasks::TaskStepState::Done : Tasks::TaskStepState::Pending;
        };
        doneUpTo(0);
        MainWindowUI::LaunchCard c = MainWindow::launchCardFor(t, true);
        check(c.phase == Phase::Running && c.stage == QStringLiteral("校验登录") && c.taskId == 42
                  && c.instanceKey == QStringLiteral("spring") && c.login == QStringLiteral("离线登录"),
              "AB7 启动卡片：第一步 = 校验登录，登录方式按账号", c.stage);
        doneUpTo(1);
        check(MainWindow::launchCardFor(t, false).stage == QStringLiteral("检查文件")
                  && MainWindow::launchCardFor(t, false).login == QStringLiteral("微软账号登录"),
              "AB7 检查启动文件还没报件数 = 检查文件");
        t.doneUnits = 312;
        t.totalUnits = 1024;
        c = MainWindow::launchCardFor(t, true);
        check(c.stage == QStringLiteral("补全文件与资源") && c.done == 312 && c.total == 1024 && !c.bytes,
              "AB7 报了件数 = 补全文件与资源 312 / 1024", c.stage);
        doneUpTo(2);
        c = MainWindow::launchCardFor(t, true);
        check(c.stage == QStringLiteral("准备 Java") && c.bytes, "AB7 准备 Java 按字节报", c.stage);
        doneUpTo(3);
        t.doneUnits = t.totalUnits = 0;
        c = MainWindow::launchCardFor(t, true);
        check(c.stage == QStringLiteral("等待游戏窗口") && c.total == 0, "AB7 等待游戏启动 = 等待游戏窗口（不定进度）", c.stage);
        t.state = Tasks::TaskState::Failed;
        t.errorSummary = QStringLiteral("游戏进程没有启动");
        c = MainWindow::launchCardFor(t, true);
        check(c.phase == Phase::Failed && c.reason == t.errorSummary && c.stage == QStringLiteral("等待游戏窗口"),
              "AB7 失败带原因 = 启动失败，停在当时那一段", c.reason);
        t.errorSummary.clear();
        check(MainWindow::launchCardFor(t, true).phase == Phase::Idle, "AB7 失败但没有原因（用户自己取消的）= 卡片收起");
        t.state = Tasks::TaskState::Cancelled;
        check(MainWindow::launchCardFor(t, true).phase == Phase::Idle, "AB7 取消 = 卡片收起");
        t.state = Tasks::TaskState::Succeeded;
        check(MainWindow::launchCardFor(t, true).phase == Phase::Succeeded, "AB7 成功 = 游戏已启动");
    }
return 0;
}


