#include "Support.hpp"

#include <QTemporaryDir>

void runTaskLedgerTests() {
    section("AL1. percentOf：测不出就回 -1，绝不编一个百分比");
    {
        const auto make = [](qint64 done, qint64 total) {
            Tasks::TaskSnapshot task;
            task.doneUnits = done;
            task.totalUnits = total;
            return task;
        };
        check(Tasks::percentOf(make(0, 0)) == -1, "总数 0 ⇒ 不可测（-1）",
              QString::number(Tasks::percentOf(make(0, 0))));
        check(Tasks::percentOf(make(5, 0)) == -1, "总数 0 但已经下了几个 ⇒ 还是不可测",
              QString::number(Tasks::percentOf(make(5, 0))));
        check(Tasks::percentOf(make(3, -10)) == -1, "负的总数 ⇒ 不可测",
              QString::number(Tasks::percentOf(make(3, -10))));
        check(Tasks::percentOf(make(1, 3)) == 33, "1/3 ⇒ 33（向下取整，不四舍五入）",
              QString::number(Tasks::percentOf(make(1, 3))));
        check(Tasks::percentOf(make(3, 3)) == 100 && Tasks::percentOf(make(400, 1000)) == 40,
              "3/3 ⇒ 100、400/1000 ⇒ 40",
              QStringLiteral("%1 / %2")
                  .arg(Tasks::percentOf(make(3, 3)))
                  .arg(Tasks::percentOf(make(400, 1000))));
        check(Tasks::percentOf(make(9999, 1000)) == 100, "完成数比总数还大 ⇒ 钳到 100",
              QString::number(Tasks::percentOf(make(9999, 1000))));
        check(Tasks::percentOf(make(-5, 100)) == 0, "负的完成数 ⇒ 钳到 0",
              QString::number(Tasks::percentOf(make(-5, 100))));
    }

    section("AL2. summaryPriority：收起态那一格的先后（那一串逐条对上）");
    {
        const auto p = [](Tasks::TaskKind kind, Tasks::TaskState state, bool unread = false) {
            Tasks::TaskSnapshot task;
            task.kind = kind;
            task.state = state;
            return Tasks::summaryPriority(task, unread);
        };
        using K = Tasks::TaskKind;
        using S = Tasks::TaskState;
        check(p(K::Download, S::WaitingUser) == 0, "卡在等用户 ⇒ 第一档（0）",
              QString::number(p(K::Download, S::WaitingUser)));
        check(p(K::Download, S::Failed, /*unread=*/true) == 1, "**没看过**的失败 ⇒ 第二档（1）",
              QString::number(p(K::Download, S::Failed, true)));
        check(p(K::Launch, S::Running) == 2, "正在启动 ⇒ 2",
              QString::number(p(K::Launch, S::Running)));
        check(p(K::Install, S::Running) == 3 && p(K::Import, S::Running) == 3,
              "安装 / 导入整合包同一档（3）",
              QStringLiteral("%1 / %2")
                  .arg(p(K::Install, S::Running))
                  .arg(p(K::Import, S::Running)));
        check(p(K::Download, S::Running) == 4, "下载 ⇒ 4",
              QString::number(p(K::Download, S::Running)));
        check(p(K::Export, S::Running) == 5, "导出 ⇒ 5",
              QString::number(p(K::Export, S::Running)));
        check(p(K::Java, S::Running) == 6 && p(K::Integrity, S::Running) == 6 &&
                  p(K::Other, S::Running) == 6,
              "Java / 体检 / 其它同一档（6）");
        // 看过的失败退到最后一档：它还在（在"刚完成"那个窗口里），只是不再抢那一格。
        check(p(K::Download, S::Failed, /*unread=*/false) == 8,
              "**看过**的失败退到最后一档（8）", QString::number(p(K::Download, S::Failed)));
        check(p(K::Launch, S::Succeeded) == 8, "成功那档也是 8（只靠时间窗决定露不露）",
              QString::number(p(K::Launch, S::Succeeded)));
        // WaitingUser 优先于 kind：同一个 Download，卡住等用户就排到最前面。
        check(p(K::Other, S::WaitingUser) < p(K::Launch, S::Running),
              "「等用户」比「正在启动」还靠前（不管它是哪一类任务）");
    }

    section("AL3. collapsedSummary：收起态显示谁（唯一出处）");
    {
        const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-12 12:00:00"),
                                                    QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        const qint64 windowMs = 8000;
        const auto at = [&now](int secondsBefore) { return now.addSecs(-secondsBefore); };

        // 空账本 ⇒ 一条都没有，红点也不该有。
        {
            const Tasks::TaskSummary s = Tasks::collapsedSummary({}, {}, now, windowMs);
            check(s.id == Tasks::invalidTaskId() && !s.hasFailure && s.extraCount == 0,
                  "AL3 一项都没有 ⇒ 无效号、没有红点、「另有 0 项」那句话也不该有",
                  QString::number(s.id));
        }

        // 三件活动任务：Download(4) / Launch(2) / Install(3) ⇒ 显示启动那件，另有 2 项。
        {
            QList<Tasks::TaskSnapshot> tasks;
            Tasks::TaskSnapshot dl;
            dl.id = 1;
            dl.kind = Tasks::TaskKind::Download;
            dl.state = Tasks::TaskState::Running;
            dl.title = QStringLiteral("下载 A");
            dl.startedAt = at(30);
            dl.doneUnits = 400;
            dl.totalUnits = 1000;
            Tasks::TaskSnapshot launch;
            launch.id = 2;
            launch.kind = Tasks::TaskKind::Launch;
            launch.state = Tasks::TaskState::Running;
            launch.title = QStringLiteral("启动 B");
            launch.startedAt = at(10);
            Tasks::TaskSnapshot install;
            install.id = 3;
            install.kind = Tasks::TaskKind::Install;
            install.state = Tasks::TaskState::Running;
            install.title = QStringLiteral("安装 C");
            install.startedAt = at(20);
            tasks << dl << launch << install;

            const Tasks::TaskSummary s = Tasks::collapsedSummary(tasks, {}, now, windowMs);
            check(s.id == 2 && s.title == QStringLiteral("启动 B"),
                  "AL3 三件一起跑 ⇒ 显示优先级最高的那件（启动）", s.title);
            check(s.extraCount == 2, "AL3 除了显示的那件还有 2 项在跑",
                  QStringLiteral("另有 %1 项").arg(s.extraCount));
            check(s.percent == -1, "AL3 启动那件没报过总数 ⇒ 百分比不可测",
                  QString::number(s.percent));

            // 同档取**后发起**的：两件 Download，后起的那件该被显示。
            QList<Tasks::TaskSnapshot> two = {dl};
            Tasks::TaskSnapshot dl2 = dl;
            dl2.id = 9;
            dl2.startedAt = at(1);
            dl2.title = QStringLiteral("下载 D");
            two << dl2;
            check(Tasks::collapsedSummary(two, {}, now, windowMs).id == 9,
                  "AL3 同一档里取**后发起**的那件（用户刚点的那件事）",
                  QString::number(Tasks::collapsedSummary(two, {}, now, windowMs).id));

            // 卡在等用户那件排第一 —— 哪怕它是档位最低的那一类（Download）。
            // NOTE: 改的是 `tasks` 里那一份（`dl` 早在上面就被按值拷进去了，改它没用）。
            tasks[0].state = Tasks::TaskState::WaitingUser;
            const Tasks::TaskSummary s3 = Tasks::collapsedSummary(tasks, {}, now, windowMs);
            check(s3.id == 1, "AL3 「等用户」那件压过启动 / 安装 / 下载",
                  QString::number(s3.id));
        }

        // 未读失败的红点：和**显示的是哪一件**无关（正在下载而上一件失败了，两个信号都在）。
        {
            QList<Tasks::TaskSnapshot> tasks;
            Tasks::TaskSnapshot running;
            running.id = 1;
            running.kind = Tasks::TaskKind::Download;
            running.state = Tasks::TaskState::Running;
            running.startedAt = at(5);
            Tasks::TaskSnapshot failed;
            failed.id = 2;
            failed.kind = Tasks::TaskKind::Install;
            failed.state = Tasks::TaskState::Failed;
            failed.finishedAt = at(1);
            tasks << running << failed;

            const QSet<Tasks::TaskId> unread{2};
            const Tasks::TaskSummary s = Tasks::collapsedSummary(tasks, unread, now, windowMs);
            check(s.hasFailure, "AL3 有未读的失败 ⇒ 红点亮着");
            check(s.id == 2, "AL3 未读的失败比正在跑的下载更该被看见（它排在下载前面）",
                  QString::number(s.id));
            const Tasks::TaskSummary read = Tasks::collapsedSummary(tasks, {}, now, windowMs);
            check(!read.hasFailure && read.id == 1,
                  "AL3 看过了之后：红点灭掉、这一格回到那件正在跑的下载",
                  QString::number(read.id));
        }

        // 未读的失败**不受**「刚完成」时间窗限制：过了窗口它照样占着这一格（导航收起时角标挂在它的环上），
        // 而截断丢掉的老失败号（账本里已经没有这一行）不再点亮红点 —— 那一颗没有任何出口能撤掉。
        {
            Tasks::TaskSnapshot failed;
            failed.id = 7;
            failed.kind = Tasks::TaskKind::Download;
            failed.state = Tasks::TaskState::Failed;
            failed.finishedAt = at(3600);
            const Tasks::TaskSummary s = Tasks::collapsedSummary({failed}, {7}, now, windowMs);
            check(s.hasFailure && s.id == 7,
                  "★ AL3 失败过了时间窗还没看过：红点照亮、这一格仍是它（不回「暂无后台任务」）",
                  QString::number(s.id));
            const Tasks::TaskSummary orphan = Tasks::collapsedSummary({}, {7}, now, windowMs);
            check(!orphan.hasFailure && orphan.id == Tasks::invalidTaskId(),
                  "★ AL3 未读集合里的号在账本里已经没了（被截断）：不点红点");
        }

        // 「刚完成」只在时间窗里露面，而且**必须有**结束时间戳。
        {
            Tasks::TaskSnapshot done;
            done.id = 5;
            done.kind = Tasks::TaskKind::Download;
            done.state = Tasks::TaskState::Succeeded;
            done.startedAt = at(60);
            const QList<Tasks::TaskSnapshot> tasks{done};

            done.finishedAt = at(1);
            check(Tasks::collapsedSummary({done}, {}, now, windowMs).id == 5,
                  "AL3 刚完成 1 秒 ⇒ 还在窗口里，露着");
            check(Tasks::collapsedSummary({done}, {}, now, 0).id == Tasks::invalidTaskId(),
                  "AL3 时间窗给 0 ⇒ 已完成的一律不显示");
            check(Tasks::collapsedSummary({done}, {}, now, 500).id == Tasks::invalidTaskId(),
                  "AL3 已经过去 1000 ms、窗口只有 500 ms ⇒ 收进历史（不显示了）");

            done.finishedAt = QDateTime();  // 无效时间戳（例：读盘回来那件「已中断」的）
            check(Tasks::collapsedSummary({done}, {}, now, windowMs).id == Tasks::invalidTaskId(),
                  "AL3 没有结束时间戳 ⇒ **不**当成「刚完成」（那种「不知道什么时候完的」不占这一格）");
            done.finishedAt = now.addSecs(30);  // 未来（时钟回拨那类）
            check(Tasks::collapsedSummary({done}, {}, now, windowMs).id == Tasks::invalidTaskId(),
                  "AL3 结束时间戳在**未来** ⇒ 也不当「刚完成」");
        }

        // 中断那件：状态是终态，但没时间戳 ⇒ 收起态不显示它（它靠「未读」或面板露面）。
        {
            Tasks::TaskSnapshot interrupted;
            interrupted.id = 7;
            interrupted.kind = Tasks::TaskKind::Install;
            interrupted.state = Tasks::TaskState::Interrupted;
            check(Tasks::collapsedSummary({interrupted}, {}, now, windowMs).id ==
                      Tasks::invalidTaskId(),
                  "AL3 读回来的「已中断」（没有结束时间戳）不占收起态那一格");
        }
    }

    section("AL4. retryOffered：那颗「重试」露不露（逐任务能力表的重试那一半）");
    // 三样缺一不可（`capabilities.canRetry` ∧ 已收尾 ∧ **那一格失败清单还是它的、还在**）。
    // 最要紧的是最后那两样：账本里的 `canRetry` 是**失败那一刻**钉下的、会跨重启留在盘上，
    // 而真那份失败清单只在内存里、只有一格 —— 少了它俩，上一件失败任务的详情页会露着一颗
    // 按下去重做**别人**那批文件的「重试」。
    {
        const auto make = [](Tasks::TaskId id, bool canRetry, Tasks::TaskState state) {
            Tasks::TaskSnapshot task;
            task.id = id;
            task.kind = Tasks::TaskKind::Download;
            task.capabilities.canRetry = canRetry;
            task.state = state;
            return task;
        };
        const Tasks::TaskId owner = 42;
        const Tasks::TaskSnapshot mine = make(owner, true, Tasks::TaskState::Failed);

        check(Tasks::retryOffered(mine, owner, true),
              "★★ AL4 允许重试 ∧ 已失败 ∧ 那一格就是它的、还在 ⇒ 露");

        // ① 号不对上：是新一批失败把那一格**盖掉**了（重做会是别人的文件）。
        check(!Tasks::retryOffered(mine, 43, true),
              "★★ AL4 那一格已经**换人**了（新一批失败盖掉了它）⇒ 上一件的「重试」必须收起来"
              "（点了会去重做后一批的文件 —— 按了做错事比没按钮更糟）");
        // ② 那一格没了：启动器重启过（账本那位 canRetry 还在，内存里那份清单没了）。
        check(!Tasks::retryOffered(mine, owner, false),
              "★★ AL4 账本里那一位还在、可那一格失败清单**没了**（重启过）⇒ 不许露"
              "（点了什么都不会发生）");
        // ③ 账本没给这个能力位。
        check(!Tasks::retryOffered(make(owner, false, Tasks::TaskState::Failed), owner, true),
              "★★ AL4 这一类活账本没给 `canRetry` ⇒ 不许露（界面不按种类自己猜）");
        // ④ 还在跑：正在下的那一项露「重试」没有含义（AB2 在真界面上也量了这一条）。
        for (const Tasks::TaskState active : {Tasks::TaskState::Queued, Tasks::TaskState::Running,
                                              Tasks::TaskState::WaitingUser,
                                              Tasks::TaskState::Paused}) {
            check(!Tasks::retryOffered(make(owner, true, active), owner, true),
                  QStringLiteral("★★ AL4 八档状态里那一档还在跑（%1）⇒ 不露").arg(int(active)));
        }
        // ⑤ 四档终态**都**允许露（能不能重试是"这一类活"的事，不是"怎么收的场"）。
        for (const Tasks::TaskState done : {Tasks::TaskState::Succeeded, Tasks::TaskState::Failed,
                                            Tasks::TaskState::Cancelled,
                                            Tasks::TaskState::Interrupted}) {
            check(Tasks::retryOffered(make(owner, true, done), owner, true),
                  QStringLiteral("★ AL4 终态（%1）都按账本那一位说话").arg(int(done)));
        }
        // ⑥ 无效号不许"误配"：没有清单时 owner 是无效号，而 `TaskManager::snapshot()` 找不到
        // 人时回的是**默认快照**（id 也是那个无效号）—— 那一条不许被判成"对得上"。
        {
            Tasks::TaskSnapshot missing;      // id = invalidTaskId()
            missing.capabilities.canRetry = true;
            missing.state = Tasks::TaskState::Failed;
            check(!Tasks::retryOffered(missing, Tasks::invalidTaskId(), false),
                  "★★ AL4 「找不到这一项」时回的默认快照 + 无效号 ⇒ 仍然不露（靠 held 那一道挡）");
        }
    }

    section("AM1. TaskManager：终态只落一次（「重复信号不生成重复完成」）");
    {
        QTemporaryDir dir;
        check(dir.isValid(), "AM1 建得出一个临时目录来放历史文件");
        if (!dir.isValid()) {
            return;
        }
        Tasks::TaskManager manager;
        manager.setHistoryPath(dir.filePath(QStringLiteral("wtfml.tasks")));

        int finishedSignals = 0;
        int changedSignals = 0;
        int historySignals = 0;
        QObject::connect(&manager, &Tasks::TaskManager::taskFinished,
                         [&finishedSignals](Tasks::TaskId, bool) { ++finishedSignals; });
        QObject::connect(&manager, &Tasks::TaskManager::taskChanged,
                         [&changedSignals](Tasks::TaskId) { ++changedSignals; });
        QObject::connect(&manager, &Tasks::TaskManager::historyChanged,
                         [&historySignals]() { ++historySignals; });

        const Tasks::TaskId a = manager.begin(Tasks::TaskKind::Download, QStringLiteral("下载 A"));
        check(a != Tasks::invalidTaskId() && manager.has(a), "AM1 发出来的号是有效的",
              QString::number(a));
        check(changedSignals == 1, "AM1 新建那一项也发了一次 taskChanged",
              QString::number(changedSignals));

        check(manager.succeed(a), "AM1 第一次收尾成功");
        check(!manager.succeed(a), "AM1 **第二次**收尾回 false");
        check(finishedSignals == 1, "AM1 taskFinished 只发了一次（不是两发）",
              QString::number(finishedSignals));
        check(historySignals == 1, "AM1 historyChanged 也只发了一次",
              QString::number(historySignals));
        check(manager.snapshot(a).state == Tasks::TaskState::Succeeded,
              "AM1 状态没被第二发改写");
        // 失败也不能把一次已经收好的尾改写成失败。
        check(!manager.fail(a, QStringLiteral("晚了")), "AM1 收过尾之后再 fail 也是 false");
        check(manager.snapshot(a).state == Tasks::TaskState::Succeeded &&
                  manager.snapshot(a).errorSummary.isEmpty(),
              "AM1 最后一步：状态还是成功、没被塞进一句错因");

        // 终态是不可变快照：执行器排队中的迟到信号不能把已完成行改回去。
        Tasks::TaskCapabilities lateCapabilities;
        lateCapabilities.canCancel = true;
        check(!manager.setDetail(a, QStringLiteral("迟到的详情"))
                  && !manager.setProgress(a, 9, 10)
                  && !manager.setBytes(a, 90, 100)
                  && !manager.setStepState(a, 0, Tasks::TaskStepState::Failed)
                  && !manager.setCapabilities(a, lateCapabilities),
              "AM1 收尾后所有进度类更新都被拒绝（终态不可变）");
        check(manager.snapshot(a).detail.isEmpty() && !manager.snapshot(a).capabilities.canCancel,
              "AM1 迟到信号没有改动已完成快照");

        // 收尾那一下会把完成数补齐（"完成了"就该是 100%）。
        const Tasks::TaskId b = manager.begin(Tasks::TaskKind::Download, QStringLiteral("下载 B"));
        manager.setProgress(b, 3, 10);
        manager.succeed(b);
        check(manager.snapshot(b).doneUnits == 10, "AM1 成功收尾时完成数补齐到总数",
              QString::number(manager.snapshot(b).doneUnits));

        {
            const Tasks::TaskId c =
                manager.begin(Tasks::TaskKind::Download, QStringLiteral("下载 C"));
            Tasks::TaskCapabilities caps = manager.snapshot(c).capabilities;
            caps.canRetry = true;
            check(manager.setCapabilities(c, caps), "AM1 活动态可以钉下 canRetry");
            check(manager.fail(c, QStringLiteral("两发失败")), "AM1 失败收尾");
            check(manager.snapshot(c).capabilities.canRetry,
                  "★★ AM1 fail() 不许抹掉收尾前钉下的 canRetry（否则那颗「重试」永远不露）");
            check(!manager.snapshot(c).capabilities.canCancel,
                  "AM1 收尾同时把「取消」收掉（它只在活动态有意义）");
            check(Tasks::retryOffered(manager.snapshot(c), c, true),
                  "★★ AM1 收尾之后 `retryOffered()` 真能靠这一位说\"露\"");
        }
        {
            Tasks::TaskCapabilities crashCaps;
            crashCaps.canExportReport = true;
            crashCaps.canOpenCrashDir = true;
            crashCaps.canOpenLog = true;
            const Tasks::TaskId crash = manager.begin(
                Tasks::TaskKind::Crash, QStringLiteral("游戏异常退出"),
                QString(), {}, crashCaps);
            check(crash != Tasks::invalidTaskId(), "AM1 崩溃项建出来了");
            manager.fail(crash, QStringLiteral("退出码 1"));
            const Tasks::TaskCapabilities after = manager.snapshot(crash).capabilities;
            check(after.canExportReport && after.canOpenCrashDir && after.canOpenLog,
                  "★★ AM1 崩溃项收尾之后「导出错误报告 / 打开崩溃报告目录 / 打开日志」都还在");
        }
    }

    section("AM2. TaskManager：一模一样的一帧不用再喊一遍 / 无效号什么都不做");
    {
        QTemporaryDir dir;
        if (!dir.isValid()) {
            check(false, "AM2 建临时目录");
            return;
        }
        Tasks::TaskManager manager;
        manager.setHistoryPath(dir.filePath(QStringLiteral("wtfml.tasks")));
        const Tasks::TaskId a = manager.begin(Tasks::TaskKind::Download, QStringLiteral("下载"));

        check(manager.setProgress(a, 400, 1000), "AM2 第一次记进度 ⇒ true");
        check(!manager.setProgress(a, 400, 1000), "AM2 一模一样的一帧 ⇒ false（不白喊一遍）");
        check(manager.setProgress(a, 401, 1000), "AM2 数变了 ⇒ 又回 true");
        check(!manager.setBytes(a, 0, 0), "AM2 字节那一对也是同一套（0/0 是第一帧，写不进去）",
              "初始就是 0/0，所以第一次 setBytes(0,0) 也该是 false");
        check(manager.setBytes(a, 1024, 2048), "AM2 字节第一次有值 ⇒ true");

        // 越界的步骤下标 / 无效号：都不许崩、都回 false。
        check(!manager.setStepState(a, 0, Tasks::TaskStepState::Done),
              "AM2 没有那一步（steps 是空的）⇒ false，不崩");
        check(!manager.setDetail(Tasks::invalidTaskId(), QStringLiteral("x")),
              "AM2 无效号 ⇒ false");
        check(!manager.succeed(Tasks::invalidTaskId()), "AM2 无效号收尾 ⇒ false");
        check(!manager.cancel(999999), "AM2 压根不存在的号 ⇒ false");

        check(!manager.setSessionId(a, 0), "AM2 会话号 0 ⇒ false");
        check(!manager.setSessionId(Tasks::invalidTaskId(), 7), "AM2 无效任务号 ⇒ false");
        check(manager.setSessionId(a, 7), "AM2 第一次对上 ⇒ true");
        check(manager.snapshot(a).sessionId == 7, "AM2 快照里读得出来那个号");
        check(!manager.setSessionId(a, 7), "AM2 同一个号再来一次 ⇒ false（不白喊）");
        check(!manager.setSessionId(a, 8) && manager.snapshot(a).sessionId == 7,
              "AM2 已经对上过的不许改口（一项任务只属于一份会话）");
    }

    section("AM3. 落盘 / 读回：盘上只有摘要，号从盘上续，没跑完的算已中断");
    {
        QTemporaryDir dir;
        if (!dir.isValid()) {
            check(false, "AM3 建临时目录");
            return;
        }
        const QString path = dir.filePath(QStringLiteral("wtfml.tasks"));
        {
            Tasks::TaskManager manager;
            manager.setHistoryPath(path);
            const Tasks::TaskId one = manager.begin(Tasks::TaskKind::Install, QStringLiteral("安装 X"),
                                                    QStringLiteral("inst-1"));
            manager.setProgress(one, 5, 10);
            manager.setDetail(one, QStringLiteral("正在下载资源对象…"));
            manager.succeed(one);
            const Tasks::TaskId two = manager.begin(Tasks::TaskKind::Download, QStringLiteral("下载 Y"));
            manager.cancel(two);
        }
        check(QFile::exists(path), "AM3 历史文件落到盘上了", path);

        const QList<Tasks::TaskSnapshot> again = Tasks::load(path);
        check(again.size() == 2, "AM3 读回来两条", QString::number(again.size()));
        const auto byId = [&again](Tasks::TaskId id) {
            for (const Tasks::TaskSnapshot &t : again) {
                if (t.id == id) {
                    return t;
                }
            }
            return Tasks::TaskSnapshot();
        };
        check(byId(1).title == QStringLiteral("安装 X") &&
                  byId(1).kind == Tasks::TaskKind::Install &&
                  byId(1).state == Tasks::TaskState::Succeeded,
              "AM3 标题 / 种类 / 状态都原样读回来");
        check(byId(1).targetId == QStringLiteral("inst-1"), "AM3 目标 id 也在",
              byId(1).targetId);
        check(byId(2).state == Tasks::TaskState::Cancelled, "AM3 取消那件也读得回来");
        // 摘要**只**存那几样：详情 / 进度 / 能力位都不在上面（见 TaskHistory.hpp 的"不存"那段）。
        check(byId(1).detail.isEmpty() && byId(1).totalUnits == 0 && byId(1).steps.isEmpty() &&
                  !byId(1).capabilities.canCancel,
              "AM3 盘上只有摘要：详情 / 进度 / 步骤 / 能力位都不往上写",
              QStringLiteral("detail=「%1」 total=%2 steps=%3")
                  .arg(byId(1).detail)
                  .arg(byId(1).totalUnits)
                  .arg(byId(1).steps.size()));

        // 号从**盘上最大的那个**续：读回之后新发的号必须比它大（不然会撞上一条旧历史）。
        {
            Tasks::TaskManager manager;
            manager.setHistoryPath(path);
            const int loaded = manager.loadHistory();
            check(loaded == 2, "AM3 loadHistory 回读到 2 条", QString::number(loaded));
            const Tasks::TaskId next = manager.begin(Tasks::TaskKind::Download,
                                                     QStringLiteral("新的"));
            check(next > 2, "AM3 新发的号在盘上那些之后（不撞号）",
                  QStringLiteral("新号 %1（盘上最大 2）").arg(next));
        }

        // AM3b：盘上那份是**普通文本文件**，重号 / 号回绕这两种坏账
        // 都得在 `loadHistory()` 里挡住 ——
        // · 重号：`find()` 只认第一条，后面那条从此收不了尾；面板拿号当行钥匙，
        // 两行共用一个钥匙 ⇒ 点一行的按钮打到另一条身上。
        // · `TaskId` 最大值：`id + 1` 回绕成 0 ⇒ 计数器退回 1 ⇒ 新任务和历史撞号，
        // 而历史那一条是终态 ⇒ 真在跑的那件事**再也收不了尾**。
        const QString badPath = dir.filePath(QStringLiteral("bad.tasks"));
        {
            QFile bad(badPath);
            if (bad.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                bad.write(R"([
                    {"id":"3","kind":"download","state":"succeeded","title":"甲"},
                    {"id":"3","kind":"download","state":"succeeded","title":"甲（重号那一条）"},
                    {"id":"18446744073709551615","kind":"download","state":"succeeded","title":"号到头了"},
                    {"id":"5","kind":"download","state":"succeeded","title":"乙"}
                ])");
                bad.close();
            }
        }
        {
            Tasks::TaskManager manager;
            manager.setHistoryPath(badPath);
            const int loaded = manager.loadHistory();
            check(loaded == 2, "AM3b 重号与最大号那两条被跳过，只留 3 与 5 两条",
                  QString::number(loaded));
            check(manager.snapshot(3).title == QStringLiteral("甲"),
                  "AM3b 重号时留**第一条**（后面那条认不出是谁）",
                  manager.snapshot(3).title);
            check(!manager.has(std::numeric_limits<Tasks::TaskId>::max()),
                  "AM3b 号等于 TaskId 最大值的那一条不进账本");
            const Tasks::TaskId next = manager.begin(Tasks::TaskKind::Download,
                                                     QStringLiteral("新的"));
            check(next == 6, "AM3b 新发的号接在盘上最大值之后，没被回绕成 1",
                  QString::number(next));
            check(manager.succeed(next) && manager.snapshot(next).state == Tasks::TaskState::Succeeded,
                  "AM3b 新那一件收得了尾（撞号的话这里会是 false）");
        }

        // AM3d：AM3b 只挡了"盘上正好写着最大值"那一档，还差半步 ——
        // 盘上最大是 `max - 1` 时，`m_nextId` 算出来就是 `max`，发出去之后 `max + 1`
        // 原地回绕成 0 ⇒ **从那以后每一件新任务都是 `invalidTaskId()`**：任务中心上
        // 一行都不出现、`finish()` 永远收不了尾。号是**发出去的**，得自己确认这一个
        // 没人用；最大值本身也不发（`loadHistory()` 下次会把它当坏账丢掉）。
        const QString wrapPath = dir.filePath(QStringLiteral("wrap.tasks"));
        {
            QFile bad(wrapPath);
            if (bad.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                bad.write(R"([
                    {"id":"1","kind":"download","state":"succeeded","title":"一"},
                    {"id":"18446744073709551614","kind":"download","state":"succeeded","title":"差一号到头"}
                ])");
                bad.close();
            }
        }
        {
            Tasks::TaskManager manager;
            manager.setHistoryPath(wrapPath);
            const int loaded = manager.loadHistory();
            check(loaded == 2, "AM3d 前提：两条都读进来了（最大值减一不算坏账）",
                  QString::number(loaded));
            const Tasks::TaskId first =
                manager.begin(Tasks::TaskKind::Download, QStringLiteral("新的甲"));
            check(first != Tasks::invalidTaskId() && first != 1 &&
                      first != std::numeric_limits<Tasks::TaskId>::max(),
                  "AM3d 号快到头时新号的落点：不是 0（回绕）、不是已被占的 1、也不是最大值"
                  "（下次读盘会被当坏账丢掉）",
                  QString::number(first));
            check(manager.succeed(first), "AM3d 新那件收得了尾",
                  QString::number(first));
            const Tasks::TaskId second =
                manager.begin(Tasks::TaskKind::Download, QStringLiteral("新的乙"));
            check(second != Tasks::invalidTaskId() && second != first && manager.succeed(second),
                  "AM3d 紧跟着下一件也拿得到号、收得了尾（先前那一版这里就是 0）",
                  QString::number(second));
        }

        // AM3c：账本读不动的时候**不许当成"没有历史"默默吞掉**。
        // `loadHistory()` 之后紧跟着的第一次 `saveHistory()` 会把文件整个覆写 ——
        // 用户看见的是"任务中心突然空了"，而盘上那份坏账本连证据都不剩。
        // 现在原文件不动，旁边留一份 `<账本>.corrupt`。
        {
            const QString corruptPath = dir.filePath(QStringLiteral("corrupt.tasks"));
            const QByteArray garbage = "{这不是 JSON，也不是数组";
            {
                QFile bad(corruptPath);
                if (bad.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    bad.write(garbage);
                    bad.close();
                }
            }
            Tasks::TaskManager manager;
            manager.setHistoryPath(corruptPath);
            check(manager.loadHistory() == 0, "AM3c 读不动的账本按空账本处理（照常起得来）");
            const QString evidencePath = corruptPath + QStringLiteral(".corrupt");
            check(QFile::exists(evidencePath), "AM3c 坏账本旁边留了一份 .corrupt", evidencePath);
            const auto readAll = [](const QString &p) {
                QFile f(p);
                return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
            };
            check(readAll(evidencePath) == garbage, "AM3c 留的是**原始字节**（不是重新序列化过的）");
            // 之后那次落盘把账本重写成正常形状了，证据那份**不许跟着被覆盖**。
            manager.begin(Tasks::TaskKind::Download, QStringLiteral("新的"));
            check(readAll(evidencePath) == garbage,
                  "AM3c 账本被重新写盘以后证据那一份原样留着");
        }

        // 「没跑完的读回来算已中断」：另起一个账本写一份**没跑完**的（begin 之后就丢下）。
        const QString halfPath = dir.filePath(QStringLiteral("half.tasks"));
        {
            Tasks::TaskManager writer;
            writer.setHistoryPath(halfPath);
            writer.begin(Tasks::TaskKind::Launch, QStringLiteral("启动 Z"));
            writer.saveHistory();
        }
        const QList<Tasks::TaskSnapshot> halfDone = Tasks::load(halfPath);
        check(halfDone.size() == 1 && halfDone.first().state == Tasks::TaskState::Interrupted,
              "AM3 盘上写着 Running（上次没跑完）⇒ 读回来是 Interrupted",
              QString::number(int(halfDone.isEmpty() ? Tasks::TaskState::Queued
                                                     : halfDone.first().state)));
        check(!halfDone.isEmpty() && !halfDone.first().finishedAt.isValid(),
              "AM3 已中断那一件**没有**结束时间戳（不许编一个「刚刚完成」出来）");
    }

    section("AM4. markInterrupted / clearFinished / 启动器级固定落点");
    {
        // markInterrupted：只动非终态那几件；终态一个字节都不碰。
        QList<Tasks::TaskSnapshot> tasks;
        Tasks::TaskSnapshot running;
        running.id = 1;
        running.state = Tasks::TaskState::Running;
        running.finishedAt = QDateTime::fromString(QStringLiteral("2026-10-12 10:00:00"),
                                                   QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        Tasks::TaskSnapshot waiting;
        waiting.id = 2;
        waiting.state = Tasks::TaskState::WaitingUser;
        Tasks::TaskSnapshot succeeded;
        succeeded.id = 3;
        succeeded.state = Tasks::TaskState::Succeeded;
        succeeded.finishedAt = QDateTime::fromString(QStringLiteral("2026-10-12 09:00:00"),
                                                     QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        tasks << running << waiting << succeeded;
        Tasks::markInterrupted(tasks);
        check(tasks.at(0).state == Tasks::TaskState::Interrupted &&
                  tasks.at(1).state == Tasks::TaskState::Interrupted,
              "AM4 非终态那两件都成了已中断");
        check(!tasks.at(0).finishedAt.isValid(),
              "AM4 原来那个时间戳也被清掉（它说的是「上次开始」，不是「什么时候完的」）");
        check(tasks.at(2).state == Tasks::TaskState::Succeeded &&
                  tasks.at(2).finishedAt.isValid(),
              "AM4 已经收尾的那件一个字节都不动",
              tasks.at(2).finishedAt.toString(Qt::ISODate));

        // clearFinished：终态的走掉、活动状态留下。
        QTemporaryDir dir;
        if (!dir.isValid()) {
            check(false, "AM4 建临时目录");
            return;
        }
        Tasks::TaskManager manager;
        manager.setHistoryPath(dir.filePath(QStringLiteral("wtfml.tasks")));
        const Tasks::TaskId active = manager.begin(Tasks::TaskKind::Download,
                                                   QStringLiteral("还在跑"));
        const Tasks::TaskId done = manager.begin(Tasks::TaskKind::Download,
                                                 QStringLiteral("跑完了"));
        manager.succeed(done);
        check(manager.finished().size() == 1 && manager.active().size() == 1,
              "AM4 清之前：一件已完成、一件在跑");
        int historySignals = 0;
        QObject::connect(&manager, &Tasks::TaskManager::historyChanged,
                         [&historySignals]() { ++historySignals; });
        manager.clearFinished();
        check(manager.finished().isEmpty() && manager.active().size() == 1 &&
                  manager.has(active) && !manager.has(done),
              "AM4 清之后：已完成那件没了、「在跑」那件还在");
        check(historySignals == 1, "AM4 清了一回 ⇒ historyChanged 发一次",
              QString::number(historySignals));
        manager.clearFinished();  // 已经没有已完成的了
        check(historySignals == 1,
              "AM4 再清一次（没有可清的了）⇒ 一个信号都不发、也不白落一次盘",
              QString::number(historySignals));

        // 任务账本属于启动器，不属于当前选中的某一份 .minecraft。
        check(Tasks::defaultHistoryPath() ==
                  AppPaths::defaultMinecraftDir() + QStringLiteral("/wtfml.tasks"),
              "AM4 默认落点 = <启动器自带数据目录>/wtfml.tasks", Tasks::defaultHistoryPath());
        const QString originalRoot = AppPaths::minecraftDir();
        AppPaths::setMinecraftDir(dir.filePath(QStringLiteral("另一份游戏目录")));
        check(Tasks::defaultHistoryPath() ==
                  AppPaths::defaultMinecraftDir() + QStringLiteral("/wtfml.tasks"),
              "AM4 切换当前游戏文件夹不会改变任务账本路径", Tasks::defaultHistoryPath());
        AppPaths::setMinecraftDir(originalRoot == AppPaths::defaultMinecraftDir() ? QString()
                                                                                  : originalRoot);
        Tasks::TaskManager lazy;
        check(lazy.historyPath() == Tasks::defaultHistoryPath(),
              "AM4 没被指过路 ⇒ historyPath() 使用启动器级默认账本", lazy.historyPath());
        const QString other = dir.filePath(QStringLiteral("换个地方.tasks"));
        lazy.setHistoryPath(other);
        check(lazy.historyPath() == other, "AM4 指过之后就按指的来", lazy.historyPath());
        lazy.setHistoryPath(QString());
        check(lazy.historyPath() == Tasks::defaultHistoryPath(),
              "AM4 指回空串 = 回到默认那一份（不是「写到空路径去」）", lazy.historyPath());
    }

    section("AM5. 压力：几百项任务一起推进 / 收尾（大量下载 + 多实例 + 批量改文件）");
    // ---------------------------------------------------------------------
    // 玩家真实形状：一边下几十个文件、一边开几个实例、一边批量启用/禁用模组 ——
    // 账本里同时躺着几百项，每 200 ms 每项来一帧进度，收尾时刻还会挤在同一毫秒里。
    // 这一节钉三件事：① 推进/收尾期间账本自身不串（数量、终态、迟到帧）；
    // ② 「已完成」那一栏的顺序是确定的（同毫秒并列按号）；③ 落盘截断留的是最近的那批。
    {
        QTemporaryDir dir;
        check(dir.isValid(), "AM5 建临时目录");
        if (!dir.isValid()) {
            return;
        }
        const QString path = dir.filePath(QStringLiteral("stress.tasks"));
        Tasks::TaskManager manager;
        manager.setHistoryPath(path);

        int finishedSignals = 0;
        int historySignals = 0;
        QObject::connect(&manager, &Tasks::TaskManager::taskFinished,
                         [&finishedSignals](Tasks::TaskId, bool) { ++finishedSignals; });
        QObject::connect(&manager, &Tasks::TaskManager::historyChanged,
                         [&historySignals]() { ++historySignals; });

        constexpr int kCount = 240;
        constexpr int kFrames = 50;
        static constexpr Tasks::TaskKind kKinds[] = {Tasks::TaskKind::Download,
                                                     Tasks::TaskKind::Launch,
                                                     Tasks::TaskKind::Install};
        QList<Tasks::TaskId> ids;
        ids.reserve(kCount);
        for (int i = 0; i < kCount; ++i) {
            QList<Tasks::TaskStep> steps;
            for (int s = 0; s < 6; ++s) {
                steps.append({QStringLiteral("步骤 %1").arg(s + 1),
                              Tasks::TaskStepState::Pending, {}});
            }
            ids.append(manager.begin(kKinds[i % 3], QStringLiteral("压力项 %1").arg(i),
                                     QStringLiteral("target-%1").arg(i % 5), steps));
        }
        check(ids.size() == kCount && ids.constLast() == ids.constFirst() + kCount - 1,
              "AM5 240 个号连着发、一个不重",
              QStringLiteral("%1 → %2").arg(ids.constFirst()).arg(ids.constLast()));
        check(manager.active().size() == kCount,
              "AM5 开局 240 项全在「正在进行」里",
              QString::number(manager.active().size()));

        // 高速推进：50 帧 × 240 项（约等于 40 件同时在跑、每 200 ms 一帧跑十秒）。
        for (int frame = 0; frame < kFrames; ++frame) {
            for (const Tasks::TaskId id : ids) {
                (void)manager.setProgress(id, qint64(frame) * 8, 400);
                (void)manager.setBytes(id, qint64(frame) * 1024, 51200);
                (void)manager.setStepState(id, frame / 9, Tasks::TaskStepState::Running);
            }
        }
        check(manager.active().size() == kCount,
              "AM5 推进 12000 帧期间没有一项被误收尾",
              QString::number(manager.active().size()));
        const Tasks::TaskSnapshot lastBefore = manager.snapshot(ids.constLast());
        check(lastBefore.doneUnits == qint64(kFrames - 1) * 8 && lastBefore.totalUnits == 400
                  && lastBefore.bytesDone == qint64(kFrames - 1) * 1024,
              "AM5 最后一帧原样落在快照上（进度 / 字节都没被别的项串走）",
              QStringLiteral("%1 / %2 · %3").arg(lastBefore.doneUnits)
                  .arg(lastBefore.totalUnits).arg(lastBefore.bytesDone));

        // 收掉一半（120 项：成功 / 失败 / 取消各 40），另一半继续跑 —— 模拟"下完一批、
        // 另一批还在下"，以及同时有实例在启动、有批量文件操作在跑。
        QList<Tasks::TaskId> terminalIds;
        for (int i = 0; i < kCount; i += 2) {
            const Tasks::TaskId id = ids.at(i);
            switch (i % 6) {
            case 0:
                (void)manager.succeed(id);
                break;
            case 2:
                (void)manager.fail(id, QStringLiteral("模拟失败"));
                break;
            default:
                (void)manager.cancel(id);
                break;
            }
            terminalIds.append(id);
        }
        check(manager.active().size() == kCount / 2 && terminalIds.size() == kCount / 2,
              "AM5 收掉一半：120 项还在跑、120 项已收尾",
              QStringLiteral("%1 / %2").arg(manager.active().size()).arg(manager.finished().size()));
        check(finishedSignals == kCount / 2 && historySignals == kCount / 2,
              "AM5 taskFinished / historyChanged 各 120 发（一项收尾只喊一次）",
              QStringLiteral("%1 / %2").arg(finishedSignals).arg(historySignals));

        // 迟到帧：终端项上再推进、再收尾 —— 一个都不许生效、快照一个字节都不许变。
        bool lateAccepted = false;
        for (const Tasks::TaskId id : terminalIds) {
            const Tasks::TaskSnapshot before = manager.snapshot(id);
            lateAccepted = manager.setProgress(id, 999, 1000) || lateAccepted;
            lateAccepted = manager.setBytes(id, 999, 1000) || lateAccepted;
            lateAccepted = manager.setStepState(id, 0, Tasks::TaskStepState::Done) || lateAccepted;
            lateAccepted = manager.succeed(id) || lateAccepted;
            lateAccepted = manager.cancel(id) || lateAccepted;
            lateAccepted = manager.fail(id, QStringLiteral("迟到")) || lateAccepted;
            const Tasks::TaskSnapshot after = manager.snapshot(id);
            if (after.state != before.state || after.doneUnits != before.doneUnits
                || after.bytesDone != before.bytesDone || after.errorSummary != before.errorSummary
                || after.steps.size() != before.steps.size()) {
                lateAccepted = true;
            }
        }
        check(!lateAccepted, "AM5 120 个终端项上的迟到帧 / 二次收尾全部被拒，快照没变");
        check(manager.active().size() == kCount / 2,
              "AM5 迟到帧也没有把任何一项从「已完成」拽回来");

        // 顺序：最近的在前；同一毫秒并列时号大的在前 —— 这条不靠运气，靠比较器里那个
        // 第二把尺（批量操作就是几十项挤在同一毫秒里收尾的）。
        //
        // 内存里也只留最近 `finishedHistoryLimit()` 条（和落盘同一把尺子）：
        // 攒一整趟会话再只在落盘时截断的话，任务中心那一栏能堆几千行，重启之后又只剩
        // 50 条，同一份账本自己跟自己说的不一样。
        const int runtimeLimit = Tasks::finishedHistoryLimit();
        const int expectedFinished = qMin(kCount / 2, runtimeLimit);
        const QList<Tasks::TaskSnapshot> finished = manager.finished();
        bool ordered = finished.size() == expectedFinished;
        bool sawTie = false;
        for (int i = 1; ordered && i < finished.size(); ++i) {
            const Tasks::TaskSnapshot &newer = finished.at(i - 1);
            const Tasks::TaskSnapshot &older = finished.at(i);
            if (newer.finishedAt < older.finishedAt) {
                ordered = false;
            } else if (newer.finishedAt == older.finishedAt) {
                sawTie = true;
                ordered = newer.id > older.id;  // 同毫秒：后发起的（号大的）排前面
            }
        }
        check(ordered, "AM5 「已完成」一栏严格按「最近在前」；同毫秒并列按号定序",
              QStringLiteral("并列=%1 第一条=%2 / 最后一条=%3")
                  .arg(sawTie ? QStringLiteral("有") : QStringLiteral("无"))
                  .arg(finished.isEmpty() ? 0 : finished.constFirst().id)
                  .arg(finished.isEmpty() ? 0 : finished.constLast().id));

        // all() = 活动的（发起顺序）在前，收尾的（最近在前）在后；已完成那截和 finished() 一样
        // 只留最近 `finishedHistoryLimit()` 条。
        const QList<Tasks::TaskSnapshot> everything = manager.all();
        bool splitOk = everything.size() == kCount / 2 + expectedFinished;
        for (int i = 0; splitOk && i < everything.size(); ++i) {
            splitOk = Tasks::isActiveState(everything.at(i).state) == (i < kCount / 2);
        }
        check(splitOk, "AM5 all() 先活动、后收尾：120 条在跑 + 最近 50 条已完成",
              QStringLiteral("%1（活动 %2 + 已完成 %3）")
                  .arg(everything.size())
                  .arg(manager.active().size())
                  .arg(finished.size()));

        // 落盘截断：账本里 120 活动 + 120 已完成，盘上只留**最近的 50 条**已完成。
        check(manager.saveHistory(), "AM5 压力账本落盘成功", path);
        const QList<Tasks::TaskSnapshot> onDisk = Tasks::load(path);
        const int limit = Tasks::finishedHistoryLimit();
        check(onDisk.size() == kCount / 2 + limit,
              "AM5 盘上 = 120 条没跑完的 + 最近 50 条已完成的",
              QStringLiteral("%1（上限 %2）").arg(onDisk.size()).arg(limit));
        bool keptNewest = false;
        bool keptOldest = false;
        for (const Tasks::TaskSnapshot &task : onDisk) {
            if (task.id == ids.at(kCount - 2)) keptNewest = true;  // 238：最后一个收尾的
            if (task.id == ids.at(0)) keptOldest = true;           // 0：最早收尾的
        }
        check(keptNewest && !keptOldest,
              "AM5 截断留下的是**最近**那 50 条（最新那件在、最早那件不在）",
              QStringLiteral("newest=%1 oldest=%2").arg(keptNewest).arg(keptOldest));

        // 读回来：没跑完的算已中断、号从盘上最大的那个续（再发一个不许撞上旧历史）。
        Tasks::TaskManager reloaded;
        reloaded.setHistoryPath(path);
        const int loaded = reloaded.loadHistory();
        check(loaded == onDisk.size() && reloaded.active().isEmpty()
                  && reloaded.finished().size() == onDisk.size(),
              "AM5 读回之后没有「正在跑」的：全成了已中断 / 已完成",
              QStringLiteral("loaded=%1 active=%2")
                  .arg(loaded).arg(reloaded.active().size()));
        const Tasks::TaskId nextId =
            reloaded.begin(Tasks::TaskKind::Download, QStringLiteral("压力之后的新项"));
        check(nextId > ids.constLast(), "AM5 新号从盘上最大的那个之后续（不撞号）",
              QStringLiteral("new=%1（压力里最大 %2）").arg(nextId).arg(ids.constLast()));
    }

    section("AN. 父任务与子任务：一次用户意图只露一行（2.1.1）");
    {
        QTemporaryDir temp;
        check(temp.isValid(), "AN0 临时账本目录建出来了");
        if (!temp.isValid()) {
            return;
        }
        const QString path = temp.filePath(QStringLiteral("tasks.json"));
        Tasks::TaskManager manager;
        manager.setHistoryPath(path);
        const Tasks::TaskId parent = manager.begin(
            Tasks::TaskKind::Install, QStringLiteral("更新实例 A → 1.21.4 Fabric"),
            QStringLiteral("A"),
            {{QStringLiteral("准备目标核心与加载器"), Tasks::TaskStepState::Running, {}},
             {QStringLiteral("准备用户文件副本"), Tasks::TaskStepState::Pending, {}},
             {QStringLiteral("替换核心并发布"), Tasks::TaskStepState::Pending, {}},
             {QStringLiteral("完成"), Tasks::TaskStepState::Pending, {}}});
        const Tasks::TaskId child = manager.beginChild(
            parent, Tasks::TaskKind::Download, QStringLiteral("安装 1.21.4 Fabric"),
            QStringLiteral("A"), {{QStringLiteral("下载核心文件"), Tasks::TaskStepState::Running, {}}},
            Tasks::TaskCapabilities(), 0);
        check(child != Tasks::invalidTaskId() && manager.has(child) && manager.has(parent),
              "AN1 子任务发到了号，而且账本里认得出它");
        check(manager.active().size() == 1 && manager.active().constFirst().id == parent
                  && manager.all().size() == 1,
              "★★ AN1 任务中心只露父任务：子任务不进 active()/all()",
              QStringLiteral("active=%1 all=%2").arg(manager.active().size()).arg(manager.all().size()));
        check(manager.childrenOf(parent).size() == 1
                  && manager.childrenOf(parent).constFirst().id == child,
              "AN1 childrenOf() 能拿到那一项（流程和验证用）");
        check(manager.snapshot(parent).steps.at(0).state == Tasks::TaskStepState::Running,
              "AN1 挂子任务时父任务那一步自动变成 Running");
        manager.setDetail(child, QStringLiteral("正在下载资源对象…"));
        manager.setBytes(child, 12, 100);
        check(manager.snapshot(parent).detail == QStringLiteral("正在下载资源对象…")
                  && manager.snapshot(parent).bytesTotal == 100,
              "AN2 子任务的进度/字节镜像到父任务（底栏那一行说的是同一件事）");
        manager.setProgress(parent, 0, 4);

        // 子任务失败：只改父任务那一步，**不把父任务收尾** —— 这一趟可能还要回滚/收材料。
        manager.fail(child, QStringLiteral("磁盘空间不足"));
        check(manager.snapshot(parent).steps.at(0).state == Tasks::TaskStepState::Failed,
              "AN3 子任务失败 ⇒ 父任务对应那一步记 Failed");
        check(Tasks::isActiveState(manager.snapshot(parent).state),
              "★★ AN3 子任务失败不会偷偷把父任务收成终态（由串流程的那一层决定）",
              QString::number(int(manager.snapshot(parent).state)));
        manager.fail(parent, QStringLiteral("准备核心失败"));
        check(manager.snapshot(parent).state == Tasks::TaskState::Failed,
              "AN3 父任务按整趟结果收成 Failed（不是 Succeeded）");

        // 取消：子任务取消不算失败，也不许把父任务显示成成功。
        const Tasks::TaskId parent2 = manager.begin(
            Tasks::TaskKind::Install, QStringLiteral("更新实例 B → 1.21.4"), QString(),
            {{QStringLiteral("准备目标核心与加载器"), Tasks::TaskStepState::Running, {}},
             {QStringLiteral("完成"), Tasks::TaskStepState::Pending, {}}});
        const Tasks::TaskId child2 = manager.beginChild(
            parent2, Tasks::TaskKind::Download, QStringLiteral("安装 1.21.4"),
            QString(), {{QStringLiteral("下载核心文件"), Tasks::TaskStepState::Running, {}}},
            Tasks::TaskCapabilities(), 0);
        manager.cancel(child2);
        check(manager.snapshot(child2).state == Tasks::TaskState::Cancelled,
              "AN4 子任务取消 ⇒ Cancelled");
        check(manager.snapshot(parent2).steps.at(0).state == Tasks::TaskStepState::Running,
              "AN4 取消不在父任务那一步上打 ✗（那一步没做错）");
        manager.cancel(parent2);
        check(manager.snapshot(parent2).state == Tasks::TaskState::Cancelled,
              "★★ AN4 父任务收成 Cancelled，不是 Succeeded");

        // 「继续未完成操作」只挂在**已经收尾**的那一项上；还在跑的不该有这一颗。
        const Tasks::TaskId stillRunning =
            manager.begin(Tasks::TaskKind::Import, QStringLiteral("还在导入"));
        check(!manager.setRecovery(stillRunning, QStringLiteral("report-running"), true),
              "AN4b 还在跑的任务不能挂「继续未完成操作」");
        check(manager.setRecovery(parent2, QStringLiteral("report-2"), true)
                  && manager.snapshot(parent2).capabilities.canRecover
                  && manager.snapshot(parent2).recoveryToken == QStringLiteral("report-2"),
              "AN4b 收尾之后挂上报告钥匙，那颗「继续未完成操作」才露得出来");
        manager.cancel(stillRunning);

        // 落盘：子任务不写盘，读回来只有父任务。
        manager.setRecovery(parent, QStringLiteral("report-1"), true);
        check(manager.saveHistory(), "AN5 账本落盘成功", path);
        const QList<Tasks::TaskSnapshot> onDisk = Tasks::load(path);
        bool onlyParents = !onDisk.isEmpty();
        for (const Tasks::TaskSnapshot &task : onDisk) {
            if (task.parentId != Tasks::invalidTaskId()) onlyParents = false;
        }
        check(onlyParents, "★★ AN5 盘上只有父任务：子任务只是这一趟的步骤，不是历史",
              QStringLiteral("盘上 %1 条").arg(onDisk.size()));

        // 清除已完成：父任务和它的子任务一起走，不留看不见的孤儿。
        manager.clearFinished();
        check(manager.childrenOf(parent).isEmpty() && manager.childrenOf(parent2).isEmpty()
                  && manager.finished().isEmpty(),
              "AN6 「清除已完成」连子任务一起清掉");
    }

    // ------------------------------------------------------------------
    // § AO 当前步骤摘取、收起态摘要步骤字段、同步等待与进度合并口径（pure functions）
    // ------------------------------------------------------------------
    {
        const QDateTime now(QDate(2026, 11, 19), QTime(12, 0, 0));

        Tasks::TaskSnapshot launch;
        launch.id = 42;
        launch.kind = Tasks::TaskKind::Launch;
        launch.state = Tasks::TaskState::Running;
        launch.title = QStringLiteral("启动 1.20.1");
        launch.startedAt = now.addSecs(-5);
        launch.steps = {
            {QStringLiteral("校验登录状态"), Tasks::TaskStepState::Done, {}},
            {QStringLiteral("检查启动文件"), Tasks::TaskStepState::Running, {}},
            {QStringLiteral("准备 Java 运行环境"), Tasks::TaskStepState::Pending, {}},
            {QStringLiteral("等待游戏启动"), Tasks::TaskStepState::Pending, {}},
            {QStringLiteral("完成"), Tasks::TaskStepState::Pending, {}},
        };
        launch.doneUnits = 3;
        launch.totalUnits = 10;

        const Tasks::TaskStepSummary activeStep = Tasks::currentStepOf(launch);
        check(activeStep.index == 1 && activeStep.total == 5
                  && activeStep.name == QStringLiteral("检查启动文件"),
              "AO1 currentStepOf 返回正在跑的那一步（index=1/5）",
              QStringLiteral("%1/%2 %3").arg(activeStep.index).arg(activeStep.total).arg(activeStep.name));

        const Tasks::TaskSummary activeSummary =
            Tasks::collapsedSummary({launch}, {}, now, 8000);
        check(activeSummary.currentStep == QStringLiteral("检查启动文件")
                  && activeSummary.currentStepIndex == 1 && activeSummary.stepCount == 5
                  && activeSummary.percent == 26,
              "AO2 完成一步，第二步做了 30%，总进度为 26%",
              QStringLiteral("step=%1 idx=%2/%3 pct=%4")
                  .arg(activeSummary.currentStep)
                  .arg(activeSummary.currentStepIndex)
                  .arg(activeSummary.stepCount)
                  .arg(activeSummary.percent));

        const Tasks::TaskSummary waitCountable =
            Tasks::applySyncWait(activeSummary, QStringLiteral("正在准备游戏文件..."));
        check(waitCountable.id == 42 && waitCountable.state == Tasks::TaskState::Running
                  && waitCountable.title == QStringLiteral("正在准备游戏文件...")
                  && waitCountable.percent == 26
                  && waitCountable.currentStep == QStringLiteral("检查启动文件"),
              "★★ AO3 同步等待保留按步骤计算的总进度（26%）",
              QStringLiteral("title=%1 pct=%2").arg(waitCountable.title).arg(waitCountable.percent));

        // 步骤没有细分计数时，保留已完成步骤的进度。
        Tasks::TaskSnapshot unknownLaunch = launch;
        unknownLaunch.doneUnits = 0;
        unknownLaunch.totalUnits = 0;
        const Tasks::TaskSummary unknownSummary =
            Tasks::collapsedSummary({unknownLaunch}, {}, now, 8000);
        const Tasks::TaskSummary waitUnknown =
            Tasks::applySyncWait(unknownSummary, QStringLiteral("正在准备游戏文件..."));
        check(waitUnknown.percent == 20 && waitUnknown.title == QStringLiteral("正在准备游戏文件..."),
              "AO4 当前步骤没有计数，保留已完成一步的进度（20%）",
              QString::number(waitUnknown.percent));

        QTemporaryDir startupHistory;
        Tasks::TaskManager steps;
        steps.setHistoryPath(startupHistory.filePath(QStringLiteral("task-history.json")));
        const auto startup = steps.begin(Tasks::TaskKind::Launch, QStringLiteral("启动"), {}, launch.steps);
        steps.setProgress(startup, 10, 10);
        check(Tasks::percentOf(steps.snapshot(startup)) == 40, "AO4b 文件计数只推到本步末尾");
        steps.setStepState(startup, 1, Tasks::TaskStepState::Done);
        steps.setStepState(startup, 2, Tasks::TaskStepState::Running);
        check(Tasks::percentOf(steps.snapshot(startup)) == 40, "AO4b 进入 Java 阶段时进度连续");
        steps.setProgress(startup, 25, 100);
        steps.fail(startup, QStringLiteral("error"));
        check(Tasks::percentOf(steps.snapshot(startup)) == 45, "AO4b 失败保留 Java 阶段的进度");

        // 刚完成的任务（8s 时间窗里 percent=100）不能被下一次同步等待误借走 100%
        Tasks::TaskSnapshot finished = launch;
        finished.state = Tasks::TaskState::Succeeded;
        finished.doneUnits = 1;
        finished.totalUnits = 1;
        finished.finishedAt = now.addSecs(-2);
        for (Tasks::TaskStep &st : finished.steps) {
            st.state = Tasks::TaskStepState::Done;
        }
        const Tasks::TaskStepSummary doneStep = Tasks::currentStepOf(finished);
        check(doneStep.index == 4 && doneStep.name == QStringLiteral("完成"),
              "AO5 启动成功后 currentStepOf 落在第 5 步「完成」");
        const Tasks::TaskSummary doneSummary =
            Tasks::collapsedSummary({finished}, {}, now, 8000);
        const Tasks::TaskSummary waitAfterDone =
            Tasks::applySyncWait(doneSummary, QStringLiteral("正在校验登录状态..."));
        check(waitAfterDone.percent == -1 && waitAfterDone.id == Tasks::invalidTaskId(),
              "★★ AO6 已完成任务在 8s 窗口内的 100% 不会漏进下一次同步等待",
              QString::number(waitAfterDone.percent));
    }
}

