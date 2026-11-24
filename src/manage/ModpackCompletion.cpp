#include "ModpackCompletion.hpp"
#include "InstanceMetaFile.hpp"
#include "ModpackInstallReport.hpp"
#include "InstanceFiles.hpp"
#include "util/AppPaths.hpp"
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

namespace ModpackCompletion {
namespace {
const QString proofName = QStringLiteral("wtfml.import-ready.json");
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
bool safe(const QString &root, const QString &relative) {
    if (!ModpackImport::isSafeRelativePath(relative)) return false;
    QString path = QDir(root).absolutePath();
    for (QString ancestor = path;;) {
        const QFileInfo info(ancestor);
        if (info.isSymLink() || info.isJunction()) return false;
        const QString parent = info.absolutePath();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    for (const auto &part : relative.split('/')) {
        path += QLatin1Char('/') + part;
        const QFileInfo info(path);
        if (info.isSymLink() || info.isJunction()) return false;
    }
    return true;
}
QByteArray bounded(const QString &path, qint64 limit = 16 * 1024 * 1024) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) return {};
    const auto bytes = file.read(limit + 1);
    return file.error() == QFileDevice::NoError && bytes.size() <= limit ? bytes : QByteArray();
}
QString digest(const QString &path, const Stop &stop) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha1);
    while (!file.atEnd()) {
        if (stop && stop()) return {};
        const auto bytes = file.read(1 << 20);
        if (bytes.isEmpty()) return {};
        hash.addData(bytes);
    }
    return file.error() == QFileDevice::NoError && !(stop && stop())
        ? QString::fromLatin1(hash.result().toHex()) : QString();
}
bool save(const QString &path, const QJsonObject &object) {
    const auto bytes = QJsonDocument(object).toJson();
    if (bytes.size() > 16 * 1024 * 1024) return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
bool markerMatches(const QString &instance, const QJsonObject &proof) {
    const auto marker = bounded(instance + QStringLiteral("/wtfml.importing"), 65536);
    return !marker.isEmpty() && proof.value("markerSha256").toString() ==
        QString::fromLatin1(QCryptographicHash::hash(marker, QCryptographicHash::Sha256).toHex()) &&
        QJsonDocument::fromJson(marker).object().value("manifestSha256") == proof.value("manifestSha256");
}
}
bool capture(const QString &instance, const QString &dataRoot, const ModpackImport::Pack &pack,
             const QString &fingerprint, const QList<VersionIntegrity::ExpectedFile> &core,
             QString *error, const Stop &stop) {
    if (error) error->clear();
    const auto abort = [&](const QString &message) { return fail(error, message); };
    const QString id = QFileInfo(instance).fileName();
    if (InstanceFiles::canonicalPath(instance) != InstanceFiles::canonicalPath(QDir(dataRoot).filePath("versions/" + id)) ||
        !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{64}$")).match(fingerprint).hasMatch() ||
        !safe(instance, proofName) || pack.filesNotDistributable + pack.filesMissing > pack.fileIssues.size())
        return abort(QObject::tr("无法建立与当前实例和完整文件清单绑定的安装凭据"));
    const auto marker = bounded(instance + QStringLiteral("/wtfml.importing"), 65536);
    if (QJsonDocument::fromJson(marker).object().value("manifestSha256").toString() != fingerprint)
        return abort(QObject::tr("导入标记与当前清单不一致"));
    QJsonArray files;
    QSet<QString> seen;
    QSet<QString> repairPaths;
    for (const auto &issue : pack.fileIssues) repairPaths.insert(issue.path);
    auto add = [&](const QString &root, const QString &relative, const QString &expectedHash, qint64 expectedSize) {
        if (stop && stop()) return false;
        if (!safe(root, relative)) return false;
        const QString path = QDir(root).filePath(relative);
        const QFileInfo info(path);
        if (!info.isFile() || (expectedSize >= 0 && info.size() != expectedSize)) return false;
        const auto sha1 = digest(path, stop);
        if (sha1.isEmpty() || (!expectedHash.isEmpty() && sha1.compare(expectedHash, Qt::CaseInsensitive) != 0)) return false;
        const QString key = InstanceFiles::canonicalPath(path);
        if (seen.contains(key)) return true;
        seen.insert(key);
        files.append(QJsonObject{{"scope", root == instance ? "instance" : "data"},
            {"path", relative}, {"size", QString::number(info.size())}, {"sha1", sha1}});
        return files.size() <= 100000;
    };
    for (const auto &file : core) {
        const auto relative = QDir(dataRoot).relativeFilePath(file.path);
        if (!add(dataRoot, relative, file.sha1, file.size > 0 ? file.size : -1))
            return abort(QObject::tr("核心文件校验失败或已取消：%1").arg(file.label));
    }
    if (!add(dataRoot, QDir(dataRoot).relativeFilePath(InstanceFiles::jarPath(id)), {}, -1))
        return abort(QObject::tr("客户端 JAR 缺失或校验取消"));
    // 将加载器与所有父版本 JSON 也锁进凭据，不能仅校验合并后的库。
    QString current = id;
    QSet<QString> parents;
    while (!current.isEmpty()) {
        if (!InstanceFiles::validatePathSegment(current).isEmpty() || parents.contains(current) || parents.size() >= 32)
            return abort(QObject::tr("版本继承链不安全"));
        parents.insert(current);
        const auto resolved = InstanceFiles::resolveInstance(current);
        const QString path = "versions/" + current + '/' + resolved.jsonName;
        if (resolved.jsonName.isEmpty() || !add(dataRoot, path, {}, -1))
            return abort(QObject::tr("版本或加载器 JSON 缺失"));
        current = QJsonDocument::fromJson(bounded(QDir(dataRoot).filePath(path))).object().value("inheritsFrom").toString();
    }
    QDirIterator it(instance, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (stop && stop()) return abort(QObject::tr("校验已取消或超时"));
        const auto path = it.next();
        const auto info = it.fileInfo();
        const auto relative = QDir(instance).relativeFilePath(path);
        if (!safe(instance, relative)) return abort(QObject::tr("实例包含链接或不安全路径"));
        if (info.isDir()) continue;
        if (relative == proofName || relative == "wtfml.importing" || relative == "wtfml.meta" ||
            relative == "wtfml.import-report.json") continue;
        // 补齐项允许从错误/缺失内容变成声明的内容；它们由强校验清单约束，不冻结旧字节。
        if (repairPaths.contains(relative)) continue;
        if (!add(instance, relative, {}, -1)) return abort(QObject::tr("覆盖文件校验失败：%1").arg(relative));
    }
    if (files.isEmpty() || (stop && stop())) return abort(QObject::tr("没有可验证文件或已取消"));
    // 缺项必须完整保存；不能从“剩余零条”推断此前每一步已经完成。
    QJsonArray issues;
    for (const auto &issue : pack.fileIssues) issues.append(QJsonObject{
        {"projectId", issue.ref.projectId}, {"fileId", issue.ref.fileId}, {"fileName", issue.fileName},
        {"path", issue.path}, {"sha1", issue.sha1}, {"size", QString::number(issue.size)},
        {"reason", issue.reason}, {"projectWebUrl", issue.projectWebUrl}});
    QJsonObject proof{{"schemaVersion", 1}, {"instance", InstanceFiles::canonicalPath(instance)},
        {"dataRoot", InstanceFiles::canonicalPath(dataRoot)}, {"manifestSha256", fingerprint},
        {"markerSha256", QString::fromLatin1(QCryptographicHash::hash(marker, QCryptographicHash::Sha256).toHex())},
        {"packName", pack.name}, {"gameVersion", pack.gameVersion}, {"loaderKey", pack.loaderKey},
        {"loaderVersion", pack.loaderVersion}, {"files", files}, {"issues", issues}};
    if (!markerMatches(instance, proof) || !save(instance + '/' + proofName, proof))
        return abort(QObject::tr("无法保存安装凭据，本次没有安装"));
    return true;
}

bool finish(const QString &instance, QString *error, const Stop &stop) {
    if (error) error->clear();
    const auto failFast = [&](const QString &message) { return fail(error, message); };
    if (!safe(instance, proofName)) return failFast(QObject::tr("安装凭据的保存路径不安全"));
    const auto proof = QJsonDocument::fromJson(bounded(instance + '/' + proofName)).object();
    const QString dataRoot = proof.value("dataRoot").toString();
    const auto id = QFileInfo(instance).fileName();
    if (proof.value("schemaVersion") != QJsonValue(1) ||
        proof.value("instance").toString() != InstanceFiles::canonicalPath(instance) ||
        dataRoot != InstanceFiles::canonicalPath(AppPaths::minecraftDir()) ||
        InstanceFiles::canonicalPath(instance) != InstanceFiles::canonicalPath(QDir(dataRoot).filePath("versions/" + id)) ||
        !markerMatches(instance, proof) || !proof.value("files").isArray() ||
        proof.value("files").toArray().isEmpty() || !proof.value("issues").isArray())
        return failFast(QObject::tr("缺少有效的安装凭据，或实例与文件清单已改变，请使用原包继续导入"));
    QList<ModpackImport::FileIssue> issues;
    for (const auto &value : proof.value("issues").toArray()) {
        const auto entry = value.toObject();
        ModpackImport::FileIssue issue;
        issue.ref = {entry.value("projectId").toInt(), entry.value("fileId").toInt()};
        issue.fileName = entry.value("fileName").toString(); issue.path = entry.value("path").toString();
        issue.sha1 = entry.value("sha1").toString(); issue.reason = entry.value("reason").toString();
        issue.projectWebUrl = entry.value("projectWebUrl").toString();
        bool ok = false; issue.size = entry.value("size").toString().toLongLong(&ok);
        if (!ok) return failFast(QObject::tr("补齐项的安装凭据已损坏"));
        issues.append(issue);
    }
    ModpackImport::Pack pack;
    pack.name = proof.value("packName").toString();
    // 任何一次提前退出都不能把盘上那份**完成报告**留在原处：这个函数的任务就是
    // “要么真正收尾（移除 importing 标记），要么让报告如实说没完成”。收尾中途取消
    // （包括在完成报告已发布之后才收到取消）都要把报告退回未完成；否则用户和补齐全流程
    // 都会看到一份 complete=true 却仍然无法启动的实例。
    const auto abort = [&](const QString &message) {
        const QString reportName = QStringLiteral("wtfml.import-report.json");
        if (safe(instance, reportName)
            && QFileInfo::exists(instance + QLatin1Char('/') + reportName)) {
            QString ignored;
            ModpackInstallReport::write(instance, pack, issues, false, &ignored, message);
        }
        return fail(error, message);
    };
    for (const auto &value : proof.value("files").toArray()) {
        const auto entry = value.toObject();
        const auto scope = entry.value("scope").toString();
        const auto relative = entry.value("path").toString();
        const auto root = scope == "instance" ? instance : dataRoot;
        const auto path = QDir(root).filePath(relative);
        bool sizeOk = false;
        const auto size = entry.value("size").toString().toLongLong(&sizeOk);
        const auto expected = entry.value("sha1").toString();
        if ((stop && stop()) || (scope != "instance" && scope != "data") || !safe(root, relative) ||
            !sizeOk || size < 0 || !QFileInfo(path).isFile() || QFileInfo(path).size() != size ||
            !QRegularExpression(QStringLiteral("^[a-f0-9]{40}$")).match(expected).hasMatch() ||
            digest(path, stop) != expected)
            return abort(QObject::tr("文件已改变、缺失或校验被取消：%1").arg(relative));
    }
    const auto verified = ModpackInstallReport::verifyExisting(instance, issues, stop);
    if (verified.cancelled || !verified.problems.isEmpty() || verified.suppliedPaths.size() != issues.size())
        return abort(QObject::tr("仍有缺项未补齐，或校验已取消：%1").arg(verified.problems.join('\n')));
    if ((stop && stop()) || !markerMatches(instance, proof)) return abort(QObject::tr("已取消或导入标记已改变"));
    if (!safe(instance, QStringLiteral("wtfml.meta")) || !safe(instance, QStringLiteral("wtfml.import-report.json")))
        return abort(QObject::tr("提交路径不安全"));
    QJsonObject metaFields;
    metaFields.insert(QStringLiteral("name"), id);
    metaFields.insert(QStringLiteral("realVersion"), proof.value("gameVersion"));
    const auto key = proof.value("loaderKey").toString();
    const QHash<QString, QString> names{{"fabric-loader", "Fabric"}, {"forge", "Forge"}, {"neoforge", "NeoForge"}, {"quilt-loader", "Quilt"}};
    if (!key.isEmpty()) {
        metaFields.insert(QStringLiteral("loaderName"), names.value(key, key));
        metaFields.insert(QStringLiteral("loaderVersion"), proof.value("loaderVersion"));
    }
    // 合并写的规矩只有一份（InstanceMetaFile）：读不懂 / 版本不认识 ⇒ 一个字节都不动。
    QString metaError;
    if (!InstanceMetaFile::mergeWrite(instance + QStringLiteral("/wtfml.meta"), metaFields, &metaError))
        return abort(QObject::tr("元数据写入失败，内容保留：%1").arg(metaError));
    if ((stop && stop()) || !markerMatches(instance, proof)) return abort(QObject::tr("已取消或导入标记改变，保留未完成状态"));
    if (!ModpackInstallReport::write(instance, pack, {}, true, error)) return false;
    // 报告写入也可能耗时；发布启动门禁前再检查取消和导入尝试绑定。
    if ((stop && stop()) || !markerMatches(instance, proof)) {
        return abort(QObject::tr("已取消或导入标记改变，保留未完成状态"));
    }
    if (!QFile::remove(instance + QStringLiteral("/wtfml.importing"))) {
        return abort(QObject::tr("无法移除未完成标记，请重试；文件均已保留"));
    }
    // 成功后凭据留作审计，但没有匹配的 importing 标记就不能重复提交。
    return true;
}
}
