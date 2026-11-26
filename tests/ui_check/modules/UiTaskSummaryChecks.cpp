#include "Support.hpp"
int UiTaskSummaryChecks(MainWindowUI &ui) {
    // 任务卡在导航轨底部；窄于 1024 的窗口里轨折成图标轨、摘要文字收起。量展开那一档。
    QWidget *const host = ui.getTaskSummaryButton() != nullptr ? ui.getTaskSummaryButton()->window() : nullptr;
    const QSize hostSize = host != nullptr ? host->size() : QSize();
    if (host != nullptr) host->resize(1280, 800);
    QApplication::processEvents();

    {
        // 这一节量的是中间那一格：它只说**一句话**，说哪一句由账本那边的
        // `Tasks::collapsedSummary()` 定 —— 那是 src/ 里的纯规则，归 verify 的 AL/AM
        // 两节；界面这一头只剩两件事可量：**把给它的那份摘要摆出来**、
        // **没有任务时摆那句占位话**，外加"那句话怎么拼"的真值表。
        using Summary = MainWindowUI::BottomBarTaskSummary;
        QLabel *status = ui.getStatusLabel();
        QLabel *extra = ui.getTaskExtraLabel();
        QProgressBar *progress = ui.getProgressBar();
        // 下面那些断言照旧按基类量（`value()/range()/isVisible()` 一个都没变），
        // 这两样只用来钉"换没换成自绘的那一支 / 那个数画在哪一格上"。
        auto *progressSelfDrawn = qobject_cast<TaskProgressBar *>(progress);
        QLabel *percent = ui.getTaskPercentLabel();
        QPushButton *cell = ui.getTaskSummaryButton();
        check(status != nullptr && extra != nullptr && progress != nullptr && cell != nullptr,
              "E4 中间那格的几块控件都在（那句话 / 「另有 N 项」 / 进度条 / 整格那颗按钮）");
        check(percent != nullptr,
              "E4 底下那一行右边那一格百分比在（那根条自己不再画数了 —— 画上去的话它会顶着"
              "「另有 N 项」的位置，而且缩起来的时候比条还长）");
        // 这一处必须是**自绘的那一支**：按 objectName 找得到基类不代表换过 ——
        // 原生的 `QProgressBar` 也找得到，画出来是 Fusion 那个方块头 + 默认那圈描边。
        check(progressSelfDrawn != nullptr,
              "★★ E4 底栏那一格用的是**自绘的进度条**（TaskProgressBar），不是原生 QProgressBar");
        if (progressSelfDrawn != nullptr) {
            // 设计稿轨底任务卡：28 见方的进度环（面板行 / 详情页仍是细条）。
            check(progressSelfDrawn->isRing() && progressSelfDrawn->size() == QSize(28, 28),
                  "★ E4 任务卡上那一格是 28×28 的进度环",
                  QStringLiteral("%1x%2 环=%3").arg(progressSelfDrawn->width())
                      .arg(progressSelfDrawn->height()).arg(progressSelfDrawn->isRing()));
        }
        if (status != nullptr && extra != nullptr && progress != nullptr && cell != nullptr) {
            // ---- ① 那句话怎么拼 ----
            // 「收尾 = 过去式」：一件刚做完的事在收起态里还顶着现在进行时的样子，看着像
            // "它还在跑"。失败比完成优先级高（两个都真时写「失败」）。
            struct Case {
                const char *title;
                TaskCenter::State state;
                const char *want;
            };
            const Case cases[] = {
                {"正在下载游戏文件", TaskCenter::State::Running, "正在下载游戏文件"},
                {"正在下载游戏文件", TaskCenter::State::Succeeded, "正在下载游戏文件（已完成）"},
                {"正在下载游戏文件", TaskCenter::State::Failed, "正在下载游戏文件（失败）"},
                {"正在下载游戏文件", TaskCenter::State::Cancelled, "正在下载游戏文件（已取消）"},
                {"正在下载游戏文件", TaskCenter::State::Interrupted, "正在下载游戏文件（已中断）"},
            };
            int wrong = 0;
            for (const Case &c : cases) {
                const QString got =
                    MainWindowUI::taskSummaryText(QString::fromUtf8(c.title), c.state);
                if (got != QString::fromUtf8(c.want)) {
                    ++wrong;
                    std::printf("        [判错] %s → 「%s」，该是「%s」\n", c.title,
                                got.toUtf8().constData(), c.want);
                }
            }
            check(wrong == 0,
                  "E4 那一行字：运行/成功/失败/取消/中断各自有准确文案",
                  QStringLiteral("错 %1 处").arg(wrong));
            check(MainWindowUI::taskSummaryDoneSuffix() != MainWindowUI::taskSummaryFailedSuffix(),
                  "E4 完成和失败那两个后缀**不是同一句**（不然「做完了」和「没做成」分不开）",
                  MainWindowUI::taskSummaryDoneSuffix() + QStringLiteral(" / ") +
                      MainWindowUI::taskSummaryFailedSuffix());
            check(!MainWindowUI::taskSummaryNoTaskText().isEmpty() &&
                      !MainWindowUI::taskSummaryTip().isEmpty(),
                  "E4 没任务时那句占位话、以及悬停提示都不是空的",
                  MainWindowUI::taskSummaryNoTaskText() + QStringLiteral(" / ") +
                      MainWindowUI::taskSummaryTip());

            // ---- ② 一项都没有：占位话 + **不显示空进度条** ----
            ui.setBottomBarTaskSummary(Summary());
            QApplication::processEvents();
            check(status->text() == MainWindowUI::taskSummaryNoTaskText(),
                  "E4 一项任务都没有：摆那句低强调度的占位话", status->text());
            check(!progress->isVisible(), "E4 一项任务都没有：那根进度条**不出现**（不留一根空的）");
            check(!extra->isVisible(), "E4 一项任务都没有：右边那句「另有 N 项」也不占地方");
            // 要求是"不显示空进度条" —— 那一行右边那个数**跟着一起收**，不是把它
            // 清成空串还占着地方（占着地方 = 那一行的高度还在，底下那根条却没了）。
            check(percent != nullptr && !percent->isVisible() && percent->text().isEmpty(),
                  "★ E4 一项任务都没有：右边那格百分比也收起来（整行都不留）",
                  percent != nullptr ? percent->text() : QStringLiteral("<没有那一格>"));

            // ---- ③ 有一件事在跑：标题原样 + 进度条是**确定态** ----
            Summary running;
            running.hasTask = true;
            running.title = QStringLiteral("正在下载 26.2");
            running.percent = 42;
            ui.setBottomBarTaskSummary(running);
            QApplication::processEvents();
            check(status->text() == QStringLiteral("正在下载 26.2") && progress->isVisible(),
                  "E4 有任务在跑：标题原样摆出来，进度条也出现", status->text());
            check(progress->minimum() == 0 && progress->maximum() == 100,
                  "E4 有百分比时进度条是**确定态**（0..100）",
                  QStringLiteral("%1..%2").arg(progress->minimum()).arg(progress->maximum()));
            // 那个数走**唯一出处** `taskCenterPercentText()`（面板那一行也是它）——
            // 两边各拼一遍的话，同一件事在这两处会写出不一样的写法来。
            check(percent != nullptr && percent->isVisible() &&
                      percent->text() == MainWindowUI::taskCenterPercentText(42),
                  "★ E4 有百分比时右边那格写着那个数（和面板那一行同一个出处）",
                  percent != nullptr ? percent->text() : QStringLiteral("<没有那一格>"));

            // ---- ④ 测不出百分比：不确定态（绝不编一个数出来） ----
            Summary unknown;
            unknown.hasTask = true;
            unknown.title = QStringLiteral("正在换登录令牌");
            unknown.percent = -1;
            ui.setBottomBarTaskSummary(unknown);
            QApplication::processEvents();
            check(progress->minimum() == 0 && progress->maximum() == 0 && progress->isVisible(),
                  "E4 测不出百分比时进度条走**不确定态**（range 0..0），不是编一个 0%",
                  QStringLiteral("%1..%2").arg(progress->minimum()).arg(progress->maximum()));
            // 测不出总量时**一个数都不许写**：编一个"0%"比不写更坏。
            check(percent != nullptr && !percent->isVisible() && percent->text().isEmpty(),
                  "★ E4 测不出百分比时右边那格**空着**（绝不编一个 0%）",
                  percent != nullptr ? percent->text() : QStringLiteral("<没有那一格>"));

            // ---- ⑤ 「另有 N 项」 ----
            Summary many = running;
            many.extraCount = 2;
            ui.setBottomBarTaskSummary(many);
            QApplication::processEvents();
            check(extra->isVisible() && extra->text() == MainWindowUI::taskSummaryMoreText(2),
                  "E4 除了正显示的这项还有别的在跑时，右边写「另有 2 项」",
                  extra->text());

            // ---- ⑥ 进度环 / 摘要按钮悬停提示（展开 & 导航轨折叠） ----
            Summary detailed;
            detailed.hasTask = true;
            detailed.title = QStringLiteral("正在准备 Java 运行时...");
            detailed.taskTitle = QStringLiteral("启动 1.20.1");
            detailed.currentStep = QStringLiteral("第 3 / 5 步 · 准备 Java 运行环境");
            detailed.percent = 42;
            ui.setBottomBarTaskSummary(detailed);
            QApplication::processEvents();
            const QString tip = progress->toolTip();
            check(tip == cell->toolTip()
                      && tip.contains(QStringLiteral("启动 1.20.1"))
                      && tip.contains(QStringLiteral("第 3 / 5 步 · 准备 Java 运行环境"))
                      && tip.contains(QStringLiteral("42%")),
                  "★ E4 进度环与摘要按钮 tooltip 包含任务名、当前步骤和百分比",
                  tip);

            // 不可数时不编造 0% 放进 tooltip
            Summary indet = detailed;
            indet.percent = -1;
            ui.setBottomBarTaskSummary(indet);
            QApplication::processEvents();
            check(!progress->toolTip().contains(QLatin1Char('%'))
                      && progress->toolTip().contains(QStringLiteral("启动 1.20.1")),
                  "E4 不可数任务 tooltip 不编造 0%", progress->toolTip());

            // 导航轨折叠（窄窗 < 1024）时进度环依然保留完整 tooltip
            if (host != nullptr) {
                host->resize(960, 680);
                QApplication::processEvents();
            }
            ui.setBottomBarTaskSummary(detailed);
            QApplication::processEvents();
            check(progress->isVisible() && progress->toolTip() == tip && cell->toolTip() == tip,
                  "★★ E4 导航轨折叠后进度环仍保留完整悬停提示", progress->toolTip());

            // #131：鼠标点开任务中心时按钮拿到焦点，焦点可见性那一路（Theme.cpp 的
            // ControlFocusVisibility）就地 unpolish + polish 它，换主题的 repolishWidgets() 也是；
            // 样式表里的 min-height 随之把最小高冲回 0。任务卡若不跟着重算，就被压回一行字高，
            // 环的下半截被按钮边裁掉。最小高被冲成 0 之后，轨底布局就按 sizeHint / minimumSizeHint
            // 给高度，所以这两个提示本身就得装得下整枚环。
            {
                Summary unread = detailed;
                unread.unreadFailure = true;
                ui.setBottomBarTaskSummary(unread);
                QApplication::processEvents();
                cell->style()->unpolish(cell);
                cell->style()->polish(cell);
                for (int i = 0; i < 4; ++i) QApplication::processEvents();
                const QRect ring = progress->geometry();
                const int need = ring.height() + 16;  // 环 + 上下各 8 的边距
                check(progress->isVisible() && cell->rect().contains(ring) && cell->sizeHint().height() >= need &&
                          cell->minimumSizeHint().height() >= need,
                      "★★ #131 任务卡按钮的尺寸提示装得下整枚进度环（polish 冲掉最小高后不被压成半截）",
                      QStringLiteral("卡 %1 高，sizeHint %2，minimumSizeHint %3，环 y=%4..%5")
                          .arg(cell->height()).arg(cell->sizeHint().height()).arg(cell->minimumSizeHint().height())
                          .arg(ring.top()).arg(ring.bottom()));
            }
            if (host != nullptr) {
                host->resize(1280, 800);
                QApplication::processEvents();
            }

            // 收尾：还回开局那一份（后面的节还可能量这一格）
            ui.setBottomBarTaskSummary(Summary());
            QApplication::processEvents();
            check(cell->toolTip() == MainWindowUI::taskSummaryTip(),
                  "E4 无任务时 tooltip 回到默认提示", cell->toolTip());
        }
    }
    if (host != nullptr) host->resize(hostSize);
    QApplication::processEvents();
 return 0;
}


