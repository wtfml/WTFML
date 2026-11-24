/**
 * @file PerfProbe.cpp
 * @brief 测量构建（`-DWTFML_PERF_PROBE=ON`）里的界面探针：按真实入口发动作、记耗时。
 *
 * 三件东西：
 *  1. **按用户点的那一颗按钮发动作**（`TitleBar::navButton()`、设置页那个主题下拉框），
 *     不绕过业务层 —— 翻页之后那一串懒加载、列表重建都算进时间里；
 *  2. 每一步量两段：动作本身（同步那部分）+ `repaint()`（把脏的整窗立刻画一遍）。
 *     后者是**上限**：真实使用里只重画脏的那几块，但上限也可以横向比较；
 *  3. 一个 20 ms 的看门狗定时器：主线程被什么占住时它会晚到，晚到多少就是阻塞多少
 *     （报告里 `maxGapMs` 含 20 ms 的节拍本身，`maxStallMs` 是减掉节拍之后的值）。
 *
 * 只有一条入口：`attach()`。argv 里没有 `--wtfml-perf-probe=<文件>` 时立即返回，
 * 所以这个 exe 不测量时和普通启动器没有区别。
 *
 * 跑法见 `tools/perf-startup.ps1 -Probe`（验收配置 `--acceptance-profile` 那一套在那边）。
 *
 * 首帧守卫：带 `--wtfml-firstframe-allowlist=<文件>` 时，每个"点击 → 首帧"表面都
 * 必须在 50 ms 内画出来；超过的要么在允许名单里，要么整趟探针以非零退出。允许名单
 * 是 `tools/perf-firstframe-allowlist.txt`，只许删不许加（详见 docs/dev/PERFORMANCE.md）。
 */

#include "perf/PerfProbe.hpp"

#include "perf/StallSampler.hpp"

#include "AccountSkinDialog.hpp"
#include "InstanceSettingsDialog.hpp"
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "NewServerPage.hpp"
#include "OverlayHost.hpp"
#include "PresetInstallPicker.hpp"
#include "Theme.hpp"
#include "TitleBar.hpp"
#include "ToolboxPage.hpp"
#include "TaskCenterPanel.hpp"
#include "PresetManagePage.hpp"
#include "controllers/InstallPageController.hpp"
#include "download/ResourceCatalog.hpp"
#include "presets/PresetStore.hpp"
#include "presets/PresetCatalog.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "util/AppPaths.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUuid>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

// `Step` 用 `{name, action}` 聚合初始化，其余成员都有默认值；GCC 的 `-Wextra` 仍按
// `-Wmissing-field-initializers` 逐条报（本文件一百多条）。测量构建按
// docs/dev/PERFORMANCE.md 带 `-Werror` 编，这里只关这一条，别的诊断照旧。
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace {

constexpr int kStallTickMs = 20;
// 心跳停两拍半才算"卡住"：低于这个数多半是正常的一帧，采下来只是噪声。
constexpr int kStallSampleAfterMs = 50;
constexpr int kBetweenStepsMs = 150;
constexpr int kFinalSettleMs = 300;
/// 硬收尾闸：走到这一刻还没走完就**照样落盘**（只记走到哪、每步多少）。
///
/// 为什么要有：`tools/perf-startup.ps1` 只等 120 s，验收配置里那道 `closeAfterMs` 到点就
/// 把主窗关掉 —— 而中途关窗时 `finish()` 根本不会跑，整份报告就没了。网络卡在某一步的
/// 等待里、或者某一步弹出了提示，都会走到这一档：宁可记一份"走到第几步"的报告，
/// 也不要白跑一趟。（不改变任何一步的量法，只决定收不收尾。）
constexpr int kHardFinalizeMs = 100000;
// 首帧守卫的门槛。和 StallSampler 的采栈门槛同一个数，守卫红了可以直接对着卡顿栈看。
constexpr double kFirstFrameLimitMs = 50.0;

QString probeTarget(const QApplication &app)
{
    const QString prefix = QStringLiteral("--wtfml-perf-probe=");
    const QStringList arguments = app.arguments();
    for (const QString &argument : arguments) {
        if (argument.startsWith(prefix)) {
            return argument.mid(prefix.size());
        }
    }
    return QString();
}

QString probeArgument(const QApplication &app, const QString &name)
{
    const QString prefix = name + QLatin1Char('=');
    const QStringList arguments = app.arguments();
    for (const QString &argument : arguments) {
        if (argument.startsWith(prefix)) {
            return argument.mid(prefix.size());
        }
    }
    return QString();
}

/// 守卫只认"点一下 → 界面画出来"的表面。合成负载（tasks / presets / concurrent）、
/// 故意同步量的 `scan:java`、换主题（整窗重刷，不是开页）和等待那一半（`*-wait`）
/// 不算首帧。
bool isFirstFrameSurface(const QString &name)
{
    static const QStringList skippedPrefixes{QStringLiteral("scan:"), QStringLiteral("theme:"),
                                             QStringLiteral("tasks:"), QStringLiteral("presets:"),
                                             QStringLiteral("concurrent:")};
    for (const QString &prefix : skippedPrefixes) {
        if (name.startsWith(prefix)) {
            return false;
        }
    }
    return !name.endsWith(QStringLiteral("-wait"));
}

double milliseconds(const QElapsedTimer &timer)
{
    return static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
}

double round1(double value)
{
    return std::round(value * 10.0) / 10.0;
}

struct StepResult {
    QString name;
    double actionMs = 0.0;
    double repaintMs = 0.0;
    /// 这一步执行时后台目录加载是否还在跑。多任务那一组 A/B 靠它判断这一发到底算不算"加载中"。
    bool loadActive = false;
    /// 动作做完后是否还要等异步内容。等的时候界面在画什么（一行灰字 / 空列表 / 弹框）。
    bool hasWait = false;
    bool waitOk = false;
    double waitMs = 0.0;
    /// 内容到了之后整窗重画一遍的耗时（A 类的"内容首帧"）。
    double contentRepaintMs = 0.0;
    QString waitingText;
    /// 等待结束后量到的内容行数（树/列表），用来核对"等到的真的是内容"。
    int contentRows = -1;
    /// 动作阻塞在嵌套事件循环里时，从循环内部量的首帧（-1 = 没量）。
    double extraMs = -1.0;
    /// 这一发是不是首帧守卫要看的表面；是的话 `firstFrameMs` 才有意义。
    bool guarded = false;
    /// 点击 → 首帧：动作同步那一截 + 整窗重画一次；嵌套事件循环里量到的用那一个。
    /// 等异步内容（`waitMs`）不算在内。
    double firstFrameMs = 0.0;
};

class Probe final : public QObject {
public:
    Probe(QApplication &app, MainWindow &window, QString outPath)
        : QObject(&window)
        , m_app(app)
        , m_window(window)
        , m_outPath(std::move(outPath))
    {
    }

    ~Probe() override
    {
        // 中途关窗时 finish() 不会跑。采样线程要是还 joinable，进程退出时
        // std::thread 的析构会直接 terminate —— 这里兜住。
        StallSampler::stop();
    }

    void start()
    {
        m_themeBefore = Theme::current().presetId;
        m_fixtureId = detectFixtureId();
        buildSteps();
        loadFirstFrameAllowlist();
        QTimer::singleShot(kHardFinalizeMs, this, [this] {
            if (m_finished) {
                return;
            }
            m_hardFinalized = true;
            qWarning().noquote() << QStringLiteral("[perf] 到 %1 ms 还没走完，按已有的那几步落盘")
                                        .arg(kHardFinalizeMs);
            finish();
        });

        m_clock.start();
        m_lastTickMs = 0;
        // 采样器会把主线程暂停几毫秒。要分清"某一步慢"是产品本身的，还是采样器压出来的，
        // 就得能单独关掉它：`WTFML_PERF_NO_STALL_SAMPLER=1` 只停采样，看门狗（心跳 /
        // 最大间隔 / slowSpans）照旧。
        m_stallSamplerOn = qEnvironmentVariableIsEmpty("WTFML_PERF_NO_STALL_SAMPLER");
        if (m_stallSamplerOn) {
            StallSampler::start(kStallSampleAfterMs);
        }
        m_watchdog = new QTimer(this);
        m_watchdog->setTimerType(Qt::PreciseTimer);
        m_watchdog->setInterval(kStallTickMs);
        connect(m_watchdog, &QTimer::timeout, this, [this] {
            // 同时是采样线程的心跳：它停了多久 = 主线程被占住了多久。
            StallSampler::beat(static_cast<int>(m_next));
            const qint64 now = m_clock.elapsed();
            const qint64 gap = now - m_lastTickMs;
            m_lastTickMs = now;
            ++m_ticks;
            if (gap > m_maxGapMs) {
                m_maxGapMs = gap;
                m_maxGapFrom = m_lastTickStep;
                m_maxGapAt = m_currentStep;
            }
            if (gap > 50) {
                ++m_overFiftyMs;
                m_slowSpans.append(QJsonObject{{QStringLiteral("gapMs"), gap},
                    {QStringLiteral("fromStep"), m_lastTickStep}, {QStringLiteral("atStep"), m_currentStep}});
            }
            m_lastTickStep = m_currentStep;
            if (m_asyncCatalogPending) {
                ++m_asyncCatalogTicks;
                m_asyncCatalogMaxGap = std::max(m_asyncCatalogMaxGap, gap);
            }
        });
        m_watchdog->start();

        // 0 ms：等窗口真正上屏、第一轮布局/绘制跑完再开始发动作。
        QTimer::singleShot(0, this, [this] { runNext(); });
    }

private:
    /// 首帧允许名单：`键<TAB>值` 的纯文本（值 = 放行时记录的最大首帧，给人看）。
    /// 只认第一列，空行和 `#` 开头跳过。没传参数就不启用守卫（手动跑探针时仍然出数）。
    void loadFirstFrameAllowlist()
    {
        m_allowlistPath = probeArgument(m_app, QStringLiteral("--wtfml-firstframe-allowlist"));
        if (m_allowlistPath.isEmpty()) {
            return;
        }
        QFile file(m_allowlistPath);
        if (!file.open(QIODevice::ReadOnly)) {
            m_guardError = QStringLiteral("允许名单打不开：%1").arg(m_allowlistPath);
            return;
        }
        QString text = QString::fromUtf8(file.readAll());
        if (text.startsWith(QChar(0xFEFF))) {
            text.remove(0, 1);
        }
        for (const QString &raw : text.split(QLatin1Char('\n'))) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
                continue;
            }
            const int tab = line.indexOf(QLatin1Char('\t'));
            const QString key = (tab < 0 ? line : line.left(tab)).trimmed();
            if (key.isEmpty()) {
                continue;
            }
            m_allowlist.insert(key, tab < 0 ? QString() : line.mid(tab + 1).trimmed());
        }
        m_guardEnabled = true;
    }

    /// 夹具实例 = versions/ 下面模组数 >= 50 的那一份（`tools/perf-startup.ps1 -InstanceMods`）。
    /// 找不到就空串，实例页那几步自动跳过，报告里能看到 `instance:open-settings` 是空的。
    QString detectFixtureId() const
    {
        const QDir versions(QDir(AppPaths::minecraftDir()).filePath(QStringLiteral("versions")));
        for (const QString &id : versions.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            const QDir mods(versions.filePath(id + QStringLiteral("/mods")));
            if (mods.entryList({QStringLiteral("*.jar")}, QDir::Files).size() >= 50) {
                return id;
            }
        }
        return QString();
    }

    void buildSteps()
    {
        // 单独测扫描成本，保留优化前后相同的探针步骤。设置页现已后台扫描，下面这条
        // 刻意同步的测量不代表产品翻页路径；它会预热文件缓存，也会进入总看门狗峰值。
        // 不把 scan:java 的耗时或总峰值误当成优化后的设置页同步阻塞。
        m_steps.push_back({QStringLiteral("scan:java"),
                           [] { (void)GlobalJavaSettings::scanLocalJavaVersions(); }});

        // 顶上四颗导航：走 `TitleBar` 上那一颗真按钮（它底下连着 MainWindowUI 的翻页
        // 和 MainWindow 的 `onNavigationApplied`，懒加载都在这条路上）。
        m_steps.push_back({QStringLiteral("nav:home"),
                           [this] { clickNav(MainWindowUI::homeTabIndex()); }});
        m_steps.push_back({QStringLiteral("nav:instances"),
                           [this] { clickNav(MainWindowUI::instancesTabIndex()); }});
        m_steps.push_back({QStringLiteral("nav:download"),
                           [this] { clickNav(MainWindowUI::downloadTabIndex()); }});
        m_steps.push_back({QStringLiteral("nav:settings"),
                           [this] { clickNav(MainWindowUI::settingsTabIndex()); }});

        // 主题：设置页那个下拉框，用户改主题走的就是 `currentIndexChanged` → 控制器
        // 读当前项 → 落盘 + `Theme::apply()`。这里只改下标，不自己调 Theme。
        if (QComboBox *combo = themeCombo()) {
            const QStringList presets = Theme::presetIds();
            for (const QString &preset : presets) {
                const int index = combo->findData(preset);
                if (index < 0) {
                    continue;
                }
                m_steps.push_back({QStringLiteral("theme:%1").arg(preset),
                                   [combo, index] { combo->setCurrentIndex(index); }});
            }
        }

        // 收尾：回主页，并把主题还原成跑之前那一档。
        m_steps.push_back({QStringLiteral("nav:home-again"),
                           [this] { clickNav(MainWindowUI::homeTabIndex()); }});
        m_steps.push_back({QStringLiteral("theme:restore"),
                           [this] { restoreTheme(); }});

        // 再进一次设置页：`onMainTabChanged()` 每次进来都刷 Java 列表 / 内存那两行，
        // 但 `JavaSettingsController::refreshList()` 里那次本地 Java 扫描有 `m_scanned`
        // 挡着。第一下和这一下的差 = 只花一次的那笔钱（页面首次铺开 + 扫描）。
        m_steps.push_back({QStringLiteral("nav:settings-again"),
                           [this] { clickNav(MainWindowUI::settingsTabIndex()); }});
        m_steps.push_back({QStringLiteral("nav:home-final"),
                           [this] { clickNav(MainWindowUI::homeTabIndex()); }});

        // Synthetic view workload: real task widgets, no network/jobs/accounts.
        // Seed outside timed actions, then change just one running task per tick.
        for (int i = 0; i < 200; ++i) {
            TaskCenter::Item item;
            item.key = QStringLiteral("perf-task-%1").arg(i);
            item.title = QStringLiteral("性能测量任务 %1").arg(i);
            item.state = i < 20 ? TaskCenter::State::Running : TaskCenter::State::Succeeded;
            item.percent = i < 20 ? 10 : 100;
            item.detail = QStringLiteral("文件校验与资源准备");
            item.finishedAt = QDateTime::fromSecsSinceEpoch(1700000000 + i);
            item.capabilities.cancel = i < 20;
            item.steps = {{QStringLiteral("检查文件"), TaskCenter::StepState::Done, {}},
                          {QStringLiteral("准备资源"), i < 20 ? TaskCenter::StepState::Running : TaskCenter::StepState::Done, {}}};
            m_tasks.append(item);
        }
        m_steps.push_back({QStringLiteral("tasks:open-200"), [this] {
            m_taskPanel = new TaskCenterPanel(&m_window, [this] { return m_tasks; }, {}, {}, {}, {}, &m_window, true);
            m_taskPanel->setGeometry(m_window.rect().adjusted(12, 80, -12, -72));
            m_taskPanel->setRowActionHandler([](const QString &, TaskCenter::RowAction) {});
            m_taskPanel->setRowActivatedHandler([](const QString &) {});
            m_taskPanel->showEmbedded();
            m_taskPanel->raise();
        }});
        for (int tick = 0; tick < 20; ++tick) {
            m_steps.push_back({QStringLiteral("tasks:update-%1").arg(tick + 1, 2, 10, QLatin1Char('0')),
                               [this, tick] {
                m_tasks[tick].percent = 20 + tick;
                m_tasks[tick].detail = QStringLiteral("正在下载资源 / %1 MB/s").arg(tick + 1);
                if (m_taskPanel) m_taskPanel->refresh();
            }});
        }
        m_steps.push_back({QStringLiteral("tasks:close"), [this] { delete m_taskPanel.data(); }});

        for (int i = 0; i < 1000; ++i) {
            PresetManagePage::PresetItem item;
            item.id = QStringLiteral("perf-preset-%1").arg(i);
            item.name = QStringLiteral("%1预设 %2").arg(i % 2 ? QStringLiteral("生存") : QStringLiteral("建筑")).arg(i, 4, 10, QLatin1Char('0'));
            item.description = QStringLiteral("游戏设置与按键说明 %1").arg(i);
            item.categoryKey = i % 2 ? QStringLiteral("mods") : QStringLiteral("settings");
            item.categoryText = i % 2 ? QStringLiteral("模组") : QStringLiteral("游戏设置");
            item.sectionCount = 2;
            item.isDefault = i == 0;
            m_presets.append(item);
        }
        m_steps.push_back({QStringLiteral("presets:open-1000"), [this] {
            m_presetPage = new PresetManagePage(&m_window);
            m_presetPage->setWindowFlags(Qt::Widget);
            m_presetPage->setGeometry(m_window.rect().adjusted(12, 80, -12, -72));
            m_presetPage->setCategories({{QString(), QStringLiteral("全部"), true},
                                          {QStringLiteral("settings"), QStringLiteral("游戏设置"), false},
                                          {QStringLiteral("mods"), QStringLiteral("模组"), false}});
            m_presetPage->setPresets(m_presets, {}, m_presets.first().id);
            m_presetPage->show();
            m_presetPage->raise();
        }});
        const QStringList queries{QStringLiteral("预设"), QStringLiteral("生存"), QString(),
                                  QStringLiteral("002"), QStringLiteral("说明"), QStringLiteral("不匹配"), QString()};
        for (int i = 0; i < queries.size(); ++i) {
            m_steps.push_back({QStringLiteral("presets:search-%1").arg(i + 1), [this, query = queries.at(i)] {
                if (m_presetPage) {
                    auto *search = m_presetPage->findChild<QLineEdit *>(QStringLiteral("presetSearchEdit"));
                    if (search) search->setText(query);
                }
            }});
        }
        m_steps.push_back({QStringLiteral("presets:close"), [this] { delete m_presetPage.data(); }});
        m_steps.push_back({QStringLiteral("presets:prepare-store-1000"), [this] {
            m_storeDir = std::make_unique<QTemporaryDir>(AppPaths::minecraftDir() + QStringLiteral("/perf-presets-XXXXXX"));
            if (!m_storeDir->isValid()) { m_storeFailed = true; return; }
            Presets::PresetIndex index;
            for (const auto &item : m_presets) {
                Presets::PresetManifest manifest;
                manifest.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                manifest.name = item.name;
                manifest.description = item.description;
                manifest.category = Presets::PresetCategory::GameSettings;
                if (!Presets::saveManifest(m_storeDir->path(), manifest)) { m_storeFailed = true; return; }
                index.order.append(manifest.id);
            }
            m_storeFailed = !Presets::saveIndex(m_storeDir->path(), index);
        }});
        for (int i = 0; i < 3; ++i) {
            m_steps.push_back({QStringLiteral("presets:read-store-%1").arg(i + 1), [this] {
                if (m_storeFailed || !m_storeDir) { m_storeFailed = true; return; }
                QList<Presets::BrokenPreset> broken;
                m_storeRows = Presets::list(m_storeDir->path(), &broken).size();
                m_storeFailed = m_storeRows != 1000 || !broken.isEmpty();
            }});
        }
        // "下一次启动"那一趟：只清进程里那张摘要，盘上那份留着（第一趟读 manifest 时写的）。
        // 量的就是正文那段省掉之后的代价 —— 每份一次 stat + 读一份摘要文件。
        m_steps.push_back({QStringLiteral("presets:read-store-from-disk-cache"), [this] {
            if (m_storeFailed || !m_storeDir) { m_storeFailed = true; return; }
            Presets::forgetSummaryCache(m_storeDir->path(), true);
            QList<Presets::BrokenPreset> broken;
            m_storeRows = Presets::list(m_storeDir->path(), &broken).size();
            m_storeFailed = m_storeRows != 1000 || !broken.isEmpty();
        }});
        // 多任务负载：同一串翻页动作跑两趟 —— 先空载（对照），再在后台目录加载期间跑。
        // 真实使用里用户在预设页转圈时照样点翻页；旧探针会把这一步之后的动作全挡住
        // （`runNext()` 里等异步目录），那种重叠量不到。
        const QList<int> navCycle{MainWindowUI::homeTabIndex(), MainWindowUI::instancesTabIndex(),
                                  MainWindowUI::downloadTabIndex(), MainWindowUI::settingsTabIndex()};
        const auto addNavCycle = [this, &navCycle](const QString &prefix) {
            for (int round = 0; round < 2; ++round) {
                for (int i = 0; i < navCycle.size(); ++i) {
                    const int index = navCycle.at(i);
                    m_steps.push_back({QStringLiteral("%1-%2")
                                           .arg(prefix)
                                           .arg(round * navCycle.size() + i + 1, 2, 10, QLatin1Char('0')),
                                       [this, index] { clickNav(index); }});
                }
            }
        };
        addNavCycle(QStringLiteral("concurrent:calm"));

        m_steps.push_back({QStringLiteral("presets:start-background-store"), [this] {
            if (m_storeFailed || !m_storeDir) { m_storeFailed = true; return; }
            // 上面那三趟 `presets:read-store-*` 已经把摘要缓存捂热了，不丢的话这一趟
            // 11 ms 就回来，"加载中"那半组翻页就压在空载上，量不到东西。
            Presets::forgetSummaryCache(m_storeDir->path());
            m_asyncCatalogPending = true;
            m_asyncCatalogClock.start();
            m_catalogLoader = new Presets::CatalogLoader(this);
            m_catalogLoader->load(m_storeDir->path(), [this](const Presets::CatalogSnapshot &snapshot) {
                m_asyncCatalogElapsed = m_asyncCatalogClock.elapsed();
                m_asyncCatalogRows = snapshot.presets.size();
                m_storeFailed = m_asyncCatalogRows != 1000 || !snapshot.broken.isEmpty();
                m_asyncCatalogPending = false;
            });
        }});
        addNavCycle(QStringLiteral("concurrent:busy"));

        // 步骤 125：加载态盘点要走的那些点击路径（一级页二次、实例页签、下载分类、
        // 资源版本树、安装页加载器树、预设弹框、账号外观、新建服务端）。都排在合成
        // 负载之后，免得先把盘和字形缓存捂热再量"首次"。
        addLoadingPathSteps();
    }

    // ---------- 步骤 125：加载态盘点走的那些点击路径 ----------
    //
    // 每一步后面的 `ready` 是"内容到了没有"的判据：动作那一瞬间界面往往是空的，用户
    // 真正在等的是这一段时间。`waitingText` 记下等待时界面在显示什么（空白 / 灰字 /
    // 占位行），交给 `.tongxi/任务书/加载盘点.md` 的 A 清单用。
    void addLoadingPathSteps()
    {
        const auto addWait = [this](const QString &name, std::function<void()> action,
                                    std::function<bool()> ready, int timeoutMs,
                                    std::function<QString()> waitingText,
                                    std::function<int()> rows = {}) {
            Step step;
            step.name = name;
            step.action = std::move(action);
            step.ready = std::move(ready);
            step.timeoutMs = timeoutMs;
            step.waitingText = std::move(waitingText);
            step.contentRows = std::move(rows);
            m_steps.push_back(std::move(step));
        };

        // 一级页：智能体、工具箱（首次 + 再次）。主页/实例/下载/设置那四颗上面已经量过。
        m_steps.push_back({QStringLiteral("nav:agent"), [this] { clickNav(MainWindowUI::agentTabIndex()); }});
        m_steps.push_back({QStringLiteral("nav:toolbox"), [this] {
            if (QAbstractButton *button = m_window.ui()->getToolboxButton()) button->click();
        }});
        m_steps.push_back({QStringLiteral("nav:agent-again"), [this] { clickNav(MainWindowUI::agentTabIndex()); }});
        m_steps.push_back({QStringLiteral("nav:toolbox-again"), [this] {
            if (QAbstractButton *button = m_window.ui()->getToolboxButton()) button->click();
        }});
        // 工具箱里每件工具第一次进（工具页按需搭，见 ui/ToolboxPage.hpp）：从目录页点那一格。
        for (const ToolboxPage::Tool tool : ToolboxPage::tools()) {
            m_steps.push_back({QStringLiteral("toolbox:tool:%1").arg(ToolboxPage::toolKey(tool)), [this, tool] {
                auto *page = m_window.findChild<ToolboxPage *>();
                if (page == nullptr) return;
                page->showCatalog();
                if (auto *cell = page->findChild<QAbstractButton *>(ToolboxPage::toolCellName(tool))) cell->click();
            }});
        }
        m_steps.push_back({QStringLiteral("toolbox:catalog"), [this] {
            if (auto *page = m_window.findChild<ToolboxPage *>()) page->showCatalog();
        }});
        m_steps.push_back({QStringLiteral("nav:instances-again"), [this] { clickNav(MainWindowUI::instancesTabIndex()); }});

        // 切当前实例：主页那个「已安装版本」下拉框（和实例页的树双向同步）。
        m_steps.push_back({QStringLiteral("instance:select-second"), [this] {
            if (QComboBox *combo = m_window.ui()->getInstalledVersionCombo(); combo != nullptr && combo->count() > 1) {
                combo->setCurrentIndex(1);
            }
        }});
        m_steps.push_back({QStringLiteral("instance:select-fixture"), [this] {
            if (QComboBox *combo = m_window.ui()->getInstalledVersionCombo()) {
                const int index = combo->findData(m_fixtureId);
                if (index >= 0) combo->setCurrentIndex(index);
            }
        }});

        // ---------- 实例设置：打开 + 各页签（50+ 模组的夹具实例） ----------
        m_steps.push_back({QStringLiteral("instance:open-settings"), [this] {
            if (m_fixtureId.isEmpty()) return;
            QMetaObject::invokeMethod(&m_window, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, m_fixtureId), Q_ARG(QString, QString()));
        }});
        addWait(QStringLiteral("instance:open-settings-wait"),
                [] {},
                [this] {
                    m_instanceDialog = m_window.findChild<InstanceSettingsDialog *>();
                    return !m_instanceDialog.isNull() && m_instanceDialog->isVisible();
                },
                15000,
                [] { return QStringLiteral("实例页 → 点实例行：覆盖层还没出来（窗口保持原样）"); },
                [this] { return m_instanceDialog.isNull() ? -1 : m_instanceDialog->pageCount(); });

        const QStringList pageKeys = [] {
            QStringList keys = InstanceSettingsDialog::pinnedPageKeys();
            keys += InstanceSettingsDialog::filePageKeys();
            keys += InstanceSettingsDialog::lifecyclePageKeys();
            return keys;
        }();
        for (const QString &key : pageKeys) {
            // 只挑盘点点名的几页 + 成本明显的那几页；全量 23 页会把报告淹掉。
            static const QStringList wanted{QStringLiteral("overview"), QStringLiteral("launch"),
                                            QStringLiteral("mod"), QStringLiteral("resourcepack"),
                                            QStringLiteral("shader"), QStringLiteral("save"),
                                            QStringLiteral("screenshots"), QStringLiteral("logs"),
                                            QStringLiteral("datapack"), QStringLiteral("config")};
            if (!wanted.contains(key)) continue;
            m_steps.push_back({QStringLiteral("instance:page:%1").arg(key), [this, key] {
                if (!m_instanceDialog.isNull()) m_instanceDialog->showPage(key);
            }});
            if (key == QLatin1String("mod") || key == QLatin1String("screenshots")) {
                m_steps.push_back({QStringLiteral("instance:page:%1-again").arg(key), [this, key] {
                    if (!m_instanceDialog.isNull()) m_instanceDialog->showPage(QStringLiteral("overview"));
                }});
                m_steps.push_back({QStringLiteral("instance:page:%1-second").arg(key), [this, key] {
                    if (!m_instanceDialog.isNull()) m_instanceDialog->showPage(key);
                }});
            }
        }
        m_steps.push_back({QStringLiteral("instance:close"), [this] {
            if (m_window.ui()->overlay() != nullptr) m_window.ui()->overlay()->dismiss();
        }});
        addWait(QStringLiteral("instance:close-wait"), [] {},
                [this] { return m_window.findChild<InstanceSettingsDialog *>() == nullptr; }, 5000,
                [] { return QStringLiteral("收页"); });

        // 第二次打开：只为量"构造 + 首次铺开"那笔一次性成本是不是每次都付。
        m_steps.push_back({QStringLiteral("instance:open-settings-again"), [this] {
            if (m_fixtureId.isEmpty()) return;
            QMetaObject::invokeMethod(&m_window, "openInstanceSettings", Qt::DirectConnection,
                                      Q_ARG(QString, m_fixtureId), Q_ARG(QString, QString()));
        }});
        addWait(QStringLiteral("instance:open-settings-again-wait"), [] {},
                [this] {
                    m_instanceDialog = m_window.findChild<InstanceSettingsDialog *>();
                    return !m_instanceDialog.isNull() && m_instanceDialog->isVisible();
                },
                15000, [] { return QStringLiteral("实例设置第二开"); });
        m_steps.push_back({QStringLiteral("instance:close-again"), [this] {
            if (m_window.ui()->overlay() != nullptr) m_window.ui()->overlay()->dismiss();
        }});
        addWait(QStringLiteral("instance:close-again-wait"), [] {},
                [this] { return m_window.findChild<InstanceSettingsDialog *>() == nullptr; }, 5000,
                [] { return QStringLiteral("收页"); });

        // ---------- 下载页 ----------
        m_steps.push_back({QStringLiteral("download:open"), [this] { clickNav(MainWindowUI::downloadTabIndex()); }});
        addWait(QStringLiteral("download:cat-game"),
                [this] {
                    if (QListWidget *list = m_window.ui()->getDownloadCategoryList()) {
                        list->setCurrentRow(MainWindowUI::downloadCategoryGameIndex());
                    }
                    // 走页头那颗「刷新」——它就是 refreshDownloadCategory(currentRow())，
                    // 也就是用户重新拉一遍在线版本清单的那一下。
                    if (QAbstractButton *refresh = m_window.ui()->getDownloadHeaderRefreshButton()) {
                        refresh->click();
                    }
                },
                [this] { return versionTreeReady(); }, 15000,
                [this] { return versionTreeWaitingText(); },
                [this] { return m_window.ui()->getVersionTree() == nullptr ? -1 : m_window.ui()->getVersionTree()->topLevelItemCount(); });

        const auto categoryStep = [this, &addWait](const QString &name, int row, const QString &key) {
            addWait(name, [this, row] {
                if (QListWidget *list = m_window.ui()->getDownloadCategoryList()) list->setCurrentRow(row);
            }, [this, key] { return resourceTreeReady(key); }, 12000,
            [this, key] { return resourceTreeWaitingText(key); },
            [this, key] {
                QTreeWidget *tree = m_window.ui()->getDownloadResourceTree(key);
                return tree == nullptr ? -1 : tree->topLevelItemCount();
            });
        };
        categoryStep(QStringLiteral("download:cat-mods"), MainWindowUI::downloadCategoryModIndex(), QStringLiteral("mod"));
        addWait(QStringLiteral("download:cat-game-back"),
                [this] { if (QListWidget *list = m_window.ui()->getDownloadCategoryList()) list->setCurrentRow(MainWindowUI::downloadCategoryGameIndex()); },
                [] { return true; }, 1000, [] { return QString(); });
        categoryStep(QStringLiteral("download:cat-mods-again"), MainWindowUI::downloadCategoryModIndex(), QStringLiteral("mod"));
        addWait(QStringLiteral("download:search-sodium"), [this] {
            QTreeWidget *tree = m_window.ui()->getDownloadResourceTree(QStringLiteral("mod"));
            if (tree == nullptr) return;
            (void)tree;
            if (QLineEdit *edit = m_window.ui()->getDownloadSearchEdit(QStringLiteral("mod"))) {
                edit->setText(QStringLiteral("sodium"));
                QMetaObject::invokeMethod(edit, "returnPressed", Qt::DirectConnection);
            }
        }, [this] { return resourceTreeReady(QStringLiteral("mod")); }, 12000,
        [this] { return resourceTreeWaitingText(QStringLiteral("mod")); },
        [this] {
            QTreeWidget *tree = m_window.ui()->getDownloadResourceTree(QStringLiteral("mod"));
            return tree == nullptr ? -1 : tree->topLevelItemCount();
        });
        addWait(QStringLiteral("download:expand-resource-versions"), [this] {
            QTreeWidget *tree = m_window.ui()->getDownloadResourceTree(QStringLiteral("mod"));
            if (tree == nullptr || tree->topLevelItemCount() == 0) return;
            QTreeWidgetItem *hit = tree->topLevelItem(0);
            hit->setExpanded(true);
            if (hit->childCount() > 0) {
                QTreeWidgetItem *game = hit->child(0);
                // 不要再自己 setExpanded(true)：处理函数看到"已经展开"就当这一下是收起，
                // 直接返回、不再发版本请求。单击那一下本来就会展开它。
                QMetaObject::invokeMethod(tree, "itemClicked", Qt::DirectConnection,
                                          Q_ARG(QTreeWidgetItem *, game), Q_ARG(int, 0));
            }
        }, [this] {
            QTreeWidget *tree = m_window.ui()->getDownloadResourceTree(QStringLiteral("mod"));
            if (tree == nullptr || tree->topLevelItemCount() == 0) return false;
            QTreeWidgetItem *hit = tree->topLevelItem(0);
            if (hit->childCount() == 0) return false;
            QTreeWidgetItem *game = hit->child(0);
            return game->childCount() > 0 && !game->child(0)->isDisabled();
        }, 15000, [this] {
            QTreeWidget *tree = m_window.ui()->getDownloadResourceTree(QStringLiteral("mod"));
            if (tree == nullptr || tree->topLevelItemCount() == 0) return QStringLiteral("(空白)");
            QTreeWidgetItem *hit = tree->topLevelItem(0);
            if (hit->childCount() == 0) return QStringLiteral("(没有游戏版本层)");
            QTreeWidgetItem *game = hit->child(0);
            return game->childCount() == 0 ? QStringLiteral("(游戏版本层底下空白)")
                                           : game->child(0)->text(MainWindowUI::versionTreeKeyColumn());
        }, [this] {
            QTreeWidget *tree = m_window.ui()->getDownloadResourceTree(QStringLiteral("mod"));
            if (tree == nullptr || tree->topLevelItemCount() == 0) return -1;
            QTreeWidgetItem *hit = tree->topLevelItem(0);
            return hit->childCount() == 0 || hit->child(0)->childCount() == 0
                       ? -1 : hit->child(0)->childCount();
        });

        for (const auto &entry : {std::pair{MainWindowUI::downloadCategoryModpackIndex(), QStringLiteral("modpack")},
                                  std::pair{5, QStringLiteral("resourcepack")},
                                  std::pair{6, QStringLiteral("shader")},
                                  std::pair{4, QStringLiteral("datapack")}}) {
            categoryStep(QStringLiteral("download:cat:%1").arg(entry.second), entry.first, entry.second);
            addWait(QStringLiteral("download:cat:game-away:%1").arg(entry.second),
                    [this] { if (QListWidget *list = m_window.ui()->getDownloadCategoryList()) list->setCurrentRow(MainWindowUI::downloadCategoryGameIndex()); },
                    [] { return true; }, 1000, [] { return QString(); });
            categoryStep(QStringLiteral("download:cat:%1-again").arg(entry.second), entry.first, entry.second);
        }

        // ---------- 安装页：四个加载器的版本树（进页面时一次性全问） ----------
        addWait(QStringLiteral("install:open"), [this] {
            m_gameVersion = pickGameVersion();
            if (InstallPageController *controller = installPage()) {
                if (!m_gameVersion.isEmpty()) controller->openPage(m_gameVersion);
            }
        }, [this] { return loaderVersionsFilled() > 0; }, 15000,
        [this] { return loaderTreeWaitingText(); },
        [this] { return loaderVersionsFilled(); });
        addWait(QStringLiteral("install:all-loaders"), [] {},
                [this] { return loaderVersionsFilled() >= 4; }, 15000,
                [this] { return loaderTreeWaitingText(); },
                [this] { return loaderVersionsFilled(); });
        for (int loader = 0; loader < 4; ++loader) {
            m_steps.push_back({QStringLiteral("install:expand-loader-%1").arg(loader), [this, loader] {
                QTreeWidget *tree = m_window.ui()->getLoaderTree();
                if (tree == nullptr) return;
                for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                    QTreeWidgetItem *item = tree->topLevelItem(i);
                    if (item->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toInt() == loader) {
                        item->setExpanded(true);
                    }
                }
            }});
        }
        // 预设弹框：`openPresetPicker()` 在 128 之后只发后台快照请求，数据回来才摆框
        // （`execPrompt` 在那条回调里阻塞）。所以这一发不能再用"动作里阻塞多久"当首帧：
        // 记点击 → `PresetInstallPicker` 真上屏的那一段（`extra`），弹出来就自己按取消，
        // 免得覆盖层留着把后头的账号弹框挡掉（那一步会一直等不到）。
        {
            Step step;
            step.name = QStringLiteral("install:preset-picker");
            step.action = [this] {
                m_promptShownMs = -1.0;
                m_promptClock.start();
                // 步骤 136 起弹框一点就出，`execPrompt` 就阻塞在这一下里：只能在它的嵌套循环里
                // 看弹框上屏、再自己按取消（等动作返回再看就互相等住，整趟跑到超时）。
                auto *poll = new QTimer(this);
                poll->setInterval(1);
                connect(poll, &QTimer::timeout, this, [this, poll] {
                    auto *picker = m_window.findChild<PresetInstallPicker *>();
                    const bool shown = picker != nullptr && picker->isVisible();
                    if (!shown && m_promptClock.elapsed() < 5000) return;
                    if (shown) m_promptShownMs = milliseconds(m_promptClock);
                    poll->stop();
                    poll->deleteLater();
                    if (m_window.ui()->overlay() != nullptr) m_window.ui()->overlay()->dismiss();
                });
                poll->start();
                if (InstallPageController *controller = installPage()) {
                    controller->openPresetPicker();
                }
            };
            step.ready = [this] { return m_promptShownMs >= 0.0; };
            step.timeoutMs = 5000;
            step.waitingText = [] {
                return QStringLiteral("安装页「预设」：后台读完预设快照才摆弹框");
            };
            step.extra = [this] { return m_promptShownMs; };
            m_steps.push_back(std::move(step));
        }

        // ---------- 账号与外观（皮肤 / 披风） ----------
        addWait(QStringLiteral("account:open"), [this] {
            QMetaObject::invokeMethod(&m_window, "onAccountSkinButtonClicked", Qt::DirectConnection);
        }, [this] {
            m_accountDialog = m_window.findChild<AccountSkinDialog *>();
            return !m_accountDialog.isNull() && m_accountDialog->isVisible();
        }, 5000, [] { return QStringLiteral("主页右上账号胶囊 → 账号与外观"); },
        [this] {
            if (m_accountDialog.isNull()) return -1;
            auto *list = m_accountDialog->findChild<QListWidget *>(AccountSkinDialog::accountListName());
            return list == nullptr ? -1 : list->count();
        });
        addWait(QStringLiteral("account:cape-list"), [this] {
            if (!m_accountDialog.isNull()) {
                auto *list = m_accountDialog->findChild<QListWidget *>(AccountSkinDialog::capeListName());
                if (list != nullptr) list->setFocus();
            }
        }, [this] {
            if (m_accountDialog.isNull()) return false;
            auto *list = m_accountDialog->findChild<QListWidget *>(AccountSkinDialog::capeListName());
            return list != nullptr;
        }, 3000, [] { return QStringLiteral("同一个弹框里的披风列表"); },
        [this] {
            if (m_accountDialog.isNull()) return -1;
            auto *list = m_accountDialog->findChild<QListWidget *>(AccountSkinDialog::capeListName());
            return list == nullptr ? -1 : list->count();
        });
        m_steps.push_back({QStringLiteral("account:close"), [this] {
            if (m_window.ui()->overlay() != nullptr) m_window.ui()->overlay()->dismiss();
        }});

        // ---------- 新建服务端：服务端版本 / 加载器版本 ----------
        addWait(QStringLiteral("server:open"), [this] {
            m_gameVersion = pickGameVersion();
            if (QPushButton *button = m_window.ui()->getDownloadNewServerButton()) {
                button->setProperty("selectedVersion", m_gameVersion);
                button->click();
            }
        }, [this] {
            m_serverPage = m_window.findChild<NewServerPage *>();
            return !m_serverPage.isNull() && m_serverPage->isVisible();
        }, 6000, [] { return QStringLiteral("下载 → 服务端 → 新建服务端"); },
        [this] { return m_serverPage.isNull() ? -1 : m_serverPage->findChildren<QComboBox *>().size(); });
        // 默认那一档（原版）既不要加载器也不要安装器，两个版本框根本不会去取数 —— 可它们
        // 的占位字是构造时写死的「正在获取…」。先挑一档要加载器的（Fabric），量的才是真的等待。
        m_steps.push_back({QStringLiteral("server:flavor-fabric"), [this] {
            if (m_serverPage.isNull()) return;
            auto *list = m_serverPage->findChild<QListWidget *>(MainWindowUI::newServerFlavorListName());
            if (list == nullptr) return;
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->data(Qt::UserRole).toString() == QStringLiteral("fabric")) {
                    list->setCurrentRow(i);
                    return;
                }
            }
        }});
        addWait(QStringLiteral("server:versions"), [] {},
                [this] {
                    if (m_serverPage.isNull()) return false;
                    auto *loaderEdit = m_serverPage->findChild<QLineEdit *>(MainWindowUI::newServerLoaderEditName());
                    auto *installerEdit = m_serverPage->findChild<QLineEdit *>(MainWindowUI::newServerInstallerEditName());
                    auto *loaderCombo = serverVersionCombo(MainWindowUI::newServerLoaderEditName());
                    auto *installerCombo = serverVersionCombo(MainWindowUI::newServerInstallerEditName());
                    Q_UNUSED(loaderEdit);
                    Q_UNUSED(installerEdit);
                    return (loaderCombo != nullptr && loaderCombo->count() > 0)
                        || (installerCombo != nullptr && installerCombo->count() > 0);
                }, 12000, [this] {
                    auto *loaderEdit = m_serverPage.isNull() ? nullptr
                        : m_serverPage->findChild<QLineEdit *>(MainWindowUI::newServerLoaderEditName());
                    auto *installerEdit = m_serverPage.isNull() ? nullptr
                        : m_serverPage->findChild<QLineEdit *>(MainWindowUI::newServerInstallerEditName());
                    return QStringLiteral("加载器框「%1」/ 安装器框「%2」")
                        .arg(loaderEdit != nullptr ? loaderEdit->placeholderText() : QStringLiteral("?"),
                             installerEdit != nullptr ? installerEdit->placeholderText() : QStringLiteral("?"));
                },
                [this] {
                    int filled = 0;
                    for (QComboBox *combo : m_serverPage.isNull() ? QList<QComboBox *>{}
                                                                 : m_serverPage->findChildren<QComboBox *>()) {
                        if (combo->count() > 0) ++filled;
                    }
                    return filled;
                });
        m_steps.push_back({QStringLiteral("server:close"), [this] {
            if (QPushButton *back = m_window.ui()->getServerFormBackButton()) back->click();
        }});
    }

    QComboBox *serverVersionCombo(const QString &editName) const
    {
        if (m_serverPage.isNull()) return nullptr;
        return m_serverPage->findChild<QComboBox *>(editName + QStringLiteral("Combo"));
    }

    bool versionTreeReady() const
    {
        QTreeWidget *tree = m_window.ui()->getVersionTree();
        if (tree == nullptr || tree->topLevelItemCount() == 0) return false;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            if (tree->topLevelItem(i)->childCount() > 0) return true;
        }
        return false;
    }

    QString versionTreeWaitingText() const
    {
        QTreeWidget *tree = m_window.ui()->getVersionTree();
        if (tree == nullptr) return QStringLiteral("(没有版本树)");
        if (tree->topLevelItemCount() == 0) return QStringLiteral("(空白版本树)");
        return QStringLiteral("%1（顶层 %2 条）")
            .arg(tree->topLevelItem(0)->text(MainWindowUI::versionTreeKeyColumn()))
            .arg(tree->topLevelItemCount());
    }

    QString pickGameVersion() const
    {
        QTreeWidget *tree = m_window.ui()->getVersionTree();
        if (tree == nullptr) return QString();
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *category = tree->topLevelItem(i);
            for (int j = 0; j < category->childCount(); ++j) {
                const QString id = category->child(j)
                                       ->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole)
                                       .toString();
                if (!id.isEmpty()) return id;
            }
        }
        return QString();
    }

    bool resourceTreeReady(const QString &key) const
    {
        QTreeWidget *tree = m_window.ui()->getDownloadResourceTree(key);
        if (tree == nullptr || tree->topLevelItemCount() == 0) return false;
        QTreeWidgetItem *first = tree->topLevelItem(0);
        if (first->isDisabled()) return false;
        return !first->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toString().isEmpty();
    }

    QString resourceTreeWaitingText(const QString &key) const
    {
        QTreeWidget *tree = m_window.ui()->getDownloadResourceTree(key);
        if (tree == nullptr) return QStringLiteral("(没有这张表)");
        if (tree->topLevelItemCount() == 0) return QStringLiteral("(空白列表)");
        QTreeWidgetItem *first = tree->topLevelItem(0);
        return QStringLiteral("%1%2")
            .arg(first->text(MainWindowUI::versionTreeKeyColumn()),
                 first->isDisabled() ? QStringLiteral("（灰）") : QString());
    }

    int loaderVersionsFilled() const
    {
        QTreeWidget *tree = m_window.ui()->getLoaderTree();
        if (tree == nullptr) return 0;
        int filled = 0;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = tree->topLevelItem(i);
            const int loader = item->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toInt();
            if (loader < 0 || item->childCount() == 0) continue;
            if (item->child(0)->text(MainWindowUI::versionTreeKeyColumn())
                != MainWindowUI::loaderVersionFetchingText()) {
                ++filled;
            }
        }
        return filled;
    }

    QString loaderTreeWaitingText() const
    {
        QTreeWidget *tree = m_window.ui()->getLoaderTree();
        if (tree == nullptr) return QStringLiteral("(没有加载器树)");
        QStringList parts;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = tree->topLevelItem(i);
            const int loader = item->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toInt();
            if (loader < 0) continue;
            parts.append(item->childCount() == 0
                             ? QStringLiteral("%1:空").arg(item->text(MainWindowUI::versionTreeKeyColumn()))
                             : QStringLiteral("%1:%2").arg(
                                   item->text(MainWindowUI::versionTreeKeyColumn()),
                                   item->child(0)->text(MainWindowUI::versionTreeKeyColumn())));
        }
        return parts.join(QStringLiteral(" / "));
    }

    /// `InstallPageController` 没有 Q_OBJECT（不是信号槽那一套），`findChild<T>()` 用不了；
    /// 它是 MainWindow 的子对象，按类型在直接子对象里认。
    InstallPageController *installPage() const
    {
        for (QObject *child : m_window.children()) {
            if (auto *controller = dynamic_cast<InstallPageController *>(child)) {
                return controller;
            }
        }
        return nullptr;
    }

    QComboBox *themeCombo() const
    {
        return m_window.findChild<QComboBox *>(MainWindowUI::themePresetComboName());
    }

    TitleBar *titleBar() const { return m_window.findChild<TitleBar *>(); }

    void clickNav(int index)
    {
        TitleBar *bar = titleBar();
        if (bar == nullptr) {
            return;
        }
        if (QAbstractButton *button = bar->navButton(index)) {
            button->click();
        }
    }

    void restoreTheme()
    {
        QComboBox *combo = themeCombo();
        if (combo == nullptr) {
            return;
        }
        const int index = combo->findData(m_themeBefore);
        if (index >= 0) {
            combo->setCurrentIndex(index);
        }
    }

    void runNext()
    {
        if (m_next >= m_steps.size()) {
            // 步骤走完了。后台那趟目录加载还没回来的话，等它收尾再落盘（有上限），
            // 这样报告里的 elapsedMs / guiTicks 才是完整的一趟。
            if (m_asyncCatalogPending) {
                if (m_asyncCatalogClock.elapsed() > 30000) {
                    m_catalogLoader->invalidate();
                    m_storeFailed = true;
                    m_asyncCatalogPending = false;
                } else {
                    QTimer::singleShot(10, this, [this] { runNext(); });
                    return;
                }
            }
            QTimer::singleShot(kFinalSettleMs, this, [this] { finish(); });
            return;
        }

        const Step &step = m_steps[static_cast<int>(m_next)];
        m_currentStep = step.name;
        StepResult result;
        result.name = step.name;
        result.loadActive = m_catalogLoader != nullptr && m_catalogLoader->isLoading();

        QElapsedTimer timer;
        timer.start();
        step.action();
        result.actionMs = milliseconds(timer);
        if (step.extra) result.extraMs = step.extra();

        timer.restart();
        m_window.repaint();
        result.repaintMs = milliseconds(timer);

        // A 类：动作只把请求发出去，内容还在路上。这一段用嵌套事件循环等，看门狗和
        // StallSampler 照常跑，所以"等的时候界面有没有被占住"也能采到。
        if (step.ready) {
            result.hasWait = true;
            result.waitingText = step.waitingText ? step.waitingText() : QString();
            QElapsedTimer waitClock;
            waitClock.start();
            const int timeout = step.timeoutMs > 0 ? step.timeoutMs : 15000;
            // 总预算闸：`tools/perf-startup.ps1` 只等 120 s，网络卡住时宁可这一步记
            // `waitOk=false`，也不能把整份报告拖没。
            const qint64 elapsed = m_clock.elapsed();
            const int budget = elapsed < 85000 ? int(85000 - elapsed) : 1000;
            QEventLoop loop;
            QTimer poll;
            poll.setTimerType(Qt::PreciseTimer);
            poll.setInterval(10);
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (step.ready() || waitClock.elapsed() >= std::min(timeout, budget)) loop.quit();
            });
            if (!step.ready()) {
                poll.start();
                loop.exec();
                poll.stop();
            }
            result.waitMs = milliseconds(waitClock);
            result.waitOk = step.ready();
            result.contentRows = step.contentRows ? step.contentRows() : -1;
            timer.restart();
            m_window.repaint();
            result.contentRepaintMs = milliseconds(timer);
            // 异步才摆出来的东西（例如后台快照到位后才上的预设弹框）在等的过程里才量得到
            // 首帧：动作刚回来时它还不在，这里补读一次。
            if (step.extra) {
                const double extra = step.extra();
                if (extra >= 0.0) result.extraMs = extra;
            }
        }

        result.guarded = isFirstFrameSurface(result.name);
        // 首帧 = 动作同步那一截 + 整窗重画一次；嵌套事件循环里能量到首帧的用它。
        // `waitMs`（等异步内容）在动作和重画之后，天然不计入。
        result.firstFrameMs = result.extraMs >= 0 ? result.extraMs : result.actionMs + result.repaintMs;

        m_results.push_back(result);
        ++m_next;

        QTimer::singleShot(kBetweenStepsMs, this, [this] { runNext(); });
    }

    void finish()
    {
        // 走完和硬收尾闸可能同时到：只落一次盘。
        if (m_finished) {
            return;
        }
        m_finished = true;
        m_watchdog->stop();
        StallSampler::stop();

        QJsonArray steps;
        for (const StepResult &result : std::as_const(m_results)) {
            QJsonObject entry;
            entry.insert(QStringLiteral("name"), result.name);
            entry.insert(QStringLiteral("actionMs"), round1(result.actionMs));
            entry.insert(QStringLiteral("repaintMs"), round1(result.repaintMs));
            entry.insert(QStringLiteral("loadActive"), result.loadActive);
            if (result.extraMs >= 0) {
                entry.insert(QStringLiteral("nestedFirstFrameMs"), round1(result.extraMs));
            }
            if (result.guarded) {
                entry.insert(QStringLiteral("firstFrameMs"), round1(result.firstFrameMs));
                entry.insert(QStringLiteral("firstFrameWhitelisted"),
                             m_allowlist.contains(result.name));
            }
            if (result.hasWait) {
                entry.insert(QStringLiteral("waitMs"), round1(result.waitMs));
                entry.insert(QStringLiteral("waitOk"), result.waitOk);
                entry.insert(QStringLiteral("contentRepaintMs"), round1(result.contentRepaintMs));
                entry.insert(QStringLiteral("waitingText"), result.waitingText);
                entry.insert(QStringLiteral("contentRows"), result.contentRows);
            }
            steps.append(entry);
        }

        // ---- 首帧守卫：超过 50 ms 的表面要么在允许名单里，要么整趟失败 ----
        QJsonArray overLimit;
        QJsonArray whitelistedOver;
        QJsonArray staleWhitelist;
        QSet<QString> seenGuarded;
        int guardedSurfaces = 0;
        bool guardFailed = false;
        for (const StepResult &result : std::as_const(m_results)) {
            if (!result.guarded) {
                continue;
            }
            ++guardedSurfaces;
            seenGuarded.insert(result.name);
            const bool over = result.firstFrameMs > kFirstFrameLimitMs;
            const bool allowed = m_allowlist.contains(result.name);
            QJsonObject detail{
                {QStringLiteral("name"), result.name},
                {QStringLiteral("firstFrameMs"), round1(result.firstFrameMs)},
                {QStringLiteral("actionMs"), round1(result.actionMs)},
                {QStringLiteral("repaintMs"), round1(result.repaintMs)}};
            if (result.extraMs >= 0) {
                detail.insert(QStringLiteral("nestedFirstFrameMs"), round1(result.extraMs));
            }
            if (over && allowed) {
                detail.insert(QStringLiteral("allowlistValue"), m_allowlist.value(result.name));
                whitelistedOver.append(detail);
            } else if (over) {
                overLimit.append(detail);
                guardFailed = true;
                qWarning().noquote() << QStringLiteral("[first-frame] %1 首帧 %2 ms（动作 %3 + 重画 %4），超过 %5 ms 且不在允许名单")
                                            .arg(result.name)
                                            .arg(round1(result.firstFrameMs), 0, 'f', 1)
                                            .arg(round1(result.actionMs), 0, 'f', 1)
                                            .arg(round1(result.repaintMs), 0, 'f', 1)
                                            .arg(kFirstFrameLimitMs, 0, 'f', 0);
            } else if (allowed) {
                staleWhitelist.append(detail);
            }
        }
        for (auto it = m_allowlist.cbegin(); it != m_allowlist.cend(); ++it) {
            if (!seenGuarded.contains(it.key())) {
                staleWhitelist.append(QJsonObject{{QStringLiteral("name"), it.key()},
                                                  {QStringLiteral("reason"), QStringLiteral("这趟没有跑到的表面")}});
            }
        }
        if (!m_guardEnabled && m_guardError.isEmpty()) {
            // 没给允许名单 = 手动跑探针，只出数不判失败；给了却读不开是配置错，要红。
            guardFailed = false;
        }

        QJsonObject firstFrameGuard;
        firstFrameGuard.insert(QStringLiteral("thresholdMs"), kFirstFrameLimitMs);
        firstFrameGuard.insert(QStringLiteral("enabled"), m_guardEnabled);
        firstFrameGuard.insert(QStringLiteral("allowlistPath"),
                               m_allowlistPath.isEmpty() ? QJsonValue() : QJsonValue(m_allowlistPath));
        if (!m_guardError.isEmpty()) {
            firstFrameGuard.insert(QStringLiteral("error"), m_guardError);
        }
        firstFrameGuard.insert(QStringLiteral("surfaces"), guardedSurfaces);
        firstFrameGuard.insert(QStringLiteral("overLimit"), overLimit);
        firstFrameGuard.insert(QStringLiteral("whitelisted"), whitelistedOver);
        firstFrameGuard.insert(QStringLiteral("staleWhitelist"), staleWhitelist);
        firstFrameGuard.insert(QStringLiteral("failed"), guardFailed);

        QJsonObject stalls;
        stalls.insert(QStringLiteral("tickMs"), kStallTickMs);
        stalls.insert(QStringLiteral("ticks"), m_ticks);
        stalls.insert(QStringLiteral("maxGapMs"), m_maxGapMs);
        stalls.insert(QStringLiteral("maxStallMs"),
                      std::max<qint64>(0, m_maxGapMs - kStallTickMs));
        stalls.insert(QStringLiteral("over50ms"), m_overFiftyMs);
        stalls.insert(QStringLiteral("maxGapFromStep"), m_maxGapFrom);
        stalls.insert(QStringLiteral("maxGapAtStep"), m_maxGapAt);
        stalls.insert(QStringLiteral("slowSpans"), m_slowSpans);

        // 卡住那一刻的调用栈。上面那些只有"卡了多久"，这里回答"卡在哪一行"：
        // 本程序的帧是 `exe+0xRVA`（姓名交给 tools/symbolize-stalls.ps1），
        // 系统 DLL 的帧带 dbghelp 认得出来的函数名。
        QJsonArray stallStacks;
        for (const QJsonObject &sample : StallSampler::samples()) {
            QJsonObject entry = sample;
            const int index = entry.value(QStringLiteral("stepIndex")).toInt(-1);
            entry.remove(QStringLiteral("stepIndex"));
            entry.insert(QStringLiteral("step"),
                         index >= 0 && index < m_steps.size() ? m_steps[index].name
                                                              : QStringLiteral("startup"));
            stallStacks.append(entry);
        }
        stalls.insert(QStringLiteral("samples"), stallStacks);

        QJsonObject root;
        root.insert(QStringLiteral("schema"), 1);
        root.insert(QStringLiteral("kind"), QStringLiteral("ui-probe"));
        root.insert(QStringLiteral("probe"), QStringLiteral("WTFML_PERF_PROBE=1"));
        root.insert(QStringLiteral("stallSampler"), m_stallSamplerOn);
        root.insert(QStringLiteral("dataDir"), AppPaths::minecraftDir());
        root.insert(QStringLiteral("appVersion"), m_app.applicationVersion());
        root.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
        root.insert(QStringLiteral("themeBefore"), m_themeBefore);
        // 这一趟是不是**没走完**就被硬收尾闸落盘的（见 kHardFinalizeMs）：那时 `steps` 只到
        // 卡住的那一步，后面的步骤不在报告里，别当成"跑过了、没问题"。
        root.insert(QStringLiteral("hardFinalized"), m_hardFinalized);
        root.insert(QStringLiteral("steps"), steps);
        root.insert(QStringLiteral("taskWorkload"), QJsonObject{
            {QStringLiteral("source"), QStringLiteral("synthetic-view-only")},
            {QStringLiteral("rows"), 200}, {QStringLiteral("running"), 20},
            {QStringLiteral("updates"), 20}, {QStringLiteral("changedPerUpdate"), 1}});
        root.insert(QStringLiteral("presetWorkload"), QJsonObject{
            {QStringLiteral("source"), QStringLiteral("synthetic-view-only")},
            {QStringLiteral("rows"), 1000}, {QStringLiteral("queries"), 7}});
        root.insert(QStringLiteral("presetStoreWorkload"), QJsonObject{
            {QStringLiteral("source"), QStringLiteral("synthetic-disk-metadata-only")},
            {QStringLiteral("expectedRows"), 1000}, {QStringLiteral("actualRows"), m_storeRows},
            {QStringLiteral("ok"), !m_storeFailed}, {QStringLiteral("reads"), 3}});
        // 多任务那一组：同一串翻页动作，空载一趟 / 后台目录加载中一趟。两趟都在同一份
        // 报告里，别拿别的构建、别的机器上的数来比。`busyDuringLoad` 说明"加载中"那
        // 一趟里有几发点下去时目录还在读 —— 少于总数的话，剩下那几发其实已经空载了。
        double calmSum = 0.0;
        double calmMax = 0.0;
        double busySum = 0.0;
        double busyMax = 0.0;
        int calmCount = 0;
        int busyCount = 0;
        int busyDuringLoad = 0;
        for (const StepResult &result : std::as_const(m_results)) {
            if (result.name.startsWith(QStringLiteral("concurrent:calm-"))) {
                calmSum += result.actionMs;
                calmMax = std::max(calmMax, result.actionMs);
                ++calmCount;
            } else if (result.name.startsWith(QStringLiteral("concurrent:busy-"))) {
                // 只把"目录还在读"的那几发算进 `busy`：加载从 2.3 s 缩到 0.4 s 之后
                // （2026-11-01 预设并行读盘），8 发里后几发其实已经空载了 —— 混进平均数
                // 只会把 `busy` 拉回 `calm`，看着"多任务不挡事"，其实那几发根本没压在窗口上。
                if (result.loadActive) {
                    busySum += result.actionMs;
                    busyMax = std::max(busyMax, result.actionMs);
                    ++busyCount;
                    ++busyDuringLoad;
                }
            }
        }
        root.insert(QStringLiteral("concurrentWorkload"), QJsonObject{
            {QStringLiteral("source"), QStringLiteral("nav-cycle")},
            {QStringLiteral("calm"), QJsonObject{
                {QStringLiteral("count"), calmCount},
                {QStringLiteral("avgMs"), calmCount > 0 ? round1(calmSum / calmCount) : 0.0},
                {QStringLiteral("maxMs"), round1(calmMax)}}},
            {QStringLiteral("busy"), QJsonObject{
                {QStringLiteral("count"), busyCount},
                {QStringLiteral("avgMs"), busyCount > 0 ? round1(busySum / busyCount) : 0.0},
                {QStringLiteral("maxMs"), round1(busyMax)}}},
            {QStringLiteral("busyDuringLoad"), busyDuringLoad},
            {QStringLiteral("catalogElapsedMs"), m_asyncCatalogElapsed}});
        root.insert(QStringLiteral("backgroundPresetStore"), QJsonObject{
            {QStringLiteral("rows"), m_asyncCatalogRows},
            {QStringLiteral("elapsedMs"), m_asyncCatalogElapsed},
            {QStringLiteral("guiTicks"), m_asyncCatalogTicks},
            {QStringLiteral("maxGuiGapMs"), m_asyncCatalogMaxGap}});
        root.insert(QStringLiteral("stalls"), stalls);
        root.insert(QStringLiteral("firstFrameGuard"), firstFrameGuard);

        QJsonObject window;
        window.insert(QStringLiteral("width"), m_window.width());
        window.insert(QStringLiteral("height"), m_window.height());
        root.insert(QStringLiteral("window"), window);

        QFile file(m_outPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            file.close();
        }

        m_app.exit(m_storeFailed || guardFailed ? 1 : 0);
    }

    struct Step {
        QString name;
        std::function<void()> action;
        /// 内容到了没有（空 = 这一步不等异步内容）。轮询间隔 10 ms。
        std::function<bool()> ready;
        int timeoutMs = 0;
        /// 等待时界面上显示什么（空白 / 灰字 / 占位行）。
        std::function<QString()> waitingText;
        /// 等完之后量到的内容行数（-1 = 不算）。
        std::function<int()> contentRows;
        /// 动作阻塞在嵌套事件循环里时，从循环内部量的首帧（-1 = 没量）。
        std::function<double()> extra;
    };

    QApplication &m_app;
    MainWindow &m_window;
    QString m_outPath;
    QList<Step> m_steps;
    QList<StepResult> m_results;
    QElapsedTimer m_clock;
    QTimer *m_watchdog = nullptr;
    QString m_themeBefore;
    /// 首帧允许名单（`--wtfml-firstframe-allowlist=`）：键 = 步骤名，值 = 放行时记录的最大首帧。
    QString m_allowlistPath;
    QMap<QString, QString> m_allowlist;
    bool m_guardEnabled = false;
    QString m_guardError;
    QList<TaskCenter::Item> m_tasks;
    QPointer<TaskCenterPanel> m_taskPanel;
    QList<PresetManagePage::PresetItem> m_presets;
    QPointer<PresetManagePage> m_presetPage;
    /// 沙盒里那个"50+ 模组"的夹具实例（由 tools/perf-startup.ps1 的 -InstanceMods 造）。
    QString m_fixtureId;
    /// 安装页 / 新建服务端页用的游戏版本号（从在线版本树里挑第一个真版本）。
    QString m_gameVersion;
    QPointer<InstanceSettingsDialog> m_instanceDialog;
    QPointer<AccountSkinDialog> m_accountDialog;
    QPointer<NewServerPage> m_serverPage;
    QPointer<PresetInstallPicker> m_presetPicker;
    QElapsedTimer m_promptClock;
    double m_promptShownMs = -1.0;
    std::unique_ptr<QTemporaryDir> m_storeDir;
    qsizetype m_storeRows = 0;
    bool m_storeFailed = false;
    QString m_currentStep = QStringLiteral("startup");
    /// finish() 只跑一次（走完 / 硬收尾闸两条路都可能叫它）。
    bool m_finished = false;
    /// 这一趟是被硬收尾闸落盘的（没走完全部步骤）。
    bool m_hardFinalized = false;
    QString m_lastTickStep = QStringLiteral("startup");
    QString m_maxGapFrom;
    QString m_maxGapAt;
    QJsonArray m_slowSpans;
    Presets::CatalogLoader *m_catalogLoader = nullptr;
    bool m_asyncCatalogPending = false;
    QElapsedTimer m_asyncCatalogClock;
    qint64 m_asyncCatalogElapsed = 0;
    qsizetype m_asyncCatalogRows = 0;
    qint64 m_asyncCatalogTicks = 0;
    qint64 m_asyncCatalogMaxGap = 0;
    qint64 m_lastTickMs = 0;
    qint64 m_maxGapMs = 0;
    qint64 m_ticks = 0;
    qint64 m_overFiftyMs = 0;
    bool m_stallSamplerOn = true;
    qsizetype m_next = 0;
};

} // namespace

void PerfProbe::attach(QApplication &app, MainWindow &window)
{
    const QString target = probeTarget(app);
    if (target.isEmpty()) {
        return;
    }
    auto *probe = new Probe(app, window, target);
    probe->start();
}
