#include "Support.hpp"
int UiCrashReportChecks() {
    // ------------------------------------------------------------------
    // 界面的那一半分三处验：那几句话本身（每一条提示从哪个字段推出来）在 verify 的
    // ZP 段、从 `onGameFinished` 补出那一项在 mw_check 的 T34、**这一项在页上多出哪两颗键**
    // 在这里。
    //
    // 这一节钉的是同一件事的两面：
    // ① 能力位里 `exportReport` / `openCrashDir` 为真 ⇒ 那两颗**在**，点一下带出去的
    // 动作也对（干活的那一半在 MainWindow）；
    // ② 别的任务身上这两颗**不在** —— 它们手里没有那份会话事实，摆一颗点了只能弹
    // "没有可导出的东西"的键，比不给还糟。
    //
    // 判据只读 `Item::capabilities`：这一页要是自己按状态再推一遍，推出来的和账本记的
    // 迟早漂开（见那一页的文件头）。
    {
        TaskDetailPage page;
        page.resize(560, 460);
        page.show();
        QApplication::processEvents();

        QString seenKey;
        int seenAction = -1;
        QObject::connect(&page, &TaskDetailPage::actionRequested,
                         [&seenKey, &seenAction](const QString &key, TaskDetailPage::Action action) {
                             seenKey = key;
                             seenAction = static_cast<int>(action);
                         });

        // ---------- ① 一份「游戏异常退出」 ----------
        TaskCenter::Item crash;
        crash.key = QStringLiteral("77");
        crash.title = QStringLiteral("游戏异常退出 · 1.20.1-我的整合包");
        crash.detail = QStringLiteral("退出码 0xC0000005（内存访问冲突）");
        crash.state = TaskCenter::State::Failed;
        crash.finishedAt = QDateTime(QDate(2026, 10, 13), QTime(10, 5));
        // 那一句原因 = 概括 + 空行 + 那几条提示（「复制摘要」复制的就是它）。
        crash.errorSummary =
            QStringLiteral("「1.20.1-我的整合包」崩溃退出（0xC0000005（内存访问冲突））。\n\n"
                           "「1.20.1-我的整合包」要求 Java 21，本次实际使用的是 Java 17。");
        crash.capabilities.openLog = true;
        crash.capabilities.openTarget = true;
        crash.capabilities.exportReport = true;
        crash.capabilities.openCrashDir = true;
        // 「询问智能体」和上面那两颗同一份判据（能力位由账本那边一起置，见
        // `ActivityCenterController::item()`）。
        crash.capabilities.askAgent = true;
        TaskCenter::Step collected;
        collected.name = QStringLiteral("收集退出信息");
        collected.state = TaskCenter::StepState::Done;
        collected.note = QStringLiteral("0xC0000005（内存访问冲突）");
        crash.steps << collected;

        page.setTask(crash);
        QApplication::processEvents();

        const auto buttonBy = [&page](const QString &name) {
            return page.findChild<QPushButton *>(name);
        };
        auto *report = buttonBy(MainWindowUI::taskDetailReportName());
        auto *crashDir = buttonBy(MainWindowUI::taskDetailCrashDirName());
        auto *askAgent = buttonBy(MainWindowUI::taskDetailAskAgentName());
        auto *cancel = buttonBy(MainWindowUI::taskDetailCancelName());
        auto *retry = buttonBy(MainWindowUI::taskDetailRetryName());
        auto *log = buttonBy(MainWindowUI::taskDetailLogName());
        auto *copy = buttonBy(MainWindowUI::taskDetailCopyName());

        check(report != nullptr && report->isVisible()
                  && report->text() == MainWindowUI::taskDetailReportText()
                  && crashDir != nullptr && page.offers(crashDir)
                  && crashDir->text() == MainWindowUI::taskDetailCrashDirText(),
              "★ U29.1 能力位说可以 ⇒ 「导出错误报告」「打开崩溃报告目录」两颗都在",
              (report != nullptr ? report->text() : QStringLiteral("<没找到>"))
                  + QStringLiteral(" / ")
                  + (crashDir != nullptr ? crashDir->text() : QStringLiteral("<没找到>")));
        // 这一项**建出来就已经结束了**（见 TaskKind::Crash 上那句）：连"正在进行"都没有过，
        // 所以「取消」「重试」两颗一颗都不该露 —— 露了就是一颗点了没反应的按钮。
        check(cancel != nullptr && !cancel->isVisible() && retry != nullptr
                  && !retry->isVisible(),
              "★ U29.1 这一项建出来就是终态 ⇒ 「取消」「重试」都不露");
        // 另外那两颗是**所有**任务都有的（日志窗常驻、目标位置空的话按数据目录开），不能因为
        // 这是崩溃那一项就一起收起来。
        // 次要钮最多露两颗，其余收进「更多…」：判"有没有提供"问 offers()，不问 isVisible()。
        check(log != nullptr && page.offers(log) && copy != nullptr && page.offers(copy),
              "★ U29.1 「打开日志」「复制摘要」照旧在（这一节加的两颗是**多出来**的，不是替换）");
        // 「询问智能体」：和崩溃卡片上那一颗同一个去处（另起崩溃急救会话），露在底条上。
        check(askAgent != nullptr && askAgent->isVisible()
                  && askAgent->text() == MainWindowUI::taskDetailAskAgentText(),
              "★ U29.1 崩溃那一项底条上多出「询问智能体」",
              askAgent != nullptr ? askAgent->text() : QStringLiteral("<没找到>"));
        if (askAgent != nullptr) {
            askAgent->click();
            QApplication::processEvents();
            check(seenKey == QStringLiteral("77")
                      && seenAction == static_cast<int>(TaskDetailPage::Action::AskAgent),
                  "★ U29.3 点「询问智能体」带出去的是它自己的动作",
                  QStringLiteral("key=%1 action=%2").arg(seenKey).arg(seenAction));
        }

        auto *detailLabel = page.findChild<QLabel *>(MainWindowUI::taskDetailDetailName());
        check(detailLabel != nullptr
                  && detailLabel->text() == QStringLiteral("退出码 0xC0000005（内存访问冲突）"),
              "★ U29.2 标题下面那行小字 = 「退出码 …」（退出码写成用户能拿去搜的那个样子）",
              detailLabel != nullptr ? detailLabel->text() : QStringLiteral("<没找到>"));
        auto *errorLabel = page.findChild<QLabel *>(MainWindowUI::taskDetailErrorName());
        check(errorLabel != nullptr && errorLabel->isVisible()
                  && errorLabel->text().contains(QStringLiteral("崩溃退出"))
                  && errorLabel->text().contains(QStringLiteral("要求 Java 21")),
              "★★ U29.2 那一句概括 + 那几条提示**在页上一眼看得见**（不只留在报告里）",
              errorLabel != nullptr ? errorLabel->text() : QStringLiteral("<没找到>"));

        if (report != nullptr) {
            report->click();
            QApplication::processEvents();
            check(seenKey == QStringLiteral("77")
                      && seenAction == static_cast<int>(TaskDetailPage::Action::ExportReport),
                  "★ U29.3 点「导出错误报告」把「哪一项、哪一颗」带出去",
                  QStringLiteral("key=%1 action=%2").arg(seenKey).arg(seenAction));
        }
        if (crashDir != nullptr) {
            crashDir->click();
            QApplication::processEvents();
            check(seenKey == QStringLiteral("77")
                      && seenAction == static_cast<int>(TaskDetailPage::Action::OpenCrashDir),
                  "★ U29.3 点「打开崩溃报告目录」带出去的是**另一颗**的动作（不是同一发）",
                  QStringLiteral("key=%1 action=%2").arg(seenKey).arg(seenAction));
        }

        // ---------- ② 一份普通任务：那两颗**收着** ----------
        //
        // 拿一份**别的都开着**的任务来比：这样能分清"这两颗按能力位收起来了"和"整排都灰了"。
        TaskCenter::Item plain;
        plain.key = QStringLiteral("78");
        plain.title = QStringLiteral("安装 Fabric 1.20.1");
        plain.state = TaskCenter::State::Failed;
        plain.errorSummary = QStringLiteral("有 3 个库文件没下下来（网络超时）。");
        plain.capabilities.retry = true;
        plain.capabilities.openLog = true;
        plain.capabilities.openTarget = true;
        page.setTask(plain);
        QApplication::processEvents();

        check(report != nullptr && !report->isVisible() && crashDir != nullptr
                  && !crashDir->isVisible(),
              "★★ U29.4 别的任务身上这两颗**不在**（它手里没有那份会话事实，摆一颗点了"
              "只能弹「没有可导出的东西」的键比不给还糟）");
        check(askAgent != nullptr && !askAgent->isVisible(),
              "★ U29.4 「询问智能体」也跟着收起来（它要的是同一份崩溃记录）");
        check(retry != nullptr && retry->isVisible() && log != nullptr && log->isVisible(),
              "★ U29.4 同一份任务上「重试」「打开日志」照常露着 —— 说明收起来的是这两颗、"
              "不是整排");

        page.close();
        QApplication::processEvents();
    }
    return 0;
}


