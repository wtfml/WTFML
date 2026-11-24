#include "PresetExecutionReport.hpp"
#include <QCoreApplication>
#include "manage/InstanceFiles.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

namespace Presets {
namespace {
QString fingerprint(const PresetManifest &manifest) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(manifestToJson(manifest))
        .toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
}
QString presetExecutionReportPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("wtfml.preset-apply.json"));
}
QJsonObject readPresetExecutionReport(const QString &root, const PresetManifest &manifest, QString *problem) {
    const auto path = presetExecutionReportPath(root);
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
        if (problem) *problem = QCoreApplication::translate("PresetExecutionReport", "无可读取的预设执行报告，或报告过大；请重新预览。");
        return {};
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    const auto object = document.object();
    if (!document.isObject() || object.value("format").toString() != QStringLiteral("wtfml-preset-apply/1") ||
        object.value("presetId").toString() != manifest.id || object.value("manifestSha256").toString() != fingerprint(manifest) ||
        object.value("targetRoot").toString() != InstanceFiles::canonicalPath(root) ||
        !object.value("rows").isArray() || object.value("rows").toArray().size() > 10000) {
        if (problem) *problem = QCoreApplication::translate("PresetExecutionReport", "执行报告与当前预设 / 实例不符，或报告已损坏；请重新预览，未自动继续。");
        return {};
    }
    QSet<QString> expected, found;
    for (const auto &section : manifest.sections) expected.insert(section.id);
    const QSet<QString> states{QStringLiteral("planned"), QStringLiteral("done"), QStringLiteral("failed"), QStringLiteral("pending-world")};
    for (const auto &value : object.value("rows").toArray()) {
        const auto row = value.toObject(); const auto id = row.value("sectionId").toString();
        if (!value.isObject() || !expected.contains(id) || found.contains(id) ||
            !states.contains(row.value("state").toString())) {
            if (problem) *problem = QCoreApplication::translate("PresetExecutionReport", "执行报告的逐项状态无效，未自动继续。");
            return {};
        }
        found.insert(id);
    }
    if (found != expected) {
        if (problem) *problem = QCoreApplication::translate("PresetExecutionReport", "执行报告缺少预设段状态，未自动继续。");
        return {};
    }
    return object;
}
bool writePresetExecutionReport(const QString &root, const PresetManifest &manifest,
    const PresetApplyResult &result, bool finished, const QJsonObject &previous, QString *problem) {
    QHash<QString, QJsonObject> rows;
    for (const auto &value : previous.value("rows").toArray()) {
        const auto row = value.toObject(); rows.insert(row.value("sectionId").toString(), row);
    }
    for (const auto &row : result.rows) {
        // options/resource-pack activation is finalized later than the row's
        // initial calculation; a crash must never mark buffered work as done.
        const bool durable = finished || (!optionsHandlesSection(row.kind) && row.kind != SectionKind::GenericFiles);
        rows.insert(row.sectionId, QJsonObject{{"sectionId", row.sectionId}, {"title", row.title},
            {"state", row.blocked ? "failed" : !durable ? "planned" : row.deferred > 0 ? "pending-world"
                : row.ok ? "done" : "planned"},
            {"written", row.written}, {"removed", row.removed}, {"deferred", row.deferred},
            {"reason", row.problem}, {"details", QJsonArray::fromStringList(row.details)}});
    }
    QJsonArray execution;
    for (const auto &section : manifest.sections) execution.append(rows.value(section.id,
        QJsonObject{{"sectionId", section.id}, {"state", "planned"}, {"title", section.title}}));
    const auto object = QJsonObject{{"format", "wtfml-preset-apply/1"}, {"presetId", manifest.id},
        {"manifestSha256", fingerprint(manifest)}, {"targetRoot", InstanceFiles::canonicalPath(root)},
        {"phase", !finished ? "apply" : result.deferred > 0 ? "pending-world" : result.ok ? "complete" : "partial"},
        {"success", finished && result.ok && result.deferred == 0},
        {"rows", execution}, {"backupDir", result.backupDir.isEmpty() ? previous.value("backupDir").toString() : result.backupDir},
        {"problem", result.problem}, {"updatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    const auto path = presetExecutionReportPath(root);
    QSaveFile file(path);
    if (bytes.size() > 4 * 1024 * 1024 || QFileInfo(path).isSymLink() ||
        !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (problem) *problem = QCoreApplication::translate("PresetExecutionReport", "无法保存预设执行报告；已写入内容保留，请检查磁盘与权限。");
        return false;
    }
    return true;
}
}
