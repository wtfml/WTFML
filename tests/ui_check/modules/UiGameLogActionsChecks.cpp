#include "Support.hpp"
#include <QFontInfo>
int UiGameLogActionsChecks(QFrame * &bar) {
    {
        // 这个窗是"给我一行字我就写上去"的一块控件，不认识"会话"是什么（见它的文件头），
        // 所以在这里验它最合适：给它几行字，看它写没写上去、跟不跟到底、两颗钮管不管用。
        //
        // NOTE: 建在**堆**上，因为它自己带 WA_DeleteOnClose —— 关一下就析构，栈上的对象
        // 那么干就是 double free。
        auto *log = new GameLogWindow(QStringLiteral("我的整合包"));
        QPointer<GameLogWindow> guard(log);
        log->show();
        QApplication::processEvents();

        // ---- (a) 标题带实例名：同时开着好几份时靠它认哪个是哪个 ----
        check(GameLogWindow::windowTitleText(QStringLiteral("我的整合包"))
                      == QStringLiteral("游戏日志 — 我的整合包"),
              "Z3 单开标题带实例名且不带序号",
              GameLogWindow::windowTitleText(QStringLiteral("我的整合包")));
        check(GameLogWindow::windowTitleText(QStringLiteral("我的整合包"), 2)
                      == QStringLiteral("游戏日志 — 我的整合包 #2"),
              "Z3 多开标题带实例名和 #序号",
              GameLogWindow::windowTitleText(QStringLiteral("我的整合包"), 2));
        // 实例名是空的（会话快照没拿到）时不留一个光秃秃的破折号或序号。
        check(GameLogWindow::windowTitleText(QString()) == QStringLiteral("游戏日志")
                  && GameLogWindow::windowTitleText(QString(), 2) == QStringLiteral("游戏日志"),
              "Z3 没有实例名时标题就是「游戏日志」",
              GameLogWindow::windowTitleText(QString()));
        log->setInstanceNumber(1);
        auto *titleInstance = log->findChild<QLabel *>(QStringLiteral("gameLogTitleInstance"));
        check(log->windowTitle() == QStringLiteral("游戏日志 — 我的整合包 #1")
                  && titleInstance != nullptr
                  && titleInstance->text() == QStringLiteral("我的整合包 #1"),
              "Z3 setInstanceNumber 会同步更新系统标题栏与自绘标题条",
              log->windowTitle());

        QPlainTextEdit *view = log->getLogView();
        check(view != nullptr && view->objectName() == GameLogWindow::logViewName(),
              "Z3 那块文本控件按名字找得回来",
              view != nullptr ? view->objectName() : QStringLiteral("<null>"));
        check(log->getHintLabel() != nullptr &&
                  log->getHintLabel()->text() == GameLogWindow::hintText() &&
                  !GameLogWindow::hintText().isEmpty(),
              "Z3 顶上那句说明在，而且有话说");
        check(log->getMoreButton() != nullptr &&
                  log->getMoreButton()->objectName() == GameLogWindow::moreButtonName() &&
                  log->clearAction() != nullptr && log->copyAction() != nullptr,
              "Z3 标题行「…」在，菜单里有清空与复制全部");

        // ---- 底下右角那两颗「导出…」 ----
        {
            QAction *exportLog = log->exportLogAction();
            QAction *exportReport = log->exportReportAction();
            check(exportLog != nullptr && exportReport != nullptr,
                  "Z3c 「…」菜单里那两项「导出…」都在");
            if (exportLog != nullptr && exportReport != nullptr) {
                check(exportLog->text() == GameLogWindow::exportLogButtonText() &&
                          exportReport->text() == GameLogWindow::exportReportButtonText(),
                      "Z3c 两项上的字就是那两句",
                      exportLog->text() + QStringLiteral(" / ") + exportReport->text());
                check(!exportLog->toolTip().isEmpty() && !exportReport->toolTip().isEmpty() &&
                          exportLog->toolTip() != exportReport->toolTip(),
                      "Z3c 两项的提示文字是**两句话**（说的不是同一件事）",
                      exportLog->toolTip() + QStringLiteral(" | ") + exportReport->toolTip());
                int logFired = 0, reportFired = 0;
                QObject::connect(log, &GameLogWindow::exportLogRequested, log, [&logFired] { ++logFired; });
                QObject::connect(log, &GameLogWindow::exportReportRequested, log, [&reportFired] { ++reportFired; });
                exportLog->trigger();
                exportReport->trigger();
                check(logFired == 1 && reportFired == 1, "Z3c 两项各发自己那一个信号（一次）");
            }
        }

        if (view != nullptr) {
            // ---- (b) 只读、等宽、有上限 ----
            check(view->isReadOnly(), "Z3 那块文本是只读的（是看输出的窗，不是输入框）");
            check(view->placeholderText() == GameLogWindow::emptyPlaceholderText() &&
                      !GameLogWindow::emptyPlaceholderText().isEmpty(),
                  "Z3 空的时候有一句占位", view->placeholderText());
            // 等宽：日志里有缩进和对齐的表格，比例字体读起来是散的。
            //
            // NOTE: 这里量的是"字体是不是 Qt 认的那个系统等宽字体"，**不是**
            // QFontInfo::fixedPitch()。后者是在问"这台机器上那个字体真的是等宽的
            // 吗"，而 offscreen 平台根本没有真字体库（Qt 只回一个叫 monospace 的
            // 占位族名、量出来 fixedPitch() 是 false）—— 那是**环境**的属性，不是
            // 我们的决定。按"颜色/字体由谁决定"来挑断言：挑字体这件事是我们干的，
            // 挑得对不对是环境的事。
            const QString fixedFamily =
                QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
            check(view->font().family() == fixedFamily,
                  "Z3 用的是 Qt 认的那个系统等宽字体",
                  QString("%1 vs %2").arg(view->font().family(), fixedFamily));
            check(view->font().family() != QApplication::font().family(),
                  "Z3 而且真的换掉了默认字体（不然上面那条是白验）",
                  QString("%1 vs %2").arg(view->font().family(), QApplication::font().family()));
            // 字号**比系统那个等宽字体大一号**（用户说的「字号稍微大一点」）。
            //
            // NOTE: 比的是**系统那一份**，不是 `GameLogWindow::logFont()` 自己 —— 跟自己比
            // 是空的（那个函数改回原样，这条跟着一起变）。另外点大小和像素大小二选一
            // （字体表给的可能是任一种，另一头是 -1），所以分两支量。
            {
                const QFont systemFixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
                const QFont logFont = view->font();
                const bool points = systemFixed.pointSizeF() > 0;
                const qreal systemSize = points ? systemFixed.pointSizeF() : systemFixed.pixelSize();
                const qreal logSize = points ? logFont.pointSizeF() : logFont.pixelSize();
                const bool bigger = logSize > systemSize;
                const QString fontDetail = QStringLiteral("日志 %1 / 系统 %2").arg(logSize).arg(systemSize);
                check(bigger && GameLogWindow::logFontPointDelta() > 0,
                      "Z3 正文字号比系统那个等宽字体**大一档**（用户说的「稍微大一点」）",
                      fontDetail);
            }
            // 行数上限**必须有**：挂了一百个模组的实例能吐几十万行，不封顶的话开一晚上
            // 就把内存吃干净了。上限是"超了丢最老的"，所以只要它 > 0 就是个真的闸。
            //
            // NOTE: 起视图的上限允许**比 maxLogBlocks() 多一格**：多出来的那一格
            // 是"还没等到换行的那半行"（它不在模型里）。两个上限必须对齐 —— 视图比模型
            // 能多留，模型里最早那一行才会被它自己同步丢掉；视图比模型少留的话，跑满
            // 两万行之后切一次筛选，最早那一行会从模型里又冒出来。
            check(GameLogWindow::maxLogBlocks() > 0 &&
                      view->maximumBlockCount() >= GameLogWindow::maxLogBlocks() &&
                      view->maximumBlockCount() <= GameLogWindow::maxLogBlocks() + 1,
                  "Z3 文本有行数上限，而且真的装上了（最多比模型多留一格给那半行）",
                  QString("maximumBlockCount=%1 模型上限=%2")
                      .arg(view->maximumBlockCount())
                      .arg(GameLogWindow::maxLogBlocks()));
            // 说明行比正文小一号、颜色是 PlaceholderText（和设置页那些说明同一套写法）。
            if (log->getHintLabel() != nullptr) {
                check(log->getHintLabel()->font().pixelSize() < log->font().pixelSize(),
                      "Z3 顶上那句说明比正文小一号",
                      QString("%1 vs %2")
                          .arg(log->getHintLabel()->font().pixelSize())
                          .arg(log->font().pixelSize()));
                check(log->getHintLabel()->palette().color(QPalette::WindowText) ==
                          log->getHintLabel()->palette().color(QPalette::PlaceholderText),
                      "Z3 那句说明的颜色是 PlaceholderText（灰色注释那一档）",
                      QStringLiteral("字 %1 / 占位 %2")
                          .arg(log->getHintLabel()
                                   ->palette()
                                   .color(QPalette::WindowText)
                                   .name(QColor::HexArgb),
                               log->getHintLabel()
                                   ->palette()
                                   .color(QPalette::PlaceholderText)
                                   .name(QColor::HexArgb)));
            }

            // ---- (c) 写进去的就是原样那几行 ----
            log->appendText(QStringLiteral("第一行\n"));
            check(log->logText() == QStringLiteral("第一行\n"),
                  "Z3 一行原样写进去（游戏自己的换行不动它）", log->logText().left(80));
            log->appendText(QString());
            check(log->logText() == QStringLiteral("第一行\n"),
                  "Z3 空串不会往文本里塞东西（也不炸）", log->logText().left(80));

            // ---- (d) 跟到底，但**用户往上翻的时候不跟** ----
            // 这一条是这类窗最招人烦的地方：正在往上翻看崩栈，新输出一来就被顶走。
            for (int i = 0; i < 200; ++i) {
                log->appendText(QStringLiteral("第 %1 行\n").arg(i));
            }
            QApplication::processEvents();
            QScrollBar *bar = view->verticalScrollBar();
            check(bar->maximum() > 0,
                  "Z3 前提：攒够内容之后滚动条真的有范围了（不然下面两条量不出东西）",
                  QString("max=%1").arg(bar->maximum()));

            // 在底下：新的一行要把它带到底
            bar->setValue(bar->maximum());
            log->appendText(QStringLiteral("跟着滚的那一行\n"));
            check(bar->value() == bar->maximum(),
                  "Z3 本来就在最底下时，新输出跟着滚到底",
                  QString("%1 vs %2").arg(bar->value()).arg(bar->maximum()));

            // 往上翻：位置不许动
            bar->setValue(0);
            const int before = bar->value();
            log->appendText(QStringLiteral("用户在读上面的时候来的一行\n"));
            check(bar->value() == before,
                  "Z3 用户在往上翻时不跟（不许把正在读的东西顶走）",
                  QString("%1 -> %2").arg(before).arg(bar->value()));
            check(log->logText().contains(QStringLiteral("用户在读上面")),
                  "Z3 那行字照样写进去了，只是没把视图带下去");

            // ---- (e) 两颗钮 ----
            log->clearAction()->trigger();
            check(log->logText().isEmpty(), "Z3 「清空」把这一块文本清空了",
                  QString::number(log->logText().size()));
            check(bar->value() == 0, "Z3 清空之后回到顶部（不至于停在一个空范围中间）",
                  QString::number(bar->value()));

            // ---- (f) 关闭：发 finished + 真的析构 ----
            bool finishedFired = false;
            QObject::connect(log, &QDialog::finished, log,
                             [&finishedFired]() { finishedFired = true; });
            log->close();
            QApplication::processEvents();
            check(finishedFired,
                  "Z3 关窗发 finished（调用方正是靠它记住「这一份用户不想再看日志了」）");
            // WA_DeleteOnClose 的销毁走 deleteLater，所以得把 DeferredDelete 那批事件放出去。
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            check(guard.isNull(),
                  "Z3 关掉之后真的析构了（这些窗会留一整份文本，藏起来不销毁就是白占内存）");
        } else {
            delete log;  // 上面已经红好几条了，这里只负责别漏内存
        }
    }
    {
        // ---- 文件查看器：同一个窗，内容来自盘上的一份文件 ----
        auto *viewer = new GameLogWindow(QString());
        QPointer<GameLogWindow> guard(viewer);
        viewer->beginFile(QStringLiteral("C:/logs/2026-11-18-1.log.gz"));
        viewer->show();
        QApplication::processEvents();
        check(viewer->isFileViewer() && viewer->windowTitle().contains(QStringLiteral("2026-11-18-1.log.gz")),
              "Z9a 查看器的窗口标题写文件名", viewer->windowTitle());
        const auto *instanceLabel = viewer->findChild<QLabel *>(QStringLiteral("gameLogTitleInstance"));
        check(instanceLabel != nullptr && instanceLabel->isVisible()
                  && instanceLabel->text() == QStringLiteral("2026-11-18-1.log.gz"),
              "Z9b 标题条上也写文件名");
        const auto *badge = viewer->findChild<QLabel *>(QStringLiteral("gameLogStateBadge"));
        check(badge != nullptr && !badge->isVisible(), "Z9c 已结束的文件不显示运行状态");
        check(viewer->externalOpenAction()->isVisible() && !viewer->exportReportAction()->isVisible()
                  && !viewer->clearAction()->isVisible(),
              "Z9d 菜单里有「用外部程序打开」，没有错误报告与清空");
        QString externalPath;
        QObject::connect(viewer, &GameLogWindow::openExternalRequested, viewer,
                         [&externalPath](const QString &path) { externalPath = path; });
        viewer->externalOpenAction()->trigger();
        check(externalPath == QStringLiteral("C:/logs/2026-11-18-1.log.gz"), "Z9e 「用外部程序打开」带出文件路径");

        // 30000 行（超过模型默认的 20000）：默认只显示末尾，模型留着全部，切到「全部」分批摆完
        QStringList lines;
        for (int i = 0; i < 30000; ++i) lines << QStringLiteral("[12:00:00] [main/INFO]: line %1").arg(i);
        viewer->setFileLines(lines, 0);
        QApplication::processEvents();
        check(viewer->lineCount() == 30000 && viewer->droppedLineCount() == 0,
              "Z9f 超过 20000 行也整份留在模型里，没有被挤掉", QString::number(viewer->lineCount()));
        check(viewer->displayLineLimit() == GameLogWindow::fileTailLines(), "Z9g 超长日志默认只显示末尾");
        check(viewer->getLogView()->toPlainText().trimmed().endsWith(QStringLiteral("line 29999"))
                  && !viewer->getLogView()->toPlainText().contains(QStringLiteral("line 24999\n")),
              "Z9h 默认视图是末尾 5000 行");
        viewer->setDisplayLineLimit(0);
        check(viewer->fileBatchPending() || viewer->getLogView()->blockCount() > 1,
              "Z9i 切到「全部」后开始上屏");
        QElapsedTimer waited;
        waited.start();
        while (viewer->fileBatchPending() && waited.elapsed() < 20000) QApplication::processEvents(QEventLoop::AllEvents, 20);
        check(!viewer->fileBatchPending() && viewer->getLogView()->blockCount() == 30001,
              "Z9j 「全部」分批摆完：30000 行 + 光标行", QString::number(viewer->getLogView()->blockCount()));
        check(viewer->getLogView()->toPlainText().startsWith(QStringLiteral("[12:00:00] [main/INFO]: line 0\n")),
              "Z9k 全部显示时从第一行开始");
        viewer->setLevelVisible(GameLogWindow::LogLevel::Info, false);
        check(viewer->getLogView()->blockCount() <= 1, "Z9l 关掉 INFO 后视图里没有 INFO 行");
        viewer->setLevelVisible(GameLogWindow::LogLevel::Info, true);
        check(viewer->fileBatchPending(), "Z9m 筛选触发重画时同样分批（不一次摆几万行）");
        while (viewer->fileBatchPending() && waited.elapsed() < 40000) QApplication::processEvents(QEventLoop::AllEvents, 20);
        check(viewer->getLogView()->blockCount() == 30001, "Z9n 重画摆完后内容完整");

        // 不超长的文件：默认就是全部
        viewer->setFileLines(lines.mid(0, 300), 0);
        check(viewer->displayLineLimit() == 0 && viewer->lineCount() == 300, "Z9o 短日志默认显示全部");
        viewer->setFileLines(lines.mid(0, 10), 1024);
        check(viewer->getHintLabel()->text().contains(QStringLiteral("只读取了末尾")), "Z9p 文件被截过头时说明里写明");
        viewer->setFileProblem(QStringLiteral("解不开"));
        check(viewer->lineCount() == 0 && viewer->getLogView()->placeholderText() == QStringLiteral("解不开"),
              "Z9q 读失败时正文写原因");
        viewer->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(guard.isNull(), "Z9r 关掉查看器即销毁");
    }
    return 0;
}


