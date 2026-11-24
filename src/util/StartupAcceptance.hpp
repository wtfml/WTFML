#pragma once
// Explicit marked-profile acceptance only. Normal startup does not enter this path.
#include "AppPaths.hpp"
#include "SecretRedactor.hpp"
#include "agent/AgentPaths.hpp"
#include <QApplication>
#include <QDebug>
#include <QSharedPointer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAbstractButton>
#include <QScrollArea>
#include <QComboBox>
#include <QStackedWidget>
#include <QTabWidget>

#include "../../ui/SettingsPageParts.hpp"  // 验收步骤要点设置页那几个页签（SlidingTabs）
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QThreadPool>
#include <QImage>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace StartupAcceptance {
struct Context {
    QString root, sentinel;
    int closeAfterMs = 30000, stressRows = 0;
    int windowWidth = 0, windowHeight = 0;
    /// 验收用：非空时窗口起来之后替人把设置页的「语言」切到这一档（`en` / `zh_CN`），
    /// 拍设置页那一行、拍提示卡，再点「立即重启」。给界面语言那一条链路留客观证据用。
    QString languageStep;
    bool enabled() const { return !root.isEmpty(); }
};
inline bool safe(const QString &path) {
    for (QString p = path;;) {
        const QFileInfo info(p);
        if (info.isSymLink() || info.isJunction() || (info.exists() && !info.isDir())) return false;
        const auto parent = info.absolutePath();
        if (p == parent) return true;
        p = parent;
    }
}
inline bool configure(Context *out, QString *error) {
    const auto args = QCoreApplication::arguments();
    QString root;
    int supplied = 0;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--acceptance-profile") {
            if (++i == args.size() || args[i].startsWith("--")) { *error = "Missing acceptance profile path"; return false; }
            root = args[i]; ++supplied;
        } else if (args[i].startsWith("--acceptance-profile=")) {
            root = args[i].mid(QString("--acceptance-profile=").size()); ++supplied;
        }
    }
    if (!supplied) return true;
    if (supplied != 1 || !QDir::isAbsolutePath(root) || !safe(root)) {
        *error = "Acceptance requires one absolute non-redirected marked profile"; return false;
    }
    root = QDir::cleanPath(root);
    QFile marker(root + "/acceptance-profile.json");
    if (QFileInfo(marker).isSymLink() || !marker.open(QIODevice::ReadOnly) || marker.size() > 4096) {
        *error = "Acceptance marker missing or invalid"; return false;
    }
    const auto object = QJsonDocument::fromJson(marker.readAll()).object();
    const int closeMs = object.value("closeAfterMs").toInt(-1);
    const int rows = object.value("stressRows").toInt(0);
    const int windowWidth = object.value("windowWidth").toInt(0);
    const int windowHeight = object.value("windowHeight").toInt(0);
    const auto sentinel = object.value("sentinel").toString();
    const auto languageStep = object.value("languageStep").toString();
    if (!languageStep.isEmpty() && languageStep != "en" && languageStep != "zh_CN") {
        *error = "Invalid acceptance language step"; return false;
    }
    if (object.value("format") != "wtfml-startup-acceptance/1" || closeMs < 2000 || closeMs > 120000 ||
        rows < 0 || rows > 200 || (rows && (sentinel.size() < 16 || sentinel.size() > 128)) ||
        (windowWidth != 0 &&
         (windowWidth < 640 || windowWidth > 3840 || windowHeight < 480 || windowHeight > 2160))) {
        *error = "Invalid acceptance limits"; return false;
    }
    if (!safe(root + "/settings") || !safe(root + "/game") || !safe(root + "/agent") ||
        !QDir().mkpath(root + "/settings") || !QDir().mkpath(root + "/game") ||
        !QDir().mkpath(root + "/agent")) {
        *error = "Cannot create isolated acceptance settings/data"; return false;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root + "/settings");
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root + "/settings-system");
    // 智能体会话/产物/日志的根（`AgentPaths::dataDir()`）：不收进来就落进真实的
    // `%LOCALAPPDATA%\…\WTFML\agent`。API Key 在 Windows 凭据管理器里，不在这份隔离中。
    qApp->setProperty("wtfml.testAgentDataDir", root + "/agent");
    qApp->setProperty("wtfml.testDefaultMinecraftDir", root + "/game");
    AppPaths::setMinecraftDir(root + "/game");
    *out = {root, sentinel, closeMs, rows, windowWidth, windowHeight, languageStep};
    SecretRedactor::remember(sentinel);
    return true;
}
inline void record(const Context &context, QJsonObject fields) {
    const auto path = context.root + "/startup-" + QString::number(QCoreApplication::applicationPid()) + ".json";
    QFile existing(path);
    QJsonObject object;
    if (existing.open(QIODevice::ReadOnly)) object = QJsonDocument::fromJson(existing.readAll()).object();
    existing.close();
    for (auto i = fields.begin(); i != fields.end(); ++i) object.insert(i.key(), i.value());
    object.insert("pid", QString::number(QCoreApplication::applicationPid()));
    object.insert("platform", QGuiApplication::platformName());
    object.insert("dataRoot", AppPaths::minecraftDir());
    object.insert("agentDataDir", AgentPaths::dataDir());
    object.insert("settingsFile", QSettings().fileName());
    QSaveFile output(path); output.setDirectWriteFallback(false);
    if (output.open(QIODevice::WriteOnly)) {
        const auto bytes = QJsonDocument(object).toJson();
        if (output.write(bytes) == bytes.size()) output.commit();
    }
}
inline void dismissRefusal(const Context &context) {
    if (!context.enabled()) return;
    QTimer::singleShot(500, qApp, [context] {
        // 提示是一张提示卡，挂在一扇以这句话为标题的小窗上（见 main.cpp 的 showStartupRefusal）。
        QWidget *dialog = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->isVisible() && top->windowTitle() == QStringLiteral("启动器已在运行或目录不可用")) dialog = top;
        const bool shown = dialog != nullptr;
        const bool shot = shown && dialog->grab().save(context.root + "/refusal-" + QString::number(QCoreApplication::applicationPid()) + ".png");
        record(context, {{"refused", true}, {"dialogShown", shown}, {"screenshot", shot}});
        if (auto *ok = dialog ? dialog->findChild<QAbstractButton *>(QStringLiteral("inlineMessageButton0")) : nullptr) ok->click();
    });
}
inline void attach(const Context &context, QWidget &window) {
    if (!context.enabled()) return;
    QTimer::singleShot(500, &window, [context, &window] {
        record(context, {{"mainWindowShown", window.isVisible()},
            {"windowWidth", window.width()}, {"windowHeight", window.height()}});
    });
    // 整窗截图等开窗「雾散开」（ui/WindowMotion，首帧后约 620 ms）播完再拍，PNG 编码和写盘放到线程池：
    // 在主线程上要卡约 300 ms，以前正好落在开场动效中间（验收 / 测速 / 录像都带这个档案）。
    QTimer::singleShot(1200, &window, [context, &window] {
        const QImage shot = window.grab().toImage();
        const QString path = context.root + "/window-" + QString::number(QCoreApplication::applicationPid()) + ".png";
        QThreadPool::globalInstance()->start([context, shot, path] {
            const bool saved = shot.save(path);
            QMetaObject::invokeMethod(qApp, [context, saved] { record(context, {{"screenshot", saved}}); });
        });
    });
    // 界面语言那一条链路的验收：切到设置页 → 拍「语言」那一行 → 真改一次（落盘 + 弹提示
    // 卡）→ 拍提示卡 → 点「立即重启」。新进程带着同一个档案起来，也会走这一遍（那时下拉框
    // 已经在目标档上，不会二次弹提示），旧进程由 onLanguageChanged() 退出。
    if (!context.languageStep.isEmpty()) {
        QTimer::singleShot(900, &window, [context, &window] {
            if (auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("mainTabWidget"))) {
                for (int i = 0; i < tabs->count(); ++i) {
                    if (tabs->widget(i) != nullptr &&
                        tabs->widget(i)->objectName() == QStringLiteral("settingsTab")) {
                        tabs->setCurrentIndex(i);
                        break;
                    }
                }
            }
            // 「语言」那一行在「个性化」页里：页签和堆叠页一起切（和产品里 showSettingsCategory
            // 同一条路），再把滚动区滚到那一行 —— 它在「主题」「背景图」下面。
            if (auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("settingsCategoryStack"))) {
                for (int i = 0; i < stack->count(); ++i) {
                    if (stack->widget(i) == nullptr ||
                        stack->widget(i)->objectName() != QStringLiteral("settingsPanel-look")) {
                        continue;
                    }
                    stack->setCurrentIndex(i);
                    if (auto *tabs = qobject_cast<SettingsParts::SlidingTabs *>(
                            window.findChild<QWidget *>(QStringLiteral("settingsTabs")))) {
                        tabs->setCurrentIndex(i);
                    }
                    break;
                }
            }
            const QString pid = QString::number(QCoreApplication::applicationPid());
            QTimer::singleShot(400, &window, [context, &window, pid] {
                if (auto *scroll =
                        qobject_cast<QScrollArea *>(window.findChild<QWidget *>(
                            QStringLiteral("settingsPanel-look")))) {
                    if (auto *combo = window.findChild<QComboBox *>(QStringLiteral("languageCombo"))) {
                        scroll->ensureWidgetVisible(combo, 0, 120);
                    }
                }
                record(context, {{"languageRowShot",
                                  window.grab().save(context.root + "/settings-language-" + pid + ".png")}});
                // NOTE: 定时器要**排在改下拉框之前**：改这一下会同步弹出提示卡（那条
                // `InlineMessage::ask()` 里跑嵌套事件循环，直到有人点按钮才回来），
                // 排在它后面的话这一段要等提示关掉才执行 —— 那就没人去点了。
                QTimer::singleShot(700, &window, [context, &window, pid] {
                    auto *restart =
                        window.findChild<QAbstractButton *>(QStringLiteral("inlineMessageButton0"));
                    record(context, {{"languagePromptShown", restart != nullptr},
                                     {"languagePromptShot",
                                      window.grab().save(context.root + "/language-prompt-" + pid + ".png")}});
                    if (restart != nullptr) restart->click();
                });
                if (auto *combo = window.findChild<QComboBox *>(QStringLiteral("languageCombo"))) {
                    const int index = combo->findData(context.languageStep);
                    if (index >= 0) combo->setCurrentIndex(index);  // 信号是真的：落盘 + 弹提示
                }
            });
        });
    }
    QTimer::singleShot(context.closeAfterMs, &window, &QWidget::close);
    if (!context.stressRows) return;
    auto finished = QSharedPointer<int>::create(0);
    for (int worker = 0; worker < 4; ++worker) {
        auto *watcher = new QFutureWatcher<void>(&window);
        QObject::connect(watcher, &QFutureWatcher<void>::finished, &window, [watcher, finished, context] {
            watcher->deleteLater();
            if (++*finished == 4) record(context, {{"stressDone", true}, {"stressRows", context.stressRows * 4}});
        });
        watcher->setFuture(QtConcurrent::run([context, worker] {
            for (int i = 0; i < context.stressRows; ++i)
                qInfo().noquote() << QString("[acceptance-stress] worker=%1 row=%2 value=%3").arg(worker).arg(i).arg(context.sentinel);
        }));
    }
}
} // namespace StartupAcceptance

