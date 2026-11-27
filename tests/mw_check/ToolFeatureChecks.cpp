#include "ToolFeatureChecks.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <functional>
#include "tools/BeaconBeam.hpp"
#include "tools/Beacon.hpp"
#include "tools/BannerRender.hpp"
#include "tools/BannerTextures.hpp"
#include "tools/BannerDesign.hpp"
#include "download/DownloadCache.hpp"
#include "controllers/ToolboxInputState.hpp"
#include "PopupPlacement.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/DirectDownloadController.hpp"
#include "ToolboxPage.hpp"
#include "TaskCenterPanel.hpp"
#include "DownloadToolPanel.hpp"
#include "MainWindowUI.hpp"
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QScrollArea>
#include <QListWidget>
#include <QFontDatabase>
#include <QClipboard>
#include "Theme.hpp"
#include <cstdio>

int runToolFeatureChecks() {
    const int fontId = QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc")));
    const auto families = QFontDatabase::applicationFontFamilies(fontId);
    if (!families.isEmpty()) qApp->setFont(QFont(families.first(), 9));
    qApp->setStyle(Theme::createStyle()); Theme::setSpec(Theme::defaultSpec()); Theme::apply(*qApp);
    int failures = 0;
    const auto expect = [&](bool ok, const char *text) { printf("%s %s\n", ok ? "PASS" : "FAIL", text); fflush(stdout); if (!ok) ++failures; };
    const auto pump = [](const std::function<bool()> &done, int limit = 20000) {
        QElapsedTimer time; time.start();
        while (!done() && time.elapsed() < limit) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1); }
        return done();
    };
    const auto empty = BeaconBeam::calculate({});
    expect(empty.ok && empty.finalColor == QColor(Qt::white), "no stained glass produces white beam");
    const auto first = BeaconBeam::calculate({14});
    expect(first.ok && first.finalColor == BeaconBeam::dyeColor(14), "first glass sets color directly without averaging white");
    const auto mixed = BeaconBeam::calculate({14, 11, 4});
    const auto swapped = BeaconBeam::calculate({4, 11, 14});
    expect(mixed.ok && mixed.segments.size() == 3 && mixed.finalColor != swapped.finalColor, "beam color depends on glass order and retains intermediate segments");
    expect(qAbs(mixed.finalColor.red() - 186) <= 1 && qAbs(mixed.finalColor.green() - 137) <= 1 && qAbs(mixed.finalColor.blue() - 83) <= 1,
        "three-layer color uses sequential weights, not a flat three-color average");
    expect(BeaconBeam::calculate({14, 14}).finalColor == first.finalColor, "same-color glass does not change matching beam");
    expect(!BeaconBeam::calculate({16}).ok && !BeaconBeam::calculate(QList<int>(65, 0)).ok, "beam rejects invalid colors and excessive layers");
    QHash<QString, QImage> textures;
    QImage mask(64, 64, QImage::Format_ARGB32); mask.fill(Qt::transparent);
    for (int y = 1; y < 41; ++y) for (int x = 1; x < 21; ++x) mask.setPixelColor(x, y, Qt::white);
    textures.insert(QStringLiteral("stripe_bottom"), mask); textures.insert(QStringLiteral("stripe_top"), mask);
    const int bottom = Banner::patternIndexById(QStringLiteral("stripe_bottom"));
    const int top = Banner::patternIndexById(QStringLiteral("stripe_top"));
    QString error;
    const auto flag = BannerRender::compose(0, {{bottom,14},{top,11}}, textures, &error);
    expect(!flag.isNull() && error.isEmpty() && flag.pixelColor(10,20) == BeaconBeam::dyeColor(11), "banner later layers cover earlier layers using texture alpha");
    expect(BannerRender::compose(0, {{top,11},{bottom,14}}, textures).pixelColor(10,20) == BeaconBeam::dyeColor(14), "banner reordering changes composed preview");
    expect(BannerRender::compose(0, {{top,11}}, {}, &error).isNull() && !error.isEmpty(), "missing banner textures produce explicit notice instead of fabricated pattern");
    expect(BannerRender::textureName(QStringLiteral("bordure")) == QStringLiteral("border"), "legacy banner pattern name maps to texture resource name");
    QTemporaryDir root;
    expect(root.isValid(), "isolated test directory exists");
    const QString designPath = root.filePath(QStringLiteral("flag.json"));
    BannerDesign::Design design{4, QStringLiteral("1.14"), {QStringLiteral("%1:14").arg(bottom), QStringLiteral("%1:11").arg(top)}};
    BannerDesign::Design loaded;
    expect(BannerDesign::save(designPath, design, &error) && BannerDesign::load(designPath, &loaded, &error)
        && loaded.base == 4 && loaded.layers == design.layers && loaded.version == design.version,
        "banner design round trips stable pattern/color ids and version atomically");
    const auto prior = loaded.layers;
    expect(!BannerDesign::decode("{bad", &loaded, &error) && error.contains(QStringLiteral("字节")) && loaded.layers == prior,
        "invalid banner JSON reports offset and preserves current design");
    auto malformed = BannerDesign::encode(design); malformed.insert(QStringLiteral("base"), 4);
    expect(!BannerDesign::decode(QJsonDocument(malformed).toJson(), &loaded, &error), "banner rejects wrong field types");
    malformed = BannerDesign::encode(design);
    malformed.insert(QStringLiteral("layers"), QJsonArray{QJsonObject{{"pattern", "unknown"}, {"color", "red"}}});
    expect(!BannerDesign::decode(QJsonDocument(malformed).toJson(), &loaded, &error), "banner rejects unknown pattern ids");
    const int flow = Banner::patternIndexById(QStringLiteral("flow"));
    expect(flow >= 0 && !BannerDesign::available(flow, QStringLiteral("1.20.4"))
        && BannerDesign::available(flow, QStringLiteral("1.21")) && BannerDesign::available(bottom, QStringLiteral("1.14")),
        "banner filters by game version without comparing version strings lexically");
    expect(BannerDesign::available(Banner::patternIndexById(QStringLiteral("field_masoned")), QStringLiteral("1.14"))
        && !BannerDesign::available(Banner::patternIndexById(QStringLiteral("piglin")), QStringLiteral("1.14")),
        "pattern filtering distinguishes existing brick design from newer piglin design");
    const QString cacheRoot = root.filePath(QStringLiteral("cache"));
    const QString idle = QDir(cacheRoot).filePath(QString(64, 'a'));
    const QString active = QDir(cacheRoot).filePath(QString(64, 'b'));
    QDir().mkpath(idle); QDir().mkpath(active);
    const auto writeFixture = [&](const QString &path, const QByteArray &bytes) {
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    expect(writeFixture(QDir(idle).filePath(QStringLiteral("part-0.bin")), QByteArray(100, 'a'))
        && writeFixture(QDir(active).filePath(QStringLiteral("part-0.bin")), QByteArray(200, 'b'))
        && writeFixture(QDir(idle).filePath(QStringLiteral("user-file.txt")), "keep"), "cache fixtures created");
    QLockFile activeLock(QDir(active).filePath(QStringLiteral("lock")));
    expect(activeLock.tryLock(0), "active cache lock held");
    const auto inspectedCache = DownloadCache::run(DownloadCache::Action::Inspect, 0, cacheRoot);
    expect(inspectedCache.bytes == 300 && inspectedCache.entries == 2, "cache statistics count managed data only");
    const auto expiredCache = DownloadCache::run(DownloadCache::Action::ClearExpired, 0, cacheRoot);
    expect(expiredCache.removed == 0, "fresh cache is not treated as expired");
    const auto trimmedCache = DownloadCache::run(DownloadCache::Action::Trim, 150, cacheRoot);
    expect(trimmedCache.removed == 100 && trimmedCache.bytes == 200 && trimmedCache.busy == 1
        && QFileInfo::exists(QDir(active).filePath(QStringLiteral("part-0.bin")))
        && QFileInfo::exists(QDir(idle).filePath(QStringLiteral("user-file.txt"))),
        "cache quota removes only idle parts, preserves active downloads and unrelated files");
    activeLock.unlock();
    const auto clearedCache = DownloadCache::run(DownloadCache::Action::ClearIdle, 0, cacheRoot);
    expect(clearedCache.removed == 200 && clearedCache.bytes == 0, "unlocked idle cache can be cleared");
    const QRect screen(-1920, 0, 1920, 1080);
    const QRect nearTop(-120, 10, 100, 60);
    const auto below = PopupPlacement::around(nearTop, QSize(600, 250), screen);
    expect(screen.contains(below) && below.top() > nearTop.bottom(), "quick popup flips below and clamps right edge on negative-origin screen");
    const QRect nearBottom(-1900, 990, 100, 60);
    const auto above = PopupPlacement::around(nearBottom, QSize(600, 250), screen);
    expect(screen.contains(above) && above.bottom() < nearBottom.top(), "quick popup prefers above when space permits");
    expect(screen.contains(PopupPlacement::around(nearBottom, QSize(4000, 4000), screen)), "oversized popup is bounded by available screen");
    ActivityCenterController activity;
    activity.tasks()->setHistoryPath(root.filePath(QStringLiteral("history.json")));
    QObject owner;
    QList<Tasks::TaskId> started;
    const auto add = [&](const QString &name) { return activity.enqueue(Tasks::TaskKind::Download, name, {}, {}, QStringLiteral("test"), &owner,
        [&](Tasks::TaskId id) { started.append(id); }); };
    const auto a = add(QStringLiteral("A")); activity.startReady();
    const auto b = add(QStringLiteral("B")), c = add(QStringLiteral("C"));
    expect(started == QList<Tasks::TaskId>{a} && activity.isQueued(b) && activity.isQueued(c), "serial lane starts one task and leaves others genuinely queued");
    expect(!activity.moveQueued(a, 1) && activity.moveQueued(c, -1), "running task cannot reorder but waiting task can move");
    const auto items = activity.items([](const Tasks::TaskSnapshot &) { return true; }, false, {});
    expect(items.size() == 3 && items[1].key == QString::number(c) && !items[1].queueGroup.isEmpty(), "activity presentation matches executor queue order");
    QWidget anchor;
    TaskCenterPanel panel(&anchor, [&] { return activity.items([](const Tasks::TaskSnapshot &) { return true; }, false, {}); }, [] {}, {}, &anchor, true);
    panel.setRowActionHandler([&](const QString &key, TaskCenter::RowAction action) {
        if (action == TaskCenter::RowAction::MoveUp || action == TaskCenter::RowAction::MoveDown)
            activity.moveQueued(key.toULongLong(), action == TaskCenter::RowAction::MoveUp ? -1 : 1);
    });
    panel.showEmbedded();
    expect(panel.findChild<QWidget *>(QStringLiteral("taskDragHandle_%1").arg(a)) == nullptr &&
           panel.findChild<QWidget *>(QStringLiteral("taskDragHandle_%1").arg(c)) != nullptr,
           "only genuine queued tasks expose drag handles");
    activity.tasks()->succeed(a);
    expect(pump([&] { return started.size() == 2; }) && started[1] == c, "executor actually starts reordered task next");
    activity.tasks()->cancel(b); activity.tasks()->succeed(c);
    QCoreApplication::processEvents();
    expect(started.size() == 2 && !activity.isQueued(b), "cancelled queued task never starts");
    auto *temporaryOwner = new QObject;
    const auto gone = activity.enqueue(Tasks::TaskKind::Other, QStringLiteral("owner disappears"), {}, {}, QStringLiteral("test"), temporaryOwner, [&](Tasks::TaskId) { expect(false, "destroyed owner callback must not execute"); });
    delete temporaryOwner; QCoreApplication::processEvents();
    expect(activity.tasks()->snapshot(gone).state == Tasks::TaskState::Cancelled, "destroyed executor owner cancels queued callback safely");
    auto *startingOwner = new QObject;
    Tasks::TaskId starting = Tasks::invalidTaskId();
    QObject::connect(activity.tasks(), &Tasks::TaskManager::taskChanged, &owner, [&](Tasks::TaskId id) {
        if (id == starting && activity.tasks()->snapshot(id).state == Tasks::TaskState::Running) { delete startingOwner; startingOwner = nullptr; }
    });
    starting = activity.enqueue(Tasks::TaskKind::Other, QStringLiteral("owner closes at dispatch"), {}, {}, QStringLiteral("test"), startingOwner,
        [&](Tasks::TaskId) { expect(false, "dispatch must recheck owner after state-change signal"); });
    activity.startReady();
    expect(activity.tasks()->snapshot(starting).state == Tasks::TaskState::Cancelled, "owner destroyed synchronously during dispatch never executes stale callback");
    QTcpServer server;
    expect(server.listen(QHostAddress::LocalHost, 0), "local server starts for real download queue verification");
    QStringList requestOrder;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                const auto data = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", data);
                if (!data.contains("\r\n\r\n") || socket->property("sent").toBool()) return;
                socket->setProperty("sent", true);
                const auto path = QString::fromLatin1(data.split(' ').value(1));
                const bool probe = data.toLower().contains("range: bytes=0-0");
                if (!probe) requestOrder.append(path);
                QTimer::singleShot(!probe && path == QStringLiteral("/a") ? 150 : 0, socket, [socket] {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\ndata"); socket->disconnectFromHost();
                });
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    ActivityCenterController downloads;
    downloads.tasks()->setHistoryPath(root.filePath(QStringLiteral("download-history.json")));
    DirectDownloadController tool(&downloads, nullptr);
    const auto request = [&](const QString &name) {
        DirectDownloadRequest parsed; QString problem;
        expect(DirectDownloadRequest::parse(QStringLiteral("http://127.0.0.1:%1/%2").arg(server.serverPort()).arg(name),
            root.path(), name + QStringLiteral(".bin"), &parsed, &problem), "queue request parses");
        return parsed;
    };
    expect(tool.start(request(QStringLiteral("a")), false, &error), "first real download starts");
    const auto da = tool.lastSubmittedTaskId();
    expect(tool.start(request(QStringLiteral("b")), false, &error), "second real download queues");
    const auto db = tool.lastSubmittedTaskId();
    expect(tool.start(request(QStringLiteral("c")), false, &error), "third real download queues");
    const auto dc = tool.lastSubmittedTaskId();
    expect(!tool.start(request(QStringLiteral("b")), false, &error), "duplicate pending target is rejected");
    expect(tool.start(request(QStringLiteral("cancelled")), false, &error), "fourth real download queues");
    const auto cancelled = tool.lastSubmittedTaskId(); tool.cancel(cancelled);
    expect(downloads.moveQueued(dc, -1), "real download priority can change while first file is running");
    expect(pump([&] { return Tasks::isTerminalState(downloads.tasks()->snapshot(da).state) &&
        Tasks::isTerminalState(downloads.tasks()->snapshot(db).state) && Tasks::isTerminalState(downloads.tasks()->snapshot(dc).state); }),
        "all real queued downloads finish within deadline");
    expect(requestOrder == QStringList{QStringLiteral("/a"), QStringLiteral("/c"), QStringLiteral("/b")},
        "network executor follows reordered A-C-B queue, not original A-B-C order");
    expect(downloads.tasks()->snapshot(cancelled).state == Tasks::TaskState::Cancelled && !QFileInfo::exists(root.filePath(QStringLiteral("cancelled.bin"))),
        "cancelling waiting download never contacts server or creates target");
    auto *shortLived = new ActivityCenterController;
    shortLived->tasks()->setHistoryPath(root.filePath(QStringLiteral("short-history.json")));
    DirectDownloadController survivor(shortLived, nullptr);
    expect(survivor.start(request(QStringLiteral("owner-loss")), false, &error), "active download starts before ledger destruction");
    const auto survivorId = survivor.taskId(); delete shortLived;
    survivor.shutdown(); survivor.shutdown();
    expect(!survivor.canCancel(survivorId), "ledger destruction and repeated shutdown cannot access freed activity state");
    ToolboxPage dragPage;
    // 工具页第一次进才搭（和 ToolboxController 一样）：先进要用到的那几片。
    for (const auto tool : {ToolboxPage::Tool::Anvil, ToolboxPage::Tool::Coord, ToolboxPage::Tool::Server,
                            ToolboxPage::Tool::Beacon, ToolboxPage::Tool::Banner, ToolboxPage::Tool::Trim})
        dragPage.showTool(tool);
    QStringList colors;
    for (int i = 0; i < Banner::colorCount(); ++i) colors.append(Banner::colorName(i));
    dragPage.setBannerColors(colors);
    dragPage.setBannerPatterns(Banner::patternNames());
    dragPage.setBannerVersions(BannerDesign::versions());
    int dragSignals = 0;
    QImage dragPreview;
    QObject::connect(&dragPage, &ToolboxPage::bannerRequested, &dragPage,
        [&](int base, const QStringList &layers) {
            ++dragSignals;
            QList<Banner::Layer> parsed;
            for (const auto &layer : layers) {
                const auto parts = layer.split(':');
                parsed.append({parts[0].toInt(), parts[1].toInt()});
            }
            dragPreview = BannerRender::compose(base, parsed, textures);
            dragPage.setBannerPreview(dragPreview, {});
            const auto plan = Banner::plan(base, parsed, dragPage.bannerVersion());
            dragPage.setBannerResult(plan.detailText());
            dragPage.setBannerCommand(plan.command);
        });
    const QString red = QStringLiteral("%1:14").arg(bottom);
    const QString blue = QStringLiteral("%1:11").arg(top);
    dragPage.setBannerLayers({red, blue});
    auto *layers = dragPage.findChild<QListWidget *>(ToolboxPage::bannerListName());
    auto *undoLayers = dragPage.findChild<QPushButton *>(ToolboxPage::bannerUndoName());
    expect(layers && layers->dragDropMode() == QAbstractItemView::InternalMove && dragSignals == 0,
        "banner enables internal drag only; setting layers does not emit");
    layers->setCurrentRow(0);
    expect(layers->model()->moveRows({}, 0, 1, {}, 2) && dragPage.bannerLayers() == QStringList{blue, red}
        && dragSignals == 1 && layers->currentRow() == 1 && undoLayers->isEnabled(),
        "drag model moves layer down, retaining selection and one undo point");
    expect(!dragPreview.isNull() && dragPreview.pixelColor(10,20) == BeaconBeam::dyeColor(14)
        && layers->item(0)->text().startsWith(QStringLiteral("第 1 层")),
        "drag updates rendered preview and layer numbering");
    expect(layers->model()->moveRows({}, 1, 1, {}, 0) && dragPage.bannerLayers() == QStringList{red, blue}
        && dragSignals == 2 && dragPreview.pixelColor(10,20) == BeaconBeam::dyeColor(11),
        "drag up restores blue top layer and renders immediately");
    expect(!layers->model()->moveRows({}, 0, 1, {}, 1) && dragSignals == 2,
        "no-op drop emits no change");
    undoLayers->click();
    expect(dragPage.bannerLayers() == QStringList{blue, red} && dragSignals == 3
        && dragPreview.pixelColor(10,20) == BeaconBeam::dyeColor(14), "drag undo restores order and rendering");
    dragPage.setBannerLayers({red, blue, red});
    expect(layers->model()->moveRows({}, 2, 1, {}, 1) && dragPage.bannerLayers() == QStringList{red, red, blue},
        "duplicate patterns retain payloads when dragged");
    undoLayers->click();
    expect(dragPage.bannerLayers() == QStringList{red, blue, red}, "duplicate layer reorder can be undone");
    dragPage.setBannerLayers({red, red});
    const int beforeIdentical = dragSignals;
    expect(layers->model()->moveRows({}, 0, 1, {}, 2) && dragSignals == beforeIdentical
        && !undoLayers->isEnabled() && layers->item(0)->text().startsWith(QStringLiteral("第 1 层"))
        && layers->item(1)->text().startsWith(QStringLiteral("第 2 层")),
        "identical layers still renumber but create no redundant undo or render");
    dragPage.setBannerLayers({});
    expect(layers->count() == 0 && !undoLayers->isEnabled(), "new banner clears reordered layers and undo history");
    dragPage.setBeamColors(colors);
    dragPage.setDimensionChoices({QStringLiteral("主世界"), QStringLiteral("下界"), QStringLiteral("末地")});
    dragPage.setEnchantmentChoices({{QStringLiteral("sharpness"), QStringLiteral("锋利")}});
    dragPage.setAnvilRows({{QStringLiteral("sharpness"), 3, true, 2, false}});
    dragPage.setBeamLayers({14, 11, 4});
    dragPage.setBannerDesign(4, {red, blue}, QStringLiteral("1.20.4"));
    dragPage.findChild<QLineEdit *>(QStringLiteral("serverAddress"))->setText(QStringLiteral("example.test:25565"));
    dragPage.findChild<QDoubleSpinBox *>(QStringLiteral("coordX"))->setValue(-123.5);
    dragPage.findChild<QCheckBox *>(ToolboxPage::trimWholeSetName())->setChecked(true);
    const auto remembered = ToolboxInputState::capture(&dragPage);
    ToolboxPage restored;
    // 工具页第一次进才搭（和 ToolboxController 一样）：先进要用到的那几片。
    for (const auto tool : {ToolboxPage::Tool::Anvil, ToolboxPage::Tool::Coord, ToolboxPage::Tool::Server,
                            ToolboxPage::Tool::Beacon, ToolboxPage::Tool::Banner, ToolboxPage::Tool::Trim})
        restored.showTool(tool);
    restored.setBannerColors(colors); restored.setBannerPatterns(Banner::patternNames());
    restored.setBannerVersions(BannerDesign::versions()); restored.setBeamColors(colors);
    restored.setDimensionChoices({QStringLiteral("主世界"), QStringLiteral("下界"), QStringLiteral("末地")});
    restored.setEnchantmentChoices({{QStringLiteral("sharpness"), QStringLiteral("锋利")}});
    int requests = 0;
    QObject::connect(&restored, &ToolboxPage::serverCheckRequested, &restored, [&](const QString &) { ++requests; });
    expect(ToolboxInputState::restore(&restored, remembered) && restored.bannerBaseColor() == 4
        && restored.bannerVersion() == QStringLiteral("1.20.4") && restored.bannerLayers() == QStringList{red, blue}
        && restored.coordinate(0) == -123.5 && requests == 0, "tool inputs restore without starting network or file jobs");
    expect(restored.findChild<QLineEdit *>(QStringLiteral("serverAddress"))->text() == QStringLiteral("example.test:25565")
        && restored.findChild<QListWidget *>(QStringLiteral("beamGlassList"))->count() == 3
        && restored.anvilRows().size() == 1 && restored.anvilRows()[0].level == 3,
        "input memory retains address, ordered glass and anvil rows");
    expect(restored.trimWholeSet() && !restored.findChild<QComboBox *>(ToolboxPage::trimPartComboName())->isEnabled()
        && !restored.findChild<QSpinBox *>(ToolboxPage::trimPiecesSpinName())->isEnabled(),
        "remembered whole-set option restores dependent control presentation");
    // ---- 坐标换算：粘贴 F3+C 那一行 + 复制成 tp 命令（UI 问题 #19） ----
    // 这两个入口在 bfeca25 收文案时被删过一次（只剩"往坐标框里 Ctrl+V"那条看不见的路），
    // 当时没有任何断言守着。这里钉住按钮本身和两条路的效果。
    {
        ToolboxPage coordPage;
        coordPage.showTool(ToolboxPage::Tool::Coord);
        QString parsed;
        QObject::connect(&coordPage, &ToolboxPage::teleportParseRequested, &coordPage,
                         [&parsed](const QString &text) { parsed = text; });
        const QString command = QStringLiteral(
            "/execute in minecraft:the_nether run tp @s 123.450000 64.000000 -78.900000 90.000000 0.000000");
        QGuiApplication::clipboard()->setText(command);
        auto *paste = coordPage.findChild<QPushButton *>(QStringLiteral("coordPasteTp"));
        expect(paste != nullptr && paste->isVisibleTo(&coordPage),
               "coord tool shows a paste-command button");
        if (paste != nullptr) paste->click();
        expect(parsed == command,
               "paste-command reads the clipboard and hands the line to the parser");

        auto *copy = coordPage.findChild<QPushButton *>(QStringLiteral("coordCopyTp"));
        const QString tp = QStringLiteral("/tp @s 1.00 64.00 -2.00");
        coordPage.setTeleportCommand(tp);
        expect(copy != nullptr, "coord tool shows a copy-tp button");
        if (copy != nullptr) copy->click();
        expect(QGuiApplication::clipboard()->text() == tp,
               "copy-tp puts the generated command back on the clipboard");
    }
    // ---- 坐标换算：生成 tp 命令时按目标维度换算（UI 问题 #133） ----
    {
        ToolboxPage tpPage;
        tpPage.showTool(ToolboxPage::Tool::Coord);
        tpPage.setDimensionChoices({QStringLiteral("主世界"), QStringLiteral("下界"),
                                    QStringLiteral("末地")});
        tpPage.setCoordinates(0, 80.5, 63.0, -76.5);
        auto *targetCombo = tpPage.findChild<QComboBox *>(QStringLiteral("coordTpDimension"));
        expect(targetCombo != nullptr && targetCombo->count() == 3
                   && targetCombo->currentIndex() == 1,
               "coord tool defaults the tp target to the converted dimension (overworld -> nether)");
        int fromIndex = -1;
        int toIndex = -1;
        double gotX = 0.0;
        double gotY = 0.0;
        double gotZ = 0.0;
        QObject::connect(&tpPage, &ToolboxPage::teleportCommandRequested, &tpPage,
                         [&](int from, int to, double x, double y, double z) {
                             fromIndex = from;
                             toIndex = to;
                             gotX = x;
                             gotY = y;
                             gotZ = z;
                         });
        auto *make = tpPage.findChild<QPushButton *>(QStringLiteral("coordMakeTp"));
        if (make != nullptr) make->click();
        expect(fromIndex == 0 && toIndex == 1 && gotX == 80.5 && gotY == 63.0 && gotZ == -76.5,
               "make-tp hands over both dimensions and the unconverted coordinates");
        if (targetCombo != nullptr) targetCombo->setCurrentIndex(2);
        if (make != nullptr) make->click();
        expect(fromIndex == 0 && toIndex == 2, "the target dimension picker reaches the command");
        if (targetCombo != nullptr) targetCombo->setCurrentIndex(1);
        tpPage.setCoordinates(1, 10.0, 64.0, -9.5);
        expect(targetCombo != nullptr && targetCombo->currentIndex() == 0,
               "changing the source dimension moves the tp target off the same dimension");
    }
    ToolboxPage equipmentPage;
    equipmentPage.showTool(ToolboxPage::Tool::Trim);
    const QStringList armorNames{"皮革","锁链","铁","金","钻石","下界合金","海龟壳"};
    equipmentPage.setTrimArmorMaterials(armorNames); equipmentPage.setTrimParts({"头盔","胸甲","护腿","靴子"});
    equipmentPage.setTrimPatterns({"海岸","哨兵"}); equipmentPage.setTrimMaterials({"紫水晶","铜"});
    for(int slot=0;slot<4;++slot) {
        auto *armor=equipmentPage.findChild<QComboBox *>(QStringLiteral("trimArmorSlot%1").arg(slot));
        armor->setCurrentIndex(armor->findData(slot+1));
        auto *pattern=equipmentPage.findChild<QComboBox *>(QStringLiteral("trimPatternSlot%1").arg(slot));
        pattern->setCurrentIndex(pattern->findData(slot%2));
    }
    ToolboxPage equipmentRestored;
    equipmentRestored.showTool(ToolboxPage::Tool::Trim);
    equipmentRestored.setTrimArmorMaterials(armorNames); equipmentRestored.setTrimParts({"头盔","胸甲","护腿","靴子"});
    equipmentRestored.setTrimPatterns({"海岸","哨兵"}); equipmentRestored.setTrimMaterials({"紫水晶","铜"});
    expect(ToolboxInputState::restore(&equipmentRestored,ToolboxInputState::capture(&equipmentPage)),
           "per-equipment trim inputs restore successfully");
    bool independent=true;
    for(int slot=0;slot<4;++slot) independent &= equipmentRestored.trimEquipment()[slot].armor==slot+1
                                                && equipmentRestored.trimEquipment()[slot].pattern==slot%2;
    expect(independent,"per-slot memory is not overwritten by legacy whole-set fields");
    auto oldDocument=QJsonDocument::fromJson(ToolboxInputState::capture(&equipmentPage)).object();
    auto oldFields=oldDocument.value("fields").toObject();
    for(int slot=0;slot<4;++slot) for(const QString &prefix:QStringList{"trimArmorSlot","trimPatternSlot","trimMaterialSlot"})
        oldFields.remove(prefix+QString::number(slot));
    oldFields.insert(ToolboxPage::trimArmorComboName(),4); oldFields.insert(ToolboxPage::trimWholeSetName(),true);
    oldDocument.insert("fields",oldFields);
    expect(ToolboxInputState::restore(&equipmentRestored,QJsonDocument(oldDocument).toJson())
           && equipmentRestored.trimEquipment()[0].armor==4 && equipmentRestored.trimEquipment()[3].armor==4,
           "legacy whole-set drafts migrate into all four equipment columns");
    // 只搭了纹饰那一片时存盘：没搭的那几片（旗帜、地址框…）沿用上一份记住的值，不被清空。
    const auto merged = QJsonDocument::fromJson(ToolboxInputState::capture(&equipmentPage, remembered)).object();
    expect(merged.value("fields").toObject().value(QStringLiteral("serverAddress")).toString() == QStringLiteral("example.test:25565")
        && merged.value("banner") == QJsonDocument::fromJson(remembered).object().value("banner")
        && merged.value("fields").toObject().contains(QStringLiteral("trimArmorSlot0")),
        "saving with only some tool pages built keeps remembered inputs of the unbuilt ones");
    expect(!ToolboxInputState::restore(&restored, "{bad") && restored.bannerLayers() == QStringList{red, blue},
        "corrupt remembered input is rejected without replacing the current draft");
    QSettings inputSettings(root.filePath(QStringLiteral("inputs.ini")), QSettings::IniFormat);
    ToolboxInputState::write(inputSettings, &restored); inputSettings.sync();
    expect(inputSettings.status() == QSettings::NoError && !inputSettings.value(ToolboxInputState::key()).toByteArray().isEmpty(),
        "tool inputs persist in an isolated settings store");
    restored.findChild<QCheckBox *>(QStringLiteral("toolboxRememberInputs"))->setChecked(false);
    ToolboxInputState::write(inputSettings, &restored); inputSettings.sync();
    expect(!inputSettings.contains(ToolboxInputState::key()) && !inputSettings.value(QStringLiteral("Toolbox/RememberInputs")).toBool(),
        "turning off input memory removes the saved draft");
    QList<bool> available;
    for (int i = 0; i < Banner::patternCount(); ++i) available.append(BannerDesign::available(i, QStringLiteral("1.20.4")));
    restored.setBannerPatternAvailability(available);
    auto *patternCombo = restored.findChild<QComboBox *>(ToolboxPage::bannerPatternComboName());
    expect(!(patternCombo->model()->flags(patternCombo->model()->index(flow, 0)) & Qt::ItemIsEnabled)
        && restored.bannerLayers() == QStringList{red, blue}, "version filtering disables future patterns without deleting the design");
    ToolboxPage page;
    page.showTool(ToolboxPage::Tool::Beacon);
    page.showTool(ToolboxPage::Tool::Banner);
    page.setBannerColors({QStringLiteral("白"), QStringLiteral("黑")});
    page.setBannerPatterns({QStringLiteral("测试图案")});
    page.setBannerPatternIcons({QIcon(QPixmap::fromImage(flag))});
    page.setBannerPreview(flag, {});
    auto *visual = page.findChild<QLabel *>(QStringLiteral("bannerVisualPreview"));
    expect(visual != nullptr && !visual->pixmap().isNull(), "banner page displays composed preview");
    page.setBeamResult(mixed.segments, QStringLiteral("test beam"));
    auto *beam = page.findChild<QLabel *>(QStringLiteral("beamColorPreview"));
    expect(beam != nullptr && !beam->pixmap().isNull(), "beacon page displays segment preview");
    expect(page.findChild<QScrollArea *>(ToolboxPage::toolPageName(ToolboxPage::Tool::Banner)) != nullptr,
        "long tool page scrolls instead of increasing main-window minimum height");
    if (qEnvironmentVariableIsSet("WTFML_BANNER_TEST_JAR")) {
        const auto loaded = BannerTextures::load(qEnvironmentVariable("WTFML_BANNER_TEST_JAR"));
        expect(loaded.error.isEmpty() && loaded.images.contains(QStringLiteral("stripe_bottom")), "real local game archive loads bounded banner textures");
        const QString previews = qEnvironmentVariable("WTFML_TOOL_PREVIEW_DIR");
        if (!previews.isEmpty() && loaded.error.isEmpty()) {
            expect(QDir().mkpath(previews), "preview directory exists");
            QStringList names; for (int i = 0; i < Banner::colorCount(); ++i) names.append(Banner::colorName(i));
            page.setBannerColors(names); page.setBannerPatterns(Banner::patternNames());
            page.setBannerVersions(BannerDesign::versions());
            QList<QIcon> icons;
            for (int i = 0; i < Banner::patternCount(); ++i) icons.append(QIcon(QPixmap::fromImage(BannerRender::compose(0, {{i,15}}, loaded.images))));
            page.setBannerPatternIcons(icons);
            const QList<Banner::Layer> layers{{bottom,14}, {top,14}, {Banner::patternIndexById(QStringLiteral("rhombus")),15}};
            QStringList encoded; for (const auto &layer : layers) encoded.append(QStringLiteral("%1:%2").arg(layer.patternIndex).arg(layer.colorIndex));
            page.setBannerLayers(encoded);
            page.setBannerPreview(BannerRender::compose(0, layers, loaded.images), QStringLiteral("已加载本地 Java 1.21.1 贴图 · 二维叠层预览"));
            page.setBannerResult(Banner::plan(0, layers).detailText());
            page.resize(1000, 820); page.showTool(ToolboxPage::Tool::Banner); page.show(); QCoreApplication::processEvents();
            expect(page.grab().save(QDir(previews).filePath(QStringLiteral("banner-preview.png"))), "save actual banner widget preview");
            page.setBeaconResult(Beacon::detailText(0, 4)); page.setBeamColors(names);
            auto *glass = page.findChild<QListWidget *>(QStringLiteral("beamGlassList"));
            if (glass != nullptr) for (int color : {14,11,4}) { auto *item = new QListWidgetItem(names[color] + QStringLiteral("玻璃"), glass); item->setData(Qt::UserRole, color); }
            page.setBeamResult(mixed.segments, QStringLiteral("红色 → 蓝色 → 黄色\n最终颜色：%1\n材料：三种染色玻璃各 1 块").arg(mixed.finalColor.name()));
            page.showTool(ToolboxPage::Tool::Beacon); QCoreApplication::processEvents();
            expect(page.grab().save(QDir(previews).filePath(QStringLiteral("beacon-preview.png"))), "save actual beacon widget preview");
            page.hide();
            DownloadToolPanel cachePreview;
            cachePreview.resize(660, 500); cachePreview.show(); QCoreApplication::processEvents();
            expect(cachePreview.grab().save(QDir(previews).filePath(QStringLiteral("download-cache-preview.png"))), "save actual download cache controls preview");
            cachePreview.hide();
        }
    }
    return failures ? 1 : 0;
}
