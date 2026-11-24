#include "Support.hpp"

#include "BrandMark.hpp"
#include "MainWindowUI.hpp"
#include "TaskProgressBar.hpp"
#include "Theme.hpp"

#include <QElapsedTimer>
#include <QPushButton>

// 主页启动卡片（issue #65）：点「启动游戏」后启动区那一行原地换成「手写 W + 步骤 / 进度 + 胶囊」。
// 钉的是：整块高度不变、胶囊三种状态同位同尺寸、三种状态的字、失败眉题危险色、成功停一会儿自己还原、
// 启动区重建时卡片那一行不跟着重建；W 的接缝连续、完成后铺满（失败转灰）。
namespace {

void settle(QApplication &app) {
    for (int i = 0; i < 12; ++i) app.processEvents();
}

QString rectText(const QRect &r) {
    return QStringLiteral("%1,%2 %3×%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

}  // namespace

int UiLaunchCardChecks(QApplication &app) {
    Theme::setSpec(Theme::defaultSpec());
    Theme::apply(app);
    QMainWindow window;
    MainWindowUI ui(&window);
    window.resize(1280, 800);
    window.show();

    MainWindowUI::HomeQuickItem item;
    item.key = QStringLiteral("spring");
    item.name = QStringLiteral("春日生存");
    item.detail = QStringLiteral("1.21.4 / Fabric 0.16.9\n3 小时前");
    item.current = true;
    ui.setHomeQuickRows({item}, {}, {});
    ui.setHomeQuickMode(MainWindowUI::HomeQuickMode::Current);
    settle(app);

    const auto find = [&](const char *name) { return window.findChild<QWidget *>(QString::fromLatin1(name)); };
    const auto label = [&](const char *name) { return window.findChild<QLabel *>(QString::fromLatin1(name)); };
    const auto text = [&](const char *name) {
        QLabel *l = label(name);
        return l == nullptr ? QStringLiteral("<没找到>") : l->text();
    };
    const auto shownAt = [&](const char *name) {
        QWidget *w = find(name);
        return w != nullptr && w->isVisibleTo(&window);
    };
    QWidget *card = find("homeCurrentInstanceCard");
    const int idleHeight = card != nullptr ? card->height() : -1;
    const QString idleMeta = text("homeCurrentVersion");
    check(card != nullptr && !ui.launchCardShown() && shownAt("homeCurrentLaunch") && !shownAt("homeLaunchCard"),
          "LC1 平常：启动区是「启动游戏」+ 设置，没有卡片", QString::number(idleHeight));

    // ---------- 加载中：补全文件 ----------
    MainWindowUI::LaunchCard running;
    running.phase = MainWindowUI::LaunchCard::Phase::Running;
    running.taskId = 7;
    running.instanceKey = item.key;
    running.stage = QStringLiteral("补全文件与资源");
    running.done = 312;
    running.total = 1024;
    running.login = QStringLiteral("离线登录");
    ui.setLaunchCard(running);
    settle(app);
    QWidget *row = find("homeLaunchCard");
    QWidget *pill = find("homeLaunchPill");
    const QRect pillRunning = pill != nullptr ? pill->geometry() : QRect();
    check(ui.launchCardShown() && shownAt("homeLaunchCard") && !shownAt("homeCurrentLaunch") && !shownAt("homeCurrentEdit"),
          "LC2 加载中：卡片顶替「启动游戏」和设置");
    check(card != nullptr && card->height() == idleHeight, "LC3 加载中：启动区整块高度不变",
          QStringLiteral("%1 → %2").arg(idleHeight).arg(card != nullptr ? card->height() : -1));
    check(text("homeCurrentEyebrow") == QStringLiteral("正在启动游戏") && text("homeLaunchStep") == running.stage
              && text("homeLaunchCount") == QStringLiteral("312 / 1024") && shownAt("homeLaunchCount"),
          "LC4 加载中：眉题、阶段名、件数",
          QStringLiteral("%1 / %2 / %3").arg(text("homeCurrentEyebrow"), text("homeLaunchStep"), text("homeLaunchCount")));
    check(text("homeCurrentVersion").endsWith(QStringLiteral("离线登录")) && !text("homeCurrentVersion").contains(QStringLiteral("3 小时前")),
          "LC5 加载中：元信息末项换成登录方式", text("homeCurrentVersion"));
    auto *pillButton = qobject_cast<QPushButton *>(pill);
    check(pillButton != nullptr && pillButton->text() == QStringLiteral("取消") && pill->isVisibleTo(&window)
              && pill->size() == QSize(84, 36) && row != nullptr && pill->geometry().bottom() == row->rect().bottom(),
          "LC6 加载中：右边「取消」胶囊 84×36，底边贴行底", rectText(pillRunning));
    auto *bar = window.findChild<TaskProgressBar *>(QStringLiteral("homeLaunchBar"));
    check(bar != nullptr && bar->maximum() == 1000 && bar->value() == 312 * 1000 / 1024, "LC7 加载中：进度条按件数走",
          bar != nullptr ? QStringLiteral("%1/%2").arg(bar->value()).arg(bar->maximum()) : QString());
    check(row != nullptr && row->accessibleName().contains(item.name) && row->accessibleName().contains(running.stage),
          "LC8 无障碍名带实例名和阶段", row != nullptr ? row->accessibleName() : QString());

    // 准备 Java 按字节报；不定进度 = setRange(0, 0)。
    MainWindowUI::LaunchCard java = running;
    java.stage = QStringLiteral("准备 Java");
    java.done = 38LL * 1048576;
    java.total = 52LL * 1048576;
    java.bytes = true;
    ui.setLaunchCard(java);
    settle(app);
    check(text("homeLaunchCount") == QStringLiteral("38 / 52 MB"), "LC9 下载 Java：件数按 MB 写", text("homeLaunchCount"));
    MainWindowUI::LaunchCard waiting = running;
    waiting.stage = QStringLiteral("等待游戏窗口");
    waiting.done = waiting.total = 0;
    ui.setLaunchCard(waiting);
    settle(app);
    check(bar != nullptr && bar->maximum() == 0 && !shownAt("homeLaunchCount"), "LC10 等待游戏窗口：不定进度，不写件数");

    // 启动区重建（实例列表刷新）：卡片那一行换父控件不重建，状态还在。
    ui.setHomeQuickRows({item}, {}, {});
    settle(app);
    check(find("homeLaunchCard") == row && ui.launchCardShown() && shownAt("homeLaunchCard") && !shownAt("homeCurrentLaunch")
              && text("homeLaunchStep") == waiting.stage,
          "LC11 启动区重建：卡片那一行原样留着（W 的动画不断）");
    card = find("homeCurrentInstanceCard");

    // ---------- 失败 ----------
    MainWindowUI::LaunchCard failed = running;
    failed.phase = MainWindowUI::LaunchCard::Phase::Failed;
    failed.reason = QStringLiteral("找不到可用的 Java 21");
    failed.stage = QStringLiteral("准备 Java");
    ui.setLaunchCard(failed);
    settle(app);
    check(card != nullptr && card->height() == idleHeight, "LC12 失败：整块高度不变",
          QString::number(card != nullptr ? card->height() : -1));
    check(pill != nullptr && pill->geometry() == pillRunning && pillButton->text() == QStringLiteral("关闭"),
          "LC13 失败：胶囊同位同尺寸，字换成「关闭」", rectText(pill != nullptr ? pill->geometry() : QRect()));
    check(text("homeCurrentEyebrow") == QStringLiteral("启动失败") && text("homeLaunchReason") == failed.reason
              && text("homeLaunchStopAt") == QStringLiteral("停在「准备 Java」") && shownAt("homeLaunchDetails")
              && !shownAt("homeLaunchStep") && !shownAt("homeLaunchBar"),
          "LC14 失败：眉题、原因、停在哪一段、「查看详情」",
          QStringLiteral("%1 / %2 / %3").arg(text("homeCurrentEyebrow"), text("homeLaunchReason"), text("homeLaunchStopAt")));
    {
        const Theme::Tokens t = Theme::tokens();
        QLabel *eyebrow = label("homeCurrentEyebrow");
        const QColor want = Theme::textTone(t.dangerInk, t.danger);
        check(eyebrow != nullptr && eyebrow->palette().color(eyebrow->foregroundRole()) == want, "LC15 失败：眉题用危险色（同警告图标）",
              eyebrow != nullptr ? eyebrow->palette().color(eyebrow->foregroundRole()).name() : QString());
        // 画出来也得是危险色（不只调色板对）：眉题里最红的那一点要明显偏红。
        {
            const QImage g = eyebrow != nullptr ? eyebrow->grab().toImage() : QImage();
            int redness = -999;
            for (int y = 0; y < g.height(); ++y)
                for (int x = 0; x < g.width(); ++x) {
                    const QColor c = g.pixelColor(x, y);
                    redness = std::max(redness, c.red() - std::max(c.green(), c.blue()));
                }
            check(redness >= 40, "LC15b 失败：眉题画出来是危险色", QString::number(redness));
        }
        // 换主题后危险色还在（followHeroInk 不把它冲回 heroInk2）。
        Theme::Spec spec = Theme::defaultSpec();
        spec.presetId = QStringLiteral("emerald");  // 冰川碧湖
        Theme::setSpec(spec);
        Theme::apply(app);
        settle(app);
        const Theme::Tokens t2 = Theme::tokens();
        eyebrow = label("homeCurrentEyebrow");
        check(eyebrow != nullptr && eyebrow->palette().color(eyebrow->foregroundRole()) == Theme::textTone(t2.dangerInk, t2.danger),
              "LC16 换主题后失败眉题仍是危险色",
              eyebrow != nullptr ? eyebrow->palette().color(eyebrow->foregroundRole()).name() : QString());
        Theme::setSpec(Theme::defaultSpec());
        Theme::apply(app);
        settle(app);
    }
    auto *mark = window.findChild<LaunchMark *>(QStringLiteral("homeLaunchMark"));
    check(mark != nullptr && mark->phase() == LaunchMark::Phase::Failed && mark->grayAmount() > 0.99,
          "LC17 失败：W 铺满后转灰", mark != nullptr ? QString::number(mark->grayAmount()) : QString());
    // 「关闭」：当场还原；同一项任务之后再来的数据不再把它叫出来。
    if (pillButton != nullptr) pillButton->click();
    settle(app);
    check(!ui.launchCardShown() && shownAt("homeCurrentLaunch") && text("homeCurrentEyebrow") == QStringLiteral("当前实例")
              && text("homeCurrentVersion") == idleMeta,
          "LC18 点「关闭」：回到平常的启动区，眉题和元信息还原", text("homeCurrentVersion"));
    ui.setLaunchCard(failed);
    settle(app);
    check(!ui.launchCardShown(), "LC19 关掉的那一项不再冒出来");

    // ---------- 成功 ----------
    MainWindowUI::LaunchCard ok = running;
    ok.taskId = 8;
    ok.phase = MainWindowUI::LaunchCard::Phase::Succeeded;
    ui.setLaunchCard(ok);
    settle(app);
    check(card != nullptr && card->height() == idleHeight, "LC20 成功：整块高度不变");
    check(pill != nullptr && pill->geometry() == pillRunning && !pill->isVisibleTo(&window),
          "LC21 成功：胶囊藏起来但位置不让（同位同尺寸）", rectText(pill != nullptr ? pill->geometry() : QRect()));
    check(text("homeCurrentEyebrow") == QStringLiteral("游戏已启动") && text("homeLaunchStep") == QStringLiteral("游戏窗口已打开")
              && bar != nullptr && bar->maximum() > 0 && bar->value() == bar->maximum(),
          "LC22 成功：眉题、说明、进度条走满",
          QStringLiteral("%1 / %2").arg(text("homeCurrentEyebrow"), text("homeLaunchStep")));
    check(mark != nullptr && mark->phase() == LaunchMark::Phase::Succeeded && mark->grayAmount() == 0.0
              && mark->inkAt(0) > 0.99 && mark->inkAt(LaunchMark::pathLength() / 2) > 0.99,
          "LC23 成功：W 铺满、不转灰");
    QElapsedTimer hold;
    hold.start();
    while (ui.launchCardShown() && hold.elapsed() < 4000) app.processEvents(QEventLoop::AllEvents, 20);
    check(!ui.launchCardShown() && shownAt("homeCurrentLaunch") && hold.elapsed() >= MainWindowUI::launchCardSuccessHoldMs() - 50,
          "LC24 成功停约 1.5 s 后自己还原", QStringLiteral("%1 ms").arg(hold.elapsed()));

    // 别的实例的启动：卡片不出来。用户取消（Idle）：卡片收起。
    MainWindowUI::LaunchCard other = running;
    other.taskId = 9;
    other.instanceKey = QStringLiteral("another");
    ui.setLaunchCard(other);
    settle(app);
    check(!ui.launchCardShown() && shownAt("homeCurrentLaunch"), "LC25 启动的不是当前实例：卡片不出来");
    MainWindowUI::LaunchCard mine = running;
    mine.taskId = 10;
    ui.setLaunchCard(mine);
    settle(app);
    const bool shownBefore = ui.launchCardShown();
    MainWindowUI::LaunchCard idle = mine;
    idle.phase = MainWindowUI::LaunchCard::Phase::Idle;
    ui.setLaunchCard(idle);
    settle(app);
    check(shownBefore && !ui.launchCardShown() && shownAt("homeCurrentLaunch"), "LC26 被取消（Idle）：卡片收起");

    // ---------- W 本身 ----------
    LaunchMark w;
    w.resize(76, 52);
    const qreal L = LaunchMark::pathLength();
    w.startWriting();
    // 接缝：笔画首尾接成环，起笔和收笔那一点墨量相同；墨段跨过一圈的那一刻位置连续。
    bool seamOk = true;
    QString seamDetail;
    for (const qreal t : {2.70, 2.78, 2.80, 2.82, 2.90, 5.60}) {
        w.setFrozenTime(t);
        const qreal a = w.inkAt(0.0), b = w.inkAt(L);
        if (std::abs(a - b) > 1e-6) seamOk = false;
        seamDetail += QStringLiteral("t=%1: %2/%3 ").arg(t).arg(a, 0, 'f', 3).arg(b, 0, 'f', 3);
    }
    check(seamOk, "LC27 W 接缝：起笔和收笔墨量一致", seamDetail);
    qreal maxJump = 0;
    for (int i = 0; i < 40; ++i) {
        const qreal t = 2.60 + i * 0.01;
        w.setFrozenTime(t);
        QVector<qreal> before;
        for (int k = 0; k < 64; ++k) before << w.inkAt(L * k / 64);
        w.setFrozenTime(t + 0.01);
        for (int k = 0; k < 64; ++k) maxJump = std::max(maxJump, std::abs(w.inkAt(L * k / 64) - before.at(k)));
    }
    check(maxJump < 0.35, "LC28 W 跨过一圈时 10 ms 内任何一点墨量都不跳", QString::number(maxJump, 'f', 3));
    bool baseOk = true;
    w.setFrozenTime(1.3);
    for (int k = 0; k < 64; ++k) baseOk = baseOk && w.inkAt(L * k / 64) >= 0.25 - 1e-9;
    check(baseOk, "LC29 加载中 W 常显 25% 淡墨作底");
    // 失败：任意时刻进入，0.25 s 铺满，再 0.45 s 转灰。
    w.setFrozenTime(4.10);
    w.finish(false);
    w.setFrozenTime(4.10 + 0.25);
    qreal minInk = 1;
    for (int k = 0; k <= 64; ++k) minInk = std::min(minInk, w.inkAt(L * k / 64));
    check(minInk > 0.99 && w.grayAmount() < 0.01, "LC30 失败 0.25 s：墨铺满整个 W，还没开始转灰",
          QStringLiteral("最淡 %1，灰 %2").arg(minInk, 0, 'f', 3).arg(w.grayAmount(), 0, 'f', 3));
    w.setFrozenTime(4.10 + 0.25 + 0.45);
    check(w.grayAmount() > 0.99, "LC31 失败 0.70 s：转灰完成", QString::number(w.grayAmount(), 'f', 3));
    return 0;
}
