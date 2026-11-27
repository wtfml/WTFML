#include "Support.hpp"
#include "TextMarks.hpp"
#include "ActivityPopover.hpp"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
int UiTaskCenterChecks(QMainWindow &window, MainWindowUI &ui, QTabWidget * &tabs) {

    {
        // 这一节量的是任务中心这颗入口按钮：中间那格写哪句话由**任务账本**
        // 决定（`Tasks::collapsedSummary()`，纯规则在 verify 的 AL/AM 两节量；"同步等待
        // 压在上面"那条在 mw_check 量）。界面这一头量的是这颗按钮**本身**：它是任务中心
        // 的入口，点一下往上弹那块面板。
        QPushButton *cell = ui.getTaskSummaryButton();
        check(cell != nullptr && cell->isVisible() && cell->isEnabled(),
              "I2 中间那格是一颗**亮着的**、能点的按钮（不再整格藏起来）");

        // 「正在启动 <显示名>…」这句话仍然只有**一个**出处：startGame() 之前那次
        // enterSyncWait() 和进程起来之后的 updateBottomBar() 都用它，两处各写一遍
        // 就会出现"文字在启动的那一瞬间跳一下"（ui_shot 那张 02-launching.png 也引它）。
        check(MainWindowUI::launchingStatusText(QStringLiteral("26.2")) ==
                  QStringLiteral("正在启动 26.2…"),
              "启动状态显示实例名称，并以省略号结尾",
              MainWindowUI::launchingStatusText(QStringLiteral("26.2")));
        check(MainWindowUI::launchingStatusText(QStringLiteral("26.2")) !=
                  MainWindowUI::launchingStatusText(QStringLiteral("1.20.1")),
              "显示名不一样，那句话就不一样（不是写死的常量）");

        if (cell != nullptr) {
            // 喂一份假内容：一件在跑 + 一件已完成（真数据是 MainWindow 的事，那几条在
            // mw_check 量 —— ui/ 不许 include src/，见 ui/TaskCenterPanel.hpp 文件头）。
            int clearCalls = 0;
            ui.setTaskCenterContent(
                []() {
                    QList<TaskCenter::Item> items;
                    TaskCenter::Item running;
                    running.key = QStringLiteral("7");
                    running.title = QStringLiteral("正在下载 26.2");
                    running.detail = QStringLiteral("已下载 3 / 9 个文件");
                    running.percent = -1;  // 测不出百分比 ⇒ 不确定态
                    running.state = TaskCenter::State::Running;
                    TaskCenter::Step step;
                    step.name = QStringLiteral("校验文件");
                    step.state = TaskCenter::StepState::Done;
                    running.steps << step;
                    items << running;
                    TaskCenter::Item done;
                    done.key = QStringLiteral("6");
                    done.title = QStringLiteral("安装 Fabric");
                    done.state = TaskCenter::State::Succeeded;
                    done.finishedAt = QDateTime::currentDateTime();
                    items << done;
                    return items;
                },
                [&clearCalls]() { ++clearCalls; });
            // 记号必须是**默认界面字体里真有的那几格**：`✓` / `✗` / `⚠` 那一档不在里面，
            // 第一次画出来之前 Qt 要先把整条字体回退链建起来 —— 本机实测 277 ms，
            // 而它落在"那一页第一次画出来的那一帧"上（见 `ui/TextMarks.hpp` 头注释）。
            // 这一条不是美学检查：把记号改回 `✓` 会让任务中心第一次打开时卡一下。
            check(TextMarks::done() == QStringLiteral("√") && TextMarks::failed() == QStringLiteral("×")
                      && TextMarks::warning() == QStringLiteral("※")
                      && TextMarks::checked() == QStringLiteral("■")
                      && TextMarks::unchecked() == QStringLiteral("□"),
                  "I1 TextMarks 那五格还是主字体里有的字（别改回 ✓ / ✗ / ⚠ / ☑ / ☐）",
                  TextMarks::done() + TextMarks::failed() + TextMarks::warning()
                      + TextMarks::checked() + TextMarks::unchecked());
            check(TaskCenter::terminalMark(TaskCenter::State::Succeeded) == TextMarks::done()
                      && TaskCenter::terminalMark(TaskCenter::State::Failed) == TextMarks::failed()
                      && TaskCenter::terminalMark(TaskCenter::State::Cancelled) == QStringLiteral("—")
                      && TaskCenter::terminalMark(TaskCenter::State::Interrupted) == QStringLiteral("!"),
                  "I2 四种终态各有自己的记号（取消/中断不冒充成功）");
            // 收尾时还停在「正在跑」的那一步：面板行 / 详情页 / 复制文本三处都走
            // `stepForTask()`。取消不能被画成 ●（任务已经结束、里面还有东西在跑）。
            {
                TaskCenter::Step runningStep;
                runningStep.name = QStringLiteral("复制文件");
                runningStep.state = TaskCenter::StepState::Running;
                const TaskCenter::Step cancelledStep =
                    TaskCenter::stepForTask(TaskCenter::State::Cancelled, runningStep);
                const TaskCenter::Step interruptedStep =
                    TaskCenter::stepForTask(TaskCenter::State::Interrupted, runningStep);
                const TaskCenter::Step succeededStep =
                    TaskCenter::stepForTask(TaskCenter::State::Succeeded, runningStep);
                const TaskCenter::Step activeStep =
                    TaskCenter::stepForTask(TaskCenter::State::Running, runningStep);
                check(cancelledStep.state == TaskCenter::StepState::Pending &&
                          cancelledStep.note == QStringLiteral("已取消") &&
                          interruptedStep.state == TaskCenter::StepState::Failed &&
                          interruptedStep.note == QStringLiteral("已中断") &&
                          succeededStep.state == TaskCenter::StepState::Done &&
                          activeStep.state == TaskCenter::StepState::Running,
                      "I2 终态任务里停在 Running 的那一步跟着收尾改写；活动中的任务照旧",
                      QStringLiteral("取消 %1/%2 · 中断 %3/%4 · 成功 %5 · 运行中 %6")
                          .arg(int(cancelledStep.state)).arg(cancelledStep.note)
                          .arg(int(interruptedStep.state)).arg(interruptedStep.note)
                          .arg(int(succeededStep.state)).arg(int(activeStep.state)));
            }
            check(MainWindowUI::taskSummaryText(QStringLiteral("任务"),
                                                TaskCenter::State::Cancelled)
                          .endsWith(MainWindowUI::taskSummaryCancelledSuffix())
                      && MainWindowUI::taskSummaryText(QStringLiteral("任务"),
                                                       TaskCenter::State::Interrupted)
                             .endsWith(MainWindowUI::taskSummaryInterruptedSuffix()),
                  "I2 底栏能分别写出已取消和已中断");
            cell->click();
            QApplication::processEvents();
            // 设计稿 panels-tasks：点任务摘要先弹浮动面板（窗口里的一张卡片，贴在摘要上方）。
            auto *popover = window.findChild<QFrame *>(ActivityPopover::objectNameValue());
            check(popover != nullptr && popover->isVisible() && !popover->isWindow() &&
                      popover->width() == ActivityPopover::kWidth &&
                      popover->mapToGlobal(QPoint(0, popover->height())).y() <= cell->mapToGlobal(QPoint(0, 0)).y() &&
                      popover->geometry().top() >= 0,
                  "★ I2 点一下中间那格，弹出 400 宽的任务中心浮动面板，贴在任务摘要上方、整块在窗口里",
                  popover ? QStringLiteral("%1×%2 @%3,%4").arg(popover->width()).arg(popover->height())
                                .arg(popover->x()).arg(popover->y()) : QStringLiteral("没弹"));
            check(popover != nullptr &&
                      popover->findChild<QWidget *>(ActivityPopover::partName(QStringLiteral("Row"), QStringLiteral("7"))) &&
                      popover->findChild<QWidget *>(ActivityPopover::partName(QStringLiteral("Row"), QStringLiteral("6"))),
                  "I2 浮动面板摆出了正在跑的那件和已结束的那件");
            cell->click();
            QApplication::processEvents();
            {
                auto *again = window.findChild<QFrame *>(ActivityPopover::objectNameValue());
                check(again == nullptr || !again->isVisible(), "I2 再点一次中间那格，浮动面板收起");
            }
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            cell->click();
            QApplication::processEvents();
            popover = window.findChild<QFrame *>(ActivityPopover::objectNameValue());
            if (auto *openAll = popover ? popover->findChild<QPushButton *>(ActivityPopover::openAllButtonName()) : nullptr)
                openAll->click();
            QApplication::processEvents();
            QWidget *panel = window.findChild<QWidget *>(MainWindowUI::taskCenterPanelName());
            check(panel != nullptr && panel->isVisible(),
                  "I2 浮动面板里点「全部任务」，打开了完整任务中心工作区");
            if (panel != nullptr) {
                check(!panel->isWindow() && ui.overlay() != nullptr
                          && ui.overlay()->pageOpen(),
                      "I2 任务中心是主窗口内容区中的完整页面，不是独立 Popup");
                QTabBar *topTabs = ui.getTabWidget() != nullptr ? ui.getTabWidget()->tabBar()
                                                                 : nullptr;
                check(topTabs != nullptr
                          && panel->mapToGlobal(QPoint(0, 0)).y()
                                 > topTabs->mapToGlobal(topTabs->rect().bottomLeft()).y(),
                      "I2 任务中心从顶端四个主选项卡下方开始，不覆盖导航按钮");
                QTabWidget *tabs = ui.getTabWidget();
                const int ownerTab = tabs != nullptr ? tabs->currentIndex() : -1;
                const int otherTab = tabs != nullptr && tabs->count() > 1
                                         ? (ownerTab + 1) % tabs->count()
                                         : -1;
                if (tabs != nullptr && otherTab >= 0) {
                    tabs->setCurrentIndex(otherTab);
                    check(!panel->isVisible(),
                          "I2 切到其它主页面时任务中心停放，不盖住新页面");
                    tabs->setCurrentIndex(ownerTab);
                    check(panel->isVisible() && ui.overlay()->graphicsEffect() == nullptr,
                          "I2 切回时任务中心立即恢复，不用灰色覆盖层淡入");
                    QWidget *curtain =
                        tabs->findChild<QWidget *>(QStringLiteral("wtfmlPageCurtain"));
                    check(curtain == nullptr || !curtain->isVisible(),
                          "I2 任务中心停放期间切主页面不显示灰色换页幕布");
                }
                check(panel->findChild<QWidget *>(QStringLiteral("taskCenterRunningPane")) == nullptr
                          && window.findChild<QPushButton *>(QStringLiteral("taskCenterRunningCollapse")) == nullptr
                          && panel->findChild<QWidget *>(QStringLiteral("taskCenterTaskList")) !=
                                 nullptr
                          && panel->findChild<QWidget *>(QStringLiteral("taskCenterDetailPane")) !=
                                 nullptr,
                      "I2 任务中心只有任务列表 / 上下文详情两列，运行中的进程不在这里（看轨底入口）");
                // 行按**行号**找得回来（名字由 taskCenterPanelRowName() 给），两栏接着数：
                // 第 0 行是「当前工作」那件、第 1 行是「已完成」那件。
                QLabel *title0 =
                    panel->findChild<QLabel *>(MainWindowUI::taskCenterPanelRowTitleName(0));
                QLabel *title1 =
                    panel->findChild<QLabel *>(MainWindowUI::taskCenterPanelRowTitleName(1));
                check(title0 != nullptr && title0->text() == QStringLiteral("正在下载 26.2") &&
                          title1 != nullptr && title1->text() == QStringLiteral("安装 Fabric"),
                      "I2 两栏各画了一行（在跑的那件在前、已完成的那件在后）",
                      (title0 != nullptr ? title0->text() : QStringLiteral("<没找到>")) +
                          QStringLiteral(" / ") +
                          (title1 != nullptr ? title1->text() : QStringLiteral("<没找到>")));
                // 认一行靠那一行的 key（**不是行号**）：属性名走唯一出处。
                QWidget *rowWidget0 =
                    panel->findChild<QWidget *>(MainWindowUI::taskCenterPanelRowName(0));
                check(rowWidget0 != nullptr &&
                          rowWidget0->property(MainWindowUI::taskCenterPanelRowKeyProperty())
                                  .toString() == QStringLiteral("7"),
                      "I2 每一行挂着自己是谁（taskCenterPanelRowKeyProperty() = 任务号）",
                      rowWidget0 != nullptr
                          ? rowWidget0->property(MainWindowUI::taskCenterPanelRowKeyProperty())
                                .toString()
                          : QStringLiteral("<没找到>"));
                // 百分比测不出时**不摆那根条**（不编一个假百分比出来）。
                // 这一行里那点信息由下面那句小字说（"已下载 3 / 9 个文件"）。
                // NOTE: 不确定态（range 0..0）是**底栏那一整格**的写法（"有东西在跑但说不清
                // 到哪了"），不是面板里每一行的 —— 一行一行都摆一根空转的条，面板
                // 一去几条就是几根在那儿空转。
                auto *bar0 =
                    panel->findChild<QProgressBar *>(MainWindowUI::taskCenterPanelRowBarName(0));
                auto *detail0 =
                    panel->findChild<QLabel *>(MainWindowUI::taskCenterPanelRowDetailName(0));
                check(bar0 == nullptr, "I2 测不出百分比那一行**不摆**那根条（不编一个假百分比出来）",
                      bar0 != nullptr
                          ? QStringLiteral("还是摆了 %1..%2").arg(bar0->minimum()).arg(bar0->maximum())
                          : QStringLiteral("<没找到>"));
                check(detail0 != nullptr &&
                          detail0->text() == QStringLiteral("已下载 3 / 9 个文件"),
                      "I2 那一行的进度改由小字那句说（「已下载 3 / 9 个文件」）",
                      detail0 != nullptr ? detail0->text() : QStringLiteral("<没找到>"));
                // 「清除已完成」那颗按钮：有处理器才出现，点了要回一声。
                // 它挂在**页标题行**（面板把 `headerActions()` 交给
                // `OverlayHost::setPageHeaderWidget()`），打开这一页时已不在面板子树里。
                auto *clear = window.findChild<QPushButton *>(
                    MainWindowUI::taskCenterPanelClearButtonName());
                check(clear != nullptr,
                      "I2 有一件已完成 ⇒ 面板标题行里有那颗「清除已完成」");
                if (clear != nullptr) {
                    clear->click();
                    QApplication::processEvents();
                    check(clearCalls == 1, "I2 点「清除已完成」把话带回了调用方（一次）",
                          QStringLiteral("回来 %1 次").arg(clearCalls));
                }
                // 行尾那颗「取消」（行内快捷动作）：
                // 只有**能力位说可以**、而且外面挂了处理器时才长。上面那件在跑的那一行
                // 没设 `capabilities.cancel` ⇒ 那一行行尾什么都不长；挂上处理器也不长。
                auto *cancel0 = panel->findChild<QPushButton *>(
                    MainWindowUI::taskCenterPanelRowCancelName(0));
                check(cancel0 == nullptr,
                      "I2 能力位说不能取消的行，行尾**不长**那颗「取消」",
                      cancel0 != nullptr ? cancel0->text() : QStringLiteral("<没找到>"));
                if (tabs != nullptr && otherTab >= 0 && ui.titleBar() != nullptr) {
                    auto *otherNav = ui.titleBar()->navButton(otherTab);
                    auto *ownerNav = ui.titleBar()->navButton(ownerTab);
                    if (otherNav != nullptr && ownerNav != nullptr) {
                        otherNav->click();
                        QApplication::processEvents();
                        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                        check(tabs->currentIndex() == otherTab && ui.overlay() != nullptr
                                  && !ui.overlay()->isOpen()
                                  && window.findChild<QWidget *>(
                                         MainWindowUI::taskCenterPanelName()) == nullptr,
                              "I2 点顶上别的选项卡 = 任务中心被收起（不再占着覆盖层那一位）");
                        ownerNav->click();
                        QApplication::processEvents();
                        check(ui.overlay() != nullptr && !ui.overlay()->isOpen()
                                  && window.findChild<QWidget *>(
                                         MainWindowUI::taskCenterPanelName()) == nullptr,
                              "I2 再点回原来那一页，任务中心不会自己又摊开");
                    }
                }
                ui.overlay()->dismiss();
                QApplication::processEvents();
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            // 收尾：那个假 provider 捕的是这一节里的局部量，用完就卸掉（留着的话，后面
            // 哪一节再点开面板就是拿着悬空引用在跑）。
            ui.setTaskCenterContent(nullptr, nullptr);
            check(window.findChild<QWidget *>(MainWindowUI::taskCenterPanelName()) == nullptr,
                  "I2 关闭之后那块面板被删掉了（不留着一块看不见的面板）");
        }
    }
    {
        // ---------- I2b 浮动面板本身：三段、队列上下移 / 移除、清除、重试、Esc 与点外面收起 ----------
        QWidget host;
        host.resize(900, 700);
        auto *anchor = new QPushButton(QStringLiteral("任务"), &host);
        anchor->setGeometry(20, 640, 160, 40);
        auto *outside = new QPushButton(QStringLiteral("别处"), &host);
        outside->setGeometry(600, 20, 100, 30);
        host.show();
        QList<QPair<QString, TaskCenter::RowAction>> actions;
        int clears = 0;
        ActivityPopover::Handlers handlers;
        handlers.items = [] {
            QList<TaskCenter::Item> items;
            TaskCenter::Item run;
            run.key = QStringLiteral("r");
            run.title = QStringLiteral("安装 Fabric 1.21.4");
            run.percent = 37;
            run.state = TaskCenter::State::Running;
            run.steps << TaskCenter::Step{QStringLiteral("下载版本清单"), TaskCenter::StepState::Done, QString()}
                      << TaskCenter::Step{QStringLiteral("下载库文件"), TaskCenter::StepState::Running,
                                          QStringLiteral("128 / 342")};
            items << run;
            for (int i = 0; i < 2; ++i) {
                TaskCenter::Item q;
                q.key = QStringLiteral("q%1").arg(i);
                q.title = QStringLiteral("排队 %1").arg(i);
                q.state = TaskCenter::State::Queued;
                q.queueGroup = QStringLiteral("dl");
                q.queueOrder = i;
                q.capabilities.cancel = true;
                items << q;
            }
            TaskCenter::Item failed;
            failed.key = QStringLiteral("f");
            failed.title = QStringLiteral("下载光影包");
            failed.state = TaskCenter::State::Failed;
            failed.errorSummary = QStringLiteral("校验失败");
            failed.capabilities.retry = true;
            failed.finishedAt = QDateTime::currentDateTime();
            items << failed;
            return items;
        };
        handlers.rowAction = [&actions](const QString &key, TaskCenter::RowAction a) { actions.append({key, a}); };
        handlers.clearFinished = [&clears] { ++clears; };
        auto *pop = new ActivityPopover(anchor, handlers);
        pop->show();
        pop->refresh();
        QApplication::processEvents();
        const auto part = [&pop](const char *name, const char *key) {
            return pop->findChild<QPushButton *>(
                ActivityPopover::partName(QString::fromLatin1(name), QString::fromLatin1(key)));
        };
        check(pop->geometry().bottom() < anchor->y() && pop->x() == anchor->x() &&
                  pop->width() == ActivityPopover::kWidth,
              "★ I2b 浮动面板贴在锚点上方、左沿对齐、400 宽",
              QStringLiteral("%1,%2 %3×%4").arg(pop->x()).arg(pop->y()).arg(pop->width()).arg(pop->height()));
        // 开着时 refresh 重建：新行要当场算进高度，不能塌成只剩标题行（曾经 68px）。
        check(pop->height() > 200, "★ I2b 开着时重建，面板高度跟着内容走", QString::number(pop->height()));
        check(part("MoveUp", "q0") && !part("MoveUp", "q0")->isEnabled() && part("MoveDown", "q0")->isEnabled() &&
                  part("MoveUp", "q1") && part("MoveUp", "q1")->isEnabled() && !part("MoveDown", "q1")->isEnabled() &&
                  part("Remove", "q1") && !part("MoveUp", "r") && part("Retry", "f"),
              "★ I2b 等待中那几行有上移 / 下移 / 移除（队首不能再上、队尾不能再下）；正在跑的不排队；失败的有「重试」");
        if (part("MoveDown", "q0")) part("MoveDown", "q0")->click();
        if (part("Remove", "q1")) part("Remove", "q1")->click();
        if (part("Retry", "f")) part("Retry", "f")->click();
        if (auto *clear = pop->findChild<QPushButton *>(ActivityPopover::clearButtonName())) clear->click();
        check(actions.size() == 3 && actions.at(0) == qMakePair(QStringLiteral("q0"), TaskCenter::RowAction::MoveDown) &&
                  actions.at(1) == qMakePair(QStringLiteral("q1"), TaskCenter::RowAction::Cancel) &&
                  actions.at(2) == qMakePair(QStringLiteral("f"), TaskCenter::RowAction::Retry) && clears == 1,
              "★ I2b 下移 / 移除 / 重试 / 清除各自转成对应的动作，一次一发",
              QString::number(actions.size()) + QStringLiteral(" / clear ") + QString::number(clears));
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(pop, &esc);
        QApplication::processEvents();
        check(!pop->isVisible(), "I2b Esc 收起浮动面板");
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        pop = new ActivityPopover(anchor, handlers);
        pop->show();
        QApplication::processEvents();
        QPointer<ActivityPopover> watch(pop);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), outside->mapToGlobal(QPointF(5, 5)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(outside, &press);
        QApplication::processEvents();
        check(watch.isNull() || !watch->isVisible(), "I2b 点在面板外面就收起");
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    {
        // ---------- #149 任务中心叠在覆盖页之上时，点击详情里的执行步骤不能把任务中心关掉 ----------
        ui.setTaskCenterContent(
            []() {
                QList<TaskCenter::Item> items;
                TaskCenter::Item running;
                running.key = QStringLiteral("i149");
                running.title = QStringLiteral("安装整合包 ATM9");
                running.detail = QStringLiteral("已下载 128 / 342 个文件");
                running.percent = 37;
                running.state = TaskCenter::State::Running;
                running.steps << TaskCenter::Step{QStringLiteral("读取整合包清单"), TaskCenter::StepState::Done, QString(), 100}
                              << TaskCenter::Step{QStringLiteral("下载整合包文件"), TaskCenter::StepState::Running,
                                                  QStringLiteral("128 / 342"), 37};
                items << running;
                return items;
            },
            nullptr);
        QWidget *underPage = ui.overlay() != nullptr
                                 ? ui.overlay()->openPage(QStringLiteral("资源详情"))
                                 : nullptr;
        QApplication::processEvents();
        check(underPage != nullptr && ui.overlay()->pageOpen(),
              "#149 前提：覆盖层先开着一页（任务中心会作为全尺寸提示层叠在上面）");
        if (underPage != nullptr) {
            int promptDismissals = 0;
            const QMetaObject::Connection spy = QObject::connect(
                ui.overlay(), &OverlayHost::promptDismissed, ui.overlay(),
                [&promptDismissals] { ++promptDismissals; });
            if (QPushButton *summary = ui.getTaskSummaryButton()) {
                summary->click();
                QApplication::processEvents();
                if (auto *pop = window.findChild<QFrame *>(ActivityPopover::objectNameValue())) {
                    if (auto *openAll = pop->findChild<QPushButton *>(ActivityPopover::openAllButtonName())) {
                        openAll->click();
                    }
                }
            }
            QApplication::processEvents();
            QWidget *workspace = window.findChild<QWidget *>(QStringLiteral("taskCenterPromptWorkspace"));
            check(workspace != nullptr && workspace->isVisible() && ui.overlay()->promptOpen(),
                  "#149 任务中心作为全尺寸提示层叠在覆盖页之上打开");
            if (workspace != nullptr) {
                if (QWidget *row0 = workspace->findChild<QWidget *>(MainWindowUI::taskCenterPanelRowName(0))) {
                    const QPoint rowLocal = row0->rect().center();
                    QMouseEvent openDetail(QEvent::MouseButtonPress, QPointF(rowLocal),
                                           row0->mapToGlobal(QPointF(rowLocal)), Qt::LeftButton,
                                           Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(row0, &openDetail);
                    QApplication::processEvents();
                }
                QWidget *stepLabel = workspace->findChild<QWidget *>(QStringLiteral("taskCenterFlowStepName_0"));
                QWidget *stepRow = workspace->findChild<QWidget *>(QStringLiteral("taskCenterFlowStep_0"));
                check(stepLabel != nullptr && stepLabel->isVisible() && stepRow != nullptr && stepRow->isVisible(),
                      "#149 详情页展示出执行流程步骤");
                if (stepLabel != nullptr) {
                    const QPoint stepLocal = stepLabel->rect().center();
                    QMouseEvent clickStep(QEvent::MouseButtonPress, QPointF(stepLocal),
                                          stepLabel->mapToGlobal(QPointF(stepLocal)), Qt::LeftButton,
                                          Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(stepLabel, &clickStep);
                    QApplication::processEvents();
                    check(promptDismissals == 0 && ui.overlay()->promptOpen() && workspace->isVisible(),
                          "★ #149 点击任务中心详情中的步骤，不触发外部点击关闭，任务中心保持打开",
                          QStringLiteral("dismiss=%1 promptOpen=%2 visible=%3")
                              .arg(promptDismissals)
                              .arg(ui.overlay()->promptOpen())
                              .arg(workspace->isVisible()));
                }
                // 同时验证：非全尺寸提示卡片内点标签不会误关，点卡片外仍能正常关闭。
                ui.overlay()->dismiss();
                QApplication::processEvents();
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                check(promptDismissals == 1 && ui.overlay()->pageOpen(),
                      "#149 主动关闭任务中心后回到原本的覆盖页");
            }
            QObject::disconnect(spy);
            ui.overlay()->closeOverlay();
            QApplication::processEvents();
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        ui.setTaskCenterContent(nullptr, nullptr);
    }
 return 0;
}


