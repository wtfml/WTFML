#include "Support.hpp"

#include "manage/WorldFiles.hpp"
#include "operations/OperationReport.hpp"

namespace {

bool writeBytes(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

OperationReport::Report sampleReport(const QString &id) {
    OperationReport::Report report;
    report.id = id;
    report.kind = OperationReport::Kind::WorldImport;
    report.title = QStringLiteral("导入世界：测试");
    report.instanceId = QStringLiteral("Instance");
    report.targetRoot = QStringLiteral("D:/game/versions/Instance/saves");
    report.sourceRoot = QStringLiteral("D:/world");
    report.stagingDir = QStringLiteral("D:/game/cache/operation-staging/") + id;
    report.conflictPolicy = OperationReport::ConflictPolicy::SkipExisting;
    report.totalFiles = 3;
    report.doneFiles = 1;
    report.skippedFiles = 1;
    report.failedFiles = 1;
    report.complete = false;
    report.startedAt = QDateTime::currentDateTime();
    report.updatedAt = report.startedAt;
    report.context = QJsonObject{{QStringLiteral("worldDir"), QStringLiteral("D:/world")},
                                 {QStringLiteral("name"), QStringLiteral("Test")}};
    report.entries = {
        OperationReport::Entry{QStringLiteral("level.dat"), 12, OperationReport::EntryState::Done, {}},
        OperationReport::Entry{QStringLiteral("region/r.0.0.mca"), 34,
                               OperationReport::EntryState::Skipped, QStringLiteral("目标已有同名项")},
        OperationReport::Entry{QStringLiteral("region/r.0.1.mca"), 56,
                               OperationReport::EntryState::Failed, QStringLiteral("文件被占用")},
    };
    return report;
}

}  // namespace

void runOperationReportTests() {
    section("RO. 文件操作报告：逐项结果 / 冲突策略 / 可续暂存 / 失败原因（2.1.2）");
    const QString originalDir = AppPaths::minecraftDir();
    QTemporaryDir temp;
    check(temp.isValid(), "RO0 临时数据根建出来了");
    if (!temp.isValid()) {
        return;
    }
    AppPaths::setMinecraftDir(temp.path());

    const QString id = QStringLiteral("11111111-2222-3333-4444-555555555555");
    {
        const auto report = sampleReport(id);
        const QByteArray bytes = QJsonDocument(report.toJson()).toJson();
        bool ok = false;
        const auto loaded = OperationReport::Report::fromJson(bytes, &ok);
        check(ok && loaded.id == id && loaded.kind == OperationReport::Kind::WorldImport
                  && loaded.conflictPolicy == OperationReport::ConflictPolicy::SkipExisting
                  && loaded.entries.size() == 3
                  && loaded.entries.at(2).state == OperationReport::EntryState::Failed
                  && loaded.context.value(QStringLiteral("name")).toString() == "Test",
              "RO1 报告能原样写回并读出来（含冲突策略、逐项状态与续做参数）");
        check(loaded.hasUnfinished() && loaded.unfinishedFiles() == 1,
              "RO1 未完成 = 失败的 + 没做的（跳过不算未完成）",
              QStringLiteral("unfinished=%1").arg(loaded.unfinishedFiles()));
        const QString summary = loaded.summaryText();
        check(summary.contains(QStringLiteral("完成 1")) && summary.contains(QStringLiteral("跳过 1"))
                  && summary.contains(QStringLiteral("失败 1")),
              "RO1 摘要把完成 / 跳过 / 失败分开说", summary);
    }
    {
        // 坏输入：不认识的 schema、越界路径、条目太多、正文太大 —— 一律拒绝，不猜测。
        auto report = sampleReport(id);
        QJsonObject json = report.toJson();
        json.insert(QStringLiteral("schema"), 99);
        bool ok = true;
        OperationReport::Report::fromJson(QJsonDocument(json).toJson(), &ok);
        check(!ok, "RO2 不认识的 schema ⇒ 报告作废");

        report.entries.append(OperationReport::Entry{QStringLiteral("../escape"), 0,
                                                     OperationReport::EntryState::Pending, {}});
        ok = true;
        OperationReport::Report::fromJson(QJsonDocument(report.toJson()).toJson(), &ok);
        check(!ok, "RO2 条目里有 `..` ⇒ 整份作废（不拿报告里的路径去写盘）");

        auto tooMany = sampleReport(id);
        QJsonArray array;
        for (int i = 0; i < OperationReport::Report::kMaxEntries + 1; ++i) {
            array.append(QJsonObject{{QStringLiteral("path"), QStringLiteral("f%1").arg(i)},
                                     {QStringLiteral("state"), QStringLiteral("pending")}});
        }
        QJsonObject many = tooMany.toJson();
        many.insert(QStringLiteral("entries"), array);
        ok = true;
        OperationReport::Report::fromJson(QJsonDocument(many).toJson(), &ok);
        check(!ok, "RO2 条目超过上限 ⇒ 报告作废（不静默截断）");

        QByteArray huge(OperationReport::Report::kMaxFileBytes + 1, 'x');
        ok = true;
        OperationReport::Report::fromJson(huge, &ok);
        check(!ok, "RO2 正文超过 4 MiB ⇒ 报告作废");
    }
    {
        const auto report = sampleReport(id);
        const QString path = OperationReport::reportPath(temp.path(), id);
        QString problem;
        check(OperationReport::saveReport(path, report, &problem)
                  && QFileInfo::exists(path),
              "RO3 报告原子写进 `<数据根>/cache/operation-reports/`", problem);
        bool ok = false;
        const auto loaded = OperationReport::loadReport(path, &ok, &problem);
        check(ok && loaded.id == id, "RO3 读回来的是同一份", problem);
        const QString failure = report.failureText(path);
        check(failure.contains(QStringLiteral("region/r.0.1.mca"))
                  && failure.contains(QStringLiteral("文件被占用"))
                  && failure.contains(QStringLiteral("继续未完成操作")),
              "RO3 失败说明逐个列出未完成项、原因和下一步（不只一句“失败了”）",
              failure.left(120));
        check(OperationReport::removeReport(path) && !QFileInfo::exists(path),
              "RO3 报告删得掉");
    }
    {
        // 自动清理只碰本模块自己的暂存目录：没有报告、且超过时限的才收；
        // 有报告的、名字不像我们编号的（用户自己的目录）一律不碰。
        const QString root = OperationReport::stagingRoot(temp.path());
        const QString orphanId = QStringLiteral("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
        const QString keptId = QStringLiteral("99999999-8888-7777-6666-555555555555");
        const QString orphan = OperationReport::stagingDirectory(temp.path(), orphanId);
        const QString kept = OperationReport::stagingDirectory(temp.path(), keptId);
        const QString foreign = QDir(root).filePath(QStringLiteral("user-notes"));
        QDir().mkpath(orphan);
        QDir().mkpath(kept);
        QDir().mkpath(foreign);
        writeBytes(orphan + QStringLiteral("/partial.bin"), "partial");
        writeBytes(kept + QStringLiteral("/partial.bin"), "partial");
        writeBytes(foreign + QStringLiteral("/mine.txt"), "mine");
        auto keptReport = sampleReport(keptId);
        QString problem;
        check(OperationReport::saveReport(
                  OperationReport::reportPath(temp.path(), keptId), keptReport, &problem),
              "RO4 有一份报告正在用那份暂存", problem);
        // 时限传 0 = 不等宽限（生产调用用 30 分钟；这里只验“谁会被清”的判据）。
        const QStringList removed = OperationReport::cleanupOwnedStaging(temp.path(), 0);
        check(removed.contains(orphanId), "RO4 没有报告、又过了时限的暂存被清掉",
              removed.join(QLatin1Char(',')));
        check(!QFileInfo::exists(orphan), "RO4 那一份陈旧暂存真的没了");
        check(QFileInfo::exists(kept) && QFileInfo::exists(foreign),
              "★★ RO4 有报告的暂存和用户自己的目录一个都没动");
    }
    {
        // 世界导入：同名 + 「跳过已有项」= 什么都不写；「取消」= 明确失败。
        QTemporaryDir root;
        const QString saves = root.path() + QStringLiteral("/saves");
        const QString world = root.path() + QStringLiteral("/world");
        QDir().mkpath(saves + QStringLiteral("/Same"));
        QDir().mkpath(world);
        check(writeBytes(world + QStringLiteral("/level.dat"), "new-world")
                  && writeBytes(saves + QStringLiteral("/Same/level.dat"), "existing"),
              "RO5 同名世界夹具");
        WorldFiles::ImportOptions skip;
        skip.conflictPolicy = OperationReport::ConflictPolicy::SkipExisting;
        auto skipped = WorldFiles::importWorld(world, saves, QStringLiteral("Same"), skip);
        check(skipped.ok && skipped.skipped && skipped.conflict
                  && readBytes(saves + QStringLiteral("/Same/level.dat")) == "existing"
                  && !QFileInfo::exists(saves + QStringLiteral("/Same/region")),
              "★★ RO5 同名冲突选「跳过已有项」：目标一个字节不动，结果报“跳过”");
        WorldFiles::ImportOptions cancel;
        cancel.conflictPolicy = OperationReport::ConflictPolicy::Cancel;
        const auto refused = WorldFiles::importWorld(world, saves, QStringLiteral("Same"), cancel);
        check(!refused.ok && refused.conflict
                  && refused.problem.contains(QStringLiteral("已经有一个叫")),
              "RO5 同名冲突选「取消」：明确拒绝，不覆盖");
    }
    {
        // 可续暂存：先拷一半，再用同一个暂存目录续做 —— 已拷好且内容一致的跳过，发布一次。
        QTemporaryDir root;
        const QString saves = root.path() + QStringLiteral("/saves");
        const QString world = root.path() + QStringLiteral("/world");
        QDir().mkpath(saves);
        QDir().mkpath(world + QStringLiteral("/region"));
        check(writeBytes(world + QStringLiteral("/level.dat"), "level-bytes")
                  && writeBytes(world + QStringLiteral("/region/r.0.0.mca"), QByteArray(4096, 'r'))
                  && writeBytes(world + QStringLiteral("/region/r.0.1.mca"), QByteArray(4096, 's')),
              "RO6 世界夹具（level.dat + 两个区域文件）");
        const QString stagingId = QStringLiteral("abcdefab-cdef-abcd-efab-cdefabcdefab");
        const QString staging = OperationReport::stagingDirectory(temp.path(), stagingId);
        const QString candidate = QDir(staging).filePath(QStringLiteral("world"));
        QDir().mkpath(candidate + QStringLiteral("/region"));
        check(writeBytes(candidate + QStringLiteral("/level.dat"), "level-bytes")
                  && writeBytes(candidate + QStringLiteral("/region/r.0.0.mca"), QByteArray(4096, 'r')),
              "RO6 先摆一份“上次拷到一半”的暂存");
        WorldFiles::ImportOptions resume;
        resume.stagingDir = staging;
        int progressCalls = 0;
        const auto result = WorldFiles::importWorld(
            world, saves, QStringLiteral("Resumed"), resume, {},
            [&progressCalls](int, int, qint64, qint64) { ++progressCalls; });
        check(result.ok && result.copiedFiles == 1 && result.skippedFiles == 2
                  && result.stagingDir.isEmpty() && progressCalls >= 3,
              "★★ RO6 续做只补缺的那一个文件（已拷好且内容一致的按内容跳过）",
              QStringLiteral("copied=%1 skipped=%2 progress=%3")
                  .arg(result.copiedFiles).arg(result.skippedFiles).arg(progressCalls));
        check(readBytes(saves + QStringLiteral("/Resumed/level.dat")) == "level-bytes"
                  && readBytes(saves + QStringLiteral("/Resumed/region/r.0.1.mca")) == QByteArray(4096, 's'),
              "RO6 发布出来的世界是完整的一份");
        check(!QFileInfo::exists(staging), "RO6 发布成功后暂存被收掉（不再占一份空间）");
    }
    {
        // 失败/取消不清暂存：报告靠它续做。
        QTemporaryDir root;
        const QString saves = root.path() + QStringLiteral("/saves");
        const QString world = root.path() + QStringLiteral("/world");
        QDir().mkpath(saves);
        QDir().mkpath(world);
        writeBytes(world + QStringLiteral("/level.dat"), "level");
        writeBytes(world + QStringLiteral("/big.bin"), QByteArray(512 * 1024, 'b'));
        const QString stagingId = QStringLiteral("12345678-1234-1234-1234-123456789abc");
        const QString staging = OperationReport::stagingDirectory(temp.path(), stagingId);
        WorldFiles::ImportOptions options;
        options.stagingDir = staging;
        bool cancel = false;
        const auto result = WorldFiles::importWorld(world, saves, QStringLiteral("Cancelled"),
                                                    options, [&] { return cancel; },
                                                    [&cancel](int done, int, qint64, qint64) {
                                                        if (done >= 1) cancel = true;
                                                    });
        check(!result.ok && result.cancelled && !result.stagingDir.isEmpty()
                  && QFileInfo::exists(result.stagingDir),
              "★★ RO7 取消时保留暂存（下次接着拷，不从头再来）");
        check(!QFileInfo::exists(saves + QStringLiteral("/Cancelled")),
              "RO7 取消时不发布半份世界");
        // 把已拷好的文件改坏（大小不变）：续做必须按内容重新拷，不能只看大小。
        if (QFileInfo::exists(QDir(staging).filePath(QStringLiteral("world/big.bin")))) {
            writeBytes(QDir(staging).filePath(QStringLiteral("world/big.bin")),
                       QByteArray(512 * 1024, 'x'));
        }
        bool cancelAgain = false;
        auto resumed = WorldFiles::importWorld(world, saves, QStringLiteral("Cancelled"),
                                               options, [&] { return cancelAgain; });
        check(resumed.ok && readBytes(saves + QStringLiteral("/Cancelled/big.bin"))
                                 == QByteArray(512 * 1024, 'b'),
              "RO7 续做之后发布的世界内容正确");
    }
    AppPaths::setMinecraftDir(originalDir);
    check(AppPaths::minecraftDir() == originalDir, "RO9 数据根换回来了");
}
