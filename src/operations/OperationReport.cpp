#include "OperationReport.hpp"
#include <QCoreApplication>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

#include "manage/FolderManager.hpp"

namespace OperationReport {
namespace {

bool validId(const QString &id) {
    // 只认 UUID 形状（`QUuid::WithoutBraces`）。暂存目录的自动清理就靠这一条把
    // “用户自己建的 user-notes / 别的模块的暂存”挡在外面 —— 放宽成“字母数字加横线”
    // 会把它们一起当成自己的。
    static const QRegularExpression pattern(QStringLiteral(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return pattern.match(id).hasMatch();
}

/// 报告里的路径只接受“相对目标根、`/` 分隔、没有 `..`”的形状。
/// 这份文件可能来自上一次会话（或别处），不能拿它直接拼路径去写盘。
bool safeRelativePath(const QString &path) {
    if (path.isEmpty() || path.size() > 4096 || QDir::isAbsolutePath(path)
        || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':'))) {
        return false;
    }
    for (const QString &segment : path.split(QLatin1Char('/'))) {
        if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String("..")) {
            return false;
        }
    }
    return true;
}

QJsonObject entryToJson(const Entry &entry) {
    return QJsonObject{{QStringLiteral("path"), entry.path},
                       {QStringLiteral("bytes"), QString::number(entry.bytes)},
                       {QStringLiteral("state"), entryStateKey(entry.state)},
                       {QStringLiteral("reason"), entry.reason}};
}

}  // namespace

QString kindKey(Kind kind) {
    switch (kind) {
    case Kind::WorldImport: return QStringLiteral("world-import");
    case Kind::InstanceTransfer: return QStringLiteral("instance-transfer");
    case Kind::FileCopy: return QStringLiteral("file-copy");
    case Kind::Batch: return QStringLiteral("batch");
    case Kind::ModReplacement: return QStringLiteral("mod-replacement");
    }
    return QStringLiteral("batch");
}

Kind kindFromKey(const QString &key) {
    if (key == QLatin1String("world-import")) return Kind::WorldImport;
    if (key == QLatin1String("instance-transfer")) return Kind::InstanceTransfer;
    if (key == QLatin1String("file-copy")) return Kind::FileCopy;
    if (key == QLatin1String("mod-replacement")) return Kind::ModReplacement;
    return Kind::Batch;
}

QString conflictPolicyKey(ConflictPolicy policy) {
    switch (policy) {
    case ConflictPolicy::Cancel: return QStringLiteral("cancel");
    case ConflictPolicy::SkipExisting: return QStringLiteral("skip-existing");
    case ConflictPolicy::Overwrite: return QStringLiteral("overwrite");
    }
    return QStringLiteral("cancel");
}

ConflictPolicy conflictPolicyFromKey(const QString &key) {
    if (key == QLatin1String("skip-existing")) return ConflictPolicy::SkipExisting;
    if (key == QLatin1String("overwrite")) return ConflictPolicy::Overwrite;
    return ConflictPolicy::Cancel;
}

QStringList conflictChoices() {
    // 用户确认过的顺序：先“取消”，再“跳过已有项”；单文件入口另外给“盖掉”。
    return {QCoreApplication::translate("OperationReport", "取消"), QCoreApplication::translate("OperationReport", "跳过已有项")};
}

QString conflictPolicyLabel(ConflictPolicy policy) {
    switch (policy) {
    case ConflictPolicy::Cancel: return QCoreApplication::translate("OperationReport", "取消");
    case ConflictPolicy::SkipExisting: return QCoreApplication::translate("OperationReport", "跳过已有项");
    case ConflictPolicy::Overwrite: return QCoreApplication::translate("OperationReport", "覆盖");
    }
    return QCoreApplication::translate("OperationReport", "取消");
}

QString entryStateKey(EntryState state) {
    switch (state) {
    case EntryState::Pending: return QStringLiteral("pending");
    case EntryState::Done: return QStringLiteral("done");
    case EntryState::Skipped: return QStringLiteral("skipped");
    case EntryState::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("pending");
}

EntryState entryStateFromKey(const QString &key) {
    if (key == QLatin1String("done")) return EntryState::Done;
    if (key == QLatin1String("skipped")) return EntryState::Skipped;
    if (key == QLatin1String("failed")) return EntryState::Failed;
    return EntryState::Pending;
}

bool Report::hasUnfinished() const {
    if (!complete) {
        return true;
    }
    for (const Entry &entry : entries) {
        if (entry.state == EntryState::Pending || entry.state == EntryState::Failed) {
            return true;
        }
    }
    return false;
}

int Report::unfinishedFiles() const {
    int count = 0;
    for (const Entry &entry : entries) {
        if (entry.state == EntryState::Pending || entry.state == EntryState::Failed) {
            ++count;
        }
    }
    // 成功项只报总数、不逐条列，所以“没做完”还要把总数与逐项对不上的那一截算进去。
    const int accounted = doneFiles + skippedFiles + failedFiles;
    if (accounted < totalFiles) {
        count += totalFiles - accounted;
    }
    return count;
}

QString Report::summaryText() const {
    QString text = QCoreApplication::translate("OperationReport", "共 %1 项：完成 %2、跳过 %3、失败 %4")
                       .arg(totalFiles).arg(doneFiles).arg(skippedFiles).arg(failedFiles);
    if (unfinishedFiles() > 0) {
        text += QCoreApplication::translate("OperationReport", "；未完成 %1").arg(unfinishedFiles());
    }
    if (!complete) {
        text += QCoreApplication::translate("OperationReport", "；本次未收尾");
    }
    return text;
}

QString Report::failureText(const QString &path) const {
    QStringList lines;
    lines.append(QCoreApplication::translate("OperationReport", "操作：%1").arg(title));
    if (!path.isEmpty()) {
        lines.append(QCoreApplication::translate("OperationReport", "报告：%1").arg(QDir::toNativeSeparators(path)));
    }
    if (!targetRoot.isEmpty()) {
        lines.append(QCoreApplication::translate("OperationReport", "目标：%1").arg(QDir::toNativeSeparators(targetRoot)));
    }
    if (!failureReason.isEmpty()) {
        lines.append(QCoreApplication::translate("OperationReport", "原因：%1").arg(failureReason));
    }
    lines.append(QCoreApplication::translate("OperationReport", "结果：%1").arg(summaryText()));
    if (!stagingDir.isEmpty()) {
        lines.append(QCoreApplication::translate("OperationReport", "暂存：%1（继续时会复用它，不会重新拷一遍）")
                         .arg(QDir::toNativeSeparators(stagingDir)));
    }
    QStringList unfinished;
    for (const Entry &entry : entries) {
        if (entry.state != EntryState::Pending && entry.state != EntryState::Failed) {
            continue;
        }
        QString line = QCoreApplication::translate("OperationReport", "- %1：%2").arg(entry.path, entryStateKey(entry.state));
        if (!entry.reason.isEmpty()) {
            line += QCoreApplication::translate("OperationReport", "（%1）").arg(entry.reason);
        }
        unfinished.append(line);
        if (unfinished.size() >= 200) {
            unfinished.append(QCoreApplication::translate("OperationReport", "- …（还有 %1 项未列出）")
                                  .arg(unfinishedFiles() - 200));
            break;
        }
    }
    if (!unfinished.isEmpty()) {
        lines.append(QCoreApplication::translate("OperationReport", "未完成项："));
        lines.append(unfinished.join(QLatin1Char('\n')));
    }
    lines.append(QCoreApplication::translate("OperationReport", "关闭占用相关文件的程序后，在任务中心选择「继续未完成操作」。已完成项目会跳过。若仍失败，可提供此报告及启动器日志。"));
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QJsonObject Report::toJson() const {
    QJsonArray array;
    for (const Entry &entry : entries) {
        array.append(entryToJson(entry));
    }
    QJsonObject json{
        {QStringLiteral("schema"), kSchema},
        {QStringLiteral("id"), id},
        {QStringLiteral("kind"), kindKey(kind)},
        {QStringLiteral("title"), title},
        {QStringLiteral("instanceId"), instanceId},
        {QStringLiteral("targetRoot"), targetRoot},
        {QStringLiteral("sourceRoot"), sourceRoot},
        {QStringLiteral("stagingDir"), stagingDir},
        {QStringLiteral("conflictPolicy"), conflictPolicyKey(conflictPolicy)},
        {QStringLiteral("entries"), array},
        {QStringLiteral("totalFiles"), totalFiles},
        {QStringLiteral("doneFiles"), doneFiles},
        {QStringLiteral("skippedFiles"), skippedFiles},
        {QStringLiteral("failedFiles"), failedFiles},
        {QStringLiteral("totalBytes"), QString::number(totalBytes)},
        {QStringLiteral("doneBytes"), QString::number(doneBytes)},
        {QStringLiteral("complete"), complete},
        {QStringLiteral("cancelled"), cancelled},
        {QStringLiteral("failureReason"), failureReason},
        {QStringLiteral("context"), context},
        {QStringLiteral("startedAt"), startedAt.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("updatedAt"), updatedAt.toUTC().toString(Qt::ISODateWithMs)},
    };
    return json;
}

Report Report::fromJson(const QByteArray &bytes, bool *ok) {
    Report report;
    if (ok != nullptr) {
        *ok = false;
    }
    if (bytes.size() > kMaxFileBytes) {
        return report;
    }
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) {
        return report;
    }
    const QJsonObject json = document.object();
    if (json.value(QStringLiteral("schema")).toInt() != kSchema) {
        return report;
    }
    report.id = json.value(QStringLiteral("id")).toString();
    if (!validId(report.id)) {
        return report;
    }
    report.kind = kindFromKey(json.value(QStringLiteral("kind")).toString());
    report.title = json.value(QStringLiteral("title")).toString();
    report.instanceId = json.value(QStringLiteral("instanceId")).toString();
    report.targetRoot = json.value(QStringLiteral("targetRoot")).toString();
    report.sourceRoot = json.value(QStringLiteral("sourceRoot")).toString();
    report.stagingDir = json.value(QStringLiteral("stagingDir")).toString();
    report.conflictPolicy =
        conflictPolicyFromKey(json.value(QStringLiteral("conflictPolicy")).toString());
    report.totalFiles = std::max(0, json.value(QStringLiteral("totalFiles")).toInt());
    report.doneFiles = std::max(0, json.value(QStringLiteral("doneFiles")).toInt());
    report.skippedFiles = std::max(0, json.value(QStringLiteral("skippedFiles")).toInt());
    report.failedFiles = std::max(0, json.value(QStringLiteral("failedFiles")).toInt());
    report.totalBytes = json.value(QStringLiteral("totalBytes")).toString().toLongLong();
    report.doneBytes = json.value(QStringLiteral("doneBytes")).toString().toLongLong();
    report.complete = json.value(QStringLiteral("complete")).toBool(false);
    report.cancelled = json.value(QStringLiteral("cancelled")).toBool(false);
    report.failureReason = json.value(QStringLiteral("failureReason")).toString();
    report.startedAt = QDateTime::fromString(
        json.value(QStringLiteral("startedAt")).toString(), Qt::ISODateWithMs);
    report.updatedAt = QDateTime::fromString(
        json.value(QStringLiteral("updatedAt")).toString(), Qt::ISODateWithMs);
    const QJsonObject context = json.value(QStringLiteral("context")).toObject();
    if (QJsonDocument(context).toJson(QJsonDocument::Compact).size() > kMaxContextBytes) {
        return report;  // `ok` 保持 false
    }
    report.context = context;

    const QJsonValue entriesValue = json.value(QStringLiteral("entries"));
    if (!entriesValue.isArray()) {
        return report;
    }
    const QJsonArray array = entriesValue.toArray();
    if (array.size() > kMaxEntries) {
        return report;
    }
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            return report;
        }
        const QJsonObject object = value.toObject();
        Entry entry;
        entry.path = object.value(QStringLiteral("path")).toString();
        if (!safeRelativePath(entry.path)) {
            return report;
        }
        entry.bytes = object.value(QStringLiteral("bytes")).toString().toLongLong();
        entry.state = entryStateFromKey(object.value(QStringLiteral("state")).toString());
        entry.reason = object.value(QStringLiteral("reason")).toString();
        report.entries.append(entry);
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return report;
}

QString reportDirectory(const QString &dataRoot) {
    return QDir(dataRoot).filePath(QStringLiteral("cache/operation-reports"));
}

QString reportPath(const QString &dataRoot, const QString &id) {
    return QDir(reportDirectory(dataRoot)).filePath(id + QStringLiteral(".json"));
}

QString stagingRoot(const QString &dataRoot) {
    return QDir(dataRoot).filePath(QStringLiteral("cache/operation-staging"));
}

QString stagingDirectory(const QString &dataRoot, const QString &id) {
    return QDir(stagingRoot(dataRoot)).filePath(id);
}

bool saveReport(const QString &path, const Report &report, QString *problem) {
    if (problem != nullptr) {
        problem->clear();
    }
    if (!validId(report.id) || report.entries.size() > Report::kMaxEntries) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("OperationReport", "报告编号或条目数不合法，未写入报告。");
        }
        return false;
    }
    const QJsonObject context = report.context;
    if (QJsonDocument(context).toJson(QJsonDocument::Compact).size() > Report::kMaxContextBytes) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("OperationReport", "报告附带参数过大，未写入报告。");
        }
        return false;
    }
    const QByteArray bytes = QJsonDocument(report.toJson()).toJson(QJsonDocument::Indented);
    if (bytes.size() > Report::kMaxFileBytes) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("OperationReport", "报告正文超过上限，未写入报告。");
        }
        return false;
    }
    const QFileInfo info(path);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("OperationReport", "无法创建报告目录：%1").arg(info.absolutePath());
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
        || !file.commit()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("OperationReport", "无法写入报告：%1（%2）")
                           .arg(QDir::toNativeSeparators(path), file.errorString());
        }
        return false;
    }
    return true;
}

QString writeFailureText(const QString &path, const Report &report, QString *problem) {
    if (problem != nullptr) {
        problem->clear();
    }
    const QString target = path + QStringLiteral(".failure.txt");
    const QByteArray bytes = report.failureText(path).toUtf8();
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
        || !file.commit()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("OperationReport", "无法保存失败详情：%1").arg(target);
        }
        return QString();
    }
    return target;
}

Report loadReport(const QString &path, bool *ok, QString *problem) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (problem != nullptr) {
        problem->clear();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("OperationReport", "读不到报告：%1").arg(QDir::toNativeSeparators(path));
        }
        return Report();
    }
    if (file.size() > Report::kMaxFileBytes) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("OperationReport", "报告超过 %1 字节上限，未采用。")
                           .arg(Report::kMaxFileBytes);
        }
        return Report();
    }
    const QByteArray bytes = file.readAll();
    file.close();
    bool parsed = false;
    Report report = Report::fromJson(bytes, &parsed);
    if (!parsed) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("OperationReport", "报告格式无效或内容超出允许范围，未读取。");
        }
        return Report();
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return report;
}

bool removeReport(const QString &path) {
    return !QFileInfo::exists(path) || QFile::remove(path);
}

QStringList cleanupOwnedStaging(const QString &dataRoot, qint64 minAgeSeconds) {
    QStringList removed;
    QList<QDir> roots;
    const QDir staging(stagingRoot(dataRoot));
    if (staging.exists()) {
        roots.append(staging);
    }
    // 世界导入的暂存放在每个实例的 `saves/.wtfml-world-staging/` 里（同卷改名发布）。
    // 这里只认**本模块的 UUID 目录**：用户自己的目录、别的模块的暂存一律不碰。
    const QDir versions(QDir(dataRoot).filePath(QStringLiteral("versions")));
    for (const QFileInfo &instance :
         versions.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QDir worldStaging(instance.absoluteFilePath()
                                + QStringLiteral("/saves/.wtfml-world-staging"));
        if (worldStaging.exists()) {
            roots.append(worldStaging);
        }
    }
    const QDateTime now = QDateTime::currentDateTime();
    for (const QDir &root : roots) {
        for (const QFileInfo &entry :
             root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time)) {
            const QString id = entry.fileName();
            // 只碰本模块自己的命名；别的目录（包括用户自己建的）一律不碰。
            if (!validId(id)) {
                continue;
            }
            if (QFileInfo::exists(reportPath(dataRoot, id))) {
                continue;  // 还有报告在用这份暂存
            }
            const QDateTime modified = entry.lastModified();
            if (modified.isValid() && modified.secsTo(now) < minAgeSeconds) {
                continue;  // 可能另一份会话正在写
            }
            if (removeTreeWithoutFollowingLinks(entry.absoluteFilePath())
                && !removed.contains(id)) {
                removed.append(id);
            }
        }
    }
    return removed;
}

}  // namespace OperationReport
