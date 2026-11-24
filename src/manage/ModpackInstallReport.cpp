#include "ModpackInstallReport.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>

namespace ModpackInstallReport {
namespace {
bool safeTarget(const QString &root, const QString &relative) {
    if (!QFileInfo(root).isDir() || !ModpackImport::isSafeRelativePath(relative)) return false;
    QString path = QDir(root).absolutePath();
    // 检查根目录的祖先，不能经由上层 junction 绕到实例以外。
    for (QString ancestor = path;;) {
        const QFileInfo info(ancestor);
        if (info.isSymLink() || info.isJunction()) return false;
        const QString parent = info.absolutePath();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    for (const auto &segment : relative.split(QLatin1Char('/'))) {
        path += QLatin1Char('/') + segment;
        if (QFileInfo(path).isSymLink() || QFileInfo(path).isJunction()) return false;
    }
    return true;
}
bool verified(const ModpackImport::FileIssue &issue, const QString &root,
              const std::function<bool()> &cancelled = {}) {
    static const QRegularExpression hash(QStringLiteral("^[0-9a-fA-F]{40}$"));
    if (!ModpackImport::isSafeRelativePath(issue.path) || issue.size < 0 ||
        !hash.match(issue.sha1).hasMatch() || !safeTarget(root, issue.path)) return false;
    const QString path = QDir(root).filePath(issue.path);
    if (!QFileInfo(path).isFile() || QFileInfo(path).size() != issue.size) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash digest(QCryptographicHash::Sha1);
    qint64 total = 0;
    while (!file.atEnd()) {
        if (cancelled && cancelled()) return false;
        const auto bytes = file.read(1 << 20);
        if (bytes.isEmpty()) return false;
        total += bytes.size();
        if (total > issue.size) return false;
        digest.addData(bytes);
    }
    return file.error() == QFileDevice::NoError && total == issue.size
        && !(cancelled && cancelled())
        && digest.result().toHex().compare(issue.sha1.toLatin1(), Qt::CaseInsensitive) == 0;
}
QString page(const ModpackImport::FileIssue &issue) {
    return ModpackImport::curseForgeFileWebUrl(issue);
}
}
bool read(const QString &instanceDir, Report *report, QString *problem) {
    if (report) *report = {};
    if (problem) problem->clear();
    auto fail = [&](const QString &text) { if (problem) *problem = text; return false; };
    if (!report || !safeTarget(instanceDir, QStringLiteral("wtfml.import-report.json")))
        return fail(QObject::tr("报告路径不安全"));
    QFile file(QDir(instanceDir).filePath(QStringLiteral("wtfml.import-report.json")));
    constexpr qint64 limit = 4 * 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
        return fail(QObject::tr("无法读取报告，或报告超过 4 MiB"));
    const QByteArray bytes = file.read(limit + 1);
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (file.error() != QFileDevice::NoError || bytes.size() > limit ||
        error.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QObject::tr("报告内容损坏"));
    const auto obj = doc.object();
    if (obj.value(QStringLiteral("schemaVersion")) != QJsonValue(1) ||
        !obj.value(QStringLiteral("packName")).isString() ||
        !obj.value(QStringLiteral("complete")).isBool() ||
        !obj.value(QStringLiteral("issues")).isArray())
        return fail(QObject::tr("不支持的报告格式"));
    Report parsed;
    parsed.packName = obj.value(QStringLiteral("packName")).toString();
    parsed.failureReason = obj.value(QStringLiteral("failureReason")).toString();
    parsed.complete = obj.value(QStringLiteral("complete")).toBool();
    const auto entries = obj.value(QStringLiteral("issues")).toArray();
    if (entries.size() > 10000) return fail(QObject::tr("报告条目过多"));
    for (const auto &value : entries) {
        if (!value.isObject()) return fail(QObject::tr("缺项条目损坏"));
        const auto entry = value.toObject();
        ModpackImport::FileIssue issue;
        for (const auto &key : {"fileName", "path", "sha1", "size", "reason"})
            if (!entry.value(QLatin1String(key)).isString())
                return fail(QObject::tr("缺项字段类型错误"));
        for (const auto &key : {"projectId", "fileId"}) {
            const auto id = entry.value(QLatin1String(key));
            if (!id.isDouble() || id.toDouble() < 0 ||
                id.toDouble() != static_cast<double>(id.toInt(-1)))
                return fail(QObject::tr("缺项编号错误"));
        }
        issue.ref = {entry.value(QStringLiteral("projectId")).toInt(), entry.value(QStringLiteral("fileId")).toInt()};
        issue.fileName = entry.value(QStringLiteral("fileName")).toString();
        issue.path = entry.value(QStringLiteral("path")).toString();
        issue.sha1 = entry.value(QStringLiteral("sha1")).toString();
        issue.reason = entry.value(QStringLiteral("reason")).toString();
        issue.projectWebUrl = entry.value(QStringLiteral("projectWebUrl")).toString();
        if (page(issue).isEmpty()) issue.projectWebUrl.clear();
        bool sizeOk = false;
        issue.size = entry.value(QStringLiteral("size")).toString().toLongLong(&sizeOk);
        static const QRegularExpression hash(QStringLiteral("^[0-9a-fA-F]{40}$"));
        if (!sizeOk || issue.size < -1 ||
            (!issue.path.isEmpty() && !ModpackImport::isSafeRelativePath(issue.path)) ||
            (!issue.sha1.isEmpty() && !hash.match(issue.sha1).hasMatch()))
            return fail(QObject::tr("缺项路径或校验信息错误"));
        parsed.issues.append(issue);
    }
    if (parsed.complete && !parsed.issues.isEmpty()) return fail(QObject::tr("报告完成状态与缺项冲突"));
    *report = parsed;
    return true;
}
bool supply(const QString &instanceDir, const ModpackImport::FileIssue &issue,
            const QString &source, QString *problem, const std::function<bool()> &cancelled) {
    if (problem) problem->clear();
    auto fail = [&](const QString &text) { if (problem) *problem = text; return false; };
    static const QRegularExpression hash(QStringLiteral("^[0-9a-fA-F]{40}$"));
    if (issue.size < 0 || !hash.match(issue.sha1).hasMatch() || !safeTarget(instanceDir, issue.path))
        return fail(QObject::tr("缺少可信路径、大小或 SHA-1，不能自动补入"));
    QFile input(source);
    if (!QFileInfo(source).isFile() || !input.open(QIODevice::ReadOnly) || input.size() != issue.size)
        return fail(QObject::tr("所选文件无法读取或大小不匹配"));
    if (cancelled && cancelled()) return fail(QObject::tr("操作已取消或超时"));
    const QString target = QDir(instanceDir).filePath(issue.path);
    // 不隐式覆盖用户已有的同名文件；本来就在正确位置的文件只作校验。
    const bool exists = QFileInfo::exists(target);
    QSaveFile output(target);
    if (!exists) {
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !safeTarget(instanceDir, issue.path) ||
            !output.open(QIODevice::WriteOnly)) return fail(QObject::tr("无法创建补齐文件"));
    }
    QCryptographicHash digest(QCryptographicHash::Sha1);
    qint64 total = 0;
    while (!input.atEnd()) {
        if (cancelled && cancelled()) return fail(QObject::tr("操作已取消或超时"));
        const auto chunk = input.read(1 << 20);
        if (chunk.isEmpty() || (!exists && output.write(chunk) != chunk.size()))
            return fail(QObject::tr("读取或写入补齐文件失败"));
        total += chunk.size();
        if (total > issue.size) return fail(QObject::tr("源文件在读取期间发生变化"));
        digest.addData(chunk);
    }
    if (input.error() != QFileDevice::NoError || total != issue.size ||
        digest.result().toHex().compare(issue.sha1.toLatin1(), Qt::CaseInsensitive) != 0)
        return fail(QObject::tr("SHA-1 校验不匹配，没有部署文件"));
    if (cancelled && cancelled()) return fail(QObject::tr("操作已取消或超时"));
    if (exists) return QFileInfo(source).canonicalFilePath() == QFileInfo(target).canonicalFilePath()
        || fail(QObject::tr("目标已存在，请直接选择目标文件进行校验；没有覆盖任何内容"));
    if (!safeTarget(instanceDir, issue.path) || QFileInfo::exists(target) || !output.commit())
        return fail(QObject::tr("目标发生变化或文件发布失败，未覆盖"));
    return true;
}
BatchResult supplyMatching(const QString &instanceDir,
        const QList<ModpackImport::FileIssue> &issues, const QStringList &sources,
        const std::function<bool()> &cancelled) {
    BatchResult result;
    for (const auto &source : sources) {
        if (cancelled && cancelled()) { result.cancelled = true; break; }
        QFile file(source);
        const QString name = QFileInfo(source).fileName();
        if (!QFileInfo(source).isFile() || !file.open(QIODevice::ReadOnly)) {
            result.problems.append(QObject::tr("%1：无法读取").arg(name)); continue;
        }
        const qint64 size = file.size();
        bool candidate = false;
        for (const auto &issue : issues) if (issue.size == size && !issue.sha1.isEmpty()) candidate = true;
        if (!candidate) { result.problems.append(QObject::tr("%1：没有大小匹配的缺项").arg(name)); continue; }
        QCryptographicHash hash(QCryptographicHash::Sha1);
        qint64 total = 0;
        while (!file.atEnd()) {
            if (cancelled && cancelled()) { result.cancelled = true; return result; }
            const auto bytes = file.read(1 << 20);
            if (bytes.isEmpty()) break;
            total += bytes.size(); hash.addData(bytes);
            if (total > size) break;
        }
        if (file.error() != QFileDevice::NoError || total != size) {
            result.problems.append(QObject::tr("%1：读取失败或源文件发生变化").arg(name)); continue;
        }
        const QString digest = QString::fromLatin1(hash.result().toHex());
        file.close();
        bool matched = false;
        for (const auto &issue : issues) {
            if (issue.size != size || digest.compare(issue.sha1, Qt::CaseInsensitive) != 0) continue;
            matched = true;
            if (result.suppliedPaths.contains(issue.path)) continue;
            QString error;
            if (supply(instanceDir, issue, source, &error, cancelled)) result.suppliedPaths.append(issue.path);
            else result.problems.append(QObject::tr("%1 → %2：%3").arg(name, issue.path, error));
            if (cancelled && cancelled()) { result.cancelled = true; return result; }
        }
        if (!matched) result.problems.append(QObject::tr("%1：没有 SHA-1 匹配的缺项，未写入").arg(name));
    }
    return result;
}
BatchResult verifyExisting(const QString &instanceDir,
        const QList<ModpackImport::FileIssue> &issues, const std::function<bool()> &cancelled) {
    BatchResult result;
    for (const auto &issue : issues) {
        if (cancelled && cancelled()) { result.cancelled = true; break; }
        const bool ok = verified(issue, instanceDir, cancelled);
        if (cancelled && cancelled()) { result.cancelled = true; break; }
        if (ok) result.suppliedPaths.append(issue.path);
        else result.problems.append(QObject::tr("%1：文件缺失、内容不符或缺少可信校验信息")
            .arg(issue.path.isEmpty() ? issue.fileName : issue.path));
    }
    return result;
}
QList<ModpackImport::FileIssue> remaining(
        const QList<ModpackImport::FileIssue> &issues, const QString &instanceDir) {
    QList<ModpackImport::FileIssue> out;
    for (const auto &issue : issues) if (!verified(issue, instanceDir)) out.append(issue);
    return out;
}
QString description(const ModpackImport::FileIssue &issue) {
    if (issue.ref.projectId <= 0 || issue.ref.fileId <= 0)
        return QStringLiteral("%1\n%2\n%3").arg(issue.fileName, issue.reason, issue.path);
    const auto url = page(issue);
    return QCoreApplication::translate("ModpackInstallReport", "%1\n项目 %2 / 文件 %3 / %4\n%5%6")
        .arg(issue.fileName.isEmpty() ? QObject::tr("文件名未知") : issue.fileName)
        .arg(issue.ref.projectId).arg(issue.ref.fileId).arg(issue.reason,
            url.isEmpty() ? QObject::tr("未取得规范网页地址；请按编号手动查找（不代表文件下架）") : url,
            issue.path.isEmpty() ? QString() : QObject::tr("\n补齐位置：%1").arg(issue.path));
}
bool write(const QString &instanceDir, const ModpackImport::Pack &pack,
           const QList<ModpackImport::FileIssue> &issues, bool complete, QString *problem,
           const QString &failureReason) {
    const QString path = QDir(instanceDir).filePath(QStringLiteral("wtfml.import-report.json"));
    if (!safeTarget(instanceDir, QStringLiteral("wtfml.import-report.json"))) {
        if (problem) *problem = QObject::tr("导入报告目录不存在或路径含链接，未写入");
        return false;
    }
    QJsonArray entries;
    for (const auto &issue : issues) entries.append(QJsonObject{
        {QStringLiteral("projectId"), issue.ref.projectId}, {QStringLiteral("fileId"), issue.ref.fileId},
        {QStringLiteral("fileName"), issue.fileName}, {QStringLiteral("path"), issue.path},
        {QStringLiteral("sha1"), issue.sha1}, {QStringLiteral("size"), QString::number(issue.size)},
        {QStringLiteral("reason"), issue.reason}, {QStringLiteral("webUrl"), page(issue)},
        {QStringLiteral("projectWebUrl"), page(issue).isEmpty() ? QString() : issue.projectWebUrl}});
    const QByteArray data = QJsonDocument(QJsonObject{
        {QStringLiteral("schemaVersion"), 1}, {QStringLiteral("packName"), pack.name},
        {QStringLiteral("complete"), complete}, {QStringLiteral("issues"), entries},
        {QStringLiteral("failureReason"), failureReason},
        {QStringLiteral("updatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}).toJson();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (problem) *problem = file.errorString();
        return false;
    }
    return true;
}
}
