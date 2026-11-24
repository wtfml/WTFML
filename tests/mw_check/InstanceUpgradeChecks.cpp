#include "InstanceUpgradeChecks.hpp"
#include "operations/InstanceUpgrade.hpp"
#include "operations/UpgradeHistory.hpp"
#include "operations/FabricVersionDeclaration.hpp"
#include "InstanceUpdateWizard.hpp"
#include <QAbstractButton>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <QElapsedTimer>
#include <QRegularExpression>
#include "manage/BackupFiles.hpp"
#include "InstanceSettingsDialog.hpp"
#include "controllers/InstanceTransferController.hpp"
#include "controllers/FileOperationRecoveryController.hpp"
#include "operations/OperationReport.hpp"
#include "manage/VersionManager.hpp"
#include "launcher/LauncherManager.hpp"
#include "OverlayHost.hpp"
#include "InlineMessage.hpp"
#include "util/AppPaths.hpp"
#include "util/ZipUtil.hpp"
#include "util/DirectorySessionLock.hpp"
#include "controllers/UpgradeAssessment.hpp"
#include "manage/InstanceIconLookup.hpp"
#include "download/ResourceStore.hpp"
#include <QPointer>
#include <QTcpServer>
#include "net/RequestBudget.hpp"
#include <QTcpSocket>
#include <QComboBox>
#include <QTreeWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QSet>
#include <QPushButton>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QDir>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QLockFile>
#include <cstdio>
#include <stdexcept>

namespace {
bool write(const QString &path, const QByteArray &bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
QByteArray read(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
}
int runInstanceUpgradeChecks() {
    int failures = 0;
    const auto check = [&](bool ok, const char *what) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++failures;
    };
    {
        QTemporaryDir root;
        check(root.isValid(), "Directory lock fixture created");
        if (root.isValid()) {
            QLockFile contender(QDir::temp().filePath(QStringLiteral("wtfml-directory-%1.lock")
                .arg(DirectorySessionLock::keyFor(root.path()))));
            contender.setStaleLockTime(0);
            const bool ownsLock = contender.tryLock(0);
            check(ownsLock, "Other launcher directory lock acquired");
            const bool wasEnabled = DirectorySessionLock::enabled;
            DirectorySessionLock::enabled = true;
            const InstanceUpgrade::Request request{root.path(), QStringLiteral("instance"),
                                                   QStringLiteral("transaction"), {}};
            const auto prepare = InstanceUpgrade::prepareAndCommit(request, {});
            const auto commit = InstanceUpgrade::commitPrepared(request);
            const auto rollback = InstanceUpgrade::rollback(request);
            const auto recover = InstanceUpgrade::recoverInterrupted(root.path());
            DirectorySessionLock::enabled = wasEnabled;
            check(ownsLock && !prepare.ok && !commit.ok && !rollback.ok
                      && prepare.problem.contains(QStringLiteral("占用"))
                      && commit.problem.contains(QStringLiteral("占用"))
                      && rollback.problem.contains(QStringLiteral("占用"))
                      && recover.size() == 1 && recover.first().contains(QStringLiteral("占用")),
                  "Upgrade prepare/commit/rollback/recovery reject another launcher's directory");
            check(QDir(root.path()).entryList(QDir::AllEntries | QDir::Hidden
                      | QDir::NoDotAndDotDot).isEmpty(),
                  "Directory lock rejection leaves no transaction or backup files");
        }
    }
    {
        using namespace FabricVersionDeclaration;
        check(predicate(QStringLiteral("~26.2-"), QStringLiteral("26.2")) == Match::Yes,
              "Fabric empty prerelease lower bound accepts the stable release");
        check(predicate(QStringLiteral("~26.2-"), QStringLiteral("26.3-alpha.1")) == Match::No,
              "Fabric tilde upper bound excludes the next minor prereleases");
        check(predicate(QStringLiteral(">=26.2 <26.3-alpha.3"), QStringLiteral("26.3-alpha.2")) == Match::Yes,
              "Fabric prerelease ordering and compound constraints");
        check(predicate(QStringLiteral("^0.2"), QStringLiteral("0.8")) == Match::Yes,
              "Fabric caret does not use the npm zero-major exception");
        check(predicate(QStringLiteral("26.2.x.x"), QStringLiteral("26.2.4+local")) == Match::Yes,
              "Fabric repeated trailing wildcard and build metadata");
        check(predicate(QStringLiteral(">=99999999999999999"), QStringLiteral("26.2")) == Match::Unknown,
              "overflow declarations remain unconfirmed");
        check(matches(QJsonArray{QStringLiteral("*"), 1}, QStringLiteral("26.2")) == Match::Unknown,
              "malformed OR arrays are not silently accepted");
        auto *target = new QWidget; auto *targetLayout = new QVBoxLayout(target);
        auto *combo = new QComboBox(target); combo->addItems({QStringLiteral("one"), QStringLiteral("two")});
        targetLayout->addWidget(combo);
        InstanceUpdateWizard wizard(target, new QWidget, new QWidget, new QWidget);
        wizard.show(); QApplication::processEvents();
        check(!wizard.button(QWizard::NextButton)->isEnabled(), "wizard blocks an incomplete target step");
        wizard.setStepReady(0, true); wizard.next();
        check(wizard.currentId() == 1 && !wizard.button(QWizard::NextButton)->isEnabled(),
              "wizard waits for mod review before confirmation");
        wizard.back();
        QWheelEvent wheel(QPointF(3, 3), QPointF(combo->mapToGlobal(QPoint(3, 3))), QPoint(), QPoint(0, -120),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(combo, &wheel);
        check(combo->currentIndex() == 0, "closed wizard dropdown ignores wheel input");
        wizard.setStepReady(1, true); wizard.next(); wizard.next();
        check(wizard.currentId() == 2, "wizard preserves review readiness when going back");
        wizard.setStepReady(2, true); wizard.next();
        check(wizard.currentId() == 3 && !wizard.button(QWizard::FinishButton)->isEnabled(),
              "wizard cannot finish before the transaction has a result");
        wizard.setStepReady(3, true);
        check(wizard.button(QWizard::FinishButton)->isEnabled(), "wizard enables finish after a terminal result");
        QWidget background;
        OverlayHost outer(&background, &background);
        QDialog prompt;
        bool routed = false;
        QTimer::singleShot(0, &prompt, [&] {
            routed = wizard.promptHost()->promptOpen() && !outer.promptOpen(); prompt.accept();
        });
        check(outer.execPrompt(&prompt) == QDialog::Accepted && routed,
              "transaction questions route inside the modal wizard rather than the blocked main window");
        QDialog closePrompt;
        QTimer::singleShot(0, &wizard, [&] { wizard.close(); });
        check(outer.execPrompt(&closePrompt) == QDialog::Rejected && wizard.isVisible(),
              "closing during a question dismisses the question without hiding the wizard or hanging");
    }
    const QString inspect = qEnvironmentVariable("WTFML_UPGRADE_INSPECT_MODS");
    if (!inspect.isEmpty()) {
        QStringList jars;
        for (const auto &file : QDir(inspect).entryInfoList({QStringLiteral("*.jar")}, QDir::Files | QDir::NoSymLinks))
            jars.append(file.absoluteFilePath());
        QElapsedTimer clock; clock.start(); QString problem;
        const auto metadata = ZipUtil::readMetadataBatch(jars, {QStringLiteral("fabric.mod.json"), QStringLiteral("META-INF/mods.toml"), QStringLiteral("META-INF/neoforge.mods.toml")}, &problem);
        int fabric = 0, declared = 0, supported = 0, unknown = 0;
        for (const auto &entry : metadata) {
            if (!entry.entries.contains(QStringLiteral("fabric.mod.json"))) continue;
            ++fabric;
            const auto rule = QJsonDocument::fromJson(entry.entries.value(QStringLiteral("fabric.mod.json"))).object()
                .value(QStringLiteral("depends")).toObject().value(QStringLiteral("minecraft"));
            if (rule.isUndefined()) continue;
            ++declared;
            const auto match = FabricVersionDeclaration::matches(rule, QStringLiteral("26.2"));
            supported += match == FabricVersionDeclaration::Match::Yes;
            unknown += match == FabricVersionDeclaration::Match::Unknown;
        }
        std::printf("READONLY METADATA jars=%lld returned=%lld fabric=%d declared=%d supports26.2=%d unknown=%d elapsedMs=%lld problem=%s\n",
            static_cast<long long>(jars.size()), static_cast<long long>(metadata.size()), fabric, declared, supported, unknown,
            static_cast<long long>(clock.elapsed()), problem.toUtf8().constData());
        check(problem.isEmpty() && metadata.size() == jars.size(), "readonly real-instance batch returns every requested archive");
    }
    QTemporaryDir scratch;
    if (!scratch.isValid()) return 1;
    {
        const QString game = scratch.filePath(QStringLiteral("assessment-game"));
        const QString source = game + QStringLiteral("/versions/Source");
        const QString jar = source + QStringLiteral("/mods/example.jar");
        const QString declaration = scratch.filePath(QStringLiteral("fabric-declaration.json"));
        const QString forgeDeclaration = scratch.filePath(QStringLiteral("mods.toml"));
        const QString neoDeclaration = scratch.filePath(QStringLiteral("neoforge.mods.toml"));
        check(write(forgeDeclaration, "[[dependencies.example]]\nmodId=\"minecraft\"\nmandatory=true\nversionRange=\"[1.21,1.22)\"\n") &&
              write(neoDeclaration, "[[dependencies.example]]\nmodId=\"minecraft\"\ntype=\"required\"\nversionRange=\"[1.20,1.21)\"\n"),
              "prepare Forge and NeoForge dependency declarations");
        const QString nestedSource = scratch.filePath("nested-source.jar");
        const QString nestedDeclaration = scratch.filePath("nested-declaration.json");
        check(write(nestedDeclaration, R"({"schemaVersion":1,"id":"embedded","name":"Embedded","version":"1.0","depends":{"minecraft":">=1.20 <1.23"}})")
            && ZipUtil::createZip(nestedSource, {{QStringLiteral("fabric.mod.json"), nestedDeclaration}})
            && write(declaration, R"({"schemaVersion":1,"id":"example","name":"Local Example","version":"1.0","depends":{"minecraft":">=1.20 <1.23"},"breaks":{"minecraft":"1.22.2"}})")
            && true
            && QDir().mkpath(source + QStringLiteral("/mods")) && ZipUtil::createZip(jar, {{QStringLiteral("fabric.mod.json"), declaration},
                {QStringLiteral("META-INF/mods.toml"), forgeDeclaration}, {QStringLiteral("META-INF/neoforge.mods.toml"), neoDeclaration},
                {QStringLiteral("META-INF/jars/embedded.jar"), nestedSource}}),
            "create real JAR containing a local compatibility declaration and a nested JAR");
        QByteArray bounded; QString boundedProblem;
        check(ZipUtil::readEntry(jar, QStringLiteral("fabric.mod.json"), &bounded, &boundedProblem, QStringLiteral("metadata fixture"), 8) == ZipUtil::EntryStatus::Failed
              && bounded.isEmpty() && !boundedProblem.isEmpty(), "oversized declarations are rejected before extraction");
        check(ZipUtil::readEntry(jar, QStringLiteral("fabric.mod.json"), &bounded, &boundedProblem, QStringLiteral("metadata fixture"), 4096) == ZipUtil::EntryStatus::Ok
              && bounded == read(declaration), "bounded metadata reader preserves the original bytes");
        const QString broken = scratch.filePath(QStringLiteral("broken.jar"));
        check(write(broken, "not a zip"), "prepare independent corrupt archive");
        QString batchProblem;
        const auto batch = ZipUtil::readMetadataBatch({broken, jar}, {QStringLiteral("fabric.mod.json"), QStringLiteral("META-INF/mods.toml")}, &batchProblem);
        check(!batch.value(broken).problem.isEmpty() && batch.value(jar).entries.value(QStringLiteral("fabric.mod.json")) == read(declaration)
              && batch.value(jar).entries.value(QStringLiteral("META-INF/mods.toml")) == read(forgeDeclaration),
              "batch metadata reads all loaders and isolates corrupt archives");
        check(write(source + QStringLiteral("/wtfml.meta"), R"({"realVersion":"1.20.1","loaderName":"Fabric"})") &&
              write(game + QStringLiteral("/versions/Target/wtfml.meta"), R"({"realVersion":"1.21.1","loaderName":"Fabric","loaderVersion":"0.16"})") &&
              write(game + QStringLiteral("/versions/Other/wtfml.meta"), R"({"realVersion":"1.22.1","loaderName":"Fabric","loaderVersion":"0.16"})") &&
              write(game + QStringLiteral("/versions/Excluded/wtfml.meta"), R"({"realVersion":"1.22.2","loaderName":"Fabric","loaderVersion":"0.16"})"),
              "prepare target metadata without modifying the source instance");
        check(write(game + QStringLiteral("/versions/ForgeTarget/wtfml.meta"), R"({"realVersion":"1.21.1","loaderName":"Forge"})") &&
              write(game + QStringLiteral("/versions/NeoTarget/wtfml.meta"), R"({"realVersion":"1.21.1","loaderName":"NeoForge"})"),
              "prepare targets for loader-specific declaration checks");
        InstanceIconLookup::Options options; options.cacheDir = scratch.filePath(QStringLiteral("identity-cache")); options.cacheKey = QStringLiteral("Source");
        ResourceCatalog::Hit hit; hit.projectId = QStringLiteral("known-project"); hit.title = QStringLiteral("Known Example");
        QJsonObject record{{"size", double(QFileInfo(jar).size())}, {"mtime", double(QFileInfo(jar).lastModified().toMSecsSinceEpoch())},
            {"sha1", QString::fromLatin1(QCryptographicHash::hash(read(jar), QCryptographicHash::Sha1).toHex())}, {"hit", ResourceCatalog::hitToJson(hit)}};
        check(write(InstanceIconLookup::cachePath(options), QJsonDocument(QJsonObject{{"files", QJsonObject{{jar, record}}}}).toJson()),
              "cache a platform identity verified against the actual JAR hash");
        QTcpServer server; bool failPlatform = false, fallbackMode = false, holdReplies = false;
        QList<QPair<QPointer<QTcpSocket>, QByteArray>> heldReplies;
        bool releaseMode = true;
        int inFlight = 0, maxInFlight = 0, fallbackLookups = 0;
        check(server.listen(QHostAddress::LocalHost, 0), "assessment platform fixture listens on loopback");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    if (socket->property("replied").toBool()) return;
                    const QByteArray request = socket->property("request").toByteArray() + socket->readAll(); socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n")) return;
                    if (request.startsWith("POST ")) {
                        const auto header = request.left(request.indexOf("\r\n\r\n"));
                        const auto length = QRegularExpression(QStringLiteral("(?i)Content-Length: (\\d+)")).match(QString::fromLatin1(header));
                        if (length.hasMatch() && request.size() - header.size() - 4 < length.captured(1).toInt()) return;
                    }
                    socket->setProperty("replied", true);
                    QByteArray body = "[]"; int status = failPlatform ? 403 : 200;
                    if (releaseMode && request.contains("known-project")) {
                        QJsonObject hashes;
                        hashes.insert(QStringLiteral("sha1"),
                                      QString::fromLatin1(QByteArray(40, 'a')));
                        QJsonObject file;
                        file.insert(QStringLiteral("filename"), QStringLiteral("example-2.0.jar"));
                        file.insert(QStringLiteral("url"),
                                    QStringLiteral("https://example.invalid/example-2.0.jar"));
                        file.insert(QStringLiteral("size"), 123);
                        file.insert(QStringLiteral("hashes"), hashes);
                        file.insert(QStringLiteral("primary"), true);
                        QJsonObject version;
                        version.insert(QStringLiteral("id"), QStringLiteral("target-2"));
                        version.insert(QStringLiteral("project_id"), QStringLiteral("known-project"));
                        version.insert(QStringLiteral("version_number"), QStringLiteral("2.0"));
                        version.insert(QStringLiteral("version_type"), QStringLiteral("release"));
                        version.insert(QStringLiteral("game_versions"),
                                       QJsonArray{QStringLiteral("1.21.1")});
                        version.insert(QStringLiteral("loaders"),
                                       QJsonArray{QStringLiteral("fabric")});
                        version.insert(QStringLiteral("files"), QJsonArray{file});
                        version.insert(QStringLiteral("dependencies"), QJsonArray{});
                        body = QJsonDocument(QJsonArray{version}).toJson(QJsonDocument::Compact);
                    }
                    if (fallbackMode && request.startsWith("GET /mods/")) status = 503;
                    if (fallbackMode && request.startsWith("POST /version_files")) {
                        ++fallbackLookups;
                        const QString sha = record.value("sha1").toString();
                        const QJsonObject file{{"filename", "example.jar"}, {"url", "https://example.invalid/file.jar"},
                            {"hashes", QJsonObject{{"sha1", sha}}}};
                        const QJsonObject version{{"id", "file-version"}, {"project_id", "fallback-project"},
                            {"version_number", "1.0"}, {"files", QJsonArray{file}}};
                        body = QJsonDocument(QJsonObject{{sha, version}}).toJson(QJsonDocument::Compact);
                    }
                    ++inFlight; maxInFlight = qMax(maxInFlight, inFlight);
                    const QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                    if (holdReplies) { heldReplies.append({QPointer<QTcpSocket>(socket), response}); return; }
                    QTimer::singleShot(0, socket, [&, socket, response] {
                        --inFlight; socket->write(response); socket->disconnectFromHost();
                    });
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        const QString oldRoot = AppPaths::minecraftDir(); AppPaths::setMinecraftDir(game);
        {
            VersionManager versions;
            InstanceSettingsDialog page(QStringLiteral("Source"), QStringLiteral("Source"), {}, {}, {}, true);
            page.setProperty("modernUpgradeFlow", true);
            page.setProperty("upgradeTargetLoader", QStringLiteral("fabric"));
            ResourceStore store;
            store.setModrinthApiBaseOverride(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
            page.setTransferTargets(QStringLiteral("upgrade"), {{QStringLiteral("Target"), QStringLiteral("Target")}, {QStringLiteral("Other"), QStringLiteral("Other")},
                {QStringLiteral("Excluded"), QStringLiteral("Excluded")}, {QStringLiteral("Forge"), QStringLiteral("ForgeTarget")}, {QStringLiteral("NeoForge"), QStringLiteral("NeoTarget")},
                {QStringLiteral("Unprepared"), QStringLiteral("@download:1.21.2")}});
            // **产品的顺序**：开页那一刻就 attach，而「升级」页是用户切过去才建的
            // （见 InstanceSettingsDialog::ensurePage()）。这一节照抄那个顺序 —— 原来这里
            // 先 showPage 再 attach，正好把"attach 的时候页还没建"那一段盖掉了。
            UpgradeAssessment::attach(&page, &versions, &store, QStringLiteral("Source"), source, options.cacheDir);
            page.showPage(QStringLiteral("upgrade"));
            auto *combo = page.findChild<QComboBox *>(InstanceSettingsDialog::transferTargetComboName(QStringLiteral("upgrade")));
            auto *tree = page.findChild<QTreeWidget *>(QStringLiteral("instanceUpgradeModChecks"));
            // 现代向导会把“平台有目标发行版 + 本地声明符合”的行收进末尾的「正常兼容」分组，
            // 所以第一行要从分组里取（顶层的分组本身 text(1) 为空）。
            const auto firstRow = [tree]() -> QTreeWidgetItem * {
                for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                    auto *item = tree->topLevelItem(i);
                    if (item->childCount() > 0 && item->text(1).isEmpty()) return item->child(0);
                    if (!item->text(1).isEmpty()) return item;
                }
                return nullptr;
            };
            const auto rowCount = [tree]() {
                int count = 0;
                for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                    auto *item = tree->topLevelItem(i);
                    count += item->childCount() > 0 && item->text(1).isEmpty()
                                 ? item->childCount() : 1;
                }
                return count;
            };
            const auto waitFor = [&](const QString &text) {
                QEventLoop loop; QTimer poll, timeout;
                QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                    auto *row = firstRow();
                    if (row != nullptr && row->text(1).contains(text)) loop.quit();
                });
                timeout.setSingleShot(true); QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
                poll.start(10); timeout.start(30000); loop.exec();
                auto *row = firstRow();
                return row != nullptr && row->text(1).contains(text);
            };
            combo->setCurrentIndex(1);
            check(!page.findChild<QPushButton *>(QStringLiteral("instanceUpgradeInPlaceButton"))->isEnabled(),
                  "in-place update waits for local archive reads to release their file handles");
            check(waitFor(QStringLiteral("平台有目标发行版")) && firstRow()->text(1).contains(QStringLiteral("本地 Minecraft 声明符合")),
                  "known mod with a matching platform release displays the target version and local declaration");
            {
                auto *choice = qobject_cast<QComboBox *>(tree->itemWidget(firstRow(), 3));
                check(choice != nullptr && choice->count() == 4
                          && choice->itemText(3).contains(QStringLiteral("替换为平台适配版 2.0")),
                      "★★ a matching platform release offers 「替换为平台适配版」 as the third choice");
                check(choice != nullptr && choice->currentIndex() == 3
                          && page.upgradeReplaceMods() == QStringList{QStringLiteral("example.jar")}
                          && page.upgradeKeptMods().isEmpty() && page.upgradePendingModCount() == 0,
                      "★★ known target version defaults to replace while still being explicit in the list");
                const QVariantMap replacements = page.upgradeModReplacements();
                check(replacements.contains(QStringLiteral("example.jar"))
                          && replacements.value(QStringLiteral("example.jar")).toMap()
                                     .value(QStringLiteral("versionId")).toString()
                                 == QStringLiteral("target-2"),
                      "replacement preview carries the exact platform version id");
                check(page.property("upgradeFileChanges").toStringList().join(QLatin1Char('\n'))
                          .contains(QStringLiteral("替换：mods/example.jar")),
                      "★ file change list is shown before replacement");
                check(firstRow()->text(2).contains(QStringLiteral("嵌套 JAR")),
                      "nested JAR is labelled as following its parent JAR");
                if (choice != nullptr) {
                    choice->setCurrentIndex(2);  // 保留启用
                    check(page.upgradeReplaceMods().isEmpty()
                              && page.upgradeKeptMods() == QStringList{QStringLiteral("example.jar")},
                          "choosing keep removes the file from the replacement plan");
                    choice->setCurrentIndex(3);  // 回到替换，后续目标切换会清掉显式选择
                }
            }
            failPlatform = true; combo->setCurrentIndex(2);
            check(waitFor(QStringLiteral("平台查询失败")) && firstRow()->text(1).contains(QStringLiteral("本地 Minecraft 声明符合")),
                  "network failure is not reported as absent release and does not erase local support");
            combo->setCurrentIndex(3);
            check(waitFor(QStringLiteral("本地明确排除此版本")), "Fabric breaks.minecraft overrides a matching depends range");
            combo->setCurrentIndex(4);
            check(waitFor(QStringLiteral("本地 Minecraft 声明符合")), "Forge TOML dependency range supports the target version");
            combo->setCurrentIndex(5);
            check(waitFor(QStringLiteral("本地声明不支持")), "NeoForge TOML exclusive upper bound rejects the target version");
            failPlatform = false; combo->setCurrentIndex(6);
            check(waitFor(QStringLiteral("平台未找到目标发行版")) && tree->topLevelItem(0)->text(1).contains(QStringLiteral("本地 Minecraft 声明符合")),
                  "unprepared target is assessed using the explicitly stated current-loader assumption");
            check(write(InstanceIconLookup::cachePath(options), "{\"files\":{}}"), "remove only the fixture's platform identity");
            const QString beforeRecheck = firstRow() != nullptr ? firstRow()->text(1) : QString();
            page.findChild<QPushButton *>(QStringLiteral("instanceUpgradeRecheck"))->click();
            check(waitFor(QStringLiteral("未识别平台项目")) && firstRow()->text(1).contains(QStringLiteral("本地 Minecraft 声明符合")),
                  "unknown platform identity still retains the local compatibility declaration after recheck");
            // 上面那一格变了 = 这一下点击真的让扫描重跑了一遍。修之前 `attach()` 里
            // `findChild("instanceUpgradeRecheck")` 拿到的是 nullptr（页还没建），
            // `clicked -> startScan` 静默没接上，点了什么都不发生，这一格不动。
            check(firstRow() != nullptr && firstRow()->text(1) != beforeRecheck,
                  "upgrade recheck still reruns the scan when the page is built after attach");
            const QJsonObject installed{{"filename", QStringLiteral("example.jar")}, {"sha1", record.value(QStringLiteral("sha1"))}, {"project", QStringLiteral("modrinth:known-project")}};
            check(write(source + QStringLiteral("/wtfml.preset-mod-install.json"), QJsonDocument(QJsonObject{
                {"format", QStringLiteral("wtfml-preset-mod-result/1")}, {"plannedFiles", QJsonArray{installed}}}).toJson()), "prepare a hash-verifiable preset install identity");
            page.findChild<QPushButton *>(QStringLiteral("instanceUpgradeRecheck"))->click();
            check(waitFor(QStringLiteral("平台未找到目标发行版")) && firstRow()->text(1).contains(QStringLiteral("本地 Minecraft 声明符合")),
                  "identified preset-installed mods are assessed even without an icon lookup cache");
            hit.source = ResourceCatalog::Source::CurseForge;
            record.insert("hit", ResourceCatalog::hitToJson(hit));
            check(write(InstanceIconLookup::cachePath(options), QJsonDocument(QJsonObject{{"files", QJsonObject{{jar, record}}}}).toJson()),
                "prepare a CurseForge identity for outage fallback");
            fallbackMode = true;
            store.setCurseForgeApiBaseOverride(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
            page.findChild<QPushButton *>(QStringLiteral("instanceUpgradeRecheck"))->click();
            check(waitFor(QStringLiteral("平台未找到目标发行版")) && fallbackLookups == 1,
                "CurseForge 503 uses exact file hash identity before querying Modrinth fallback");
            fallbackMode = false; maxInFlight = 0;
            hit.source = ResourceCatalog::Source::Modrinth; record.insert("hit", ResourceCatalog::hitToJson(hit));
            QJsonObject records{{jar, record}};
            for (int i = 0; i < 5; ++i) {
                const QString copy = source + QStringLiteral("/mods/copy-%1.jar").arg(i);
                check(QFile::copy(jar, copy), "prepare a parallel-query fixture JAR");
                auto copyRecord = record; copyRecord.insert("mtime", double(QFileInfo(copy).lastModified().toMSecsSinceEpoch()));
                records.insert(copy, copyRecord);
            }
            check(write(InstanceIconLookup::cachePath(options), QJsonDocument(QJsonObject{{"files", records}}).toJson()), "cache six valid fixture identities");
            // 确定性写法：① 先等本地扫描**这件事**做完（它要起 PowerShell 读嵌套 JAR，快慢看机器，
            // 和并发无关；上限只是防挂死）；② 夹具按住每一条回复，直到在飞的凑满 4 条才放行 ——
            // 重叠不再靠"120 ms 内碰上"，而是请求被按住、只能靠并发额度一起摆在外面。
            const auto pumpUntil = [&](const std::function<bool()> &done, int guardMs) {
                QElapsedTimer guard; guard.start();
                while (!done() && guard.elapsed() < guardMs) QApplication::processEvents(QEventLoop::AllEvents, 10);
                return done();
            };
            holdReplies = true; heldReplies.clear();
            page.findChild<QPushButton *>(QStringLiteral("instanceUpgradeRecheck"))->click();
            check(pumpUntil([&] { return !page.property("upgradeAssessmentReading").toBool() && rowCount() == 6; }, 120000),
                  "parallel-query fixture: local scan finishes and lists all six JARs");
            const bool saturated = pumpUntil([&] { return inFlight >= 4; }, 30000);
            // 额度满了以后再多转几轮事件：排队那两条每 25 ms 试一次额度，真漏了就会在这里冒出来。
            pumpUntil([] { return false; }, 200);
            const auto lane = RequestBudget::snapshot();
            check(saturated && maxInFlight == 4 && inFlight == 4 && lane.control == 4,
                  "platform queries overlap with a hard four-request concurrency limit");
            holdReplies = false;
            for (const auto &held : std::as_const(heldReplies)) {
                --inFlight;
                if (held.first) { held.first->write(held.second); held.first->disconnectFromHost(); }
            }
            heldReplies.clear();
            check(pumpUntil([&] { return !page.property("upgradeAssessmentQuerying").toBool(); }, 30000) && maxInFlight == 4,
                  "released queries drain without ever exceeding the four-request limit");
        }
        AppPaths::setMinecraftDir(oldRoot);
    }
    {
        // 2.1.4：Quilt / 旧 Forge（mcmod.info 与 @Mod 注解）/ 嵌套 JAR / 加载器版本 / Java。
        const QString game = scratch.filePath("decl-game");
        const QString source = game + QStringLiteral("/versions/DeclSource");
        QDir().mkpath(source + QStringLiteral("/mods"));
        const auto meta = [&](const QString &id, const QString &version, const QString &loader,
                              const QString &loaderVersion = QString()) {
            return write(game + "/versions/" + id + "/wtfml.meta",
                         QJsonDocument(QJsonObject{{"realVersion", version},
                                                   {"loaderName", loader},
                                                   {"loaderVersion", loaderVersion}}).toJson());
        };
        check(write(source + "/wtfml.meta", R"({"realVersion":"1.20.1","loaderName":"Fabric"})")
                  && meta("DeclFabric", "1.21.1", "Fabric", "0.16.0")
                  && meta("DeclQuilt", "1.21.1", "Quilt", "0.26.4")
                  && meta("DeclForge", "1.21.1", "Forge", "47.2.0"),
              "prepare declaration-extension targets");
        const auto jar = [&](const QString &name, const QList<ZipUtil::ZipSource> &entries) {
            return ZipUtil::createZip(source + "/mods/" + name, entries);
        };
        const QString quiltDecl = scratch.filePath("quilt-decl.json");
        const QString quiltOldDecl = scratch.filePath("quilt-old-decl.json");
        const QString fabricJava = scratch.filePath("fabric-java.json");
        const QString fabricLoader = scratch.filePath("fabric-loader.json");
        const QString fabricNested = scratch.filePath("fabric-nested.json");
        const QString legacyInfo = scratch.filePath("mcmod.info");
        const QString nestedGarbage = scratch.filePath("nested-garbage.bin");
        check(write(quiltDecl, R"({"schema_version":1,"quilt_loader":{"id":"example","version":"2.0","metadata":{"name":"Quilt Example"}},"depends":[{"id":"minecraft","versions":">=1.20 <1.23"},{"id":"quilt_loader","versions":">=0.20"},{"id":"java","versions":">=17"}],"breaks":[]})")
                  && write(quiltOldDecl, R"({"schema_version":1,"quilt_loader":{"id":"old","version":"1.0"},"depends":[{"id":"minecraft","versions":">=1.20 <1.23"},{"id":"quilt_loader","versions":">=0.99"}]})")
                  && write(fabricJava, R"({"schemaVersion":1,"id":"java","version":"1.0","depends":{"minecraft":">=1.20 <1.23","java":">=99"}})")
                  && write(fabricLoader, R"({"schemaVersion":1,"id":"loader","version":"1.0","depends":{"minecraft":">=1.20 <1.23","fabricloader":">=99"}})")
                  && write(fabricNested, R"({"schemaVersion":1,"id":"nested-parent","version":"1.0","depends":{"minecraft":">=1.20 <1.23"}})")
                  && write(legacyInfo, R"json([{"modid":"legacy","name":"Legacy Mod","version":"1.0","acceptedMinecraftVersions":"[1.20,1.23)"}])json")
                  && write(nestedGarbage, "this-is-not-a-jar"),
              "prepare declaration fixtures");
        // 旧 Forge @Mod 注解：只造常量池（解析器只需要这一段），值是 acceptedMinecraftVersions 后面的范围。
        const auto fakeForgeClass = [] {
            QByteArray bytes;
            bytes.append('\xCA');
            bytes.append('\xFE');
            bytes.append('\xBA');
            bytes.append('\xBE');
            bytes.append('\x00');
            bytes.append('\x00');
            bytes.append('\x00');
            bytes.append('\x34');
            const QList<QByteArray> strings{
                QByteArray("Lnet/minecraftforge/fml/common/Mod;"),
                QByteArray("acceptedMinecraftVersions"),
                QByteArray("[1.20,1.23)")};
            bytes.append('\x00');
            bytes.append(char(strings.size() + 1));
            for (const QByteArray &text : strings) {
                bytes.append('\x01');
                bytes.append(char(text.size() >> 8));
                bytes.append(char(text.size() & 0xFF));
                bytes.append(text);
            }
            return bytes;
        };
        const QString forgeClass = scratch.filePath("forge-mod.class");
        check(write(forgeClass, fakeForgeClass()), "prepare old Forge annotation class fixture");
        check(jar("quilt.jar", {{QStringLiteral("quilt.mod.json"), quiltDecl}})
                  && jar("quilt-old.jar", {{QStringLiteral("quilt.mod.json"), quiltOldDecl}})
                  && jar("java.jar", {{QStringLiteral("fabric.mod.json"), fabricJava}})
                  && jar("loader.jar", {{QStringLiteral("fabric.mod.json"), fabricLoader}})
                  && jar("legacy.jar", {{QStringLiteral("mcmod.info"), legacyInfo}})
                  && jar("legacy-annotation.jar", {{QStringLiteral("x/mod/Main.class"), forgeClass}})
                  && jar("nested-bad.jar", {{QStringLiteral("fabric.mod.json"), fabricNested},
                                             {QStringLiteral("META-INF/jars/bad.jar"), nestedGarbage}}),
              "create declaration-extension JARs");
        const QString oldRoot = AppPaths::minecraftDir();
        AppPaths::setMinecraftDir(game);
        VersionManager versions;
        ResourceStore store;
        InstanceSettingsDialog page(QStringLiteral("DeclSource"), QStringLiteral("DeclSource"),
                                    {}, {}, {}, true);
        // 页是第一次切过去才建的 ⇒ 先切到「升级」那一页，再摸它的控件。
        page.showPage(QStringLiteral("upgrade"));
        page.setProperty("modernUpgradeFlow", true);
        const QString runtime = scratch.filePath("decl-runtime");
        check(write(runtime + QStringLiteral("/bin/java.exe"), "fixture")
                  && write(runtime + QStringLiteral("/release"), "JAVA_VERSION=\"21.0.1\"\n"),
              "create runtime release metadata for asynchronous Java assessment");
        page.setProperty("upgradeRuntimeJavaMajor", 0);
        page.setProperty("upgradeRuntimeJavaPath", runtime + QStringLiteral("/bin/java.exe"));
        page.setTransferTargets(QStringLiteral("upgrade"),
                                {{QStringLiteral("FabricTarget"), QStringLiteral("DeclFabric")},
                                 {QStringLiteral("QuiltTarget"), QStringLiteral("DeclQuilt")},
                                 {QStringLiteral("ForgeTarget"), QStringLiteral("DeclForge")}});
        // 这一节断言的是"嵌套 / 旧注解检查的结论"，不是"机器够不够快"：关掉墙钟预算。
        UpgradeAssessment::setWallClockBudgetsForTesting(false);
        UpgradeAssessment::attach(&page, &versions, &store, QStringLiteral("DeclSource"), source,
                                  scratch.filePath("decl-identity"));
        auto *combo = page.findChild<QComboBox *>(
            InstanceSettingsDialog::transferTargetComboName(QStringLiteral("upgrade")));
        auto *tree = page.findChild<QTreeWidget *>(QStringLiteral("instanceUpgradeModChecks"));
        const auto settle = [&] {
            QElapsedTimer clock; clock.start();
            while (clock.elapsed() < 30000
                   && (page.property("upgradeAssessmentReading").toBool()
                       || page.property("upgradeAssessmentQuerying").toBool())) {
                QApplication::processEvents(QEventLoop::AllEvents, 10);
            }
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        };
        const auto rowFor = [tree](const QString &file) -> QTreeWidgetItem * {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                auto *top = tree->topLevelItem(i);
                if (top->data(0, Qt::UserRole).toString() == file) return top;
                for (int j = 0; j < top->childCount(); ++j) {
                    auto *child = top->child(j);
                    if (child->data(0, Qt::UserRole).toString() == file) return child;
                }
            }
            return nullptr;
        };
        const auto localText = [&](const QString &file) {
            auto *row = rowFor(file);
            return row == nullptr ? QString() : row->text(1);
        };
        const auto defaultIndex = [&](const QString &file) {
            auto *row = rowFor(file);
            auto *choice = row == nullptr ? nullptr
                                          : qobject_cast<QComboBox *>(tree->itemWidget(row, 3));
            return choice == nullptr ? -1 : choice->currentIndex();
        };
        combo->setCurrentIndex(1);
        check(page.property("upgradeRuntimeJavaMajor").toInt() == 0,
              "target selection returns before Java metadata probing completes");
        settle();
        check(page.property("upgradeRuntimeJavaMajor").toInt() == 21,
              "background assessment publishes the detected runtime Java version");
        check(localText(QStringLiteral("quilt.jar")).contains(QStringLiteral("缺少目标加载器声明")),
              "Quilt 模组在 Fabric 目标下不冒充兼容");
        check(localText(QStringLiteral("nested-bad.jar"))
                      .contains(QStringLiteral("嵌套 JAR 声明无法解析"))
                  && defaultIndex(QStringLiteral("nested-bad.jar")) == 0,
              "★★ nested JAR parse failure is unknown and forces an explicit choice");
        check(localText(QStringLiteral("java.jar")).contains(QStringLiteral("需要 Java"))
                  && defaultIndex(QStringLiteral("java.jar")) == 1,
              "★★ mod Java requirement above the runtime Java is reported and defaults to disable");
        check(localText(QStringLiteral("loader.jar")).contains(QStringLiteral("加载器版本不满足")),
              "★★ declared loader version range is checked against the selected loader version");
        check(page.property("upgradeUnknownMods").toStringList().join(QLatin1Char('\n'))
                  .contains(QStringLiteral("嵌套 JAR 声明无法解析")),
              "unknown declaration reasons are available for the log/report");

        combo->setCurrentIndex(2);
        settle();
        check(localText(QStringLiteral("quilt.jar")).contains(QStringLiteral("本地 Minecraft 声明符合")),
              "Quilt 声明（含 quilt_loader/java 范围）在 Quilt 目标下被接受");
        check(localText(QStringLiteral("quilt-old.jar")).contains(QStringLiteral("加载器版本不满足")),
              "Quilt 模组要求的 quilt_loader 版本不满足时给出原因");
        check(localText(QStringLiteral("legacy.jar")).contains(QStringLiteral("缺少目标加载器声明")),
              "旧 Forge mcmod.info 不会在 Quilt 目标下被当成兼容");

        combo->setCurrentIndex(3);
        settle();
        check(localText(QStringLiteral("legacy.jar")).contains(QStringLiteral("本地 Minecraft 声明符合")),
              "★★ old Forge mcmod.info acceptedMinecraftVersions is parsed for a Forge target");
        check(localText(QStringLiteral("legacy-annotation.jar"))
                      .contains(QStringLiteral("本地 Minecraft 声明符合")),
              "★★ old Forge @Mod annotation acceptedMinecraftVersions is parsed from the class file");
        AppPaths::setMinecraftDir(oldRoot);
    }
    {
        const QString source = scratch.filePath("world-source");
        const QString saves = scratch.filePath("saves");
        check(write(source + "/level.dat", "world") && QDir().mkpath(saves), "prepare background world import");
        bool busy = false, completed = false, success = false, returned = false;
        bool callbackWasAsync = false;
        InstanceTransferController::Host host;
        host.isBeingModified = [&](const QString &) { return busy; };
        host.instanceDirForKey = [&](const QString &, const QString &) { return saves; };
        host.setFileOperationBusy = [&](const QString &, bool value) { busy = value; };
        InstanceTransferController controller(nullptr, nullptr, nullptr, host);
        QEventLoop loop;
        controller.importWorldInBackground("id", source, "Imported", [&](bool ok, const QString &problem) {
            callbackWasAsync = returned;
            completed = true;
            success = ok;
            loop.quit();
        });
        returned = true;
        check(busy && !completed, "background world import returns immediately and reserves instance");
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        check(completed && success && callbackWasAsync && !busy,
              "background world import finishes asynchronously and releases reservation");
        check(read(saves + "/Imported/level.dat") == "world" && read(source + "/level.dat") == "world",
              "background world import publishes complete copy without altering source");
    }
    {
        // 后台导入世界的「取消」：任务中心那颗按钮的判据 + 协作式停机。
        // 源世界故意做一个 32 MiB 的 region —— 拷贝器每 1 MiB 看一次取消位，
        // 主线程发完请求立刻按下取消，不会出现"还没按就已经拷完"的竞态。
        const QString source = scratch.filePath("world-cancel-source");
        const QString saves = scratch.filePath("saves-cancel");
        check(write(source + "/level.dat", "world") && QDir().mkpath(saves)
                  && write(source + "/region/r.0.0.mca", QByteArray(32 * 1024 * 1024, 'x')),
              "prepare cancellable world import");
        bool busy = false, completed = false, success = true, markedCancelled = false;
        bool finishCalled = false;
        Tasks::TaskId importTask = Tasks::invalidTaskId();
        InstanceTransferController::Host host;
        host.isBeingModified = [&](const QString &) { return busy; };
        host.instanceDirForKey = [&](const QString &, const QString &) { return saves; };
        host.setFileOperationBusy = [&](const QString &, bool value) { busy = value; };
        host.beginTask = [&](Tasks::TaskKind, const QString &, const QString &,
                             const QList<Tasks::TaskStep> &) {
            importTask = 4242;
            return importTask;
        };
        host.finishTask = [&](Tasks::TaskId, bool, const QString &) { finishCalled = true; };
        host.markTaskCancelled = [&](Tasks::TaskId id) { markedCancelled = id == importTask; };
        QString recoveryToken;
        host.registerRecovery = [](const OperationReport::Report &report) {
            return !report.id.isEmpty() && !report.context.isEmpty();
        };
        // 恢复入口必须挂在**终态之后**：这里顺便钉住那个顺序。
        host.setTaskRecovery = [&](Tasks::TaskId id, const QString &token) {
            if (id == importTask) recoveryToken = token;
        };
        InstanceTransferController controller(nullptr, nullptr, nullptr, host);
        QEventLoop loop;
        controller.importWorldInBackground("id", source, "Cancelled", [&](bool ok, const QString &) {
            completed = true;
            success = ok;
            loop.quit();
        });
        check(controller.canCancelTask(importTask),
              "running background world import advertises a working cancel");
        check(!controller.canCancelTask(Tasks::invalidTaskId()),
              "invalid task id is never cancellable");
        check(controller.cancelTask(importTask), "background world import accepts cancellation");
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        check(completed && !success && markedCancelled && !finishCalled && !busy,
              "cancelled world import ends as cancelled, not failed, and releases instance");
        check(!recoveryToken.isEmpty(),
              "★★ cancelled world import offers 「继续未完成操作」 after the task is terminal");
        check(!QFileInfo::exists(saves + "/Cancelled"),
              "cancelled world import publishes nothing");
        check(read(source + "/level.dat") == "world", "cancelled world import leaves source intact");
    }
    {
        // 后台删除世界：确认之后整棵树在工作线程里删，删完才回调。
        // 控制器只肯删「当前实例的 saves 直接子目录」，所以数据目录也摆成真实形状。
        const QString deleteRoot = scratch.filePath("game-delete");
        const QString previousDeleteRoot = AppPaths::minecraftDir();
        AppPaths::setMinecraftDir(deleteRoot);
        const QString world = AppPaths::versionDir(QStringLiteral("id")) + "/saves/world-delete";
        check(write(world + "/level.dat", "world")
                  && write(world + "/region/r.0.0.mca", QByteArray(2 * 1024 * 1024, 'y')),
              "prepare background world delete");
        bool busy = false, completed = false, success = false, returned = false;
        bool callbackWasAsync = false, taskFinished = false, finishSuccess = false;
        Tasks::TaskId deleteTask = Tasks::invalidTaskId();
        bool deleteCapabilitiesSeen = false, deleteCancelExposed = true;
        QString deleteBlockedReason;
        InstanceTransferController::Host host;
        host.isBeingModified = [&](const QString &) { return busy; };
        host.beginTask = [&](Tasks::TaskKind, const QString &, const QString &,
                             const QList<Tasks::TaskStep> &) {
            deleteTask = 4343;
            return deleteTask;
        };
        host.setTaskCapabilities = [&](Tasks::TaskId, const Tasks::TaskCapabilities &caps) {
            deleteCapabilitiesSeen = true;
            deleteCancelExposed = caps.canCancel;
            deleteBlockedReason = caps.cancelBlockedReason;
        };
        host.setFileOperationBusy = [&](const QString &, bool value) { busy = value; };
        host.finishTask = [&](Tasks::TaskId id, bool ok, const QString &) {
            taskFinished = id == deleteTask;
            finishSuccess = ok;
        };
        InstanceTransferController controller(nullptr, nullptr, nullptr, host);
        QEventLoop loop;
        controller.deleteWorldInBackground("id", world, [&](bool ok, const QString &) {
            callbackWasAsync = returned;
            completed = true;
            success = ok;
            loop.quit();
        });
        returned = true;
        check(busy && !completed,
              "background world delete returns immediately and reserves instance");
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        check(completed && success && callbackWasAsync && !busy,
              "background world delete finishes asynchronously and releases reservation");
        check(!QFileInfo::exists(world), "background world delete removes the whole tree");
        check(taskFinished && finishSuccess,
              "background world delete closes its task as succeeded");
        check(deleteCapabilitiesSeen && !deleteCancelExposed && !deleteBlockedReason.isEmpty(),
              "world delete says why it has no cancel instead of only hiding the button");
        AppPaths::setMinecraftDir(previousDeleteRoot);
    }
    {
        // 删除的闸：实例正被别的操作占着时一个字都不许动盘，当场拒绝。
        const QString world = scratch.filePath("world-delete-busy");
        check(write(world + "/level.dat", "world"), "prepare rejected world delete");
        InstanceTransferController::Host host;
        host.isBeingModified = [&](const QString &) { return true; };
        host.displayName = [&](const QString &) { return QStringLiteral("Busy"); };
        bool began = false;
        host.beginTask = [&](Tasks::TaskKind, const QString &, const QString &,
                             const QList<Tasks::TaskStep> &) {
            began = true;
            return 4344;
        };
        InstanceTransferController controller(nullptr, nullptr, nullptr, host);
        bool called = false, ok = true;
        controller.deleteWorldInBackground("id", world, [&](bool value, const QString &) {
            called = true;
            ok = value;
        });
        check(called && !ok && !began,
              "busy instance refuses background world delete before touching disk");
        check(read(world + "/level.dat") == "world",
              "refused world delete leaves the world intact");
    }
    {
        InstanceSettingsDialog page("Test", "Instance", InstanceSettingsDialog::Override(),
                                   InstanceSettingsDialog::GlobalValues(), {}, true);
        // 页是第一次切过去才建的 ⇒ 先切到「升级」那一页，再摸它的控件。
        page.showPage(QStringLiteral("upgrade"));
        page.setTransferTargets("upgrade", {{"Target", "target-id"}});
        bool upgrade = false, rollback = false;
        QObject::connect(&page, &InstanceSettingsDialog::upgradeInPlaceRequested, &page,
                         [&](const QString &id) { upgrade = id == "target-id"; });
        QObject::connect(&page, &InstanceSettingsDialog::rollbackUpgradeRequested, &page,
                         [&] { rollback = true; });
        auto *apply = page.findChild<QPushButton *>("instanceUpgradeInPlaceButton");
        auto *restore = page.findChild<QPushButton *>("instanceUpgradeRollbackButton");
        check(apply && !apply->isEnabled(), "upgrade requires an explicit target selection instead of silently choosing the first core");
        page.findChild<QComboBox *>(InstanceSettingsDialog::transferTargetComboName(QStringLiteral("upgrade")))->setCurrentIndex(1);
        check(apply && apply->isEnabled() && restore, "upgrade controls exist without migration selection");
        if (apply) apply->click();
        if (restore) restore->click();
        check(upgrade && rollback, "in-place upgrade and rollback emit distinct actions");
        page.setProperty("modernUpgradeFlow", true);
        auto *tree = page.findChild<QTreeWidget *>(QStringLiteral("instanceUpgradeModChecks"));
        tree->setColumnCount(4);
        page.setUpgradeModChecks({{"Compatible", "local compatible", "range", "keep.jar", "keep"},
                                  {"Unknown", "unknown", "", "unknown.jar", "review"}}, "review");
        check(page.upgradeKeptMods() == QStringList{"keep.jar"} && page.upgradePendingModCount() == 1,
              "review retains compatible files but requires an explicit decision for unknown files");
        auto *choice = qobject_cast<QComboBox *>(tree->itemWidget(tree->topLevelItem(1), 3));
        choice->setCurrentIndex(2);
        check(page.upgradePendingModCount() == 0, "explicit per-file choice clears the unresolved count");
        page.setUpgradeModChecks({{"Compatible", "query updated", "range", "keep.jar", "keep"},
                                  {"Unknown", "query failed", "", "unknown.jar", "disable"}}, "updated");
        check(page.upgradeKeptMods().contains("unknown.jar"), "platform updates do not discard explicit per-file decisions");
        page.setProperty("modernUpgradeWizard", true);
        const QList<QStringList> grouped{{"Compatible", "compatible", "range", "keep.jar", "keep", "compatible"},
            {"Needs review", "query failed", "", "review.jar", "review", ""}};
        page.setUpgradeModChecks(grouped, "grouped");
        auto *group = tree->topLevelItem(1);
        check(tree->topLevelItemCount() == 2 && tree->topLevelItem(0)->text(0) == "Needs review"
            && group->childCount() == 1 && !group->isExpanded(), "compatible mods move to a collapsed group after problem rows");
        check(page.upgradeKeptMods().contains("keep.jar") && page.upgradePendingModCount() == 1,
            "collapsed compatible rows retain real choice controls and correct counts");
        group->setExpanded(true); page.setUpgradeModChecks(grouped, "refreshed");
        check(tree->topLevelItem(1)->isExpanded(), "query refresh preserves the user's expanded compatible group");
    }
    {
        InstanceUpgrade::Request r{scratch.path() + "/prepare", "Instance", "prepare-core", "Original"};
        const QString old = r.gameDir + "/versions/Instance";
        check(write(old + "/Instance.json", R"({"id":"Instance"})")
              && write(old + "/Instance.jar", "old-core")
              && write(old + "/wtfml.meta", R"({"instanceId":"stable-id","name":"User name","customSetting":42})")
              && write(old + "/config/custom.cfg", "user-settings")
              && write(old + "/screenshots/picture.png", "picture")
              && write(old + "/saves/World/level.dat", "world")
              && write(old + "/mods/old-mod.jar", "mod")
              && write(old + "/mods/compatible.jar", "compatible-mod")
              && write(r.gameDir + "/versions/Target/Target.jar", "new-core")
              && write(r.gameDir + "/versions/Target/saves/Unwanted/level.dat", "must-not-copy"),
              "prepare full preservation fixture");
        InstanceUpgrade::Core core;
        core.clientJar = r.gameDir + "/versions/Target/Target.jar";
        core.gameVersion = "1.21.1";
        core.loaderName = "Fabric";
        core.loaderVersion = "0.16.0";
        core.keepOldMods = {QStringLiteral("compatible.jar")};
        core.mergedJson = QJsonObject{{"id", "Target"}, {"inheritsFrom", "Instance"}, {"jar", "Instance"},
            {"mainClass", "ExampleMain"}, {"downloads", QJsonObject{{"client", QJsonObject{{"sha1",
                QString::fromLatin1(QCryptographicHash::hash("new-core", QCryptographicHash::Sha1).toHex())}}}}}};
        const auto result = InstanceUpgrade::prepareAndCommit(r, core);
        check(result.ok && result.disabledMods == 1, "prepare and commit disables unverified old mods");
        check(read(old + "/mods/compatible.jar") == "compatible-mod"
              && !QFileInfo::exists(old + "/mods/compatible.jar.disabled"),
              "reviewed compatible mod remains enabled inside the snapshot-protected transaction");
        const auto meta = QJsonDocument::fromJson(read(old + "/wtfml.meta")).object();
        const auto json = QJsonDocument::fromJson(read(old + "/Instance.json")).object();
        check(meta.value("instanceId") == "stable-id" && meta.value("name") == "User name"
              && meta.value("customSetting").toInt() == 42 && meta.value("realVersion") == "1.21.1",
              "stable identity and user settings survive core replacement");
        check(json.value("id") == "Instance" && !json.contains("inheritsFrom") && !json.contains("jar"),
              "flattened target profile cannot inherit through replaced original id");
        check(read(old + "/config/custom.cfg") == "user-settings" && read(old + "/screenshots/picture.png") == "picture"
              && read(old + "/saves/World/level.dat") == "world" && read(old + "/mods/old-mod.jar.disabled") == "mod"
              && !QFileInfo::exists(old + "/saves/Unwanted"),
              "all original user content stays and target user content is not imported");
    }
    {
        // 2.1.1 副本模式：源实例保持不动，只复制一份候选并完整校验；
        // **不再为副本另做整份 ZIP 快照**（空间从四次写盘降到一次复制）。
        InstanceUpgrade::Request r{scratch.path() + "/copy-mode", "CopyName", "copy-ok", "Source"};
        const QString source = r.gameDir + "/versions/Source";
        check(write(source + "/Source.json", R"({"id":"Source"})")
              && write(source + "/Source.jar", "old-core")
              && write(source + "/wtfml.meta", R"({"instanceId":"copy-stable","name":"User name"})")
              && write(source + "/config/custom.cfg", "user-settings")
              && write(source + "/saves/World/level.dat", "world")
              && write(source + "/mods/old-mod.jar", "mod")
              && write(r.gameDir + "/versions/Target/Target.jar", "new-core")
              && write(r.gameDir + "/versions/Target/Target.json", R"({"id":"Target","mainClass":"ExampleMain"})"),
              "prepare copy-mode fixture");
        InstanceUpgrade::Core core;
        core.clientJar = r.gameDir + "/versions/Target/Target.jar";
        core.gameVersion = "1.21.1";
        core.loaderName = "Fabric";
        core.mergedJson = QJsonObject{{"id", "Target"},
            {"mainClass", "ExampleMain"},
            {"downloads", QJsonObject{{"client", QJsonObject{{"sha1",
                QString::fromLatin1(QCryptographicHash::hash("new-core", QCryptographicHash::Sha1).toHex())}}}}}};
        const auto result = InstanceUpgrade::prepareCopyAndCommit(r, core, source);
        const QString copyDir = r.gameDir + "/versions/CopyName";
        check(result.ok && result.requiresReload && result.snapshotZip.isEmpty(),
              "★ copy mode publishes a verified copy without a ZIP snapshot");
        check(read(source + "/Source.jar") == "old-core" && read(source + "/Source.json") == R"({"id":"Source"})"
              && read(source + "/mods/old-mod.jar") == "mod",
              "★★ copy mode leaves the source instance byte-for-byte intact");
        check(read(copyDir + "/CopyName.jar") == "new-core" && read(copyDir + "/mods/old-mod.jar.disabled") == "mod"
              && read(copyDir + "/saves/World/level.dat") == "world"
              && read(copyDir + "/config/custom.cfg") == "user-settings",
              "copy mode replaces only the core and archives unverified mods");
        const auto copyMeta = QJsonDocument::fromJson(read(copyDir + "/wtfml.meta")).object();
        check(copyMeta.value("instanceId") == "copy-stable" && copyMeta.value("realVersion") == "1.21.1",
              "copy mode keeps stable identity and records the new game version");
        check(QDir(r.gameDir + "/cache/backups/instances").entryList({"*.zip"}, QDir::Files).isEmpty(),
              "★★ no full ZIP snapshot is made for a copy (source instance is the rollback material)");
        check(InstanceUpgrade::recoverableTransactions(r.gameDir, "CopyName").isEmpty(),
              "copy mode offers no in-place rollback entry");
        const auto recoverMessages = InstanceUpgrade::recoverInterrupted(r.gameDir);
        check(!recoverMessages.isEmpty() && !QFileInfo::exists(InstanceUpgrade::transactionDir(r))
                  && read(copyDir + "/CopyName.jar") == "new-core",
              "startup recovery clears a committed copy transaction and keeps the published copy");
    }
    {
        // U5：副本模式按类别过滤 —— 只复核选中的那几条，未选的不进新实例。
        InstanceUpgrade::Request r{scratch.path() + "/copy-roots", "CopyRoots", "copy-roots",
                                   "Source"};
        r.copyRoots = {QStringLiteral("saves"), QStringLiteral("config"),
                       QStringLiteral("defaultconfigs")};
        r.sourceInstanceId = QStringLiteral("Source");
        const QString source = r.gameDir + "/versions/Source";
        check(write(source + "/Source.json", R"({"id":"Source"})")
                  && write(source + "/Source.jar", "old-core")
                  && write(source + "/wtfml.meta", R"({"instanceId":"roots-stable"})")
                  && write(source + "/saves/World/level.dat", "world")
                  && write(source + "/config/custom.cfg", "user-settings")
                  && write(source + "/mods/keep-me.jar", "mod")
                  && write(source + "/screenshots/shot.png", "png")
                  && write(source + "/options.txt", "options")
                  && write(r.gameDir + "/versions/Target/Target.jar", "new-core")
                  && write(r.gameDir + "/versions/Target/Target.json",
                           R"({"id":"Target","mainClass":"ExampleMain"})"),
              "prepare copy-roots fixture");
        InstanceUpgrade::Core core;
        core.clientJar = r.gameDir + "/versions/Target/Target.jar";
        core.gameVersion = "1.21.1";
        core.mergedJson = QJsonObject{{"id", "Target"}, {"mainClass", "ExampleMain"}};
        const auto result = InstanceUpgrade::prepareCopyAndCommit(r, core, source);
        const QString copyDir = r.gameDir + "/versions/CopyRoots";
        check(result.ok, "U5 只复制选中类别的副本也能发布");
        check(read(copyDir + "/saves/World/level.dat") == "world"
                  && read(copyDir + "/config/custom.cfg") == "user-settings",
              "★★ U5 选中的存档与配置确实复制过去了");
        check(!QFileInfo::exists(copyDir + "/mods/keep-me.jar")
                  && !QFileInfo::exists(copyDir + "/screenshots/shot.png")
                  && !QFileInfo::exists(copyDir + "/options.txt"),
              "★★ U5 没选的类别一个字节都没进新实例");
        check(read(source + "/mods/keep-me.jar") == "mod"
                  && read(source + "/screenshots/shot.png") == "png",
              "U5 源实例里的未选类别原样保留");
        // U6：提交之后新旧两边各留一条升级历史，同一个事务号。
        const QList<UpgradeHistory::Entry> targetHistory = UpgradeHistory::read(copyDir);
        check(targetHistory.size() == 1, "U6 新实例那边有一条历史");
        check(!targetHistory.isEmpty() && targetHistory.first().transactionId == QStringLiteral("copy-roots"),
              "U6 历史里是这次的事务号");
        check(!targetHistory.isEmpty() && targetHistory.first().sourceId == QStringLiteral("Source"),
              "U6 历史记着来源实例 id");
        check(!targetHistory.isEmpty() && targetHistory.first().targetId == QStringLiteral("CopyRoots"),
              "U6 历史记着目标实例 id");
        check(!targetHistory.isEmpty() && targetHistory.first().categories == r.copyRoots,
              "U6 历史记着这次实际迁移的类别");
        check(UpgradeHistory::read(source).size() == 1,
              "★ U6 源实例也留一条（同一份事务）");
        {
            // U4：只搬 options、不搬资源包时，启用列表里不能留着没迁入的包。
            InstanceUpgrade::Request r2{scratch.path() + "/copy-options", "CopyOptions",
                                        "copy-options", "Source"};
            r2.copyRoots = {QStringLiteral("options.txt")};
            r2.sourceInstanceId = QStringLiteral("Source");
            const QString source2 = r2.gameDir + "/versions/Source";
            check(write(source2 + "/Source.json", R"({"id":"Source"})")
                      && write(source2 + "/Source.jar", "old-core")
                      && write(source2 + "/options.txt",
                               "fov:0.5\nresourcePacks:[\"vanilla\",\"file/keep.zip\"]\n")
                      && write(source2 + "/resourcepacks/keep.zip", "zip")
                      && write(r2.gameDir + "/versions/Target/Target.jar", "new-core")
                      && write(r2.gameDir + "/versions/Target/Target.json",
                               R"({"id":"Target","mainClass":"ExampleMain"})"),
                  "prepare copy-options fixture");
            InstanceUpgrade::Core core2;
            core2.clientJar = r2.gameDir + "/versions/Target/Target.jar";
            core2.gameVersion = "1.21.1";
            core2.mergedJson = QJsonObject{{"id", "Target"}, {"mainClass", "ExampleMain"}};
            const auto result2 = InstanceUpgrade::prepareCopyAndCommit(r2, core2, source2);
            const QString copiedOptions = r2.gameDir + "/versions/CopyOptions/options.txt";
            check(result2.ok, "U4 只搬 options 的副本也能发布");
            check(QString::fromUtf8(read(copiedOptions)).contains(QStringLiteral("fov:0.5")),
                  "U4 普通键还在");
            check(QString::fromUtf8(read(copiedOptions)).contains(QStringLiteral("resourcePacks:[\"vanilla\"]")),
                  "★★ U4 没迁入的资源包不出现在启用列表里");
            // U8：九类全选 / 只选两类 / 目标重名 / 空间预算不足。
            const QStringList allRoots{QStringLiteral("saves"), QStringLiteral("mods"),
                                       QStringLiteral("config"), QStringLiteral("defaultconfigs"),
                                       QStringLiteral("options.txt"),
                                       QStringLiteral("resourcepacks"),
                                       QStringLiteral("shaderpacks"),
                                       QStringLiteral("screenshots"),
                                       QStringLiteral("structures"),
                                       QStringLiteral("servers.dat")};
            InstanceUpgrade::Request r3{scratch.path() + "/copy-all", "CopyAll", "copy-all",
                                        "Source"};
            r3.copyRoots = allRoots;
            r3.sourceInstanceId = QStringLiteral("Source");
            const QString source3 = r3.gameDir + "/versions/Source";
            check(write(source3 + "/Source.json", R"({"id":"Source"})")
                      && write(source3 + "/Source.jar", "old-core")
                      && write(source3 + "/wtfml.meta", R"({"instanceId":"all-stable"})")
                      && write(source3 + "/saves/W/level.dat", "w")
                      && write(source3 + "/mods/m.jar", "m")
                      && write(source3 + "/config/c.cfg", "c")
                      && write(source3 + "/defaultconfigs/d.cfg", "d")
                      && write(source3 + "/options.txt", "fov:0.5")
                      && write(source3 + "/resourcepacks/p.zip", "p")
                      && write(source3 + "/shaderpacks/s.zip", "s")
                      && write(source3 + "/screenshots/a.png", "a")
                      && write(source3 + "/structures/x.nbt", "x")
                      && write(source3 + "/servers.dat", "srv")
                      && write(r3.gameDir + "/versions/Target/Target.jar", "new-core")
                      && write(r3.gameDir + "/versions/Target/Target.json",
                               R"({"id":"Target","mainClass":"ExampleMain"})"),
                  "prepare copy-all fixture");
            InstanceUpgrade::Core core3;
            core3.clientJar = r3.gameDir + "/versions/Target/Target.jar";
            core3.gameVersion = "1.21.1";
            core3.mergedJson = QJsonObject{{"id", "Target"}, {"mainClass", "ExampleMain"}};
            const auto allResult = InstanceUpgrade::prepareCopyAndCommit(r3, core3, source3);
            const QString allDir = r3.gameDir + "/versions/CopyAll";
            check(allResult.ok, "U8 九类全选也能发布");
            check(read(allDir + "/saves/W/level.dat") == "w"
                      && read(allDir + "/mods/m.jar.disabled") == "m"
                      && read(allDir + "/config/c.cfg") == "c"
                      && read(allDir + "/defaultconfigs/d.cfg") == "d"
                      && read(allDir + "/options.txt").contains("fov")
                      && read(allDir + "/resourcepacks/p.zip") == "p"
                      && read(allDir + "/shaderpacks/s.zip") == "s"
                      && read(allDir + "/screenshots/a.png") == "a"
                      && read(allDir + "/structures/x.nbt") == "x"
                      && read(allDir + "/servers.dat") == "srv",
                  "★★ U8 九类全选时每一类都进了新实例");

            InstanceUpgrade::Request r4{r3.gameDir, "CopyTwo", "copy-two",
                                        "Source"};
            r4.copyRoots = {QStringLiteral("saves"), QStringLiteral("servers.dat")};
            r4.sourceInstanceId = QStringLiteral("Source");
            const auto twoResult =
                InstanceUpgrade::prepareCopyAndCommit(r4, core3, source3);
            const QString twoDir = r4.gameDir + "/versions/CopyTwo";
            check(twoResult.ok, "U8 只选两类也能发布");
            check(read(twoDir + "/saves/W/level.dat") == "w"
                      && read(twoDir + "/servers.dat") == "srv"
                      && !QFileInfo::exists(twoDir + "/mods/m.jar")
                      && !QFileInfo::exists(twoDir + "/screenshots/a.png"),
                  "★★ U8 只选两类时其余七类一个字节都不进");

            // 目标重名：目标目录已经存在 ⇒ 一个字节都不写。
            write(r3.gameDir + "/versions/Taken/keep.txt", "keep");
            InstanceUpgrade::Request r5{r3.gameDir, "Taken", "copy-taken",
                                        "Source"};
            r5.copyRoots = {QStringLiteral("saves")};
            r5.sourceInstanceId = QStringLiteral("Source");
            const auto takenResult = InstanceUpgrade::prepareCopyAndCommit(r5, core3, source3);
            check(!takenResult.ok && read(r3.gameDir + "/versions/Taken/keep.txt") == "keep",
                  "★★ U8 目标重名时拒绝且不动那一份");

            // 空间预算不足：预算 0 ⇒ 拒绝、源实例不变。
            InstanceUpgrade::Request r6{r3.gameDir, "CopyBudget",
                                        "copy-budget", "Source"};
            r6.copyRoots = {QStringLiteral("saves")};
            r6.spaceBudgetBytes = 0;
            r6.sourceInstanceId = QStringLiteral("Source");
            const auto budgetResult =
                InstanceUpgrade::prepareCopyAndCommit(r6, core3, source3);
            check(!budgetResult.ok && !QFileInfo::exists(r6.gameDir + "/versions/CopyBudget")
                      && read(source3 + "/saves/W/level.dat") == "w",
                  "★★ U8 空间预算不足时拒绝、源实例不变");

            // 跨根目录边界：源实例不在这次事务的游戏目录里 ⇒ 拒绝（第一版不支持跨根）。
            InstanceUpgrade::Request r7{r6.gameDir, "CopyCross", "copy-cross", "Source"};
            r7.copyRoots = {QStringLiteral("saves")};
            const QString foreign = scratch.path() + "/foreign/versions/Source";
            check(write(foreign + "/Source.json", R"({"id":"Source"})")
                      && write(foreign + "/Source.jar", "old-core")
                      && write(foreign + "/saves/W/level.dat", "w"),
                  "prepare cross-root fixture");
            const auto crossResult = InstanceUpgrade::prepareCopyAndCommit(r7, core3, foreign);
            check(!crossResult.ok
                      && !QFileInfo::exists(r6.gameDir + "/versions/CopyCross")
                      && read(foreign + "/saves/W/level.dat") == "w",
                  "★★ U8 跨根目录的源实例被拒绝，且源文件一个字节没动");
        }
    }
    {
        // 副本模式失败：源实例不动、目标名不出、暂存自己收掉。
        InstanceUpgrade::Request r{scratch.path() + "/copy-fail", "CopyFail", "copy-fail", "Source"};
        const QString source = r.gameDir + "/versions/Source";
        check(write(source + "/Source.json", R"({"id":"Source"})")
              && write(source + "/Source.jar", "old-core")
              && write(source + "/wtfml.meta", "{not json")
              && write(r.gameDir + "/versions/Target/Target.jar", "new-core"),
              "prepare failing copy-mode fixture");
        InstanceUpgrade::Core core;
        core.clientJar = r.gameDir + "/versions/Target/Target.jar";
        core.gameVersion = "1.21.1";
        core.mergedJson = QJsonObject{{"mainClass", "ExampleMain"}};
        const auto result = InstanceUpgrade::prepareCopyAndCommit(r, core, source);
        check(!result.ok && !QFileInfo::exists(r.gameDir + "/versions/CopyFail")
                  && read(source + "/Source.jar") == "old-core",
              "failed copy mode publishes nothing and leaves the source intact");
        check(!QFileInfo::exists(InstanceUpgrade::transactionDir(r)),
              "failed copy mode removes its staging copy");
    }
    {
        // 副本模式取消：停在复制阶段，源实例不动、暂存不会漏。
        InstanceUpgrade::Request r{scratch.path() + "/copy-cancel", "CopyCancel", "copy-cancel", "Source"};
        const QString source = r.gameDir + "/versions/Source";
        check(write(source + "/Source.json", R"({"id":"Source"})")
              && write(source + "/Source.jar", "old-core")
              && write(source + "/saves/Big/level.dat", QByteArray(8 * 1024 * 1024, 'c'))
              && write(r.gameDir + "/versions/Target/Target.jar", "new-core"),
              "prepare cancellable copy-mode fixture");
        InstanceUpgrade::Core core;
        core.clientJar = r.gameDir + "/versions/Target/Target.jar";
        core.gameVersion = "1.21.1";
        core.mergedJson = QJsonObject{{"mainClass", "ExampleMain"}};
        bool cancel = false;
        const auto result = InstanceUpgrade::prepareCopyAndCommit(r, core, source,
            [&](InstanceUpgrade::Stage stage) {
                if (stage == InstanceUpgrade::Stage::Copy) cancel = true;
            },
            [&] { return cancel; });
        check(!result.ok && result.cancelled
                  && !QFileInfo::exists(r.gameDir + "/versions/CopyCancel")
                  && read(source + "/Source.jar") == "old-core",
              "cancelled copy mode publishes nothing and keeps the source intact");
        check(!QFileInfo::exists(InstanceUpgrade::transactionDir(r)),
              "cancelled copy mode removes its staging copy");
        const QList<UpgradeHistory::Entry> cancelHistory = UpgradeHistory::read(source);
        check(!cancelHistory.isEmpty() && cancelHistory.first().cancelled,
              "★ U8 取消也留一条历史（不是只在成功时记）");
        const auto retry = InstanceUpgrade::prepareCopyAndCommit(r, core, source);
        check(retry.ok && QFileInfo::exists(r.gameDir + "/versions/CopyCancel"),
              "★★ U8 取消之后同一事务重入能成功（暂存已收干净）");
    }
    {
        // 副本模式在发布阶段被外部打断（这里用异常模拟"进程没走完"）：
        // 重启恢复只收自己那份未发布暂存，不碰版本目录。
        InstanceUpgrade::Request r{scratch.path() + "/copy-crash", "CopyCrash", "copy-crash", "Source"};
        const QString source = r.gameDir + "/versions/Source";
        check(write(source + "/Source.json", R"({"id":"Source"})")
              && write(source + "/Source.jar", "old-core")
              && write(r.gameDir + "/versions/Target/Target.jar", "new-core"),
              "prepare copy-mode crash fixture");
        InstanceUpgrade::Core core;
        core.clientJar = r.gameDir + "/versions/Target/Target.jar";
        core.gameVersion = "1.21.1";
        core.mergedJson = QJsonObject{{"mainClass", "ExampleMain"}};
        bool interrupted = false;
        try {
            InstanceUpgrade::prepareCopyAndCommit(r, core, source,
                [](InstanceUpgrade::Stage stage) {
                    if (stage == InstanceUpgrade::Stage::Publish) throw std::runtime_error("simulated crash");
                });
        } catch (const std::runtime_error &) { interrupted = true; }
        check(interrupted && QFileInfo::exists(InstanceUpgrade::transactionDir(r) + "/prepared")
                  && !QFileInfo::exists(r.gameDir + "/versions/CopyCrash"),
              "crash before publish leaves a copy staging directory");
        const auto messages = InstanceUpgrade::recoverInterrupted(r.gameDir);
        check(!messages.isEmpty() && !QFileInfo::exists(InstanceUpgrade::transactionDir(r))
                  && read(source + "/Source.jar") == "old-core"
                  && !QFileInfo::exists(r.gameDir + "/versions/CopyCrash"),
              "startup recovery removes unpublished copy staging without touching the source");
    }
    {
        InstanceUpgrade::Request r{scratch.path() + "/unsafe", "Instance", "unsafe-id", "Original"};
        const QString old = r.gameDir + "/versions/Instance";
        write(old + "/Instance.json", R"({"id":"../../../../victim"})");
        write(old + "/Instance.jar", "old-core");
        write(old + "/wtfml.meta", R"({"name":"Original"})");
        const QString victim = QDir::cleanPath(InstanceUpgrade::candidateDir(r) + "/../../../../victim.jar");
        write(victim, "keep");
        const QString client = r.gameDir + "/target.jar";
        write(client, "new-core");
        InstanceUpgrade::Core core;
        core.clientJar = client; core.gameVersion = "1.21.1";
        core.mergedJson = QJsonObject{{"mainClass", "ExampleMain"}};
        const auto result = InstanceUpgrade::prepareAndCommit(r, core);
        check(!result.ok && read(victim) == "keep" && read(old + "/Instance.jar") == "old-core",
              "traversal in old version id aborts upgrade without changing original");
    }
    {
        InstanceUpgrade::Request r{scratch.path() + "/active", "Instance", "active-stage", "Original"};
        const QString txn = InstanceUpgrade::transactionDir(r);
        write(txn + "/prepared/old.jar", "old");
        write(txn + "/journal.json", R"({"instanceId":"Instance"})");
        QLockFile lock(txn + ".lock");
        lock.setStaleLockTime(0);
        check(lock.tryLock(), "active transaction owns recovery exclusion lock");
        const auto competing = InstanceUpgrade::prepareAndCommit(r, InstanceUpgrade::Core{});
        check(!competing.ok && QFileInfo::exists(txn + "/prepared/old.jar"),
              "competing failed preparation cannot discard an active transaction");
        InstanceUpgrade::recoverInterrupted(r.gameDir);
        check(QFileInfo::exists(txn + "/prepared/old.jar"),
              "recovery leaves locked active preparation untouched");
    }
    const auto fixture = [&](const QString &name) {
        InstanceUpgrade::Request r{scratch.path() + '/' + name, "Instance", "tx-test", "Display name"};
        const QString old = r.gameDir + "/versions/Instance";
        check(write(old + "/Instance.json", R"({"id":"Instance"})")
              && write(old + "/Instance.jar", "old-core")
              && write(old + "/assets/asset", "asset-bytes")
              && write(old + "/libraries/library.jar", "library-bytes")
              && write(old + "/saves/World/level.dat", "world-bytes")
              && QDir().mkpath(old + "/empty-directory"), "prepare complete original fixture");
        const QString candidate = InstanceUpgrade::candidateDir(r);
        check(write(candidate + "/Instance.json", R"({"id":"Instance","target":"new"})")
              && write(candidate + "/Instance.jar", "new-core")
              && write(candidate + "/saves/World/level.dat", "world-bytes"), "prepare upgrade candidate");
        return r;
    };
    {
        const QString game = scratch.path() + "/controller";
        const QString source = game + "/versions/Instance";
        check(write(source + "/Instance.json", R"({"id":"Instance","mainClass":"OldMain"})")
              && write(source + "/Instance.jar", "old-core")
              && write(source + "/wtfml.meta", R"({"realVersion":"1.20.1","instanceId":"controller-stable"})")
              && write(source + "/mods/old.jar", "old-mod")
              && write(game + "/versions/Target/Target.json", R"({"id":"Target","mainClass":"NewMain"})")
              && write(game + "/versions/Target/Target.jar", "new-core")
              && write(game + "/versions/Target/wtfml.meta", R"({"realVersion":"1.21.1"})"),
              "prepare controller integration fixture");
        const QString previousRoot = AppPaths::minecraftDir();
        AppPaths::setMinecraftDir(game);
        QWidget anchor;
        QWidget home(&anchor);
        OverlayHost overlay(&anchor, &home);
        VersionManager versions;
        LauncherManager launcher;
        QSet<QString> locks;
        int started = 0, refreshed = 0;
        bool succeeded = false, lockReleasedBeforeFinish = false;
        bool upgradeCancelExposed = false, cancelledMarked = false;
        QSet<Tasks::TaskId> cancelExposedTasks;
        InstanceTransferController::Host host;
        host.displayName = [](const QString &id) { return id; };
        host.isBeingModified = [&](const QString &id) { return locks.contains(id); };
        host.setFileOperationBusy = [&](const QString &id, bool busy) {
            if (busy) locks.insert(id); else locks.remove(id);
        };
        host.beginTask = [&](Tasks::TaskKind, const QString &, const QString &, const QList<Tasks::TaskStep> &) {
            return Tasks::TaskId(++started);
        };
        // Host 是按值交给控制器的：这两个观察点必须在构造之前挂上，之后再改就只剩本地副本。
        host.setTaskCapabilities = [&](Tasks::TaskId id, const Tasks::TaskCapabilities &caps) {
            if (caps.canCancel) cancelExposedTasks.insert(id); else cancelExposedTasks.remove(id);
        };
        host.markTaskCancelled = [&](Tasks::TaskId) { cancelledMarked = true; };
        host.refreshInstances = [&] { ++refreshed; };
        host.finishTask = [&](Tasks::TaskId, bool ok, const QString &) {
            succeeded = ok;
            lockReleasedBeforeFinish = locks.isEmpty();
        };
        InstanceTransferController controller(&versions, &launcher, &overlay, host);
        QTimer answers;
        bool switchRoot = false;
        QObject::connect(&answers, &QTimer::timeout, &overlay, [&] {
            if (auto *box = overlay.findChild<InlineMessageBox *>()) {
                if (switchRoot) {
                    AppPaths::setMinecraftDir(scratch.path() + "/other-game");
                    switchRoot = false;
                }
                if (auto *button = box->findChild<QPushButton *>(InlineMessageBox::buttonName(0))) button->click();
            }
        });
        answers.start(10);
        QEventLoop wait;
        QTimer deadline;
        deadline.setSingleShot(true);
        bool timedOut = false, done = false, reload = false;
        QObject::connect(&deadline, &QTimer::timeout, &wait, [&] {
            timedOut = true;
            if (auto *box = overlay.findChild<InlineMessageBox *>()) box->reject();
            wait.quit();
        });
        const auto completed = [&](bool changed, const QString &) {
            reload = changed; done = true; wait.quit();
        };
        deadline.start(30000);
        controller.upgradeInPlace("Instance", "Target", completed);
        check(controller.copying() && locks.contains("Instance") && locks.contains("Target"),
              "controller holds source and target locks during background work");
        if (!done) wait.exec();
        deadline.stop();
        check(!timedOut && done && reload && succeeded && started == 1 && refreshed == 1
              && lockReleasedBeforeFinish && !controller.copying(),
              "controller completes task, releases locks, refreshes, and requests stale-page reload");
        check(read(source + "/Instance.jar") == "new-core" && read(source + "/mods/old.jar.disabled") == "old-mod",
              "controller upgrade reaches real disk transaction");
        done = false; reload = false;
        deadline.start(30000);
        controller.rollbackUpgrade("Instance", completed);
        if (!done) wait.exec();
        deadline.stop();
        check(!timedOut && done && reload && succeeded && started == 2 && refreshed == 2 && locks.isEmpty()
              && read(source + "/Instance.jar") == "old-core" && read(source + "/mods/old.jar") == "old-mod",
              "controller rollback restores real original files and lock lifecycle");
        done = false; reload = false; switchRoot = true;
        deadline.start(30000);
        controller.upgradeInPlace("Instance", "Target", completed);
        deadline.stop();
        check(!timedOut && done && !reload && started == 2 && locks.isEmpty()
              && read(source + "/Instance.jar") == "old-core",
              "changing game root inside confirmation aborts without writing either instance");
        AppPaths::setMinecraftDir(game);
        check(write(game + "/versions/Child/Child.json", R"({"id":"Child","inheritsFrom":"Instance"})"),
              "prepare dependent loader profile");
        done = false; reload = false;
        deadline.start(30000);
        controller.upgradeInPlace("Instance", "Target", completed);
        deadline.stop();
        check(done && !reload && started == 2 && read(source + "/Instance.jar") == "old-core",
              "controller refuses replacing a core referenced by another profile");
        check(write(game + "/versions/1.20.1/1.20.1.json", R"({"id":"1.20.1","mainClass":"OldMain"})")
              && write(game + "/versions/1.20.1/1.20.1.jar", "base-core")
              && write(game + "/versions/1.20.1/wtfml.meta", R"({"realVersion":"1.20.1"})"),
              "prepare shared vanilla base fixture");
        done = false; reload = false;
        deadline.start(30000);
        controller.upgradeInPlace("1.20.1", "Target", completed);
        deadline.stop();
        check(done && !reload && started == 2 && read(game + "/versions/1.20.1/1.20.1.jar") == "base-core",
              "canonical vanilla directory cannot be repurposed as a different core");
        // 后台原地升级**支持取消**：引擎在准备 / 快照 / 校验这些安全点看取消位。
        // 源实例故意做一个 64 MiB 的存档，按下取消那一刻它必然还在准备阶段。
        const QString cancelDir = game + "/versions/CancelMe";
        check(write(cancelDir + "/CancelMe.json", R"({"id":"CancelMe","mainClass":"OldMain"})")
              && write(cancelDir + "/CancelMe.jar", "old-core")
              && write(cancelDir + "/wtfml.meta", R"({"realVersion":"1.20.1","instanceId":"cancel-stable"})")
              && write(cancelDir + "/saves/Big/level.dat", QByteArray(64 * 1024 * 1024, 'c'))
              && write(game + "/versions/Target3/Target3.json", R"({"id":"Target3","mainClass":"NewMain"})")
              && write(game + "/versions/Target3/Target3.jar", "new-core")
              && write(game + "/versions/Target3/wtfml.meta", R"({"realVersion":"1.21.1"})"),
              "prepare cancellable controller upgrade fixture");
        done = false; reload = false;
        deadline.start(60000);
        controller.upgradeInPlace("CancelMe", "Target3", completed);
        const Tasks::TaskId cancelId(started);
        check(controller.canCancelTask(cancelId),
              "running in-place upgrade advertises a working cancel");
        upgradeCancelExposed = cancelExposedTasks.contains(cancelId);
        check(controller.cancelTask(cancelId), "in-place upgrade accepts cancellation");
        if (!done) wait.exec();
        deadline.stop();
        check(!timedOut && done && !reload && cancelledMarked && upgradeCancelExposed
                  && locks.isEmpty() && !controller.copying(),
              "★★ cancelled upgrade ends as Cancelled (not Failed), releases locks, keeps the page");
        check(read(cancelDir + "/CancelMe.jar") == "old-core"
                  && read(cancelDir + "/saves/Big/level.dat").size() == 64 * 1024 * 1024,
              "cancelled upgrade leaves the original instance byte-for-byte intact");
        answers.stop();
        AppPaths::setMinecraftDir(previousRoot);
    }
    {
        // 2.1.1 副本模式 + 父任务：任务中心只出现一个父任务（复制/校验/发布是它的步骤），
        // 源实例一个字节都不动，而且**不再为副本另做一份 ZIP 快照**。
        const QString game = scratch.path() + "/copy-controller";
        const QString source = game + "/versions/Source";
        check(write(source + "/Source.json", R"({"id":"Source","mainClass":"OldMain"})")
              && write(source + "/Source.jar", "old-core")
              && write(source + "/wtfml.meta", R"({"realVersion":"1.20.1","instanceId":"copy-controller"})")
              && write(source + "/saves/World/level.dat", "world")
              && write(source + "/mods/old.jar", "old-mod")
              && write(game + "/versions/Target/Target.json", R"({"id":"Target","mainClass":"NewMain"})")
              && write(game + "/versions/Target/Target.jar", "new-core")
              && write(game + "/versions/Target/wtfml.meta", R"({"realVersion":"1.21.1"})"),
              "prepare copy-mode controller fixture");
        const QString previousRoot = AppPaths::minecraftDir();
        AppPaths::setMinecraftDir(game);
        QWidget anchor;
        QWidget home(&anchor);
        OverlayHost overlay(&anchor, &home);
        VersionManager versions;
        LauncherManager launcher;
        bool childStarted = false, standaloneStarted = false, finished = false, success = false;
        bool parentStepDone = false, settledOk = false, settledCancelled = false;
        Tasks::TaskId seenParent = 0;
        int seenParentStep = -1;
        InstanceTransferController::Host host;
        host.displayName = [](const QString &id) { return id; };
        host.isBeingModified = [](const QString &) { return false; };
        host.setFileOperationBusy = [](const QString &, bool) {};
        host.beginTask = [&](Tasks::TaskKind, const QString &, const QString &,
                             const QList<Tasks::TaskStep> &) {
            standaloneStarted = true;
            return Tasks::TaskId(900);
        };
        host.beginChildTask = [&](Tasks::TaskId parent, Tasks::TaskKind, const QString &,
                                  const QString &, const QList<Tasks::TaskStep> &) {
            childStarted = true;
            seenParent = parent;
            return Tasks::TaskId(901);
        };
        host.setTaskStepState = [&](Tasks::TaskId id, int index, Tasks::TaskStepState state) {
            if (id == Tasks::TaskId(77) && index == 2 && state == Tasks::TaskStepState::Done) {
                parentStepDone = true;
            }
        };
        host.setTaskCapabilities = [](Tasks::TaskId, const Tasks::TaskCapabilities &) {};
        host.setTaskDetail = [](Tasks::TaskId, const QString &) {};
        host.setTaskProgress = [](Tasks::TaskId, qint64, qint64) {};
        host.finishTask = [&](Tasks::TaskId, bool ok, const QString &) {
            finished = true;
            success = ok;
        };
        host.markTaskCancelled = [](Tasks::TaskId) {};
        host.refreshInstances = [] {};
        InstanceTransferController controller(&versions, &launcher, &overlay, host);
        // 收尾那一发会弹一句结果（成功/失败都弹）：这里只负责按掉它，免得挂住事件循环。
        QTimer answers;
        QObject::connect(&answers, &QTimer::timeout, &overlay, [&] {
            if (auto *box = overlay.findChild<InlineMessageBox *>()) {
                if (auto *button = box->findChild<QPushButton *>(InlineMessageBox::buttonName(0))) {
                    button->click();
                } else {
                    box->reject();
                }
            }
        });
        answers.start(10);
        QEventLoop wait;
        QTimer deadline;
        deadline.setSingleShot(true);
        bool timedOut = false;
        QObject::connect(&deadline, &QTimer::timeout, &wait, [&] { timedOut = true; wait.quit(); });
        const QHash<int, int> parentSteps{
            {int(InstanceUpgrade::Stage::Checking), 1},
            {int(InstanceUpgrade::Stage::Copy), 1},
            {int(InstanceUpgrade::Stage::VerifyCopy), 1},
            {int(InstanceUpgrade::Stage::Publish), 2},
            {int(InstanceUpgrade::Stage::Verify), 2}};
        controller.upgradeToCopy("Source", "CopyOut", "Target",
            [&](bool ok, const QString &) { finished = true; success = ok; wait.quit(); },
            {}, true, Tasks::TaskId(77), parentSteps,
            [&](bool ok, bool cancelled) { settledOk = ok; settledCancelled = cancelled; });
        deadline.start(30000);
        if (!finished) wait.exec();
        deadline.stop();
        check(!timedOut && finished && success && childStarted && !standaloneStarted
                  && seenParent == Tasks::TaskId(77) && parentStepDone,
              "★★ copy-mode upgrade is one child of the update parent task (no extra task row)");
        check(settledOk && !settledCancelled, "copy-mode reports the exact settled outcome");
        check(read(source + "/Source.jar") == "old-core"
                  && read(source + "/mods/old.jar") == "old-mod",
              "★★ copy-mode leaves the source instance untouched");
        check(read(game + "/versions/CopyOut/CopyOut.jar") == "new-core"
                  && read(game + "/versions/CopyOut/saves/World/level.dat") == "world"
                  && read(game + "/versions/CopyOut/mods/old.jar.disabled") == "old-mod",
              "copy-mode publishes the new core with the copied user files");
        check(QDir(game + "/cache/backups/instances").entryList({"*.zip"}, QDir::Files).isEmpty(),
              "★★ copy-mode makes no extra ZIP snapshot (space optimization)");
        answers.stop();
        AppPaths::setMinecraftDir(previousRoot);
    }
    {
        // 2.1.3 适配替换：只改候选目录，成功后发布；文件变更日志跟着事务一起落地。
        // Editor archive extensions watch build/ and can keep real JARs open.
        // Use a disposable user-cache fixture outside the watched checkout;
        // controlled lock rejection is tested separately below.
        const QString fixtureRoot = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                                    + QStringLiteral("/WTFML-tests");
        check(QDir().mkpath(fixtureRoot), "create isolated replacement fixture parent");
        QTemporaryDir replacementScratch(fixtureRoot + QStringLiteral("/upgrade-XXXXXX"));
        check(replacementScratch.isValid(), "create replacement fixture outside editor archive watchers");
        if (!replacementScratch.isValid()) return failures;
        InstanceUpgrade::Request r{replacementScratch.path() + "/replace-ok", "Instance", "replace-ok", "Original"};
        const QString old = r.gameDir + "/versions/Instance";
        const QString oldNested = replacementScratch.path() + "/old-nested.jar";
        const QString newNested = replacementScratch.path() + "/new-nested.jar";
        check(write(oldNested, "old-nested-content") && write(newNested, "new-nested-content")
                  && write(old + "/Instance.json", R"({"id":"Instance"})")
                  && write(old + "/Instance.jar", "old-core")
                  && write(old + "/wtfml.meta", R"({"instanceId":"replace-stable","realVersion":"1.20.1"})")
                  && QDir().mkpath(old + "/mods")
                  && ZipUtil::createZip(old + "/mods/old-mod.jar",
                                        {{QStringLiteral("META-INF/jars/embedded.jar"), oldNested}})
                  && ZipUtil::createZip(replacementScratch.path() + "/new-mod.jar",
                                        {{QStringLiteral("META-INF/jars/embedded.jar"), newNested}})
                  && write(r.gameDir + "/versions/Target/Target.jar", "new-core"),
              "prepare mod replacement fixture with a nested JAR");
        const QByteArray newJarBytes = read(replacementScratch.path() + "/new-mod.jar");
        const QString staged = replacementScratch.path() + "/staged-new-mod.jar";
        check(write(staged, newJarBytes), "stage a verified replacement JAR");
        InstanceUpgrade::Core core;
        core.clientJar = r.gameDir + "/versions/Target/Target.jar";
        core.gameVersion = "1.21.1";
        core.loaderName = "Fabric";
        core.mergedJson = QJsonObject{{"id", "Target"}, {"mainClass", "ExampleMain"},
            {"downloads", QJsonObject{{"client", QJsonObject{{"sha1",
                QString::fromLatin1(QCryptographicHash::hash("new-core", QCryptographicHash::Sha1).toHex())}}}}}};
        InstanceUpgrade::ModReplacement replacement;
        replacement.stagedPath = staged;
        replacement.fileName = "new-mod.jar";
        replacement.sha1 = QString::fromLatin1(
            QCryptographicHash::hash(newJarBytes, QCryptographicHash::Sha1).toHex());
        replacement.size = newJarBytes.size();
        replacement.replaces = "old-mod.jar";
        replacement.projectKey = "modrinth:example";
        replacement.versionId = "target-2";
        replacement.title = "Example Mod";
        replacement.fromVersion = "1.0";
        replacement.toVersion = "2.0";
        core.replacements = {replacement};
#ifdef Q_OS_WIN
        const auto originalJarBytes = read(old + "/mods/old-mod.jar");
        const auto lockedPath = QDir::toNativeSeparators(old + "/mods/old-mod.jar").toStdWString();
        HANDLE blocker = CreateFileW(lockedPath.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        check(blocker != INVALID_HANDLE_VALUE, "hold a real JAR without delete sharing");
        if (blocker != INVALID_HANDLE_VALUE) {
            auto lockedRequest = r;
            lockedRequest.transactionId = QStringLiteral("replace-locked");
            const auto blocked = InstanceUpgrade::prepareAndCommit(lockedRequest, core);
            CloseHandle(blocker);
            // 两段钉：**拒绝与数据完整性**是硬条件；"占用者是谁"是**尽力而为**的诊断
            // （`directoryLockOwners()` 走 Windows Restart Manager，代码里自己写着
            // "Unlisted/deep owners may remain unknown; the original error still stands"）。
            // 合成一条的话，一台 RM 起不来的机器（实测某台开发机上 `RmStartSession` 直接
            // 返回 29）会把"拒绝得对不对"也一起判红。
            const bool refusedKeepsData = !blocked.ok && !blocked.snapshotZip.isEmpty()
                && QFileInfo::exists(blocked.snapshotZip)
                && read(old + "/mods/old-mod.jar") == originalJarBytes
                && read(old + "/Instance.jar") == "old-core"
                && !QFileInfo::exists(old + "/mods/new-mod.jar");
            if (!refusedKeepsData) {
                std::printf("  locked upgrade problem: %s\n", blocked.problem.toUtf8().constData());
            }
            check(refusedKeepsData,
                  "locked upgrade refuses and preserves original files and snapshot");
            // 有占用者就点名，点错也不行；一个都点不出来时不判（那是诊断能力问题，
            // 不是"拒绝"这件事没做对）。
            const bool ownerOk = blocked.problem.contains(
                                     QStringLiteral("PID %1").arg(GetCurrentProcessId()))
                || !blocked.problem.contains(QStringLiteral("PID"));
            if (!ownerOk) {
                std::printf("  owner mismatch: %s\n", blocked.problem.toUtf8().constData());
            }
            check(ownerOk, "locked upgrade identifies the owner when the OS reports it");
        }
#endif
        const auto result = InstanceUpgrade::prepareAndCommit(r, core);
        if (!result.ok) {
            replacementScratch.setAutoRemove(false);
            std::printf("Replacement failure: %s; fixture retained: %s\n",
                        result.problem.toUtf8().constData(), replacementScratch.path().toUtf8().constData());
        }
        check(result.ok && !result.changes.isEmpty()
                  && result.changes.join(QLatin1Char('\n')).contains(QStringLiteral("替换")),
              "★★ replacement is applied inside the transaction and reported");
        check(!QFileInfo::exists(old + "/mods/old-mod.jar")
                  && read(old + "/mods/new-mod.jar") == newJarBytes
                  && !QFileInfo::exists(old + "/mods/new-mod.jar.disabled"),
              "★★ replaced old JAR is gone, new JAR is present and not disabled");
        QByteArray nested;
        QString nestedProblem;
        check(ZipUtil::readEntry(old + "/mods/new-mod.jar",
                                 QStringLiteral("META-INF/jars/embedded.jar"), &nested,
                                 &nestedProblem, QStringLiteral("change fixture"), 4096)
                      == ZipUtil::EntryStatus::Ok
                  && nested == "new-nested-content",
              "★★ nested JAR is replaced together with its parent JAR (parent-level replacement)");
        const auto log = QJsonDocument::fromJson(read(old + "/wtfml.update-changes.json")).object();
        const auto runs = log.value(QStringLiteral("runs")).toArray();
        const auto run = runs.isEmpty() ? QJsonObject() : runs.last().toObject();
        const auto changes = run.value(QStringLiteral("changes")).toArray();
        check(log.value(QStringLiteral("format")).toString() == "wtfml-update-changes/1"
                  && runs.size() == 1 && changes.size() == 1
                  && changes.first().toObject().value(QStringLiteral("action")).toString() == "replace"
                  && changes.first().toObject().value(QStringLiteral("replacedFile")).toString() == "old-mod.jar"
                  && changes.first().toObject().value(QStringLiteral("file")).toString() == "new-mod.jar"
                  && !run.value(QStringLiteral("nestedNote")).toString().isEmpty(),
              "★★ file change log records the replacement and the nested-JAR rule");
    }
    {
        // 同名替换：旧文件名与新文件名相同（更新包里很常见），也必须先移除再写入。
        InstanceUpgrade::Request r{scratch.path() + "/replace-same", "Instance", "replace-same", "Original"};
        const QString old = r.gameDir + "/versions/Instance";
        check(write(old + "/Instance.json", R"({"id":"Instance"})")
                  && write(old + "/Instance.jar", "old-core")
                  && write(old + "/wtfml.meta", R"({"instanceId":"replace-same"})")
                  && QDir().mkpath(old + "/mods")
                  && write(old + "/mods/same.jar", "old-content")
                  && write(scratch.path() + "/same-new.jar", "new-content")
                  && write(r.gameDir + "/versions/Target/Target.jar", "new-core"),
              "prepare same-name replacement fixture");
        InstanceUpgrade::Core core;
        core.clientJar = r.gameDir + "/versions/Target/Target.jar";
        core.gameVersion = "1.21.1";
        core.mergedJson = QJsonObject{{"mainClass", "ExampleMain"}};
        InstanceUpgrade::ModReplacement replacement;
        replacement.stagedPath = scratch.path() + "/same-new.jar";
        replacement.fileName = "same.jar";
        replacement.sha1 = QString::fromLatin1(QCryptographicHash::hash(
            QByteArray("new-content"), QCryptographicHash::Sha1).toHex());
        replacement.size = QFileInfo(replacement.stagedPath).size();
        replacement.replaces = "same.jar";
        core.replacements = {replacement};
        const auto result = InstanceUpgrade::prepareAndCommit(r, core);
        check(result.ok && read(old + "/mods/same.jar") == "new-content"
                  && !QFileInfo::exists(old + "/mods/same.jar.disabled"),
              "★★ same-name replacement removes the old file before writing the verified new one");
    }
    {
        // 替换失败（哈希不符）：整趟回滚，原实例一个字节不动，也不写变更日志。
        InstanceUpgrade::Request r{scratch.path() + "/replace-bad", "Instance", "replace-bad", "Original"};
        const QString old = r.gameDir + "/versions/Instance";
        check(write(old + "/Instance.json", R"({"id":"Instance"})")
                  && write(old + "/Instance.jar", "old-core")
                  && write(old + "/wtfml.meta", R"({"instanceId":"replace-bad"})")
                  && QDir().mkpath(old + "/mods")
                  && write(old + "/mods/old-mod.jar", "old-mod")
                  && write(scratch.path() + "/bad-mod.jar", "bad-mod")
                  && write(r.gameDir + "/versions/Target/Target.jar", "new-core"),
              "prepare failing replacement fixture");
        InstanceUpgrade::Core core;
        core.clientJar = r.gameDir + "/versions/Target/Target.jar";
        core.gameVersion = "1.21.1";
        core.mergedJson = QJsonObject{{"mainClass", "ExampleMain"}};
        InstanceUpgrade::ModReplacement replacement;
        replacement.stagedPath = scratch.path() + "/bad-mod.jar";
        replacement.fileName = "new-mod.jar";
        replacement.sha1 = QByteArray(40, 'b');
        replacement.size = QFileInfo(replacement.stagedPath).size();
        replacement.replaces = "old-mod.jar";
        core.replacements = {replacement};
        const auto result = InstanceUpgrade::prepareAndCommit(r, core);
        check(!result.ok && !result.problem.isEmpty()
                  && read(old + "/mods/old-mod.jar") == "old-mod"
                  && !QFileInfo::exists(old + "/mods/new-mod.jar")
                  && !QFileInfo::exists(old + "/wtfml.update-changes.json"),
              "★★ failed replacement rolls the whole run back and leaves no change log");
        check(!QFileInfo::exists(InstanceUpgrade::transactionDir(r)),
              "failed replacement removes its staging copy");
    }
    {
        // 2.1.5a：整个游戏目录改名/搬走之后，用 journal 里的根指纹在候选根发现并恢复。
        const auto r = fixture("moved-root");
        bool interrupted = false;
        try {
            InstanceUpgrade::commitPrepared(r, [](InstanceUpgrade::Stage stage) {
                if (stage == InstanceUpgrade::Stage::Publish) throw std::runtime_error("crash");
            });
        } catch (const std::runtime_error &) {
            interrupted = true;
        }
        check(interrupted && QFileInfo::exists(InstanceUpgrade::transactionDir(r) + "/original")
                  && !QFileInfo::exists(r.gameDir + "/versions/Instance"),
              "prepare a crash state with retained original before moving the game root");
        QJsonObject journal;
        {
            QFile file(InstanceUpgrade::transactionDir(r) + "/journal.json");
            if (file.open(QIODevice::ReadOnly)) {
                journal = QJsonDocument::fromJson(file.readAll()).object();
            }
        }
        check(!journal.value("rootFingerprint").toString().isEmpty()
                  && !journal.value("rootRelative").toString().isEmpty()
                  && journal.value("rootName").toString() == QStringLiteral("moved-root"),
              "journal records root fingerprint / relative path / root name");
        const QString moved = scratch.path() + "/moved-root-renamed";
        check(QDir().rename(r.gameDir, moved), "move the whole game root to a new name");
        const auto messages = InstanceUpgrade::recoverInterruptedInRoots({moved});
        check(!messages.isEmpty()
                  && read(moved + "/versions/Instance/Instance.jar") == "old-core"
                  && read(moved + "/versions/Instance/saves/World/level.dat") == "world-bytes",
              "★★ renamed game root is discovered and the interrupted upgrade is rolled back");
        check(InstanceUpgrade::recoverInterruptedInRoots({moved}).isEmpty()
                  || InstanceUpgrade::recoverInterruptedInRoots({moved}).join(QString())
                         .contains(QStringLiteral("已恢复")),
              "recovery after a moved root is idempotent");
    }
    {
        // 指纹不符：不自动动盘，明确回退到完整 ZIP 快照。
        const auto r = fixture("moved-bad");
        bool interrupted = false;
        try {
            InstanceUpgrade::commitPrepared(r, [](InstanceUpgrade::Stage stage) {
                if (stage == InstanceUpgrade::Stage::Publish) throw std::runtime_error("crash");
            });
        } catch (const std::runtime_error &) {
            interrupted = true;
        }
        const QString journalPath = InstanceUpgrade::transactionDir(r) + "/journal.json";
        QJsonObject journal;
        {
            QFile file(journalPath);
            if (file.open(QIODevice::ReadOnly)) journal = QJsonDocument::fromJson(file.readAll()).object();
        }
        journal.insert(QStringLiteral("rootFingerprint"), QStringLiteral("deadbeef"));
        check(interrupted && write(journalPath, QJsonDocument(journal).toJson()),
              "tamper the root fingerprint fixture");
        const QString moved = scratch.path() + "/moved-bad-new";
        check(QDir().rename(r.gameDir, moved), "move the tampered game root");
        const auto messages = InstanceUpgrade::recoverInterruptedInRoots({moved});
        check(!messages.isEmpty()
                  && messages.join(QLatin1Char('\n')).contains(QStringLiteral("找不到匹配"))
                  && !QFileInfo::exists(moved + "/versions/Instance")
                  && QFileInfo::exists(moved + "/cache/instance-upgrades/tx-test/original/Instance.jar"),
              "★★ mismatched root fingerprint is not recovered and points to the ZIP fallback");
    }
    {
        const auto r = fixture("success");
        const auto result = InstanceUpgrade::commitPrepared(r);
        check(result.ok && result.rollbackAvailable && !result.rolledBack, "commit retains rollback material");
        check(read(r.gameDir + "/versions/Instance/Instance.jar") == "new-core", "new core is published");
        check(QFileInfo::exists(InstanceUpgrade::transactionDir(r) + "/original/empty-directory"),
              "retained original preserves empty directories too");
        bool ok = false; QString problem;
        const auto manifest = BackupFiles::readManifest(result.snapshotZip, &ok, &problem);
        bool assets = false, libraries = false;
        for (const auto &file : manifest.files) {
            assets |= file.path == "assets/asset";
            libraries |= file.path == "libraries/library.jar";
        }
        check(ok && manifest.complete && manifest.excluded.isEmpty() && assets && libraries,
              "verified safety snapshot includes assets and libraries");
        check(QFileInfo(result.snapshotZip).fileName().startsWith(BackupFiles::backupFilePrefix(r.instanceId))
              && manifest.sourceInstanceName == r.label,
              "safety snapshot is discoverable by existing restore UI after display name changes");
        check(InstanceUpgrade::recoverableTransactions(r.gameDir, r.instanceId).contains(r.transactionId),
              "committed transaction can be discovered after restart");
        const auto restored = InstanceUpgrade::rollback(r);
        check(restored.ok && restored.rolledBack && !restored.rollbackAvailable, "explicit rollback succeeds");
        check(read(r.gameDir + "/versions/Instance/Instance.jar") == "old-core"
              && read(InstanceUpgrade::transactionDir(r) + "/replaced/Instance.jar") == "new-core",
              "rollback restores original and preserves replaced version");
        check(InstanceUpgrade::recoverableTransactions(r.gameDir, r.instanceId).isEmpty()
              && !InstanceUpgrade::rollback(r).ok, "completed rollback is not offered twice");
    }
    {
        const auto r = fixture("tampered-retained");
        const auto committed = InstanceUpgrade::commitPrepared(r);
        check(committed.ok && write(InstanceUpgrade::transactionDir(r) + "/original/Instance.jar", "bad-core"),
              "alter retained original after successful commit");
        const auto restored = InstanceUpgrade::rollback(r);
        check(!restored.ok && !restored.rolledBack
              && read(r.gameDir + "/versions/Instance/Instance.jar") == "new-core",
              "tampered retained directory cannot replace current version during rollback");
    }
    {
        auto r = fixture("space");
        r.spaceBudgetBytes = 0;
        const auto result = InstanceUpgrade::commitPrepared(r);
        check(!result.ok && result.snapshotZip.isEmpty()
              && read(r.gameDir + "/versions/Instance/Instance.jar") == "old-core",
              "insufficient space stops before snapshot and replacement");
        check(!QFileInfo::exists(InstanceUpgrade::transactionDir(r)),
              "space rejection removes the staged copy instead of leaking it");
    }
    {
        InstanceUpgrade::Request r{scratch.path() + "/prepare-failure", "Instance", "tx-cleanup", "Original"};
        const QString old = r.gameDir + "/versions/Instance";
        const QString target = r.gameDir + "/versions/Target";
        check(write(old + "/Instance.json", R"({"id":"Instance"})")
              && write(old + "/Instance.jar", "old-core")
              && write(old + "/saves/World/level.dat", "world")
              && write(old + "/wtfml.meta", "{not json")
              && write(target + "/Target.jar", "new-core"),
              "prepare failed-upgrade fixture");
        InstanceUpgrade::Core core;
        core.clientJar = target + "/Target.jar";
        core.gameVersion = "1.21.1";
        core.mergedJson = QJsonObject{{"mainClass", "ExampleMain"}};
        const auto result = InstanceUpgrade::prepareAndCommit(r, core);
        check(!result.ok && read(old + "/Instance.jar") == "old-core"
              && read(old + "/saves/World/level.dat") == "world",
              "unreadable original settings abort the upgrade");
        check(!QFileInfo::exists(InstanceUpgrade::transactionDir(r)),
              "failed preparation does not leave an instance-sized staging copy");
    }
    {
        const auto r = fixture("orphan-prepared");
        bool interrupted = false;
        try {
            InstanceUpgrade::commitPrepared(r, [&](InstanceUpgrade::Stage stage) {
                if (stage == InstanceUpgrade::Stage::RetainOriginal) throw std::runtime_error("simulated crash");
            });
        } catch (const std::runtime_error &) { interrupted = true; }
        check(interrupted && QFileInfo::exists(InstanceUpgrade::transactionDir(r) + "/prepared"),
              "crash fixture leaves an unpublished staging copy");
        const auto messages = InstanceUpgrade::recoverInterrupted(r.gameDir);
        check(!messages.isEmpty() && !QFileInfo::exists(InstanceUpgrade::transactionDir(r))
              && read(r.gameDir + "/versions/Instance/Instance.jar") == "old-core",
              "startup recovery removes staging that never retained the original");
    }
    {
        const auto r = fixture("interrupted");
        bool interrupted = false;
        try {
            InstanceUpgrade::commitPrepared(r, [&](InstanceUpgrade::Stage stage) {
                if (stage == InstanceUpgrade::Stage::Publish) throw std::runtime_error("simulated crash");
            });
        } catch (const std::runtime_error &) { interrupted = true; }
        check(interrupted && !QFileInfo::exists(r.gameDir + "/versions/Instance"),
              "crash fixture stops between original retention and publish");
        const auto messages = InstanceUpgrade::recoverInterrupted(r.gameDir);
        check(!messages.isEmpty() && read(r.gameDir + "/versions/Instance/Instance.jar") == "old-core",
              "persisted journal recovers interrupted upgrade without in-memory state");
        check(InstanceUpgrade::recoverInterrupted(r.gameDir).isEmpty(), "startup recovery is idempotent");
    }
    {
        auto r = fixture("invalid");
        r.transactionId = "../escape";
        check(!InstanceUpgrade::commitPrepared(r).ok
              && read(r.gameDir + "/versions/Instance/Instance.jar") == "old-core",
              "path traversal is rejected before touching original");
    }
    {
        const auto r = fixture("wrong-core");
        check(write(InstanceUpgrade::candidateDir(r) + "/Instance.json", R"({"id":"Other"})"), "alter candidate id");
        check(!InstanceUpgrade::commitPrepared(r).ok
              && read(r.gameDir + "/versions/Instance/Instance.jar") == "old-core",
              "mismatched core identity is rejected");
    }
    {
        const auto r = fixture("corrupt-snapshot");
        const auto result = InstanceUpgrade::commitPrepared(r, [&](InstanceUpgrade::Stage stage) {
            if (stage == InstanceUpgrade::Stage::VerifySnapshot) {
                const QDir dir(r.gameDir + "/cache/backups/instances");
                const auto zips = dir.entryList({"*.zip"}, QDir::Files);
                if (!zips.isEmpty()) check(write(dir.filePath(zips.first()), "broken"), "inject corrupt snapshot");
            }
        });
        check(!result.ok && !result.rollbackAvailable
              && read(r.gameDir + "/versions/Instance/Instance.jar") == "old-core",
              "snapshot corruption stops replacement");
    }
    {
        const auto r = fixture("cancel-before-publish");
        bool cancelled = false;
        const auto result = InstanceUpgrade::commitPrepared(r, [&](InstanceUpgrade::Stage stage) {
            if (stage == InstanceUpgrade::Stage::Publish) cancelled = true;
        }, [&] { return cancelled; });
        check(!result.ok && result.rolledBack && result.cancelled && result.requiresReload
              && read(r.gameDir + "/versions/Instance/Instance.jar") == "old-core",
              "interruption after retaining original automatically rolls back");
    }
    {
        const auto r = fixture("cancel-rollback-blocked");
        bool cancelled = false;
        const auto result = InstanceUpgrade::commitPrepared(r, [&](InstanceUpgrade::Stage stage) {
            if (stage == InstanceUpgrade::Stage::Publish) {
                // Simulate external files occupying both rename destinations.
                check(write(r.gameDir + "/versions/Instance/block.txt", "CURRENT")
                      && write(InstanceUpgrade::transactionDir(r) + "/replaced/block.txt", "KEEP"),
                      "prepare blocked rollback during cancellation");
                cancelled = true;
            }
        }, [&] { return cancelled; });
        check(!result.ok && !result.cancelled && !result.rolledBack && result.requiresReload
                  && read(InstanceUpgrade::transactionDir(r) + "/original/Instance.jar") == "old-core"
                  && read(r.gameDir + "/versions/Instance/block.txt") == "CURRENT",
              "cancel request cannot hide rollback failure or delete retained user data");
    }
    {
        const auto r = fixture("corrupt-published");
        const auto result = InstanceUpgrade::commitPrepared(r, [&](InstanceUpgrade::Stage stage) {
            if (stage == InstanceUpgrade::Stage::Verify)
                check(write(r.gameDir + "/versions/Instance/Instance.jar", "bad-core"), "inject same-size corruption");
        });
        check(!result.ok && result.rolledBack
              && read(r.gameDir + "/versions/Instance/Instance.jar") == "old-core"
              && read(InstanceUpgrade::transactionDir(r) + "/replaced/Instance.jar") == "bad-core",
              "hash mismatch triggers rollback without deleting failed version");
    }
    {
        // 2.1.2「继续未完成操作」：报告登记 → 任务中心那一颗 → 补完并收尾。
        const QString game = scratch.path() + "/recovery-game";
        const QString world = game + "/source-world";
        const QString saves = game + "/versions/Recovery/saves";
        check(write(world + "/level.dat", "level-bytes")
                  && write(world + "/region/r.0.0.mca", QByteArray(2048, 'r'))
                  && QDir().mkpath(saves),
              "prepare recovery fixture");
        const QString previousRoot = AppPaths::minecraftDir();
        AppPaths::setMinecraftDir(game);
        QWidget anchor;
        QWidget home(&anchor);
        OverlayHost overlay(&anchor, &home);
        int started = 0;
        bool finished = false, success = false;
        QString finishedProblem, recoveryToken;
        FileOperationRecoveryController::Host host;
        host.displayName = [](const QString &id) { return id; };
        host.isInstanceBusy = [](const QString &) { return false; };
        host.log = [](const QString &) {};
        host.beginTask = [&](Tasks::TaskKind, const QString &, const QString &,
                             const QList<Tasks::TaskStep> &) {
            return Tasks::TaskId(++started);
        };
        host.setTaskDetail = [](Tasks::TaskId, const QString &) {};
        host.setTaskStepState = [](Tasks::TaskId, int, Tasks::TaskStepState) {};
        host.finishTask = [&](Tasks::TaskId, bool ok, const QString &problem) {
            finished = true;
            success = ok;
            finishedProblem = problem;
        };
        host.setTaskRecovery = [&](Tasks::TaskId, const QString &token) { recoveryToken = token; };
        FileOperationRecoveryController controller(&overlay, host);

        const QString reportId = QStringLiteral("aaaaaaaa-1111-2222-3333-444444444444");
        OperationReport::Report report;
        report.id = reportId;
        report.kind = OperationReport::Kind::WorldImport;
        report.title = QStringLiteral("导入世界：Recovery");
        report.instanceId = QStringLiteral("Recovery");
        report.sourceRoot = world;
        report.targetRoot = saves;
        report.stagingDir = OperationReport::stagingDirectory(game, reportId);
        report.conflictPolicy = OperationReport::ConflictPolicy::SkipExisting;
        report.totalFiles = 2;
        report.complete = false;
        report.startedAt = QDateTime::currentDateTime();
        report.updatedAt = report.startedAt;
        report.context = QJsonObject{{QStringLiteral("dataRoot"), game},
                                     {QStringLiteral("worldDir"), world},
                                     {QStringLiteral("savesDir"), saves},
                                     {QStringLiteral("name"), QStringLiteral("Recovered")}};
        report.entries.append(OperationReport::Entry{
            QStringLiteral("Recovered"), 0, OperationReport::EntryState::Pending, {}});
        check(controller.registerReport(report) && controller.has(reportId)
                  && QFileInfo::exists(OperationReport::reportPath(game, reportId)),
              "★ recovery report registers with its on-disk report");
        check(controller.resume(reportId) && started == 1,
              "★ task center's resume starts one operation task");
        QEventLoop wait;
        QTimer deadline;
        deadline.setSingleShot(true);
        bool timedOut = false;
        QObject::connect(&deadline, &QTimer::timeout, &wait, [&] { timedOut = true; wait.quit(); });
        const auto poll = new QTimer(&wait);
        QObject::connect(poll, &QTimer::timeout, &wait, [&] {
            if (finished) wait.quit();
        });
        poll->start(20);
        deadline.start(30000);
        if (!finished) wait.exec();
        poll->stop();
        deadline.stop();
        check(!timedOut && finished && success && recoveryToken.isEmpty(),
              "★★ resume completes the world import and leaves no recovery entry");
        check(read(saves + "/Recovered/level.dat") == "level-bytes"
                  && read(saves + "/Recovered/region/r.0.0.mca") == QByteArray(2048, 'r'),
              "★★ resumed import publishes the complete world");
        check(!QFileInfo::exists(OperationReport::reportPath(game, reportId))
                  && !controller.has(reportId) && !QFileInfo::exists(report.stagingDir),
              "resume clears the report and staging once everything is done");

        // 做不完时：收失败、重新挂上恢复入口、并写出“为什么做不完”的报告。
        const QString blockedId = QStringLiteral("bbbbbbbb-1111-2222-3333-444444444444");
        OperationReport::Report blocked = report;
        blocked.id = blockedId;
        blocked.stagingDir = OperationReport::stagingDirectory(game, blockedId);
        blocked.context = QJsonObject{{QStringLiteral("dataRoot"), game},
                                      {QStringLiteral("worldDir"), game + "/missing-world"},
                                      {QStringLiteral("savesDir"), saves},
                                      {QStringLiteral("name"), QStringLiteral("Blocked")}};
        blocked.entries = {OperationReport::Entry{
            QStringLiteral("Blocked"), 0, OperationReport::EntryState::Pending, {}}};
        check(controller.registerReport(blocked) && controller.resume(blockedId),
              "register and resume a world import whose source is gone");
        finished = false;
        success = true;
        poll->start(20);
        deadline.start(30000);
        if (!finished) wait.exec();
        poll->stop();
        deadline.stop();
        const QString failurePath =
            OperationReport::reportPath(game, blockedId) + QStringLiteral(".failure.txt");
        check(!timedOut && finished && !success && recoveryToken == blockedId
                  && QFileInfo::exists(failurePath),
              "★★ an impossible resume fails honestly, keeps the entry and writes a failure report");
        check(QString::fromUtf8(read(failurePath)).contains(QStringLiteral("继续未完成操作"))
                  && QString::fromUtf8(read(failurePath)).contains(QStringLiteral("未完成")),
              "failure report explains what is unfinished and what the user can do next");
        AppPaths::setMinecraftDir(previousRoot);
    }
    return failures == 0 ? 0 : 1;
}








