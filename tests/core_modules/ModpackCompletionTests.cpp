#include "Support.hpp"
#include "manage/ModpackCompletion.hpp"
#include "manage/ModpackInstallReport.hpp"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif


void runModpackCompletionTests(const QString &sandbox) {
    section("ZG8. 缺项导入、手动补齐与继续安装（真实状态机，本地文件源）");
    const QString previousRoot = AppPaths::minecraftDir();
    const QString root = sandbox + QStringLiteral("/completion-engine");
    QDir().mkpath(root);
    const QByteArray clientBytes("client-fixture"), modBytes("manual-mod");
    const auto hash = [](const QByteArray &bytes) {
        return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
    };
    writeFileAt(root + "/client.bin", clientBytes);
    const QJsonObject client{{"url", QUrl::fromLocalFile(root + "/client.bin").toString()},
        {"sha1", hash(clientBytes)}, {"size", clientBytes.size()}};
    const QJsonObject base{{"id", "1.99.76"}, {"type", "release"},
        {"mainClass", "net.minecraft.client.main.Main"}, {"libraries", QJsonArray{}},
        {"downloads", QJsonObject{{"client", client}}}};
    writeFileAt(root + "/base.json", QJsonDocument(base).toJson());
    writeFileAt(root + "/index.json", QJsonDocument(QJsonObject{{"versions", QJsonArray{
        QJsonObject{{"id", "1.99.76"}, {"type", "release"},
        {"url", QUrl::fromLocalFile(root + "/base.json").toString()}}}}}).toJson());
    writeFileAt(root + "/config.txt", "keep-config");
    QString error;
    const QString archive = root + "/pack.zip";
    check(ZipUtil::createZip(archive, {{"overrides/config/test.txt", root + "/config.txt"}}, &error),
          "ZG8 准备覆盖内容测试包", error);
    ModpackImport::Pack pack;
    pack.format = ModpackImport::Format::CurseForge;
    pack.gameVersion = QStringLiteral("1.99.76"); pack.name = QStringLiteral("需要补齐的包");
    pack.overridesDir = QStringLiteral("overrides"); pack.declaredFileCount = 1;
    pack.filesNotDistributable = 1;
    pack.fileIssues.append({{123,456}, QStringLiteral("manual.jar"), QStringLiteral("mods/manual.jar"),
        hash(modBytes), modBytes.size(), QStringLiteral("平台标记不可用")});
    AppPaths::setMinecraftDir(root);
    const QString target = root + QStringLiteral("/versions/completion-test");
    const QString fingerprint(64, QLatin1Char('a'));
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            writeFileAt(target + "/mods/manual.jar", attempt == 1 ? QByteArray(modBytes.size(),'x') : modBytes);
        }
        VersionManager manager;
        manager.setUseMirror(false);
        manager.setVersionManifestUrlOverride(QUrl::fromLocalFile(root + "/index.json").toString());
        QEventLoop loop;
        bool done = false, success = false; QString message;
        QList<VersionManager::ModpackStage> stages;
        QObject::connect(&manager, &VersionManager::modpackImportStageChanged, &loop,
            [&](VersionManager::ModpackStage stage, const QString &) { stages.append(stage); });
        QObject::connect(&manager, &VersionManager::versionDownloadFinished, &loop,
            [&](const QString &id, bool ok, const QString &text) {
                if (id != QLatin1String("completion-test")) return;
                done = true; success = ok; message = text; loop.quit();
            });
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        QTimer::singleShot(0, &manager, [&] {
            manager.importModpack(archive, QStringLiteral("completion-test"), pack, {}, fingerprint);
        });
        loop.exec();
        check(done && success == (attempt == 2), "ZG8 只有补齐且哈希一致时才发成功终态", message);
        if (attempt == 2) {
            const QList<VersionManager::ModpackStage> expectedStages{
                VersionManager::ModpackStage::LoaderAndVanilla,
                VersionManager::ModpackStage::ModpackFiles,
                VersionManager::ModpackStage::ExtractingOverrides,
                VersionManager::ModpackStage::Verifying};
            check(stages == expectedStages,
                  "ZG8 导入期间按顺序发出原版加载器/清单文件/解压覆盖/校验收尾四个阶段信号");
        }
        QFile report(target + QStringLiteral("/wtfml.import-report.json"));
        check(report.open(QIODevice::ReadOnly), "ZG8 每次收尾都有持久报告");
        const auto json = QJsonDocument::fromJson(report.readAll()).object(); report.close();
        check(json.value(QStringLiteral("complete")).toBool() == (attempt == 2),
              "ZG8 报告完成状态与实际结果一致");
        check(QFileInfo::exists(target + "/config/test.txt"), "ZG8 缺项不删除已解压内容");
        if (attempt == 0) {
            const auto finish = [&](const std::function<bool()> &stop = {}) {
                return ModpackCompletion::finish(target, &error, stop);
            };
            check(QFileInfo::exists(target + "/wtfml.import-ready.json"), "ZG9 阶段完成后才持久化安装凭据");
            check(!finish(), "ZG9 未补齐不能凭安装凭据直接放行", error);
            writeFileAt(target + "/mods/manual.jar", modBytes);
            check(!finish([] { return true; }) && InstanceFiles::isUnfinishedImport("completion-test"),
                  "ZG9 校验取消保留未完成标记");
            writeFileAt(target + "/config/test.txt", "changed-config");
            check(!finish(), "ZG9 覆盖文件被改动时拒绝收尾", error);
            writeFileAt(target + "/config/test.txt", "keep-config");
            const auto markerPath = InstanceFiles::importMarkerPath("completion-test");
            QFile marker(markerPath);
            check(marker.open(QIODevice::ReadOnly), "ZG9 读取当前导入标记");
            const auto markerBytes = marker.readAll(); marker.close();
            writeFileAt(markerPath, "{}");
            check(!finish(), "ZG9 凭据不匹配导入尝试时拒绝收尾", error);
            writeFileAt(markerPath, markerBytes);
            const QString metaPath = AppPaths::versionMetaPath("completion-test");
            check(QDir().mkpath(metaPath), "ZG10 目录占住元数据发布位置");
            check(!finish() && QFileInfo::exists(markerPath) &&
                      QFileInfo::exists(target + "/mods/manual.jar"),
                  "ZG10 无原包收尾不能写 meta 时保留文件及启动门禁", error);
            check(QDir().rmdir(metaPath), "ZG10 清除空元数据目录夹具");
            const QString reportPath = target + QStringLiteral("/wtfml.import-report.json");
            check(QFile::rename(reportPath, reportPath + ".held") && QDir().mkpath(reportPath),
                  "ZG10 目录占住报告发布位置");
            check(!finish() && QFileInfo::exists(markerPath) && InstanceFiles::isUnfinishedImport("completion-test"),
                  "ZG10 meta 已发布而报告失败时仍不能启动", error);
            check(QDir().rmdir(reportPath) && QFile::rename(reportPath + ".held", reportPath),
                  "ZG10 恢复原报告，不丢缺项信息");
            const QString proofPath = target + QStringLiteral("/wtfml.import-ready.json");
            check(QFile::rename(proofPath, proofPath + ".held"), "ZG10 移开安装凭据");
            check(!finish() && QFileInfo::exists(markerPath), "ZG10 没有凭据不使用空/完成报告放行", error);
            check(QDir().mkpath(proofPath), "ZG10 目录占住凭据发布位置");
            check(!ModpackCompletion::capture(target, root, pack, fingerprint, {}, &error, {}) &&
                      QFileInfo::exists(markerPath), "ZG10 无法发布凭据时保持未完成", error);
            check(QDir().rmdir(proofPath) && QFile::rename(proofPath + ".held", proofPath),
                  "ZG10 恢复原安装凭据夹具");
            // 模拟提交报告/meta 后、摘最后标记前进程中断；完成报告本身不解除门禁。
            writeFileAt(AppPaths::versionMetaPath("completion-test"), R"({"jvmArgs":["-Dkeep=user-setting"]})");
            check(ModpackInstallReport::write(target, pack, {}, true, &error) &&
                      InstanceFiles::isUnfinishedImport("completion-test"),
                  "ZG9 提交中断现场：即使 meta/报告都存在，最后标记仍阻止启动", error);
            check(QFile::rename(archive, archive + ".away"), "ZG9 移开原包以验证无原包路径");
            // 上一段故意留下 complete=true 的中断现场；不能把旧报告误当作本次发布。
            // 先回到未完成，再由 stop 回调观察本次 finish 真正发布完成报告的边界。
            check(ModpackInstallReport::write(target, pack, pack.fileIssues, false, &error),
                  "ZG9 取消夹具先清除上一段的完成报告，避免校验阶段提前触发", error);
            // 在完成报告发布后才取消，不能仅依赖写报告之前的取消检查。
            bool cancelledAtPublication = false;
            const bool cancelledResult = finish([&] {
                QFile published(target + "/wtfml.import-report.json");
                if (!published.open(QIODevice::ReadOnly)) return false;
                const bool complete = QJsonDocument::fromJson(published.readAll()).object()
                                          .value("complete").toBool();
                cancelledAtPublication |= complete;
                return complete;
            });
            check(cancelledAtPublication && !cancelledResult && QFileInfo::exists(markerPath) &&
                      InstanceFiles::isUnfinishedImport("completion-test"),
                  "ZG9 完成报告发布后取消仍保留启动门禁", error);
            {
                QFile cancelledReport(target + "/wtfml.import-report.json");
                check(cancelledReport.open(QIODevice::ReadOnly) &&
                          !QJsonDocument::fromJson(cancelledReport.readAll()).object().value("complete").toBool(),
                      "ZG9 发布边界取消将报告恢复为未完成");
            }
#ifdef Q_OS_WIN
            // 允许读取凭据绑定，但拒绝删除：模拟杀毒软件占住最后发布标记。
            const HANDLE heldMarker = CreateFileW(reinterpret_cast<LPCWSTR>(markerPath.utf16()),
                GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            check(heldMarker != INVALID_HANDLE_VALUE, "ZG9 创建禁止删除的标记占用句柄");
            if (heldMarker != INVALID_HANDLE_VALUE) {
                const bool finishedWhileHeld = finish();
                CloseHandle(heldMarker);
                check(!finishedWhileHeld && error.contains(QStringLiteral("无法移除")) &&
                          QFileInfo::exists(markerPath) && InstanceFiles::isUnfinishedImport("completion-test"),
                      "ZG9 最后删除失败不放行启动且提示可重试", error);
                QFile blockedReport(target + "/wtfml.import-report.json");
                check(blockedReport.open(QIODevice::ReadOnly) &&
                          !QJsonDocument::fromJson(blockedReport.readAll()).object().value("complete").toBool(),
                      "ZG9 最后删除失败将持久报告恢复为未完成");
                check(QFileInfo::exists(target + "/mods/manual.jar") &&
                          QFileInfo::exists(target + "/wtfml.import-ready.json"),
                      "ZG9 标记占用不删除已安装文件与重试凭据");
            }
#endif
            check(finish() && !InstanceFiles::isUnfinishedImport("completion-test") &&
                      !QFileInfo::exists(markerPath), "ZG9 无原包校验后提交元数据并清除门禁", error);
            QFile recoveredMeta(AppPaths::versionMetaPath("completion-test"));
            check(recoveredMeta.open(QIODevice::ReadOnly) &&
                      QJsonDocument::fromJson(recoveredMeta.readAll()).object().value("jvmArgs").toArray().contains(QStringLiteral("-Dkeep=user-setting")),
                  "ZG9 中断恢复合并元数据，保留已有用户启动参数");
            recoveredMeta.close();
            check(!finish(), "ZG9 已提交凭据不能重复提交");
            check(QFile::rename(archive + ".away", archive), "ZG9 恢复原包夹具");
        }
        if (attempt == 1) {
            check(QFileInfo::exists(InstanceFiles::importMarkerPath("completion-test")) &&
                      InstanceFiles::isUnfinishedImport("completion-test"),
                  "ZG8 重新导入失败仍保留标记，即使旧 meta 存在也阻止启动");
            check(InstanceFiles::checkImportTargetName("completion-test", &error, fingerprint) ==
                      InstanceFiles::ImportNameVerdict::OkResume,
                  "ZG8 同一包与同一实例名可继续导入", error);
        } else {
            check(!QFileInfo::exists(InstanceFiles::importMarkerPath("completion-test")) &&
                      QFileInfo::exists(AppPaths::versionMetaPath("completion-test")),
                  "ZG8 真正补齐后才写成功 meta 并清除未完成标记");
            QFile metaFile(AppPaths::versionMetaPath("completion-test"));
            const bool metaOk = metaFile.open(QIODevice::ReadOnly);
            const QJsonObject metaObj = metaOk ? QJsonDocument::fromJson(metaFile.readAll()).object()
                                               : QJsonObject();
            metaFile.close();
            check(metaObj.value(QStringLiteral("name")).toString() == QStringLiteral("completion-test") &&
                      (attempt != 2 || !metaObj.value(QStringLiteral("instanceId")).toString().isEmpty()),
                  "ZG8 导入提交后在 wtfml.meta 中写入自定义实例名 name 和 instanceId");
        }
    }
    {
        const QString id = QStringLiteral("completion-meta-failure");
        const QString dir = AppPaths::versionDir(id);
        writeFileAt(dir + "/mods/manual.jar", modBytes);
        // 目录占住 meta 文件名，确定性模拟最后写入失败，不碰机器 ACL。
        QDir().mkpath(AppPaths::versionMetaPath(id));
        VersionManager manager;
        manager.setUseMirror(false);
        manager.setVersionManifestUrlOverride(QUrl::fromLocalFile(root + "/index.json").toString());
        QEventLoop loop;
        bool done = false, success = true;
        QString message;
        QObject::connect(&manager, &VersionManager::versionDownloadFinished, &loop,
            [&](const QString &finished, bool ok, const QString &text) {
                if (finished != id) return;
                done = true; success = ok; message = text; loop.quit();
            });
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        QTimer::singleShot(0, &manager, [&] { manager.importModpack(archive, id, pack, {}, fingerprint); });
        loop.exec();
        check(done && !success && message.contains(QStringLiteral("元数据")) &&
                  QFileInfo::exists(InstanceFiles::importMarkerPath(id)) &&
                  InstanceFiles::isUnfinishedImport(id),
              "ZG8 文件全齐但 meta 写入失败时，不报成功且仍拦截启动", message);
        QFile report(dir + "/wtfml.import-report.json");
        check(report.open(QIODevice::ReadOnly) &&
                  !QJsonDocument::fromJson(report.readAll()).object().value("complete").toBool(),
              "ZG8 元数据提交失败时持久报告也保留未完成");
        check(QFileInfo::exists(dir + "/mods/manual.jar") && QFileInfo::exists(dir + "/config/test.txt"),
              "ZG8 提交失败不删除已补齐文件和覆盖内容");
    }
    AppPaths::setMinecraftDir(previousRoot);
}
