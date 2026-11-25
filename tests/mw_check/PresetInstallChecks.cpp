#include "PresetInstallChecks.hpp"
#include "PresetDependencyChecks.hpp"
#include "PresetManifestChecks.hpp"
#include "PresetPreviewChecks.hpp"
#include "Theme.hpp"
#include <QJsonObject>
#include "presets/PresetModInstaller.hpp"
#include "presets/PresetModTargets.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/DownloadBatchController.hpp"
#include "download/DownloadManager.hpp"
#include <QProcess>
#include "download/ResourceStore.hpp"
#include "PresetModEditor.hpp"
#include "PresetEditorPage.hpp"
#include "PresetOptionsEditor.hpp"
#include "PresetManagePage.hpp"
#include "PresetModReview.hpp"
#include "manage/VersionManager.hpp"
#include "util/AppPaths.hpp"
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QTableWidget>
#include <QLineEdit>
#include <QLabel>
#include <QTreeWidget>
#include <QListView>
#include <QSortFilterProxyModel>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QSlider>
#include "presets/PresetStore.hpp"
#include "presets/PresetCatalog.hpp"
#include "controllers/PresetController.hpp"
#include "presets/PresetApply.hpp"
#include "presets/PresetWorkspace.hpp"
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <QEventLoop>
#include <QFile>
#include <QDir>
#include <QTableWidget>
#include <QFontDatabase>
#include <QApplication>
#include <QPointer>
#include <cstdio>
#include <QSemaphore>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <memory>

int runPresetInstallChecks() {
    const auto args = QCoreApplication::arguments();
    const int childAt = args.indexOf("--preset-stage-resume");
    if (childAt >= 0) {
        const auto root = args.value(childAt + 1), endpoint = args.value(childAt + 2);
        if (!QDir::isAbsolutePath(root) || !QFileInfo(root + "/tasks.json").isFile()) return 2;
        ActivityCenterController activity; activity.tasks()->setHistoryPath(root + "/tasks.json"); activity.loadHistory();
        DownloadManager files; DownloadBatchController recovery(&activity, &files, [] { return Tasks::invalidTaskId(); });
        ResourceStore store; store.setModrinthApiBaseOverride(endpoint);
        int continued = 0; bool deployed = false;
        recovery.setContinuation({}, [&](Tasks::TaskId id, const QJsonObject &intent) {
            if (intent.value("kind") != "preset-mods" || Presets::snapshotModTargets(root) != intent.value("modTargets").toObject()) return false;
            ++continued;
            auto *task = new Presets::PresetModInstaller(&store, &store);
            task->setRecovery(&activity, id, intent);
            Presets::ModReference ref; ref.platform = "modrinth"; ref.projectId = "main";
            task->start({ref}, root, "1.20.1", "fabric", {}, [] { return false; }, [&](auto result) {
                deployed = result.ok; activity.finish(id, result.ok, {}, false);
            });
            return true;
        });
        recovery.load();
        const auto wait = [](auto done) { QElapsedTimer timer; timer.start(); while (!done() && timer.elapsed() < 10000) QApplication::processEvents(); return done(); };
        for (const auto &task : activity.tasks()->all()) if (recovery.owns(task.id)) {
            if (!recovery.resume(task.id) || !wait([&] { return activity.tasks()->snapshot(task.id).state == Tasks::TaskState::Paused; })) return 3;
            if (continued || QFileInfo::exists(root + "/mods/main.jar")) return 4;
            if (!recovery.resume(task.id) || !wait([&] { return deployed; }) || continued != 1) return 5;
            return QFileInfo::exists(root + "/mods/main.jar") && QFileInfo::exists(root + "/mods/dep.jar") ? 0 : 6;
        }
        return 7;
    }
    int failures = 0;
    auto check = [&](bool ok, const char *name, const QString &detail = QString()) {
        std::printf("%s %s%s\n", ok ? "PASS" : "FAIL", name,
            !ok && !detail.isEmpty() ? (" [" + detail + "]").toUtf8().constData() : "");
        if (!ok) ++failures;
    };
    const auto pump = [](const std::function<bool()> &done) {
        QElapsedTimer deadline;
        deadline.start();
        while (!done() && deadline.elapsed() < 4000) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        QApplication::processEvents();
        return done();
    };
    struct CatalogGate {
        QSemaphore entered, release;
        std::atomic<int> calls{0};
        std::atomic<bool> done{false};
    };
    const auto scanGate = [](const std::shared_ptr<CatalogGate> &gate, const QString &root) {
        ++gate->calls;
        gate->entered.release();
        (void)gate->release.tryAcquire(1, 3000); // A failing test cannot leave a worker stuck.
        gate->done = true;
        Presets::CatalogSnapshot snapshot;
        snapshot.root = root;
        return snapshot;
    };
    {
        const auto first = std::make_shared<CatalogGate>();
        const auto second = std::make_shared<CatalogGate>();
        int deliveries = 0;
        QString deliveredRoot;
        bool onGuiThread = false;
        Presets::CatalogLoader loader(nullptr, [=](const QString &root) {
            return scanGate(root == QLatin1String("A") ? first : second, root);
        });
        const auto completed = [&](const Presets::CatalogSnapshot &snapshot) {
            ++deliveries;
            deliveredRoot = snapshot.root;
            onGuiThread = QThread::currentThread() == qApp->thread();
        };
        loader.load(QStringLiteral("A"), completed);
        loader.load(QStringLiteral("A"), completed);
        check(pump([&] { return first->entered.available() > 0; }) && loader.isLoading() && !first->done && first->calls == 1,
              "catalog reader runs in background and repeated opens share one request");
        loader.load(QStringLiteral("B"), completed);
        check(pump([&] { return second->entered.available() > 0; }), "new catalog root starts a new generation");
        first->release.release();
        check(pump([&] { return first->done.load(); }) && deliveries == 0 && loader.isLoading(),
              "old root completion cannot replace a newer in-flight catalog");
        second->release.release();
        check(pump([&] { return !loader.isLoading(); }) && deliveries == 1 &&
                  deliveredRoot == QLatin1String("B") && onGuiThread,
              "only current catalog is delivered on the GUI thread");
    }
    {
        const auto gate = std::make_shared<CatalogGate>();
        int deliveries = 0;
        auto loader = std::make_unique<Presets::CatalogLoader>(nullptr, [=](const QString &root) { return scanGate(gate, root); });
        loader->load(QStringLiteral("cancelled"), [&](const auto &) { ++deliveries; });
        check(pump([&] { return gate->entered.available() > 0; }), "invalidated catalog fixture is running");
        loader->invalidate();
        gate->release.release();
        check(pump([&] { return gate->done.load(); }) && !loader->isLoading() && deliveries == 0,
              "explicit refresh invalidates older asynchronous results");
        loader.reset();
    }
    {
        const auto gate = std::make_shared<CatalogGate>();
        int deliveries = 0;
        auto loader = std::make_unique<Presets::CatalogLoader>(nullptr, [=](const QString &root) { return scanGate(gate, root); });
        loader->load(QStringLiteral("destroyed"), [&](const auto &) { ++deliveries; });
        check(pump([&] { return gate->entered.available() > 0; }), "destroyed loader fixture is running");
        loader.reset();
        gate->release.release();
        check(pump([&] { return gate->done.load(); }) && deliveries == 0,
              "destroying loader never calls back into deleted UI");
    }
    {
        const auto gate = std::make_shared<CatalogGate>();
        QWidget host;
        PresetController::Host callbacks;
        callbacks.readCatalog = [=](const QString &root) { return scanGate(gate, root); };
        PresetController controller(nullptr, &host, callbacks);
        controller.open();
        controller.open();
        auto *page = host.findChild<PresetManagePage *>();
        bool uiEvent = false;
        QTimer::singleShot(0, &host, [&] { uiEvent = true; });
        check(pump([&] { return uiEvent && gate->entered.available() > 0; }) &&
                  page && page->isBusy() && !gate->done && gate->calls == 1,
              "real preset page opens and processes input before catalog I/O finishes");
        auto *status = page ? page->findChild<QLabel *>(PresetManagePage::statusLabelName()) : nullptr;
        check(status && !status->isHidden() && status->parentWidget() == page &&
                  status->text().contains(QStringLiteral("读取")),
              "catalog loading feedback stays outside the scrolling editor");
        gate->release.release();
        check(pump([&] { return page && !page->isBusy(); }) &&
                  page->findChild<QPushButton *>(PresetManagePage::newButtonName())->isEnabled(),
              "catalog completion restores preset actions");
        gate->done = false;
        (void)gate->entered.tryAcquire(gate->entered.available());
        controller.refreshList();
        page->showStatus(QStringLiteral("保存了。"));
        uiEvent = false;
        QTimer::singleShot(0, &host, [&] { uiEvent = true; });
        check(pump([&] { return uiEvent && gate->entered.available() > 0; }) &&
                  page->isBusy() && !gate->done,
              "mutation catalog refresh keeps GUI responsive while reading storage");
        gate->release.release();
        check(pump([&] { return !page->isBusy(); }) && status->text() == QStringLiteral("保存了。"),
              "mutation refresh restores actions without erasing operation feedback");
        gate->done = false;
        (void)gate->entered.tryAcquire(gate->entered.available());
        controller.open();
        check(pump([&] { return gate->entered.available() > 0; }), "page destruction fixture is running");
        delete page;
        gate->release.release();
        check(pump([&] { return !controller.catalogLoading(); }) && gate->done && !host.findChild<PresetManagePage *>(),
              "catalog completion tolerates page deletion while controller remains alive");
    }
    {
        QTemporaryDir root;
        check(root.isValid(), "large preset naming fixture has an isolated data root");
        if (root.isValid()) {
            const auto previousRoot = qApp->property("wtfml.testDefaultMinecraftDir");
            qApp->setProperty("wtfml.testDefaultMinecraftDir", root.path());
            {
                Presets::CatalogSnapshot catalog;
                for (int i = 1; i <= 1000; ++i) {
                    Presets::PresetSummary summary;
                    summary.id = Presets::newPresetId();
                    summary.name = i == 1 ? QStringLiteral("新预设") : QStringLiteral("新预设 %1").arg(i);
                    summary.category = Presets::PresetCategory::GameSettings;
                    catalog.presets.append(summary);
                }
                const auto calls = std::make_shared<std::atomic<int>>(0);
                PresetController::Host callbacks;
                callbacks.readCatalog = [catalog, calls](const QString &path) {
                    return ++*calls == 1 ? catalog : Presets::readCatalog(path);
                };
                QWidget host;
                PresetController controller(nullptr, &host, callbacks);
                controller.open();
                check(pump([&] { return !controller.catalogLoading() && !controller.detailsLoading(); }), "large preset naming catalog loads");
                controller.createPreset(Presets::presetCategoryKey(Presets::PresetCategory::GameSettings));
                check(pump([&] { return !controller.catalogLoading() && !controller.detailsLoading(); }), "new preset refresh completes");
                auto *page = host.findChild<PresetManagePage *>();
                bool ok = false;
                const auto created = Presets::loadManifest(PresetController::presetsRoot(), page ? page->currentPresetId() : QString(), &ok);
                // The 1000 catalog names deliberately have no files on disk. A
                // synchronous disk rescan would lose them and choose the base name.
                check(ok && created.name == QStringLiteral("新预设 1001") && *calls == 2,
                      "new preset uses loaded names beyond 999 without rescanning manifests");
                if (ok) Presets::remove(PresetController::presetsRoot(), created.id);
            }
            qApp->setProperty("wtfml.testDefaultMinecraftDir", previousRoot);
        }
    }
    {
        QTemporaryDir root;
        check(root.isValid(), "install picker fixture has an isolated data root");
        if (root.isValid()) {
            const auto previousRoot = qApp->property("wtfml.testDefaultMinecraftDir");
            qApp->setProperty("wtfml.testDefaultMinecraftDir", root.path());
            const QString presetsRoot = PresetController::presetsRoot();
            const QString gameId = Presets::newPresetId();
            const QString modsId = Presets::newPresetId();
            const QString packsId = Presets::newPresetId();
            const QString dataId = Presets::newPresetId();
            const QString compositeId = Presets::newPresetId();
            const QString childModsId = Presets::newPresetId();
            const QString childPacksId = Presets::newPresetId();
            const auto add = [&presetsRoot](const QString &id, const QString &name,
                                            Presets::PresetCategory category,
                                            const QJsonObject &components = QJsonObject()) {
                Presets::PresetManifest manifest;
                manifest.id = id;
                manifest.name = name;
                manifest.category = category;
                if (!components.isEmpty()) {
                    manifest.editorState.insert(QStringLiteral("components"), components);
                }
                return Presets::saveManifest(presetsRoot, manifest);
            };
            QJsonObject parts;
            parts.insert(Presets::presetCategoryKey(Presets::PresetCategory::Mods), childModsId);
            parts.insert(Presets::presetCategoryKey(Presets::PresetCategory::ResourcePacks),
                         childPacksId);
            Presets::PresetIndex index;
            index.order = {gameId, modsId, packsId, dataId, compositeId};
            const bool written =
                add(gameId, QStringLiteral("游戏设置"), Presets::PresetCategory::GameSettings)
                && add(modsId, QStringLiteral("模组"), Presets::PresetCategory::Mods)
                && add(packsId, QStringLiteral("资源包"), Presets::PresetCategory::ResourcePacks)
                && add(dataId, QStringLiteral("数据包"), Presets::PresetCategory::DataPacks)
                && add(compositeId, QStringLiteral("综合"), Presets::PresetCategory::Unset, parts)
                && Presets::saveIndex(presetsRoot, index);
            check(written, "install picker fixture writes four presets plus a composite");
            if (written) {
                QWidget host;
                PresetController controller(nullptr, &host, PresetController::Host{});
                const PresetController::InstallPickerData picker = controller.installPickerData();
                const auto idsOf = [&picker](Presets::PresetCategory category) {
                    QStringList ids;
                    for (const Presets::PresetSummary &summary :
                         picker.optionsFor(Presets::presetCategoryKey(category))) {
                        ids.append(summary.id);
                    }
                    return ids;
                };
                check(picker.presets.size() == 5,
                      "install picker reads the whole store in one pass");
                check(idsOf(Presets::PresetCategory::GameSettings) == QStringList{gameId},
                      "install picker only offers a category's own presets in it");
                // 复合预设那几段跟着父项次序排在普通预设后面，子项带着父项的名字。
                check(idsOf(Presets::PresetCategory::Mods) == QStringList{modsId, childModsId},
                      "install picker offers a composite preset's mod part under mods");
                check(idsOf(Presets::PresetCategory::ResourcePacks)
                          == QStringList{packsId, childPacksId},
                      "install picker offers a composite preset's pack part under resource packs");
                check(idsOf(Presets::PresetCategory::DataPacks) == QStringList{dataId},
                      "install picker does not invent a part a composite preset lacks");
                check(picker.optionsFor(QStringLiteral("no-such-category")).isEmpty(),
                      "install picker answers an unknown category with nothing");
                bool compositeSeen = false;
                for (const PresetController::InstallPickerPreset &row : picker.presets) {
                    if (row.summary.id != compositeId) continue;
                    compositeSeen =
                        row.componentsLoaded
                        && row.components.value(
                               Presets::presetCategoryKey(Presets::PresetCategory::Mods))
                               == childModsId
                        && row.components.size() == 2;
                }
                check(compositeSeen,
                      "install picker carries the composite's parts without rereading the manifest");
                for (const Presets::PresetSummary &option :
                     picker.optionsFor(Presets::presetCategoryKey(Presets::PresetCategory::Mods))) {
                    if (option.id == childModsId) {
                        check(option.name == QStringLiteral("综合")
                                  && option.category == Presets::PresetCategory::Mods,
                              "a composite part keeps the parent's name and takes the asked category");
                    }
                }
            }
            qApp->setProperty("wtfml.testDefaultMinecraftDir", previousRoot);
        }
    }
    QTemporaryDir scratch;
    QTcpServer server;
    check(scratch.isValid() && server.listen(QHostAddress::LocalHost, 0), "local preset fixture starts");
    if (!server.isListening()) return 1;
    const QByteArray payload("preset-fixture-jar");
    const QByteArray legacyPayload("legacy-installed-jar");
    int scenario = 0;
    int exactQueries = 0;
    QPointer<Presets::PresetModInstaller> activeTask;
    QString activeRoot;
    bool mutatedInventory = false;
    int hashQueries = 0;
    bool validHashRequests = true;
    bool holdPresetDownload = false;
    int presetFileRequests = 0;
    bool expireNextPresetFile = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                const auto request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("served").toBool()) return;
                const int headerEnd = request.indexOf("\r\n\r\n") + 4;
                qint64 contentLength = 0;
                for (const auto &line : request.left(headerEnd).split('\n'))
                    if (line.toLower().startsWith("content-length:")) contentLength = line.mid(15).trimmed().toLongLong();
                if (request.size() < headerEnd + contentLength) return;
                socket->setProperty("served", true);
                if (request.contains("/file.jar")) {
                    ++presetFileRequests;
                    if (holdPresetDownload) { socket->write("HTTP/1.1 200 OK\r\nContent-Length: 18\r\n\r\np"); return; }
                    if (expireNextPresetFile) {
                        expireNextPresetFile = false;
                        socket->write("HTTP/1.1 403 Expired\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost(); return;
                    }
                }
                if (scenario == 5) return;
                QByteArray body = payload;
                if (scenario == 29 && request.contains("/project/") && !mutatedInventory) {
                    QFile::remove(activeRoot + QStringLiteral("/wtfml.preset-mod-install.json"));
                    mutatedInventory = QDir().rmdir(activeRoot);
                }
                if (scenario == 25 && request.contains("/file.jar") && !mutatedInventory) {
                    mutatedInventory = true;
                    QFile changed(activeRoot + QStringLiteral("/mods/renamed.jar"));
                    if (changed.open(QIODevice::WriteOnly)) changed.write("externally-changed");
                }
                int status = 200;
                if (request.contains("/project/")) {
                    const bool missing = request.contains("/missing/");
                    const bool dependency = request.contains("/dep/");
                    QJsonArray versions;
                    if (!missing) {
                        const QString hash = (scenario == 3 || ((scenario == 7 || scenario == 10) && dependency)) ? QString(40, QLatin1Char('0')) :
                            QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha1).toHex());
                        QJsonArray dependencies;
                        if (!dependency) dependencies.append(QJsonObject{{"project_id", "dep"}, {"dependency_type", "required"}});
                        if (!dependency && scenario == 8) {
                            auto pin = dependencies[0].toObject(); pin.insert("version_id", "dep-old"); dependencies[0] = pin;
                        }
                        if (!dependency && scenario >= 11) {
                            dependencies = QJsonArray{QJsonObject{{"version_id", "dep-exact"}, {"dependency_type", "required"}}};
                            if (scenario == 16) dependencies.append(dependencies[0]);
                        }
                        versions.append(QJsonObject{{"id", dependency ? "dep-version" : "main-version"},
                            {"version_number", "1"}, {"game_versions", QJsonArray{"1.20.1"}}, {"loaders", QJsonArray{"fabric"}},
                            {"dependencies", dependencies}, {"files", QJsonArray{QJsonObject{
                                {"url", QStringLiteral("http://127.0.0.1:%1/file.jar").arg(server.serverPort())},
                                {"filename", dependency ? "dep.jar" : "main.jar"}, {"size", payload.size()}, {"primary", true},
                                {"hashes", QJsonObject{{"sha1", hash}}}}}}});
                        if (dependency && scenario == 8) {
                            auto old = versions[0].toObject(); old.insert("id", "dep-old");
                            auto files = old.value("files").toArray(); auto file = files[0].toObject();
                            file.insert("filename", "dep-old.jar"); files[0] = file; old.insert("files", files);
                            versions.append(old);
                        }
                    }
                    body = QJsonDocument(versions).toJson(QJsonDocument::Compact);
                }
                if (request.contains("/version/")) {
                    ++exactQueries;
                    if (scenario == 17) {
                        // Cancel only after the request actually arrived: no timing race
                        // with metadata processing on a slow CI runner.
                        if (activeTask) QTimer::singleShot(0, activeTask, [task = activeTask] {
                            if (task) task->cancel();
                        });
                        return;
                    }
                    const bool leaf = request.contains("/leaf-exact");
                    QJsonObject release{
                        {"id", scenario == 14 ? "wrong-release" : (leaf ? "leaf-exact" : "dep-exact")},
                        {"project_id", leaf ? "leaf" : "dep"},
                        {"version_number", "1"},
                        {"game_versions", QJsonArray{scenario == 15 ? "1.19.4" : "1.20.1"}},
                        {"loaders", QJsonArray{"fabric"}},
                        {"dependencies", scenario == 18 && !leaf
                            ? QJsonArray{QJsonObject{{"version_id", "leaf-exact"}, {"dependency_type", "required"}}}
                            : QJsonArray{}},
                        {"files", QJsonArray{QJsonObject{
                            {"url", QStringLiteral("http://127.0.0.1:%1/file.jar").arg(server.serverPort())},
                            {"filename", leaf ? "leaf-exact.jar" : "dep-exact.jar"},
                            {"size", payload.size()}, {"primary", true},
                            {"hashes", QJsonObject{{"sha1", QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha1).toHex())}}}}}}};
                    if (scenario == 12) status = 404;
                    if (scenario == 19) release.remove("project_id");
                    body = scenario == 13 ? QByteArray("not-json") : QJsonDocument(release).toJson(QJsonDocument::Compact);
                }
                if (request.startsWith("POST /version_files ")) {
                    ++hashQueries;
                    const auto lookup = QJsonDocument::fromJson(request.mid(headerEnd)).object();
                    const auto hashes = lookup.value("hashes").toArray();
                    QSet<QString> unique;
                    for (const auto &hash : hashes) unique.insert(hash.toString());
                    validHashRequests = validHashRequests && !hashes.isEmpty() && hashes.size() <= 100
                        && hashes.size() == unique.size() && lookup.size() == 2
                        && lookup.value("algorithm").toString() == QStringLiteral("sha1");
                    if (scenario == 38) {
                        if (activeTask) QTimer::singleShot(0, activeTask, [task = activeTask] { if (task) task->cancel(); });
                        return;
                    }
                    QJsonObject found;
                    const QString legacySha = QString::fromLatin1(QCryptographicHash::hash(legacyPayload, QCryptographicHash::Sha1).toHex());
                    if (scenario >= 30 && scenario != 33 && scenario != 34 && scenario != 35 && scenario != 40) {
                        const QJsonObject release{
                            {"id", scenario == 31 ? "dep-old" : "dep-exact"}, {"project_id", "dep"},
                            {"version_number", "old"}, {"game_versions", QJsonArray{scenario == 32 ? "1.19.4" : "1.20.1"}},
                            {"loaders", QJsonArray{"fabric"}}, {"dependencies", QJsonArray{}},
                            {"files", QJsonArray{QJsonObject{{"filename", "old-download-name.jar"},
                                {"url", QStringLiteral("http://127.0.0.1:%1/file.jar").arg(server.serverPort())},
                                {"size", legacyPayload.size()}, {"primary", true},
                                {"hashes", QJsonObject{{"sha1", scenario == 37 ? QString(40, QLatin1Char('0')) : legacySha}}}}}}};
                        found.insert(scenario == 36 ? QString(40, QLatin1Char('1')) : legacySha, release);
                    }
                    if (scenario == 34) status = 429;
                    body = scenario == 35 ? QByteArray("invalid-json") : QJsonDocument(found).toJson(QJsonDocument::Compact);
                }
                socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Length: " + QByteArray::number(body.size()) +
                              "\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    for (bool expired : {false, true}) {
        const auto root = scratch.filePath(expired ? "expired-preset" : "restart-preset"); QDir().mkpath(root);
        ActivityCenterController activity; activity.tasks()->setHistoryPath(root + "/tasks.json");
        const auto id = activity.begin(Tasks::TaskKind::Install, "preset stage", "test");
        ResourceStore store; const auto endpoint = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        store.setModrinthApiBaseOverride(endpoint);
        QPointer<Presets::PresetModInstaller> task = new Presets::PresetModInstaller(&store, &store);
        const QJsonObject intent{{"kind", "preset-mods"}, {"modTargets", Presets::snapshotModTargets(root)}};
        task->setRecovery(&activity, id, intent);
        Presets::ModReference ref; ref.platform = "modrinth"; ref.projectId = "main";
        bool finished = false; holdPresetDownload = true; const int before = presetFileRequests;
        task->start({ref}, root, "1.20.1", "fabric", {}, [] { return false; }, [&](auto) { finished = true; });
        check(pump([&] { return presetFileRequests > before; }) && task && task->pauseDownload() && !finished,
              "preset file stage pauses without deployment or a failure terminal signal");
        check(activity.tasks()->saveHistory(), "paused preset task history saved");
        delete task.data(); holdPresetDownload = false; expireNextPresetFile = expired;
        QProcess child; auto childArgs = args.mid(1); childArgs << "--preset-stage-resume" << root << endpoint;
        child.start(QCoreApplication::applicationFilePath(), childArgs);
        QElapsedTimer timer; timer.start();
        while (child.state() != QProcess::NotRunning && timer.elapsed() < 15000) QApplication::processEvents();
        check(child.state() == QProcess::NotRunning && child.exitCode() == 0,
              "fresh process fills preset stage then waits for explicit deployment and rechecks metadata", QString::fromUtf8(child.readAllStandardOutput() + child.readAllStandardError()));
        if (child.state() != QProcess::NotRunning) { child.kill(); child.waitForFinished(1000); }
        check(presetFileRequests == before + (expired ? 3 : 2) && QFileInfo::exists(root + "/mods/main.jar"),
              "restart reuses verified stage; expired source requires explicit requery and never silently deploys");
    }
    {
        const auto previousRoot = AppPaths::minecraftDir();
        for (const QString mode : {QString("normal"), QString("cancel"), QString("changed"), QString("destroy"), QString("running")}) {
            scenario = 0;
            const auto gameRoot = scratch.filePath("controller-resume-" + mode);
            const QString instance = "resume-fixture";
            const auto root = gameRoot + "/versions/" + instance;
            QDir().mkpath(root + "/mods");
            AppPaths::setMinecraftDir(gameRoot);
            VersionManager versions;
            check(versions.updateVersionMeta(instance, {{"realVersion","1.20.1"},{"loaderName","Fabric"},{"loaderVersion","0.16.9"}}),
                  "controller resume owns isolated version metadata");
            QFile bulk(root + "/mods/local-data.bin");
            bool prepared=bulk.open(QIODevice::WriteOnly);
            const QByteArray chunk(1024*1024,'r');
            for(int i=0;i<32 && prepared;++i) prepared=bulk.write(chunk)==chunk.size();
            bulk.close();
            check(prepared,"controller resume owns a 32 MiB original");
            if(!prepared)continue;
            auto manifest=Presets::create(PresetController::presetsRoot(),"resume fixture",{},Presets::PresetCategory::Mods);
            Presets::ModReference ref;ref.platform="modrinth";ref.projectId="main";manifest.modReferences.append(ref);
            check(Presets::saveManifest(PresetController::presetsRoot(),manifest),"controller resume stores exact preset consent");
            const auto targets=Presets::snapshotModTargets(root);
            if(mode=="changed") {QFile added(root+"/mods/external.json");check(added.open(QIODevice::WriteOnly)&&added.write("user-file")==9,"controller resume external change fixture");}
            QString loader,loaderVersion;versions.instanceLoaderInfo(instance,&loader,&loaderVersion);
            const QJsonObject intent{{"kind","preset-mods"},{"root",gameRoot},{"instance",instance},
                {"game",versions.instanceGameVersion(instance)},{"loader",loader},{"modTargets",targets},
                {"selection",QJsonArray{Presets::manifestToJson(manifest)}}};
            ActivityCenterController activity;activity.tasks()->setHistoryPath(gameRoot+"/tasks.json");
            const auto id=activity.begin(Tasks::TaskKind::Install,"controller continuation",instance);
            QWidget host;bool running=false,busy=false;
            PresetController::Host callbacks;
            callbacks.isRunning=[&](const QString &){return running;};
            callbacks.setFileOperationBusy=[&](const QString &,bool value){busy=value;};
            auto controller=std::make_unique<PresetController>(&versions,&host,callbacks);
            controller->setRecoveryHost(&activity,{});
            const int before=presetFileRequests;
            QElapsedTimer submit;submit.start();
            check(controller->continueModInstallation(id,intent)&&controller->isInstallingMods()&&busy,
                "real continuation admits background validation while retaining instance busy guard");
            printf("MEASURE preset-controller-resume mode=%s bytes=33554432 submit_us=%lld\n",qPrintable(mode),static_cast<long long>(submit.nsecsElapsed()/1000));
            check(activity.tasks()->snapshot(id).detail.contains(QStringLiteral("后台建立"))&&presetFileRequests==before,
                "real continuation returns before target hash, platform queries or deployment");
            for(auto *store:controller->findChildren<ResourceStore *>())store->setModrinthApiBaseOverride(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
            check(!controller->continueModInstallation(id,intent),"duplicate continuation cannot create a second validator");
            if(mode=="running")running=true;
            if(mode=="cancel")controller->cancelModInstall();
            if(mode=="destroy") {controller.reset();QApplication::processEvents();}
            else check(pump([&]{
                if (mode == "normal") {
                    for (auto *dialog : host.findChildren<QDialog *>()) {
                        auto *review = dynamic_cast<PresetModReview *>(dialog);
                        if (review && review->isVisible())
                            review->findChild<QPushButton *>(PresetModReview::confirmButtonName())->click();
                    }
                }
                return !controller->isInstallingMods();
            }),"real continuation finishes validation or safely rejects it");
            check(QFileInfo::exists(root+"/mods/main.jar")==(mode=="normal"),"real continuation never deploys after change, cancellation, destruction or running state");
            if(mode=="normal")check(activity.tasks()->snapshot(id).state==Tasks::TaskState::Succeeded,"unchanged continuation completes the same original task");
            else check(presetFileRequests==before,"rejected real continuation does not download a mod");
            if(mode=="cancel")check(activity.tasks()->snapshot(id).state==Tasks::TaskState::Cancelled,"user-cancelled continuation is cancelled rather than failed");
            check(QFileInfo(root+"/mods/local-data.bin").size()==32LL*1024*1024,"real continuation retains original user file");
        }
        AppPaths::setMinecraftDir(previousRoot);
    }
    for (const QString mode : {QString("normal"), QString("cancel"), QString("changed"), QString("destroy")}) {
        scenario = 0;
        const auto root = scratch.filePath("initial-snapshot-" + mode);
        QDir().mkpath(root + "/mods");
        const auto original = root + "/mods/local-data.bin";
        QFile bulk(original);
        bool prepared = bulk.open(QIODevice::WriteOnly);
        const QByteArray chunk(1024 * 1024, 's');
        for (int i = 0; i < 32 && prepared; ++i) prepared = bulk.write(chunk) == chunk.size();
        bulk.close();
        check(prepared, "initial preset snapshot owns a 32 MiB local file");
        if (!prepared) continue;
        ActivityCenterController activity; activity.tasks()->setHistoryPath(root + "/tasks.json");
        const auto id = activity.begin(Tasks::TaskKind::Install, "initial snapshot", "test");
        ResourceStore store;
        store.setModrinthApiBaseOverride(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        QPointer<Presets::PresetModInstaller> task = new Presets::PresetModInstaller(&store, &store);
        QJsonObject intent{{"kind", "preset-mods"}};
        if (mode == "changed") {
            intent.insert("modTargets", Presets::snapshotModTargets(root));
            QFile added(root + "/mods/external.json");
            check(added.open(QIODevice::WriteOnly) && added.write("user-file") == 9, "initial snapshot external change fixture");
        }
        task->setRecovery(&activity, id, intent);
        Presets::ModReference ref; ref.platform = "modrinth"; ref.projectId = "main";
        bool finished = false; Presets::PresetModInstaller::Result result;
        const int before = presetFileRequests;
        QElapsedTimer submit; submit.start();
        task->start({ref}, root, "1.20.1", "fabric", {}, [] { return false; }, [&](auto value) { result = value; finished = true; });
        const auto submittedUs = submit.nsecsElapsed()/1000;
        check(!finished && activity.tasks()->snapshot(id).detail.contains(QStringLiteral("后台建立")) && presetFileRequests == before,
              "initial snapshot returns before hashing completion, network or deployment");
        printf("MEASURE preset-initial-snapshot mode=%s bytes=33554432 submit_us=%lld\n",qPrintable(mode),static_cast<long long>(submittedUs));
        if (mode == "cancel") task->cancel();
        if (mode == "destroy") {
            delete task.data();
            QApplication::processEvents();
            check(task.isNull() && !finished, "destroyed initial snapshot owner never delivers a completion callback");
        } else {
            check(pump([&]{return finished;}), "initial snapshot reaches a bounded completion");
            check(mode == "normal" ? result.ok : (!result.ok && (mode != "cancel" || result.cancelled)),
                  "initial snapshot accepts normal state and rejects cancelled or changed state");
        }
        check(QFileInfo::exists(root + "/mods/main.jar") == (mode == "normal"), "initial snapshot cancellation/change/destruction never deploys");
        check(QFileInfo(original).size() == 32LL*1024*1024, "initial snapshot preserves the user file");
        if (mode != "normal") check(presetFileRequests == before, "rejected initial snapshot never downloads a mod");
    }
    for (const QString mode : {QString("resume"), QString("changed"), QString("cancel")}) {
        const auto root = scratch.filePath("resume-scan-" + mode);
        QDir().mkpath(root + "/mods");
        QFile bulk(root + "/mods/local-data.bin");
        bool prepared = bulk.open(QIODevice::WriteOnly);
        const QByteArray chunk(1024 * 1024, 'm');
        for (int i = 0; i < 32 && prepared; ++i) prepared = bulk.write(chunk) == chunk.size();
        bulk.close();
        check(prepared, "preset resume scan owns a 32 MiB local file");
        if (!prepared) continue;
        ActivityCenterController activity; activity.tasks()->setHistoryPath(root + "/tasks.json");
        const auto id = activity.begin(Tasks::TaskKind::Install, "resume scan", "test");
        ResourceStore store;
        store.setModrinthApiBaseOverride(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        QPointer<Presets::PresetModInstaller> task = new Presets::PresetModInstaller(&store, &store);
        task->setRecovery(&activity, id, {{"kind", "preset-mods"}, {"modTargets", Presets::snapshotModTargets(root)}});
        Presets::ModReference ref; ref.platform = "modrinth"; ref.projectId = "main";
        bool finished = false; Presets::PresetModInstaller::Result result;
        holdPresetDownload = true; const int before = presetFileRequests;
        task->start({ref}, root, "1.20.1", "fabric", {}, [] { return false; }, [&](auto value) { result = value; finished = true; });
        const bool paused = pump([&] { return presetFileRequests > before; }) && task && task->pauseDownload();
        check(paused, "live preset download pauses before asynchronous target scan");
        holdPresetDownload = false;
        if (!paused) { if (task) task->cancel(); continue; }
        if (mode == "changed") {
            QFile added(root + "/mods/external.json");
            check(added.open(QIODevice::WriteOnly) && added.write("user-file") == 9,
                "external user file appears after preset consent");
            added.close();
        }
        QElapsedTimer submit; submit.start();
        check(task->resumeDownload() && !finished, "preset resume accepts background scanning without deploying synchronously");
        printf("MEASURE preset-resume-submit mode=%s bytes=33554432 elapsed_us=%lld\n", qPrintable(mode), static_cast<long long>(submit.nsecsElapsed() / 1000));
        fflush(stdout);
        check(!task->resumeDownload(), "preset resume rejects duplicate directory scans");
        if (mode == "cancel") task->cancel();
        if (mode == "changed") {
            check(pump([&] { return activity.tasks()->snapshot(id).detail.contains(QStringLiteral("外部修改")); }) && !finished &&
                !QFileInfo::exists(root + "/mods/main.jar") && QFileInfo::exists(root + "/mods/external.json"),
                "changed directory blocks resume and preserves the external user file");
            if (task) task->cancel();
        } else {
            check(pump([&] { return finished; }) && (mode == "cancel" ? result.cancelled : result.ok),
                "background preset scan resumes successfully or honours cancellation");
            check(QFileInfo::exists(root + "/mods/main.jar") == (mode == "resume"),
                "cancelled directory scan never deploys a mod");
        }
    }
    for (scenario = 0; scenario < 41; ++scenario) {
        exactQueries = 0;
        hashQueries = 0;
        validHashRequests = true;
        const QString root = scratch.filePath(QString::number(scenario)); QDir().mkpath(root);
        activeRoot = root;
        mutatedInventory = false;
        ResourceStore store;
        store.setModrinthApiBaseOverride(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        auto *task = new Presets::PresetModInstaller(&store, &store);
        activeTask = task;
        QList<Presets::ModReference> refs;
        Presets::ModReference main; main.platform = QStringLiteral("modrinth");
        main.projectId = scenario == 1 ? QStringLiteral("missing") : QStringLiteral("main");
        if (scenario == 7 || scenario == 10) main.required = false;
        if (scenario == 9 || scenario == 26) main.gameVersion = QStringLiteral("1.19.4");
        refs.append(main);
        if (scenario == 28) { auto optional = main; optional.projectId = QStringLiteral("optional"); optional.required = false; refs.append(optional); }
        if (scenario == 10) { auto dependency = main; dependency.projectId = QStringLiteral("dep"); dependency.required = true; refs.append(dependency); }
        if (scenario == 2) { auto optional = main; optional.projectId = QStringLiteral("missing"); optional.required = false; refs.append(optional); }
        if (scenario == 4) { QDir().mkpath(root + "/mods"); QFile f(root + "/mods/main.jar"); if (f.open(QIODevice::WriteOnly)) f.write("existing"); }
        if (scenario >= 20 && scenario < 29) {
            QDir().mkpath(root + "/mods");
            const QString name = scenario == 22 ? QStringLiteral("renamed.jar.disabled") : QStringLiteral("renamed.jar");
            QFile existing(root + "/mods/" + name);
            if (existing.open(QIODevice::WriteOnly)) existing.write(payload);
            existing.close();
            const auto identity = [&](const QString &filename) { return QJsonObject{
                {"filename", filename}, {"sha1", QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha1).toHex())},
                {"project", scenario == 24 ? "modrinth:main" : (scenario == 28 ? "modrinth:optional" : "modrinth:dep")},
                {"versionId", scenario == 24 ? "main-version" : ((scenario == 21 || scenario == 28) ? "dep-old" : "dep-exact")}}; };
            QJsonArray seeded{identity(name)};
            if (scenario == 23) {
                QFile duplicate(root + "/mods/duplicate.jar");
                if (duplicate.open(QIODevice::WriteOnly)) duplicate.write(payload);
                seeded.append(identity(QStringLiteral("duplicate.jar")));
            }
            QFile audit(root + "/wtfml.preset-mod-install.json");
            if (scenario != 27 && audit.open(QIODevice::WriteOnly)) audit.write(QJsonDocument(QJsonObject{
                {"format", "wtfml-preset-mod-result/1"}, {"plannedFiles", seeded}}).toJson());
        }
        if (scenario >= 30) {
            QDir().mkpath(root + "/mods");
            const int count = scenario == 40 ? 101 : (scenario == 39 ? 2 : 1);
            for (int i = 0; i < count; ++i) {
                QFile old(root + QStringLiteral("/mods/untracked-%1.jar").arg(i));
                if (old.open(QIODevice::WriteOnly)) old.write(scenario == 40 ? legacyPayload + QByteArray::number(i) : legacyPayload);
            }
        }
        QEventLoop loop; QTimer deadline; deadline.setSingleShot(true);
        Presets::PresetModInstaller::Result result;
        bool completed = false;
        QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { task->cancel(); }); deadline.start(6000);
        task->start(refs, root, QStringLiteral("1.20.1"), QStringLiteral("fabric"), {},
            [&] { return scenario == 6; }, [&](const auto &value) { result = value; completed = true; loop.quit(); });
        if (scenario == 5) QTimer::singleShot(50, task, [task] { task->cancel(); });
        if (!completed) loop.exec();
        check(completed && result.ok == (scenario == 0 || scenario == 8 || scenario == 11 || scenario == 16 || scenario == 18
                                         || scenario == 20 || scenario == 22 || scenario == 24 || scenario == 30
                                         || (scenario >= 33 && scenario <= 37) || scenario == 40), "preset resolves/downloads or reports an explicit partial/failure result", QString::number(scenario) + ": " + result.lines.join("; "));
        if (scenario >= 30) {
            check(validHashRequests && hashQueries == (scenario == 40 ? 2 : 1),
                  "exact installed hash requests are deduplicated, bounded to 100 and contain only hashes/algorithm");
            if (scenario == 30) check(result.installed == 1 && QFile::exists(root + "/mods/main.jar")
                && !QFile::exists(root + "/mods/dep-exact.jar"),
                "hash lookup recognizes an untracked renamed exact dependency without downloading a duplicate");
            if (scenario == 31 || scenario == 32 || scenario == 38 || scenario == 39)
                check(!QFile::exists(root + "/mods/main.jar") && !QFile::exists(root + "/mods/dep-exact.jar"),
                      "old/incompatible/duplicate installed identity or cancellation blocks new writes");
            if (scenario == 38) check(result.cancelled, "hash lookup cancellation is explicit");
            if ((scenario >= 33 && scenario <= 37) || scenario == 40) {
                bool unknownReported = false;
                for (const auto &line : result.lines) if (line.contains(QStringLiteral("身份尚未确认"))) unknownReported = true;
                check(unknownReported && QFile::exists(root + "/mods/untracked-0.jar"),
                      "unmatched/throttled/malformed/extra/mismatched hash response stays unknown and preserves old files");
            }
        }
        if (scenario >= 11 && scenario < 20) {
            check(exactQueries == (scenario == 18 ? 2 : 1), "version-only dependency lookup is bounded and deduplicated");
            if (scenario == 11 || scenario == 16 || scenario == 18) {
                check(QFile::exists(root + "/mods/main.jar") && QFile::exists(root + "/mods/dep-exact.jar")
                          && !QFile::exists(root + "/mods/dep.jar"),
                      "version-only dependency installs its exact release, never a guessed latest release");
                if (scenario == 18) check(QFile::exists(root + "/mods/leaf-exact.jar"), "version-only transitive dependency resolves recursively");
            } else {
                check(!QFile::exists(root + "/mods/main.jar") && !QFile::exists(root + "/mods/dep-exact.jar"),
                      "failed/malformed/mismatched/incompatible/cancelled identity lookup writes no mods");
                check(!result.lines.isEmpty(), "identity lookup failure has an explicit report");
                if (scenario == 17) check(result.cancelled, "identity lookup cancellation remains explicit");
            }
        }
        if (scenario == 20) check(result.installed == 1 && QFile::exists(root + "/mods/main.jar")
            && !QFile::exists(root + "/mods/dep-exact.jar"), "renamed installed exact dependency is satisfied without a duplicate JAR");
        if (scenario == 21 || scenario == 23) check(!QFile::exists(root + "/mods/main.jar") && !QFile::exists(root + "/mods/dep-exact.jar"),
            "different installed release or duplicate project blocks the install without replacing anything");
        if (scenario == 22) check(QFile::exists(root + "/mods/renamed.jar.disabled") && QFile::exists(root + "/mods/dep-exact.jar"),
            "disabled dependency is preserved but does not satisfy an enabled dependency");
        if (scenario == 24) check(result.installed == 1 && !QFile::exists(root + "/mods/main.jar")
            && QFile::exists(root + "/mods/dep-exact.jar"), "installed parent is kept while its missing dependency is repaired");
        if (scenario == 25) check(mutatedInventory && !QFile::exists(root + "/mods/main.jar"),
            "inventory mutation during downloads aborts before writing new mods");
        if (scenario == 27) check(!result.ok && !QFile::exists(root + "/mods/main.jar")
            && !QFile::exists(root + "/mods/dep-exact.jar"),
            "a partial hash match is reconsidered when another project shares the bytes; ambiguity blocks duplicates");
        if (scenario == 28) check(result.optionalFailures > 0 && QFile::exists(root + "/mods/main.jar")
            && QFile::exists(root + "/mods/dep-exact.jar"),
            "unknown installed optional release is reported without blocking required mods");
        if (scenario == 29) check(mutatedInventory && !QDir(root).exists(),
            "externally removed empty instance is not recreated by the commit phase");
        if (scenario == 0 || scenario == 2)
            check(QFile::exists(root + "/mods/main.jar") && QFile::exists(root + "/mods/dep.jar"), "main mod and required dependency reach the instance");
        if (scenario == 1 || scenario == 3 || scenario == 5 || scenario == 6)
            check(!QFile::exists(root + "/mods/main.jar"), "missing/checksum/cancel/running guard never writes a main mod");
        if (scenario == 4) { QFile f(root + "/mods/main.jar"); check(f.open(QIODevice::ReadOnly) && f.readAll() == "existing", "existing conflicting jar is preserved"); }
        if (scenario == 2) check(result.optionalFailures > 0, "optional missing is reported independently from installed required mods");
        if (scenario == 5) check(result.cancelled, "cancellation is explicit");
        if (scenario == 7) check(!QFile::exists(root + "/mods/main.jar") && result.optionalFailures > 0,
                                 "optional parent is not installed when its required dependency download fails");
        if (scenario == 8) check(QFile::exists(root + "/mods/dep-old.jar") && !QFile::exists(root + "/mods/dep.jar"),
                                 "exact dependency version is selected rather than the newest incompatible release");
        if (scenario == 9) check(!QFile::exists(root + "/mods/main.jar"), "preset version restriction never installs into the wrong game version");
        if (scenario == 10) check(!QFile::exists(root + "/mods/main.jar") && !QFile::exists(root + "/mods/dep.jar"),
                                  "dependency encountered through optional parent stays required for an explicit required root");
        QFile audit(root + "/wtfml.preset-mod-install.json");
        const bool audited = audit.open(QIODevice::ReadOnly);
        const auto record = audited ? QJsonDocument::fromJson(audit.readAll()).object() : QJsonObject();
        if (scenario >= 30) {
            bool tracked = false;
            for (const auto &entry : record.value("plannedFiles").toArray())
                if (entry.toObject().value("filename").toString() == QStringLiteral("untracked-0.jar")) tracked = true;
            check(tracked == (scenario == 30 || scenario == 31 || scenario == 32 || scenario == 39),
                  "only verified hash identity is recorded for preexisting untracked files");
        }
        if (scenario != 29) check(audited && record.value("gameVersion").toString() == QStringLiteral("1.20.1") &&
              record.value("success").toBool() == result.ok, "preset operation records context and truthful result");
        if (scenario >= 20 && scenario != 27 && scenario < 29) {
            bool retained = false;
            for (const auto &entry : record.value("plannedFiles").toArray())
                if (entry.toObject().value("filename").toString().startsWith(QStringLiteral("renamed.jar"))) retained = true;
            check(retained, "success or failure preserves previously recorded installed identities");
        }
    }
    PresetModEditor editor({{QStringLiteral("modrinth"), QStringLiteral("abc123"), QStringLiteral("Example"), {}, {}, false}});
    const auto values = editor.entries();
    check(values.size() == 1 && values[0].projectId == QStringLiteral("abc123") && !values[0].required,
          "mod preset editor round-trips stable identity and optional status");
    auto *quickInput = editor.findChild<QLineEdit *>(QStringLiteral("presetModQuickAdd"));
    auto *quickAdd = editor.findChild<QPushButton *>(QStringLiteral("presetModQuickAddButton"));
    quickInput->setText(QStringLiteral("https://modrinth.com/mod/sodium?from=test")); quickAdd->click();
    check(editor.entries().size() == 2 && editor.entries().last().projectId == QStringLiteral("sodium"),
          "mod preset accepts a public Modrinth URL without asking the user for an ID");
    quickInput->setText(QStringLiteral("https://modrinth.com/mod/sodium")); quickAdd->click();
    check(editor.entries().size() == 2, "duplicate quick add locates existing row instead of duplicating it");
    quickInput->setText(QStringLiteral("https://untrusted.example/mod/foo")); quickAdd->click();
    check(editor.entries().size() == 2, "unknown site cannot become a trusted project reference");
    {
        // 从实例提取时两个平台都没认出来的行：平台那格停在「未知来源（请选择）」。
        // 「只看没认出来的」只藏行、不动数据；「移除没认出来的」才真删，并且不碰认出来的那些。
        PresetModEditor unknownEditor(
            {{QString(), QString(), QStringLiteral("mystery.jar"), {}, {}, true},
             {QStringLiteral("modrinth"), QStringLiteral("abc123"), QStringLiteral("Example"), {}, {}, true},
             {QStringLiteral("ftb"), QString(), QStringLiteral("odd.jar"), {}, {}, true}});
        auto *onlyUnknown = unknownEditor.findChild<QCheckBox *>(QStringLiteral("presetModOnlyUnknown"));
        auto *removeUnknown = unknownEditor.findChild<QPushButton *>(QStringLiteral("presetModRemoveUnknown"));
        auto *table = unknownEditor.findChild<QTableWidget *>(QStringLiteral("presetModEditorTable"));
        auto *problem = unknownEditor.findChild<QLabel *>(QStringLiteral("presetModProblem"));
        check(onlyUnknown != nullptr && removeUnknown != nullptr && table != nullptr && problem != nullptr
                  && table->rowCount() == 3,
              "★ 模组编辑页有「只看没认出来的」与「移除没认出来的」两件（没认出来的行照样在表里）");
        if (onlyUnknown && removeUnknown && table && problem) {
            check(!onlyUnknown->isChecked() && !table->isRowHidden(0) && !table->isRowHidden(2),
                  "★ 默认不筛：没认出来的行和认出来的一起露着");
            onlyUnknown->setChecked(true);
            check(!table->isRowHidden(0) && table->isRowHidden(1) && !table->isRowHidden(2)
                      && unknownEditor.entries().size() == 3,
                  "★★ 勾上「只看没认出来的」⇒ 只露没认出来的那两行，数据一行都没少",
                  QStringLiteral("%1 %2 %3 / %4")
                      .arg(table->isRowHidden(0)).arg(table->isRowHidden(1))
                      .arg(table->isRowHidden(2)).arg(unknownEditor.entries().size()));
            onlyUnknown->setChecked(false);
            check(!table->isRowHidden(0) && !table->isRowHidden(2),
                  "★ 取消筛选 ⇒ 之前藏起来的行都回来");
            removeUnknown->click();
            const auto left = unknownEditor.entries();
            check(left.size() == 1 && left.first().platform == QStringLiteral("modrinth")
                      && left.first().projectId == QStringLiteral("abc123"),
                  "★★ 「移除没认出来的」只删平台还不认识的那几行，认出来的原样留着",
                  QString::number(left.size()));
            check(problem->text().contains(QStringLiteral("已移除 2 条来源未知的记录")) &&
                      problem->text().contains(QStringLiteral("选择「取消」可撤回本次编辑")),
                  "★ 删完说清楚删了几行（并且提醒「取消」能撤回）", problem->text());
            removeUnknown->click();
            check(problem->text().contains(QStringLiteral("无未识别的模组")),
                  "★ 再点一次 ⇒ 说「没有没认出来的行」，不是默默无反应", problem->text());
        }
    }
    PresetOptionsEditor options({{QStringLiteral("fov"), QStringLiteral("0.5")},
                                {QStringLiteral("key_key.jump"), QStringLiteral("key.keyboard.space")}});
    check(options.entries().size() == 2 && options.entries()[0].value == QStringLiteral("0.5"),
          "game settings editor round-trips selected options and keys");
    auto *optionTable = options.findChild<QTableWidget *>(QStringLiteral("presetOptionsEditorTable"));
    auto *fovEditor = optionTable ? optionTable->cellWidget(0, 1) : nullptr;
    auto *degrees = fovEditor ? fovEditor->findChild<QSlider *>() : nullptr;
    check(degrees && degrees->value() == 90, "stored FOV is edited in human-readable degrees");
    if (degrees) {
        degrees->setValue(80);
        check(options.entries()[0].value == QStringLiteral("0.25"), "human-readable FOV writes the original normalized game format");
    }
    {
        PresetManagePage manage;
        PresetManagePage::PresetItem one; one.id = QStringLiteral("one"); one.name = QStringLiteral("建筑"); one.description = QStringLiteral("creative keys");
        PresetManagePage::PresetItem two; two.id = QStringLiteral("two"); two.name = QStringLiteral("生存");
        manage.setPresets({one, two}, {}, one.id);
        auto *search = manage.findChild<QLineEdit *>(QStringLiteral("presetSearchEdit"));
        search->setText(QStringLiteral("CREATIVE"));
        check(manage.currentPresetId() == one.id, "preset search matches descriptions case-insensitively");
        search->setText(QStringLiteral("no-match"));
        check(manage.currentPresetId().isEmpty(), "empty search results cannot retain an actionable stale preset");
        search->clear();
        check(!manage.currentPresetId().isEmpty(), "clearing preset search restores selectable entries");
        auto *body = manage.findChild<QWidget *>(QStringLiteral("presetCaptureBody"));
        auto *toggle = manage.findChild<QPushButton *>(QStringLiteral("presetCaptureToggle"));
        check(body && body->isHidden(), "advanced extraction starts collapsed");
        toggle->click(); check(!body->isHidden(), "extraction remains accessible with one click");
    }
    {
        int selectedCalls = 0;
        int clearedCalls = 0;
        QString defaultId;
        bool makeDefault = true;
        PresetManagePage manage;
        manage.setCategories({{QString(), QStringLiteral("全部"), true},
                              {QStringLiteral("settings"), QStringLiteral("设置"), false},
                              {QStringLiteral("mods"), QStringLiteral("模组"), false}});
        QList<PresetManagePage::PresetItem> items;
        for (int i = 0; i < 1000; ++i) {
            PresetManagePage::PresetItem item;
            item.id = QString::number(i);
            item.name = QStringLiteral("Preset %1").arg(i);
            item.description = QStringLiteral("creative keys");
            item.categoryKey = i % 2 ? QStringLiteral("mods") : QStringLiteral("settings");
            item.isDefault = i == 0;
            items.append(item);
        }
        manage.setPresets(items, {{QStringLiteral("broken"), QStringLiteral("bad manifest")}}, QStringLiteral("700"));
        // 左栏是**按类别分组**的一棵树（组头 + 预设行）：没有筛选 chip、也没有代理模型，
        // 行就长在各自的组下面。"哪一行是谁"按 `rowIdRole()` 认，组头没有 id。
        auto *view = manage.findChild<QTreeWidget *>(PresetManagePage::presetListName());
        auto *search = manage.findChild<QLineEdit *>(QStringLiteral("presetSearchEdit"));
        const auto rows = [view] {
            int n = 0;
            if (view != nullptr)
                for (int g = 0; g < view->topLevelItemCount(); ++g) n += view->topLevelItem(g)->childCount();
            return n;
        };
        const auto visibleRows = [view] {
            int n = 0;
            if (view == nullptr) return n;
            for (int g = 0; g < view->topLevelItemCount(); ++g) {
                QTreeWidgetItem *group = view->topLevelItem(g);
                if (group->isHidden()) continue;
                for (int i = 0; i < group->childCount(); ++i)
                    if (!group->child(i)->isHidden()) ++n;
            }
            return n;
        };
        const auto rowFor = [view](const QString &id) -> QTreeWidgetItem * {
            if (view == nullptr) return nullptr;
            for (int g = 0; g < view->topLevelItemCount(); ++g) {
                QTreeWidgetItem *group = view->topLevelItem(g);
                for (int i = 0; i < group->childCount(); ++i)
                    if (group->child(i)->data(0, PresetManagePage::rowIdRole()).toString() == id)
                        return group->child(i);
            }
            return nullptr;
        };
        check(view && rows() == 1001 && manage.currentPresetId() == QStringLiteral("700"),
              "large preset list keeps every entry and the requested identity");
        check(view && view->topLevelItemCount() == 3 && view->topLevelItem(0)->text(0) == QStringLiteral("设置") &&
                  view->topLevelItem(1)->text(0) == QStringLiteral("模组") &&
                  view->topLevelItem(2)->text(0) == QStringLiteral("无法读取"),
              "groups follow the category order the controller supplies and unreadable presets get their own last group");
        bool grouped = view != nullptr;
        for (int g = 0; g < (view ? view->topLevelItemCount() : 0); ++g) {
            QTreeWidgetItem *group = view->topLevelItem(g);
            const bool mods = group->text(0) == QStringLiteral("模组");
            if (group->text(0) != QStringLiteral("设置") && !mods) continue;
            for (int i = 0; i < group->childCount(); ++i) {
                const int id = group->child(i)->data(0, PresetManagePage::rowIdRole()).toString().toInt();
                grouped = grouped && (id % 2 == 1) == mods;
            }
        }
        check(grouped, "every preset row sits under its own category header");
        if (search) {
            QObject::connect(&manage, &PresetManagePage::presetSelected, &manage,
                             [&](const QString &id) { if (id.isEmpty()) ++clearedCalls; else ++selectedCalls; });
            search->setText(QStringLiteral("  CREATIVE  "));
            check(rows() == 1001 && selectedCalls == 0 && manage.currentPresetId() == QStringLiteral("700"),
                  "search trims input, matches descriptions case-insensitively and emits no duplicate callbacks");
            search->setText(QStringLiteral("no matches"));
            check(visibleRows() == 1 && manage.currentPresetId().isEmpty() && clearedCalls == 1 &&
                      !manage.findChild<QPushButton *>(PresetManagePage::deleteButtonName())->isEnabled(),
                  "broken presets remain visible but cannot become a stale actionable selection");
            check(rowFor(QStringLiteral("broken")) && !rowFor(QStringLiteral("broken"))->isHidden(),
                  "the unreadable group is not hidden by an unrelated search query");
            search->clear();
            check(rows() == 1001 && manage.currentPresetId() == QStringLiteral("0") && selectedCalls == 1,
                  "clearing search selects the first visible preset exactly once");
            manage.setPresets(items, {}, QStringLiteral("700"));
            check(rows() == 1000 && manage.currentPresetId() == QStringLiteral("700"),
                  "data refresh keeps the requested identity when it is still present");
            view->setCurrentItem(rowFor(QStringLiteral("0")));
            QObject::connect(&manage, &PresetManagePage::defaultRequested, &manage,
                             [&](const QString &id, bool enabled) { defaultId = id; makeDefault = enabled; });
            manage.findChild<QPushButton *>(PresetManagePage::defaultButtonName())->click();
            check(defaultId == QStringLiteral("0") && !makeDefault && rowFor(QStringLiteral("0")) &&
                      rowFor(QStringLiteral("0"))->text(0).contains(QStringLiteral("默认")),
                  "default indicator and toggle refer to the selected preset's identity");
        }
    }
    const auto presetRoot = scratch.filePath(QStringLiteral("presets"));
    auto manifest = Presets::create(presetRoot, QStringLiteral("settings"), {}, Presets::PresetCategory::GameSettings);
    Presets::PresetSection section;
    section.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    section.kind = Presets::SectionKind::VanillaOptions;
    section.items = QJsonArray{QJsonObject{{"key", "fov"}, {"value", "0.5"}}};
    manifest.sections.append(section);
    check(Presets::saveManifest(presetRoot, manifest), "edited settings manifest saves atomically");
    const auto gameRoot = scratch.filePath(QStringLiteral("settings-instance")); QDir().mkpath(gameRoot);
    QFile config(gameRoot + "/options.txt");
    check(config.open(QIODevice::WriteOnly) &&
              config.write("fov:0.0\nkey_key.jump:key.keyboard.r\nunknownPreserved:yes\n") > 0,
          "settings target fixture is isolated"); config.close();
    Presets::PresetApplyRequest apply;
    apply.instanceId = QStringLiteral("test"); apply.instanceRoot = gameRoot;
    const auto applied = Presets::applyPresetToInstance(presetRoot, manifest, apply);
    const bool opened = config.open(QIODevice::ReadOnly);
    const auto contents = config.readAll(); config.close();
    check(applied.ok && opened && contents.contains("fov:0.5") && contents.contains("unknownPreserved:yes"),
          "settings application changes selected keys and preserves unknown fields");
    {
        PresetEditorPage fresh(QJsonObject{{"id", manifest.id}, {"name", manifest.name},
            {"enabledParts", QJsonObject{{"gameSettings", true}}},
            {"modReferences", QJsonArray{QJsonObject{{"platform", "modrinth"}, {"projectId", "test-mod"}}}}});
        auto *enableGame = fresh.findChild<QCheckBox *>("presetEditorEnabled_gameSettings");
        check(enableGame && !enableGame->isChecked(), "legacy presets without game data require opting into an override");
        const auto disabled = Presets::saveEditorDocument(presetRoot, fresh.document(), false);
        check(disabled.ok && disabled.manifest.sections.isEmpty() &&
                  !disabled.manifest.editorState.value("sections").toArray().isEmpty(),
              "disabling the group excludes it from application and retains its editable values");
        if (enableGame) enableGame->setChecked(true);
        const auto enabled = Presets::saveEditorDocument(presetRoot, fresh.document(), false);
        auto withGameSettings = enabled.manifest;
        withGameSettings.modReferences.clear();
        const auto overwritten = Presets::applyPresetToInstance(presetRoot, withGameSettings, apply);
        const bool reopened = config.open(QIODevice::ReadOnly);
        const auto after = config.readAll(); config.close();
        check(enableGame && enabled.ok && overwritten.ok && reopened && after.contains("fov:0.0") &&
                  after.contains("renderDistance:12") && after.contains("key_key.jump:key.keyboard.space") &&
                  after.contains("unknownPreserved:yes"),
              "enabling the group overwrites settings and key bindings even when their controls were untouched");
    }
    const QString preview = qEnvironmentVariable("WTFML_PRESET_PREVIEW_DIR");
    if (!preview.isEmpty()) {
        QDir().mkpath(preview);
        const int font = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        const auto families = QFontDatabase::applicationFontFamilies(font);
        if (!families.isEmpty()) QApplication::setFont(QFont(families.first(), 10));
        qApp->setStyleSheet(Theme::widgetStyleSheet());
        editor.show(); QApplication::processEvents();
        check(editor.grab().save(QDir(preview).filePath(QStringLiteral("preset-mod-editor.png"))), "mod editor preview renders");
        editor.hide(); options.show(); QApplication::processEvents();
        check(options.grab().save(QDir(preview).filePath(QStringLiteral("preset-options-editor.png"))), "settings editor preview renders");
        options.hide();
    }
    failures += runPresetManifestChecks();
    failures += runPresetPreviewChecks();
    failures += runPresetDependencyChecks();
    return failures ? 1 : 0;
}
