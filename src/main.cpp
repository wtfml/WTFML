/**
 * @file main.cpp
 * @brief Entry point for the WTFML Minecraft launcher application.
 * @note Sets up QApplication, application metadata, and shows the main window.
 */

#include <QApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QThread>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QStyleFactory>
#include <QTextStream>
#include <QRecursiveMutex>
#include <QMutexLocker>
#include "util/StartupAcceptance.hpp"

#include "MainWindow.hpp"
#include "util/FileAssociations.hpp"
#include "../ui/WindowMotion.hpp"
#include "perf/PerfProbe.hpp"  // 测量构建专用；普通构建里 attach() 是内联空函数
#include "../ui/ChromeDialog.hpp"
#include "../ui/Animations.hpp"  // 按下按钮那一份动效（见下面那段 NOTE）
#include "../ui/WheelFocusFilter.hpp"  // 下拉框、数字框没有焦点时不认滚轮
#include "../ui/InlineMessage.hpp"
#include "../ui/OverlayHost.hpp"
#include "../ui/AppIcon.hpp" // 窗口 / 任务栏图标（自绘标题栏里不画，见那个文件头）
#include "../ui/Theme.hpp"   // 主题从盘上读回来、铺开（见下面那段 NOTE）
#include "../ui/UiLanguage.hpp" // 界面语言：解析设置值、装翻译（见下面那段）
#include "util/AppPaths.hpp"
#include "util/SecretRedactor.hpp"
#include "util/DownloadDiagnostics.hpp"
#include "util/DirectorySessionLock.hpp"
#include "manage/FolderManager.hpp"
#include "settings/LauncherSettings.hpp"

namespace {

class TimingApplication final : public QApplication {
public:
    using QApplication::QApplication;
    bool notify(QObject *receiver, QEvent *event) override {
        if (QThread::currentThread() != thread()) return QApplication::notify(receiver, event);
        const QPointer<QObject> guard(receiver);
        const int kind = int(event->type());
        QElapsedTimer elapsed;
        elapsed.start();
        const bool result = QApplication::notify(receiver, event);
        const qint64 ms = elapsed.elapsed();
        if (ms > 8) qInfo() << "[UI event]" << kind << ms << "ms"
                           << (guard ? guard->metaObject()->className() : "deleted")
                           << (guard ? guard->objectName() : QString());
        return result;
    }
};

/// 这一次启动的日志文件。**它开没开就是唯一那份状态**：没开 = 还早，先攒着（见下面那段
/// NOTE）；开了 = 直接写进去。不另立一个 bool —— 两份状态迟早会不一致。
struct FileLogState {
    QRecursiveMutex mutex;
    QFile file;
    QTextStream stream;
    QStringList pending;
    bool gaveUp = false;
};
FileLogState &fileLogState() {
    // Qt may emit from worker threads and during static teardown.
    static auto *state = new FileLogState;
    return *state;
}

/// 把 Qt 的日志（qDebug / qWarning；产品里所有 [WTFML] 那句话、以及游戏自己的输出
/// 都从这里过，见 LauncherManager）落一份到盘上，同时照旧往 stderr 写一份。
///
/// 【为什么要它】
///
/// 这个 exe 是 **GUI 子系统**（见 CMakeLists 里那两行 set_target_properties）：
/// 双击启动时不会弹黑框。代价是"那些话没人看得见"，所以要有个地方补上：出问题的时候能查。
///
/// 落在 `<数据目录>/logs/wtfml.log`，**每次启动截断重写**：和 Minecraft 自己的
/// `logs/latest.log` 一个习惯（上次那份不保留），也就和它并排放在一个目录里 ——
/// "启动器说了什么"和"游戏说了什么"翻起来都在那儿。
///
/// NOTE: **第一条消息来得比"当前游戏文件夹"还早** —— 最早那几句是 QApplication 构造时
/// 字体库打的（`QFontDatabase: Cannot find font directory ...`），那时
/// FolderManager 连构造都还没轮到。所以这里只接消息、把行**攒着**，等窗口立起来
/// （main() 里 show() 之后那一下）再开文件、把攒下的一起补进去。早开的话路径会按
/// 默认那一份算，于是"日志落在 exe 旁边、数据在别处"—— 那就成了两处东西。
/// NOTE: 中途换游戏文件夹不会把这个文件搬走：它记的是**这一次启动**，不是"这一份
/// .minecraft 的日志"（这一次启动写在哪，就在哪）。
/// NOTE: 文件写不进去（目录不可写之类）只是不写 —— 日志这件事绝不能影响启动。
/// NOTE: stderr 那一份照旧写。产品里所有验证工程都是"跑 exe、看 stderr"，而且装了自定义
/// handler 之后 Qt 那套"没有控制台就不往 stderr 写"的判断本来就不起作用了 ——
/// 所以这里自己写，验证工程连 QT_FORCE_STDERR_LOGGING 都不必再依赖。
void installFileLog() {
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context,
                              const QString &text) {
        auto &state = fileLogState();
        QMutexLocker guard(&state.mutex);
        const QString line = qFormatLogMessage(type, context, SecretRedactor::redact(text));
        if (state.file.isOpen()) {
            // 进程结束时由系统收这个句柄 —— 这里没有可挂的析构点，而且最后那几行往往
            // 正是"要退出"那些话，早收反而丢东西。
            // NOTE: 不要写 `<< Qt::endl` 再单独 flush()：`Qt::endl` 本身就会 flush，
            // 那等于每条日志两次系统调用；而游戏输出是**每来一块就一行**地走这里
            //（见 LauncherManager 的 [Game Output]），行数可以很密。这里只在行尾留 `\n`、
            // 由下面这一次 flush 负责落盘。
            state.stream << line << QLatin1Char('\n');
            state.stream.flush();
        } else if (!state.gaveUp) {
            state.pending.append(line);
        }
        const QByteArray raw = line.toUtf8();
        std::fprintf(stderr, "%s\n", raw.constData());
        std::fflush(stderr);
    });
}

/// 现在可以把日志写进盘上了：建目录、开文件、把之前攒下的行一起补进去。
/// 调用点见 main()（排在窗口 show() 之后 —— 那一刻"当前游戏文件夹"才是定下来的）。
void openFileLog() {
    auto &state = fileLogState();
    QMutexLocker guard(&state.mutex);
    const QString dir = AppPaths::minecraftDir() + QStringLiteral("/logs");
    QDir().mkpath(dir);
    state.file.setFileName(dir + QStringLiteral("/wtfml.log"));
    // 开不了就只是不写（见 installFileLog 的 NOTE）；攒下的行照样丢掉，不再越攒越多。
    if (state.file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        state.stream.setDevice(&state.file);
        for (const QString &line : state.pending) {
            // 同上：补写这一批只在启动时发生一次，用 `\n` + 循环末尾一次 flush 就够。
            state.stream << line << QLatin1Char('\n');
        }
        state.stream.flush();
    } else {
        // **记下"试过了、没开成"**。不然 handler 那边分不清"还轮不到开文件"
        // 和"开了但失败了"，会把后面每一行继续往 state.pending 里塞（见那一位的说明）。
        state.gaveUp = true;
    }
    state.pending.clear();
}

} // namespace

int main(int argc, char *argv[]) {
    // **handler 排在 QApplication 构造之前**。上面那段 NOTE 说设计动机正是
    // 要留住"字体库那几句"（它们比当前游戏文件夹还早）：那一句若排在 QApplication
    // 构造**之后**，那几句话发生在 handler 装上之前，既没进 state.pending 也没进日志文件，
    // 于是"按 NOTE 去日志文件里查那句警告"永远查不到。
    // NOTE: 这一步不需要 QApplication 已经存在（qInstallMessageHandler / qFormatLogMessage
    // 都是纯 QtCore 的东西），所以提前是安全的。
    installFileLog();

    TimingApplication app(argc, argv);
    // 自检钩子：只动 WTFML_ASSOC_CLASSES_ROOT 指的那个测试子键，没设就拒绝跑（不碰真实
    // HKCU\Software\Classes）。清理逻辑和启动时那一趟是同一份。
    if (app.arguments().contains(QStringLiteral("--assoc-self-test"))) {
        const QString report = FileAssociations::runSelfTest();
        QString output = qEnvironmentVariable("WTFML_ASSOC_TEST_OUT");
        if (output.isEmpty()) output = QDir::tempPath() + QStringLiteral("/wtfml-assoc-selftest.txt");
        if (!output.isEmpty()) {
            QFile file(output);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(report.isEmpty() ? QByteArrayLiteral("PASS\n") : report.toUtf8());
            }
        }
        return report.isEmpty() ? 0 : 1;
    }

    // 刻意不跟随系统原生样式，固定用 Fusion。
    //
    // 原因是原生样式（Windows 11 样式）和自定义外观互斥：只要给复杂控件（QComboBox、
    // QScrollBar 这类）写上一段 QSS，原生绘制就整体让位给样式表 —— 只写了一半的地方
    // 会露出残破外观，控制台还会刷警告。想自己定义外观，就得先离开原生样式。
    //
    // NOTE: 全仓的 QSS 有 11 处，散在 ui/ 的五个文件里（`grep -rn setStyleSheet ui/`
    // 是唯一出处）。样式基线固定 Fusion 之后，"等界面铺开之后再换"的顾虑没有了。
    //
    // NOTE: 这也决定了主题那一层的取色方式：样式固定 Fusion 之后，`QStyle::standardPalette()`
    // 就是"这一档深浅下我们该有的那一份调色板"，`Theme::apply()` 拿它当基色
    // （见 `ui/Theme.cpp`）。跟随原生样式的话，那份基色就是系统给的，改不动。
    // 从 `Theme::createStyle()` 拿（= Fusion + 下拉框那个 `∨` 箭头）。基样式还是 Fusion，
    // 只是外面套了一层 QProxyStyle —— 换样式基线的时候所有入口（这里 + 各验证工程）一起换。
    QApplication::setStyle(Theme::createStyle());

    // Configure QSettings to use proper registry/config paths
    app.setApplicationName("WTFML");
    // 版本号来自 CMakeLists.txt 的 `project(... VERSION ...)`（`WTFML_VERSION` 那个宏），
    // **不许在这儿写第二份字面量** —— 这两处长期对不上（CMakeLists 0.1.0 / 这儿 1.0.0）。
    app.setApplicationVersion(QStringLiteral(WTFML_VERSION));
    app.setOrganizationName("WTFML");

    // 应用图标：这一句铺给**所有**顶层窗口（主窗口、ChromeDialog、游戏日志窗、任务栏、
    // Alt+Tab），之后建的窗口继承它，各窗口不用各自再 setWindowIcon。
    // 自绘标题栏里不画图标（用户定的），这里只做窗口与任务栏那两处。
    QApplication::setWindowIcon(wtfmlWindowIcon());

    StartupAcceptance::Context acceptance;
    QString acceptanceProblem;
    if (!StartupAcceptance::configure(&acceptance, &acceptanceProblem)) {
        qCritical().noquote() << acceptanceProblem;
        return 2;
    }

    // 界面语言：**排在所有界面创建之前**（`MainWindow`、提示窗、日志窗都按这一份翻译
    // 建自己的字）。源文是中文，所以只有英文要装翻译 —— 那一份 .qm 由 lrelease 在构建时
    // 从 `assets/i18n/wtfml_en.ts` 生成、编进资源（见 CMakeLists 与 docs/dev/I18N.md）。
    //
    // NOTE: 这一句必须排在 `setOrganizationName` / `setApplicationName` **之后**（设置值从
    // `QSettings` 读，它按 org/app 两个名字找那一份），也必须排在验收档案**建好之后**
    // （那一步把 QSettings 指到隔离目录里，见 StartupAcceptance::configure）。
    // NOTE: 资源里没有那一份时只记一句警告、界面退回中文 —— 构建管线已经把「找不到
    // lrelease」拦在配置阶段了，走到这儿说明产物被换过，运行期没有补救办法。
    {
        const QString languageSetting = UiLanguage::readSetting();
        const QString uiLanguage =
            UiLanguage::resolve(languageSetting, QLocale::system().uiLanguages());
        qInfo() << "[WTFML] 界面语言" << languageSetting << "→" << uiLanguage;  // i18n-ignore
        if (!UiLanguage::installTranslation(app, uiLanguage)) {
            qWarning() << "[WTFML] 界面语言为 English，但翻译资源 :/i18n/wtfml_en.qm 加载失败，界面退回中文";  // i18n-ignore
        }
    }

    // 旧版本把 `.nbt` / `.litematic` / `.schematic` / `.schem` / `.mrpack` 设成了默认打开方式，
    // 并把 `.jar` / `.zip` 挂进「打开方式」候选。现在不注册关联了，启动时把本程序写下的那几条
    // 清掉：只认 ProgID 是本程序的那几个键/值，别的程序和用户自己设的默认值一律不动。
    // 验收档不碰真实的 HKCU\Software\Classes：要连清理一起验，就把 WTFML_ASSOC_CLASSES_ROOT
    // 指到临时子键（和自检同一条缝）。
    if (!acceptance.enabled() || !qEnvironmentVariableIsEmpty("WTFML_ASSOC_CLASSES_ROOT")) {
        QStringList associationProblems;
        const int removed = FileAssociations::cleanupLegacyAssociations(&associationProblems);
        if (removed > 0) {
            qInfo() << "[WTFML] 清掉了旧版注册的文件关联" << removed << "项";
        }
        for (const QString &problem : associationProblems) {
            qWarning().noquote() << "[WTFML] 清理旧版文件关联时出错：" << problem;
        }
    }

    DirectorySessionLock::enabled = true;
    LauncherSettings startupSettings;
    FolderManager startupFolders(&startupSettings);
    QString lockProblem;
    if (!DirectorySessionLock::acquire(startupFolders.currentFolder(), &lockProblem) ||
        !DirectorySessionLock::acquire(FolderManager::defaultFolder(), &lockProblem)) {
        Theme::setSpec(Theme::load());
        Theme::apply(app);
        StartupAcceptance::dismissRefusal(acceptance);
        // 主窗还没建：开一扇小窗当提示卡的底，和主窗里的提示长一个样（自绘窄条，不带系统标题栏；
        // 提示卡摆在条下面那块，条上的 × 和拖动照常能用）。
        ChromeDialog host;  // surface-registry-ignore: 主窗建不起来时的拒开提示小窗，进程随即退出，sweep 够不到
        host.setWindowTitle(QStringLiteral("启动器已在运行或目录不可用"));
        host.resize(OverlayHost::promptWidth() + 80, 320 + ChromeDialog::stripHeight());
        auto *overlay = new OverlayHost(host.body(), host.body());
        host.show();
        InlineMessage::information(overlay, QStringLiteral("启动器已在运行或目录不可用"), lockProblem);
        return 1;
    }

    // 主题（背景 / 字色 / 文字不透明度 / 语义色）从盘上读回来铺开。
    //
    // **必须在 `MainWindow` 构造之前**：界面里那些"建的时候取一次色"的东西（条底色、
    // 底栏底色、几张卡片、11 处 QSS）都是按**那一刻**的调色板算的 —— 铺晚了它们就全是
    // 出厂色，而且是那种"改了设置重启之后只有一半地方变了"的错。
    //
    // NOTE: 盘上一项都没有（新装、或者用户从没改过）时 `load()` 给的是出厂那一份
    // —— 暮色雪山（`dark`），`presetId` / 主题色都跟着它走。
    Theme::setSpec(Theme::load());
    Theme::apply(app);

    // 动效那一层（用户点名的那三样：按下按钮 / 切换页面 / 展开折叠）。
    //
    // 这一句是**按钮那一份动效的唯一接线点**：它往 `QApplication` 上挂一个事件过滤器，
    // 谁被按下去就给谁压暗一档（全仓 92 个 `new QPushButton` 一个字都不用改，见 ui/Animations.hpp）。
    // 换页和展开折叠不走这里：它们由真搭界面的那两处调（`Anim::pageAppear()` /
    // `Anim::animateExpansion()`）。
    Anim::installPressAnimation(app);
    // 另一份：**滚动条一露面就淡进来**（用户 ：「每一次切页面的时候滚动条都是瞬间
    // 出现的」）。同样是往上挂一个事件过滤器，全仓每一处滚动区都不用改。
    Anim::installScrollBarAnimation(app);
    // 下拉框、数字框没有焦点时不认滚轮（全局，一处处理）。
    WheelFocusFilter::install(app);


    MainWindow window;
    if (acceptance.enabled() && acceptance.windowWidth > 0) {
        window.resize(acceptance.windowWidth, acceptance.windowHeight);
    }
    // 开程序「雾散开」：首帧画成起手样子（磨砂、四组内容藏着、品牌标空着），首帧之后才动。
    WindowMotion::armOpening(&window);
    window.show();

    // 日志文件到这一刻才开：窗口立起来 = FolderManager 已经把「当前游戏文件夹」从
    // QSettings 里恢复好了，落点才是对的（见 installFileLog 的 NOTE）。
    openFileLog();
    qInfo() << "[WTFML] version" << app.applicationVersion();
    StartupAcceptance::attach(acceptance, window);

    // 关联/命令行带进来的文件：第一个"存在且能打开"的普通参数就是它。
    {
        QString externalFile;
        for (int i = 1; i < argc; ++i) {
            const QString argument = QString::fromLocal8Bit(argv[i]);
            if (argument.startsWith(QStringLiteral("--")) || !QFileInfo::exists(argument)) continue;
            if (FileAssociations::isOpenable(QLatin1Char('.') + QFileInfo(argument).suffix())) {
                externalFile = argument;
                break;
            }
        }
        if (!externalFile.isEmpty()) {
            const QString file = externalFile;
            QTimer::singleShot(0, &window, [&window, file] { window.openExternalFile(file); });
        }
    }

    // 特供版（-DWTFML_DIAG_DOWNLOAD=ON）的下载明细：和 wtfml.log 同一个目录、同一份
    // "这一次启动"。普通构建里 enabled() 是编译期常量 false，这两句什么都不做。
    DownloadDiag::init(AppPaths::minecraftDir() + QStringLiteral("/logs"));
    DownloadDiag::logEnvironment();

    // 测量构建（-DWTFML_PERF_PROBE=ON）的界面探针：argv 里没有
    // `--wtfml-perf-probe=<文件>` 就立即返回。普通构建里这一句什么都不做。
    PerfProbe::attach(app, window);

    return app.exec();
}






