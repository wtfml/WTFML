// Native layout evidence only. No real accounts, game data or system settings are modified.
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "Theme.hpp"
#include "Animations.hpp"
#include "util/AppPaths.hpp"
#include "UiLanguage.hpp"
#include "support/SettingsIsolation.hpp"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QScreen>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <QWindow>
#include "DesktopAcceptanceScenarios.hpp"
#include "DesktopLogAcceptance.hpp"
#include "DesktopInputAcceptance.hpp"
#include "DesktopDesignShots.hpp"
#include "DesktopPageShots.hpp"
#include "DesktopLaunchCardShots.hpp"
#include "DesktopHomeStripShots.hpp"
#include "DesktopRailRunShots.hpp"
#include <QProcess>
#include "auth/AccountStore.hpp"
#include "launcher/LauncherManager.hpp"

int main(int argc, char **argv) {
    bool designShots = false;
    bool pageShots = false;
    bool launchCardShots = false;
    bool homeStripShots = false;
    bool railRunShots = false;
    for (int i = 1; i < argc; ++i) pageShots |= qstrcmp(argv[i], "--page-shots") == 0;
    for (int i = 1; i < argc; ++i) launchCardShots |= qstrcmp(argv[i], "--launch-card-shots") == 0;
    for (int i = 1; i < argc; ++i) homeStripShots |= qstrcmp(argv[i], "--home-strip-shots") == 0;
    for (int i = 1; i < argc; ++i) railRunShots |= qstrcmp(argv[i], "--rail-run-shots") == 0;
    for (int i = 1; i < argc; ++i) designShots |= qstrcmp(argv[i], "--design-shots") == 0 || pageShots || launchCardShots || homeStripShots || railRunShots;
    // grab() 也要截到毛玻璃底下的风景；真屏抓边缘样张时可从外面指定别的档（如 acrylic）。
    if (designShots && qEnvironmentVariableIsEmpty("WTFML_BACKDROP")) qputenv("WTFML_BACKDROP", "scene");
    // 离屏平台在 Windows 上用 FreeType 字库，只扫 Qt 自己的 lib/fonts（没有这个目录）：系统字体一支
    // 都没有，正文只能落到我们注册的手写体上（西文被画成 Dancing Script）。指到系统字体目录，再在下面
    // 把 app 字体钉成真桌面上的默认字（微软雅黑 UI），离屏样张才和真桌面一致。
    const bool offscreenPlatform = qgetenv("QT_QPA_PLATFORM").startsWith("offscreen");
    if (designShots && offscreenPlatform && qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", (qgetenv("WINDIR").isEmpty() ? QByteArray("C:\\Windows") : qgetenv("WINDIR")) + "\\Fonts");
    QApplication app(argc, argv);
    if (designShots && offscreenPlatform) {
        QFont font = app.font();
        font.setFamilies({QStringLiteral("Microsoft YaHei UI")});
        app.setFont(font);
    }
    // 样张要英文界面时带 WTFML_SHOT_LANG=en（只装翻译，不写设置）。
    if (qgetenv("WTFML_SHOT_LANG") == "en" && !UiLanguage::installTranslation(app, QStringLiteral("en"))) return 2;
    app.setOrganizationName("WTFML-desktop-acceptance");
    app.setApplicationName("isolated");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"root", "New isolated evidence directory", "path"});
    parser.addOption({"extended", "Controlled long text, overlays, background and keyboard scenarios"});
    parser.addOption({"log-stress", "Two native log windows with bounded controlled output"});
    parser.addOption({"game-root", "Explicitly marked isolated game fixture for manual native smoke", "path"});
    parser.addOption({"java-path", "Java for the isolated game fixture", "path"});
    parser.addOption({"manual-input", "90-second native keyboard observation without synthesizing keys"});
    parser.addOption({"design-shots", "Design comparison screenshots at device pixel ratio 1"});
    parser.addOption({"page-shots", "Settings / tasks / account / log / dialog comparison screenshots"});
    parser.addOption({"launch-card-shots", "Home launch card (#65): three themes x four states, plus W frame strips"});
    parser.addOption({"home-strip-shots", "Home strip (#250): expand / collapse / full-row screenshots and card-fit measurements"});
    parser.addOption({"rail-run-shots", "Rail run-state entry (#85): collapsed / expanded, idle / running, task center"});
    parser.process(app);
    const bool extended = parser.isSet("extended");
    const int sceneCount = extended ? 10 : 4;
    const QString root = QDir::cleanPath(parser.value("root"));
    if (!QDir::isAbsolutePath(root) || QFileInfo::exists(root)) return 2;
    // Qt environment overrides would simulate scaling rather than test Windows DPI.
    // 设计稿对照截图要 1:1 像素：系统缩放不是 100% 时，带抵消用的 QT_SCALE_FACTOR 重开自己一次。
    if (designShots && qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR")) {
        const qreal dpr = QGuiApplication::primaryScreen()->devicePixelRatio();
        if (!qFuzzyCompare(dpr, 1.0)) {
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("QT_SCALE_FACTOR", QString::number(1.0 / dpr, 'g', 10));
            QProcess child;
            child.setProcessEnvironment(env);
            child.setProcessChannelMode(QProcess::ForwardedChannels);
            child.start(QCoreApplication::applicationFilePath(), QCoreApplication::arguments().mid(1));
            if (!child.waitForFinished(180000)) return 124;
            return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : 1;
        }
    }
    for (const char *key : {"QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_FONT_DPI"})
        if (!designShots && !qEnvironmentVariableIsEmpty(key)) return 2;
    // 设计稿对照也可以离屏出（QT_QPA_PLATFORM=offscreen：不弹窗、不占桌面；只出 grab() 那几张，真屏抓的 edge-* 除外）。
    const bool offscreenShots = designShots && QGuiApplication::platformName() == "offscreen";
    if (QGuiApplication::platformName() != "windows" && !offscreenShots) return 2;
    if (!QDir().mkpath(root + "/settings") || !QDir().mkpath(root + "/game")) return 2;
    app.setProperty("wtfml.testDefaultMinecraftDir", root + "/game");
    AppPaths::setMinecraftDir(root + "/game");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root + "/settings");
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root + "/settings-system");
    requireIsolatedSettings(root, "desktop_acceptance");
    QSettings settings;
    settings.setValue("Launcher/Folders", QStringList{root + "/game"});
    settings.setValue("Launcher/CurrentFolder", root + "/game");
    const QString gameRoot = QDir::cleanPath(parser.value("game-root"));
    if (parser.isSet("game-root")) {
        QFile marker(gameRoot + "/p1-game-acceptance.json");
        if (!QDir::isAbsolutePath(gameRoot) || !marker.open(QIODevice::ReadOnly) || marker.size() > 4096 ||
            QJsonDocument::fromJson(marker.readAll()).object().value("format") != "wtfml-isolated-game/1" ||
            !QFileInfo::exists(parser.value("java-path"))) return 2;
        settings.setValue("Launcher/Folders", QStringList{gameRoot});
        settings.setValue("Launcher/CurrentFolder", gameRoot);
        settings.setValue("Java/Path", parser.value("java-path"));
        settings.setValue("Version/LastLaunched", "fabric-loader-0.19.3-1.21.11");
        AppPaths::setMinecraftDir(gameRoot);
        QString error;
        if (!AccountStore::addOffline("DesktopTest", &error)) return 2;
    }
    if (designShots) {
        QString error;
        // 账号下拉样张要几行：另两个先加，Wanderer 最后加、仍是当前账号。
        for (const char *name : {"Builder", "Steve_test", "Wanderer"})
            if (!AccountStore::addOffline(name, &error)) return 2;
        DesignShots::seedInstances(root + "/game/versions");
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) return 2;
    QApplication::setStyle(Theme::createStyle());
    Theme::setSpec(Theme::defaultSpec());
    Theme::apply(app);
    Anim::installPressAnimation(app);
    Anim::installScrollBarAnimation(app);
    MainWindow window;
    window.setWindowTitle("WTFML Desktop Acceptance");
    window.resize(1040, 700);
    window.show();
    if (designShots) {
        Anim::setEnabled(false);  // 截图要确定的终态
        if (railRunShots) {
            RailRunShots::run(app, window, root);
        } else if (homeStripShots) {
            HomeStripShots::run(app, window, root);
        } else if (launchCardShots) {
            Anim::setEnabled(true);  // 卡片要看真的动画那一刻（不定进度条、W 的墨段）
            LaunchCardShots::run(app, window, root);
        } else if (pageShots) {
            PageShots::run(app, window, root);
        } else {
            DesktopAcceptance::seedTasks(window);
            DesignShots::run(app, window, root);
        }
        QTimer::singleShot(150000, &app, [&] { app.exit(124); });
        return app.exec();
    }
    if (parser.isSet("manual-input")) {
        window.setWindowTitle("WTFML Keyboard Acceptance");
        new DesktopInputAcceptance(window, root);
        return app.exec();
    }
    if (parser.isSet("game-root")) {
        auto *launcher = window.findChild<LauncherManager *>();
        if (!launcher) return 2;
        QObject::connect(launcher, &LauncherManager::gameFinished, &window,
            [&](int session, int code, QProcess::ExitStatus status) {
                const QJsonObject report{{"session",session},{"exitCode",code},{"normalExit",status==QProcess::NormalExit},
                    {"scope","real isolated offline game; main menu must be separately observed"}};
                QFile output(root+"/game-exit.json");
                const auto bytes=QJsonDocument(report).toJson();
                const bool saved=output.open(QIODevice::WriteOnly) && output.write(bytes)==bytes.size();
                app.exit(saved && code==0 && status==QProcess::NormalExit ? 0 : 1);
            });
        QTimer::singleShot(300000,&window,[&] {
            for (const auto &game:launcher->runningGames()) launcher->stopGame(game.sessionId);
            app.exit(124);
        });
        return app.exec();
    }
    if (parser.isSet("log-stress")) {
        QTimer::singleShot(500, &window, [&] { DesktopLogAcceptance::start(app, window, root); });
        return app.exec();
    }
    const auto activeTask = extended ? DesktopAcceptance::seedTasks(window) : Tasks::invalidTaskId();
    const auto picture = extended ? DesktopAcceptance::background(root) : QString();
    QHash<int, QJsonObject> keyboardReports;
    QHash<int, bool> opened;
    QJsonArray samples;
    // Actual available preset IDs are the source of truth.
    QStringList themes = Theme::presetIds();
    themes.removeAll("system");
    int sample = 0;
    QTimer next;
    next.setInterval(extended ? 900 : 700);
    QObject::connect(&next, &QTimer::timeout, &app, [&] {
        const int total = themes.size() * sceneCount * 2;
        if (sample >= total) {
            next.stop();
            const auto screen = window.windowHandle()->screen();
            const QJsonObject report{{"platform", QGuiApplication::platformName()},
                {"devicePixelRatio", window.devicePixelRatioF()},
                {"logicalDpi", screen->logicalDotsPerInch()},
                {"availableWidth", screen->availableGeometry().width()},
                {"availableHeight", screen->availableGeometry().height()},
                {"settingsFile", settings.fileName()}, {"samples", samples},
                {"extended", extended}, {"scope", extended ? "controlled geometry and Tab fixtures; real content and full accessibility require separate review" : "native basic page screenshots"}};
            QFile output(root + "/report.json");
            const QByteArray bytes = QJsonDocument(report).toJson();
            const bool saved = output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size();
            bool allSaved = samples.size() == total;
            for (const auto &value : samples) {
                const auto entry = value.toObject();
                allSaved &= entry.value("saved").toBool() && entry.value("secondaryPaletteMismatches").toInt() == 0;
                if (extended) {
                    const auto extra = entry.value("extra").toObject();
                    allSaved &= extra.value("actionAccepted").toBool();
                    if (entry.value("page").toInt() == 6)
                        allSaved &= extra.value("selectedTaskRows").toInt() == 1 && extra.value("visibleFlowSteps").toInt() > 0;
                }
            }
            app.exit(saved && allSaved ? 0 : 1);
            return;
        }
        const int page = sample % sceneCount;
        const int size = (sample / sceneCount) % 2;
        const auto theme = themes.at(sample / (sceneCount * 2));
        if (page == 0) {
            auto spec = Theme::presetSpec(theme);
            if (extended && size == 1 && !picture.isEmpty()) {
                Theme::PartStyle image;
                image.hasImage = true; image.imagePath = picture; image.imageOpacity = 25;
                spec.parts.insert(int(Theme::Part::PageBackground), image);
            }
            Theme::setSpec(spec);
            Theme::apply(app);
            Anim::setEnabled(extended ? true : size == 0);
            window.resize(size == 0 ? QSize(1040, 700) : QSize(900, 600));
        }
        if (!extended) window.ui()->getTabWidget()->setCurrentIndex(page);
        // Capture on the next event-loop turn, after layout and paint have settled.
        const int current = sample++;
        QTimer::singleShot(extended ? 650 : 450, &window, [&, page, size, theme, current] {
            const QString name = QString("%1-%2-%3.png").arg(theme).arg(size).arg(page);
            const QJsonObject keyboard = keyboardReports.value(current);
            const bool saved = window.grab().save(root + '/' + name);
            int secondaryLabels = 0, secondaryMismatches = 0;
            for (auto *label : window.findChildren<QLabel *>()) {
                if (label->property(Theme::textRolePropertyName().toUtf8().constData()).toString() != Theme::secondaryTextRoleValue()) continue;
                ++secondaryLabels;
                if (label->palette().color(QPalette::WindowText) != app.palette().color(QPalette::PlaceholderText)) ++secondaryMismatches;
            }
            QJsonObject extra;
            if (extended) {
                extra.insert("background", size == 1);
                extra.insert("effectiveAnimations", Anim::enabled());
                extra.insert("actionAccepted", opened.value(current, false));
                extra.insert("overlayShown", window.ui()->overlay()->isOpen());
                if (page < 4) extra.insert("keyboard", keyboard);
                int selected = 0, flow = 0;
                for (auto *child : window.findChildren<QWidget *>()) {
                    // 不问 isVisible()：整页窄于 820 时列表和详情是"整块换"的（点开详情 = 列表收起），
                    // 选中的那一行此刻在被收起的列表里，但选中状态照旧只有它一行。
                    if (child->property("taskCenterSelected").toBool()) ++selected;
                    if (child->isVisible() && child->objectName().startsWith("taskCenterFlowStepName_")) ++flow;
                }
                extra.insert("selectedTaskRows", selected);
                extra.insert("visibleFlowSteps", flow);
                if (auto *popup = QApplication::activePopupWidget()) {
                    extra.insert("popupSaved", popup->grab().save(root + "/popup-" + name));
                    extra.insert("popupWidth", popup->width());
                }
                if (auto *modal = QApplication::activeModalWidget())
                    extra.insert("modalSaved", modal->grab().save(root + "/modal-" + name));
            }
            samples.append(QJsonObject{{"extra", extra}, {"index", current}, {"theme", theme}, {"page", page},
                {"secondaryLabels", secondaryLabels}, {"secondaryPaletteMismatches", secondaryMismatches},
                {"animations", size == 0}, {"requestedWidth", size == 0 ? 1040 : 900},
                {"requestedHeight", size == 0 ? 700 : 600}, {"width", window.width()},
                {"height", window.height()}, {"saved", saved}, {"file", name}});
            if (extended) DesktopAcceptance::closeTransient(window);
        });
        if (extended) QTimer::singleShot(0, &window, [&, page, current] {
            opened.insert(current, true); // The action may enter a nested page loop; capture verifies the actual surface.
            const bool accepted = DesktopAcceptance::scene(window, page, activeTask);
            opened.insert(current, accepted);
            if (page < 4) QTimer::singleShot(150, &window, [&, current] {
                keyboardReports.insert(current, DesktopAcceptance::keyboard(window));
            });
        });
    });
    QTimer::singleShot(1000, &next, [&] { next.start(); });
    QTimer::singleShot(120000, &app, [&] { app.exit(124); });
    return app.exec();
}
