// 折叠行开合的逐帧量法：真点标题行，开着动效，跑真事件循环，每 16ms 采一帧
// 「标题行的全局 y、滚动条值、滚动范围、body 高」，并记下标题行每一次真正被画出来时的 y。
// 判据：标题行每一帧、每一次绘制都在原位（展开 / 收起），body 高度确有过渡，终态显隐正确。
// 覆盖设置页两处「高级」（ExpandSection，滚到顶 / 滚到底 × 两种窗宽）和另外三处折叠：
// 世界设置的存档文件框、主题细分颜色、实例设置·启动的高级选项。
// 参数 `--frames`：把逐帧数据整表印出来（调查用）。
#include "Support.hpp"
#include "MainWindowPageLayout.hpp"
#include "SettingsPageParts.hpp"
#include "ThemeAdvancedEditor.hpp"
#include "WorldSettingsPage.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace {

struct Frame {
    int ms = 0;
    int y = 0;
    int scroll = 0;
    int range = 0;
    int bodyH = 0;
};

void click(QWidget *w) {
    const QPoint p = w->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, p, w->mapToGlobal(p), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, p, w->mapToGlobal(p), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/// 标题行每次真正被画出来时它在屏幕上的 y。
class PaintLog : public QObject {
public:
    PaintLog(QWidget *toggle, const QElapsedTimer &clock) : QObject(toggle), m_toggle(toggle), m_clock(clock) {
        toggle->installEventFilter(this);
    }
    struct Paint {
        int ms;
        int y;
    };
    QList<Paint> paints;
    /// 每次标题行被画出来时调用（调查出图用：拍下当时整窗的样子）。
    std::function<void(int ms)> onPaint;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == m_toggle && event->type() == QEvent::Paint && !m_inHook) {
            const int ms = static_cast<int>(m_clock.elapsed());
            paints.append({ms, m_toggle->mapToGlobal(QPoint(0, 0)).y()});
            if (onPaint) {
                m_inHook = true;  // grab() 会再画一遍标题行
                onPaint(ms);
                m_inHook = false;
            }
        }
        return false;
    }

private:
    QWidget *m_toggle;
    const QElapsedTimer &m_clock;
    bool m_inHook = false;
};

struct Run {
    QList<Frame> frames;
    QList<PaintLog::Paint> paints;
};

/// 点一下标题行，之后跑真事件循环 `ms` 毫秒，每 16ms 采一帧（第 0 帧是点之前）。
Run clickAndRecord(QWidget *toggle, QWidget *body, QScrollArea *scroll, int ms = 360,
                   const std::function<void(int)> &onPaint = {}) {
    Run run;
    QElapsedTimer clock;
    clock.start();
    PaintLog log(toggle, clock);
    log.onPaint = onPaint;
    const auto sample = [&] {
        Frame f;
        f.ms = static_cast<int>(clock.elapsed());
        f.y = toggle->mapToGlobal(QPoint(0, 0)).y();
        f.scroll = scroll != nullptr ? scroll->verticalScrollBar()->value() : 0;
        f.range = scroll != nullptr ? scroll->verticalScrollBar()->maximum() : 0;
        f.bodyH = body->isVisible() ? body->height() : 0;
        run.frames.append(f);
    };
    wait(20);
    clock.restart();
    log.paints.clear();
    sample();
    click(toggle);
    while (clock.elapsed() < ms) {
        wait(16);
        sample();
    }
    run.paints = log.paints;
    return run;
}

/// 点开再点收，逐次判：标题行不位移、body 有过渡、终态对。
void checkFold(const QString &tag, QWidget *toggle, QWidget *body, QScrollArea *scroll, bool dump) {
    for (const bool open : {true, false}) {
        const Run run = clickAndRecord(toggle, body, scroll);
        if (dump) {
            std::printf("--- %s %s\n", qPrintable(tag), open ? "open" : "close");
            for (const Frame &f : run.frames)
                std::printf("  t=%3d y=%d scroll=%d/%d bodyH=%d\n", f.ms, f.y, f.scroll, f.range, f.bodyH);
            for (const auto &paint : run.paints) std::printf("  paint t=%3d y=%d\n", paint.ms, paint.y);
        }
        int minY = run.frames.first().y;
        int maxY = minY;
        int maxBody = 0;
        int mid = 0;
        for (const Frame &f : run.frames) {
            minY = qMin(minY, f.y);
            maxY = qMax(maxY, f.y);
            maxBody = qMax(maxBody, f.bodyH);
        }
        for (const Frame &f : run.frames)
            if (f.bodyH > 0 && f.bodyH < maxBody) ++mid;
        for (const auto &paint : run.paints) {
            minY = qMin(minY, paint.y);
            maxY = qMax(maxY, paint.y);
        }
        const QString what = QStringLiteral("%1 %2").arg(tag, open ? QStringLiteral("展开") : QStringLiteral("收起"));
        check(body->isVisible() == open, what + QStringLiteral("：终态显隐正确"));
        check(mid >= 2, what + QStringLiteral("：body 高度有过渡（不是一步到位）"), QStringLiteral("过渡帧 %1").arg(mid));
        check(minY == maxY, what + QStringLiteral("：标题行每一帧（含真画出来的）都在原位"),
              QStringLiteral("y 在 %1..%2").arg(minY).arg(maxY));
    }
}

} // namespace

int UiFoldMotionChecks(QApplication &app) {
    const bool dump = app.arguments().contains(QStringLiteral("--frames"));
    Anim::setEnabled(true);
    {
        QMainWindow window;
        MainWindowUI ui(&window);
        window.show();
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
        QWidget *page = ui.getTabWidget()->widget(MainWindowUI::settingsTabIndex());
        auto *tabs = ui.getSettingsTabs();
        const QStringList keys = MainWindowUI::settingsCategoryKeys();
        for (const QSize size : {QSize(1280, 800), QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight)}) {
            window.resize(size);
            for (const QString &key : {QStringLiteral("launch"), QStringLiteral("look")}) {
                const int index = keys.indexOf(key);
                tabs->setCurrentIndex(index);
                wait(300);  // 换页动效
                auto *scroll = qobject_cast<QScrollArea *>(ui.getSettingsCategoryStack()->widget(index));
                auto *section = page->findChild<SettingsParts::ExpandSection *>(QStringLiteral("settingsAdvanced-%1").arg(key));
                if (scroll == nullptr || section == nullptr) {
                    check(false, QStringLiteral("%1 页有「高级」折叠").arg(key));
                    continue;
                }
                check(section->header()->property("wtfmlFoldCard").toBool(),
                      QStringLiteral("%1 页「高级」是卡片式折叠行（不再是一条透明正文）").arg(key));
                for (const bool atBottom : {false, true}) {
                    scroll->verticalScrollBar()->setValue(atBottom ? scroll->verticalScrollBar()->maximum() : 0);
                    wait(20);
                    checkFold(QStringLiteral("%1×%2 设置·%3 %4")
                                  .arg(size.width())
                                  .arg(size.height())
                                  .arg(key, atBottom ? QStringLiteral("滚到底") : QStringLiteral("在顶")),
                              section->header(), section->body(), scroll, dump);
                }
            }
        }
    }
    if (const QString shotDir = qEnvironmentVariable("WTFML_FOLD_SHOTS"); !shotDir.isEmpty()) {
        // 逐帧拼图用：设置·启动，滚到底，点「高级」展开，标题行每被画出来一次就拍一张整窗（最多 8 张，间隔 ≥ 14ms）。
        QDir().mkpath(shotDir);
        QMainWindow window;
        MainWindowUI ui(&window);
        window.resize(1280, 800);
        window.show();
        ui.getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
        const int index = MainWindowUI::settingsCategoryKeys().indexOf(QStringLiteral("launch"));
        ui.getSettingsTabs()->setCurrentIndex(index);
        wait(300);
        auto *scroll = qobject_cast<QScrollArea *>(ui.getSettingsCategoryStack()->widget(index));
        auto *section = window.findChild<SettingsParts::ExpandSection *>(QStringLiteral("settingsAdvanced-launch"));
        if (scroll != nullptr && section != nullptr) {
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            wait(20);
            int shots = 0;
            int lastMs = -100;
            clickAndRecord(section->header(), section->body(), scroll, 220, [&](int ms) {
                if (shots >= 8 || ms - lastMs < 14) return;
                lastMs = ms;
                window.grab().save(QDir(shotDir).filePath(QStringLiteral("frame-%1.png").arg(shots++, 2, 10, QLatin1Char('0'))));
            });
        }
    }
    {
        // 主题细分颜色：放进一个滚动区，下面留足空白（展开后内容比视口高）。
        QScrollArea scroll;
        scroll.setWidgetResizable(true);
        auto *holder = new QWidget;
        auto *layout = new QVBoxLayout(holder);
        auto *editor = new ThemeAdvancedEditor(holder);
        layout->addWidget(editor);
        layout->addStretch(1);
        scroll.setWidget(holder);
        scroll.resize(640, 360);
        scroll.show();
        wait(50);
        auto *toggle = editor->findChild<QWidget *>(QStringLiteral("themeAdvancedToggle"));
        auto *body = editor->findChild<QWidget *>(QStringLiteral("themeAdvancedBody"));
        check(toggle != nullptr && body != nullptr, QStringLiteral("主题细分颜色：有折叠行和折叠体"));
        if (toggle != nullptr && body != nullptr) checkFold(QStringLiteral("主题细分颜色"), toggle, body, &scroll, dump);
    }
    {
        WorldSettingsPage world;
        world.resize(820, 560);
        world.show();
        wait(50);
        checkFold(QStringLiteral("世界设置·存档文件"), world.fileToggle(), world.fileBody(), nullptr, dump);
    }
    {
        InstanceSettingsDialog::GlobalValues global;
        InstanceSettingsDialog dialog(QStringLiteral("1.20.1"), QStringLiteral("1.20.1-Forge"),
                                      InstanceSettingsDialog::Override(), global, QStringList(), false);
        dialog.resize(820, 560);
        dialog.show();
        dialog.showPage(InstanceSettingsDialog::launchCategoryKey());
        wait(300);
        auto *toggle = dialog.findChild<QWidget *>(InstanceSettingsDialog::advancedToggleName());
        auto *body = dialog.findChild<QWidget *>(InstanceSettingsDialog::advancedBoxName());
        check(toggle != nullptr && body != nullptr, QStringLiteral("实例设置·启动：有折叠行和折叠体"));
        check(toggle != nullptr && toggle->property("wtfmlFoldCard").toBool(),
              QStringLiteral("实例设置·启动「高级选项」也是卡片式折叠行"));
        if (toggle != nullptr && body != nullptr)
            checkFold(QStringLiteral("实例设置·启动·高级选项"), toggle, body, nullptr, dump);
    }
    return 0;
}
