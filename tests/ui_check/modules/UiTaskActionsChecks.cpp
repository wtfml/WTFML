#include "Support.hpp"
#include "ActivityPopover.hpp"
#include "TextMarks.hpp"
int UiTaskActionsChecks(QMainWindow &window, MainWindowUI &ui) {

    {
        // 这一节管两件事：
        // ① 面板里那一行**行尾那颗「取消」**（行内快捷动作）—— 只在能力位说可以、
        // 而且外面真挂了处理器的时候才长；
        // ② **任务详情页**上那一排按钮（「失败能看到失败阶段、简短原因和
        // 『查看日志/重试』」）—— 露与不露**只读能力位**，界面这边一步都不推。
        QPushButton *cell = ui.getTaskSummaryButton();
        if (cell != nullptr) {
            int cancelCalls = 0;
            QString cancelledKey;
            TaskCenter::RowAction cancelledAction = TaskCenter::RowAction::Cancel;
            ui.setTaskCenterRowActionHandler(
                [&cancelCalls, &cancelledKey, &cancelledAction](const QString &key,
                                                                TaskCenter::RowAction action) {
                    ++cancelCalls;
                    cancelledKey = key;
                    cancelledAction = action;
                });
            ui.setTaskCenterContent(
                []() {
                    QList<TaskCenter::Item> items;
                    TaskCenter::Item running;
                    running.key = QStringLiteral("41");
                    running.title = QStringLiteral("正在下载 26.2");
                    running.state = TaskCenter::State::Running;
                    running.percent = 30;
                    running.capabilities.cancel = true;  // ★ 账本说这一项**可以**取消
                    items << running;
                    return items;
                },
                nullptr);
            cell->click();
            ActivityPopover::openAllIn(&window);  // 摘要先弹浮动面板，从「全部任务」进整页
            QApplication::processEvents();
            QWidget *panel = window.findChild<QWidget *>(MainWindowUI::taskCenterPanelName());
            check(panel != nullptr && panel->isVisible(), "I6 任务中心页面再次打开（喂了一件在跑的）");
            if (panel != nullptr) {
                // 面板里那一行那根条也是**自绘的那一支**（三处之一）。测得出百分比
                // 的行才有它（上面 I2 量的是"测不出就不摆"那一侧）。
                auto *rowBar = panel->findChild<QProgressBar *>(
                    MainWindowUI::taskCenterPanelRowBarName(0));
                check(qobject_cast<TaskProgressBar *>(rowBar) != nullptr,
                      "★★ I6 面板里那一行那根条也是**自绘的**（TaskProgressBar），不是原生 QProgressBar",
                      rowBar == nullptr ? QStringLiteral("<那一行没有条>")
                                        : QStringLiteral("类名 %1")
                                              .arg(QString::fromLatin1(rowBar->metaObject()->className())));
                if (rowBar != nullptr) {
                    check(rowBar->height() == MainWindowUI::taskCenterPanelBarHeight(),
                          "★ I6 它和底栏那一根**同一个高度出处**（`taskCenterPanelBarHeight()`）",
                          QStringLiteral("高 %1 期望 %2")
                              .arg(rowBar->height())
                              .arg(MainWindowUI::taskCenterPanelBarHeight()));
                }
                auto *cancel = panel->findChild<QPushButton *>(
                    MainWindowUI::taskCenterPanelRowCancelName(0));
                check(cancel != nullptr && cancel->isVisible(),
                      "I6 能力位说可以取消的那一行，行尾**长出**那颗「取消」",
                      cancel != nullptr ? cancel->text() : QStringLiteral("<没找到>"));
                if (cancel != nullptr) {
                    cancel->click();
                    QApplication::processEvents();
                    check(cancelCalls == 1
                              && cancelledKey == QStringLiteral("41")
                              && cancelledAction == TaskCenter::RowAction::Cancel,
                          "I6 点那颗「取消」把「哪一行、点的是哪一颗」带回了调用方",
                          QStringLiteral("回来 %1 次 / key=%2").arg(cancelCalls).arg(cancelledKey));
                }
                ui.overlay()->dismiss();
                QApplication::processEvents();
                QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            }
            // 收尾：假 provider 和处理器捕的都是这一节里的局部量，用完就卸掉。
            ui.setTaskCenterContent(nullptr, nullptr);
            ui.setTaskCenterRowActionHandler(nullptr);
        }

        // ---------- 任务详情页 ----------
        TaskDetailPage page;
        page.resize(560, 460);
        page.show();
        QApplication::processEvents();

        // 一项**失败、可以重试**的任务：步骤里有一步是失败的、有错因、能取消也能看日志。
        TaskCenter::Item failed;
        failed.key = QStringLiteral("52");
        failed.title = QStringLiteral("安装 Fabric 1.20.1");
        failed.detail = QStringLiteral("正在下载库文件");
        failed.state = TaskCenter::State::Failed;
        failed.percent = 62;
        failed.bytesText = QStringLiteral("已下载 12.3 MB / 45.6 MB");
        failed.startedAt = QDateTime(QDate(2026, 10, 12), QTime(19, 42));
        failed.finishedAt = QDateTime(QDate(2026, 10, 12), QTime(19, 48));
        failed.errorSummary = QStringLiteral("有 3 个库文件没下下来（网络超时）。");
        failed.capabilities.retry = true;
        failed.capabilities.openLog = true;
        failed.capabilities.openTarget = true;
        TaskCenter::Step stepDone;
        stepDone.name = QStringLiteral("校验文件");
        stepDone.state = TaskCenter::StepState::Done;
        TaskCenter::Step stepFailed;
        stepFailed.name = QStringLiteral("下载库文件");
        stepFailed.state = TaskCenter::StepState::Failed;
        stepFailed.note = QStringLiteral("3 个失败");
        failed.steps << stepDone << stepFailed;

        QString seenKey;
        int seenAction = -1;
        QObject::connect(&page, &TaskDetailPage::actionRequested,
                         [&seenKey, &seenAction](const QString &key, TaskDetailPage::Action action) {
                             seenKey = key;
                             seenAction = static_cast<int>(action);
                         });
        page.setTask(failed);
        QApplication::processEvents();

        check(page.taskKey() == QStringLiteral("52"),
              "I6 详情页认得住「这一项是谁」（回话时要带上它）", page.taskKey());
        auto *stateLabel = page.findChild<QLabel *>(MainWindowUI::taskDetailStateName());
        check(stateLabel != nullptr
                  && stateLabel->text() == TextMarks::failed() + QStringLiteral(" 失败"),
              "I6 状态那一行 = 终端记号 + 那一个词（失败不写成一串别的话）",
              stateLabel != nullptr ? stateLabel->text() : QStringLiteral("<没找到>"));
        auto *titleLabel = page.findChild<QLabel *>(MainWindowUI::taskDetailTitleName());
        check(titleLabel != nullptr && titleLabel->text() == QStringLiteral("安装 Fabric 1.20.1"),
              "I6 标题原样摆出来",
              titleLabel != nullptr ? titleLabel->text() : QStringLiteral("<没找到>"));
        auto *bytesLabel = page.findChild<QLabel *>(MainWindowUI::taskDetailBytesName());
        check(bytesLabel != nullptr && bytesLabel->isVisible()
                  && bytesLabel->text() == QStringLiteral("已下载 12.3 MB / 45.6 MB"),
              "I6 按字节记账的那一项，字节那一句在",
              bytesLabel != nullptr ? bytesLabel->text() : QStringLiteral("<没找到>"));
        auto *detailBar = page.findChild<QProgressBar *>(MainWindowUI::taskDetailBarName());
        check(detailBar != nullptr && detailBar->isVisible() && detailBar->value() == 62,
              "I6 测得出百分比 ⇒ 那根条在，而且摆的是**确定态**的那个值",
              detailBar != nullptr ? QString::number(detailBar->value()) : QStringLiteral("<没找到>"));
        // 详情页那根条同样是**自绘的那一支**（三处里的最后一处），而且高度和
        // 另外两处**同一个出处** —— 三处各写一个数的话，同一根条在这三页上会长短不一。
        check(qobject_cast<TaskProgressBar *>(detailBar) != nullptr,
              "★★ I6 详情页那根条也是**自绘的**（TaskProgressBar），不是原生 QProgressBar",
              detailBar == nullptr ? QStringLiteral("<没找到>")
                                   : QStringLiteral("类名 %1").arg(
                                         QString::fromLatin1(detailBar->metaObject()->className())));
        check(detailBar != nullptr
                  && detailBar->height() == MainWindowUI::taskCenterPanelBarHeight(),
              "★ I6 它和底栏 / 面板那两根**同一个高度出处**（`taskCenterPanelBarHeight()`）",
              detailBar != nullptr ? QStringLiteral("高 %1 期望 %2")
                                         .arg(detailBar->height())
                                         .arg(MainWindowUI::taskCenterPanelBarHeight())
                                   : QStringLiteral("<没找到>"));
        auto *percentLabel = page.findChild<QLabel *>(MainWindowUI::taskDetailPercentName());
        check(percentLabel != nullptr && percentLabel->text() == QStringLiteral("62%"),
              "I6 百分比那一句走 `taskCenterPercentText()`（和面板那一行同一个出处）",
              percentLabel != nullptr ? percentLabel->text() : QStringLiteral("<没找到>"));
        auto *step0 = page.findChild<QLabel *>(MainWindowUI::taskDetailStepName(0));
        auto *step1 = page.findChild<QLabel *>(MainWindowUI::taskDetailStepName(1));
        check(step0 != nullptr && step0->text() == TextMarks::done() + QStringLiteral(" 校验文件")
                  && step1 != nullptr
                  && step1->text() == TextMarks::failed() + QStringLiteral(" 下载库文件 / 3 个失败"),
              "I6 步骤那一栏：记号 + 名字（+ 小字），失败的**那一步**单独说清楚",
              (step0 != nullptr ? step0->text() : QStringLiteral("<没找到>")) + QStringLiteral(" / ")
                  + (step1 != nullptr ? step1->text() : QStringLiteral("<没找到>")));
        auto *errorLabel = page.findChild<QLabel *>(MainWindowUI::taskDetailErrorName());
        check(errorLabel != nullptr && errorLabel->isVisible()
                  && errorLabel->text().contains(QStringLiteral("有 3 个库文件没下下来")),
              "I6 ★ 失败的原因**在页上一眼看得见**（不只留在日志里）",
              errorLabel != nullptr ? errorLabel->text() : QStringLiteral("<没找到>"));
        auto *retryButton = page.findChild<QPushButton *>(MainWindowUI::taskDetailRetryName());
        check(retryButton != nullptr && retryButton->isVisible(),
              "I6 ★ 能力位说可以重试 ⇒ 那一页上那颗「重试」在");
        auto *cancelPageButton = page.findChild<QPushButton *>(MainWindowUI::taskDetailCancelName());
        check(cancelPageButton != nullptr && !cancelPageButton->isVisible(),
              "I6 能力位说不能取消 ⇒ 这一页上**没有**「取消」（不是露一颗点了没反应的）");
        auto *logButton = page.findChild<QPushButton *>(MainWindowUI::taskDetailLogName());
        check(logButton != nullptr && page.offers(logButton), "I6 「打开日志」跟着能力位露出来");
        auto *copyButton = page.findChild<QPushButton *>(MainWindowUI::taskDetailCopyName());
        check(copyButton != nullptr && page.offers(copyButton),
              "I6 「复制摘要」**不需要任何能力位**（摘要就是这一页上那几行字，随时抄得走）");
        auto *closeButton = page.findChild<QPushButton *>(MainWindowUI::taskDetailCloseName());
        check(closeButton != nullptr && closeButton->isVisible(),
              "I6 「关闭」永远在（它是这一页的出路之一，另一条是顶上那颗返回）");
        if (retryButton != nullptr) {
            retryButton->click();
            QApplication::processEvents();
            check(seenKey == QStringLiteral("52")
                      && seenAction == static_cast<int>(TaskDetailPage::Action::Retry),
                  "I6 点「重试」把「哪一项、哪一颗」带出去（干活的那一半在 MainWindow）",
                  QStringLiteral("key=%1 action=%2").arg(seenKey).arg(seenAction));
        }

        // 时间那一行：同一天只写时分（下面是唯一出处，面板那一行也用同一个）。
        check(MainWindowUI::taskDetailTimeLineText(failed.startedAt, failed.finishedAt)
                  == QStringLiteral("19:42 开始 / 19:48 结束"),
              "I6 起止时间写成一行；同一天只写时分",
              MainWindowUI::taskDetailTimeLineText(failed.startedAt, failed.finishedAt));
        check(MainWindowUI::taskDetailTimeLineText(QDateTime(),
                                                   failed.finishedAt)
                      == QStringLiteral("19:48 结束")
                  && MainWindowUI::taskDetailTimeLineText(QDateTime(), QDateTime()).isEmpty(),
              "I6 只有一个时间（或者一个都没有）时，那一行跟着变短 / 整行收起来");
        check(MainWindowUI::taskDetailStateText(TaskCenter::State::Cancelled)
                      == QStringLiteral("已取消")
                  && MainWindowUI::taskDetailStateText(TaskCenter::State::Interrupted)
                         == QStringLiteral("已中断")
                  && MainWindowUI::taskDetailStateText(TaskCenter::State::Succeeded)
                         == QStringLiteral("已完成")
                  && MainWindowUI::taskDetailStateText(TaskCenter::State::Running)
                         == QStringLiteral("正在进行"),
              "I6 四个终态各有自己那个词（取消 / 中断不冒充成功）");

        // 一项**正在跑、百分比测不出**的任务：那一行进度整块收起来（不编一个假百分比），
        // 而且**没有步骤**时要写一句"没有分步"（验证工程分得清"本来就没有"和"没画出来"）。
        TaskCenter::Item running;
        running.key = QStringLiteral("53");
        running.title = QStringLiteral("正在下载资源对象");
        running.state = TaskCenter::State::Running;
        running.percent = -1;
        running.capabilities.cancel = true;
        page.setTask(running);
        QApplication::processEvents();
        check(detailBar != nullptr && !detailBar->isVisible() && percentLabel != nullptr
                  && !percentLabel->isVisible(),
              "I6 百分比测不出 ⇒ 那根条和那个数**整行收起来**（不摆一根空转的、也不写 0%）");
        check(errorLabel != nullptr && !errorLabel->isVisible(),
              "I6 还在跑的任务不露出错那一块（那一位是空的）");
        check(cancelPageButton != nullptr && cancelPageButton->isVisible()
                  && retryButton != nullptr && !retryButton->isVisible(),
              "I6 在跑的那一项：能取消、不能重试（能力位反过来也跟着反过来）");
        auto *noSteps = page.findChild<QLabel *>(MainWindowUI::taskDetailNoStepsName());
        check(noSteps != nullptr && noSteps->text() == MainWindowUI::taskDetailNoStepsText(),
              "I6 没有步骤的任务写一句「这一项没有分步。」（不留一块空白）",
              noSteps != nullptr ? noSteps->text() : QStringLiteral("<没找到>"));

        // #130：智能体发起的那一项。「查看会话」只跟着能力位露，点了把「哪一项」带出去。
        auto *sessionButton = page.findChild<QPushButton *>(QStringLiteral("taskDetailAgentSession"));
        check(sessionButton != nullptr && !page.offers(sessionButton),
              "I7 普通任务没有「查看会话」");
        TaskCenter::Item agent;
        agent.key = QStringLiteral("54");
        agent.title = QStringLiteral("智能体：修改启动器设置");
        agent.detail = QStringLiteral("等待确认");
        agent.state = TaskCenter::State::WaitingUser;
        agent.percent = -1;
        agent.capabilities.cancel = true;
        agent.capabilities.openAgentSession = true;
        page.setTask(agent);
        QApplication::processEvents();
        check(sessionButton != nullptr && page.offers(sessionButton) && sessionButton->text() == QStringLiteral("查看会话"),
              "I7 智能体任务露出「查看会话」", sessionButton != nullptr ? sessionButton->text() : QStringLiteral("<没找到>"));
        check(cancelPageButton != nullptr && page.offers(cancelPageButton), "I7 等待确认时也能取消");
        if (sessionButton != nullptr) {
            sessionButton->click();
            QApplication::processEvents();
            check(seenKey == QStringLiteral("54")
                      && seenAction == static_cast<int>(TaskDetailPage::Action::OpenAgentSession),
                  "I7 点「查看会话」把这一项带出去", QStringLiteral("key=%1 action=%2").arg(seenKey).arg(seenAction));
        }

        page.close();
        QApplication::processEvents();
    }
 return 0;
}


