#pragma once

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include "util/WriteDirectoryGuard.hpp"
#include <QSet>
#include <QStringList>
#include <QUuid>

// 只修改 options.txt 最后一条 resourcePacks 的值，其余字节（包括未知键、BOM、行尾）原样保留。
namespace ResourcePackOptions {
inline constexpr qint64 byteLimit = 1024 * 1024;
inline bool isLocal(const QString &id) { return id.startsWith(QStringLiteral("file/")); }
inline bool safeLocal(const QString &id) {
    const QString leaf = id.mid(5);
    return isLocal(id) && !leaf.isEmpty() && leaf != QStringLiteral(".") && leaf != QStringLiteral("..")
        && !leaf.contains(QLatin1Char('/')) && !leaf.contains(QLatin1Char('\\'))
        && !leaf.contains(QChar::Null) && !leaf.contains(QLatin1Char(':'));
}
struct Document {
    QByteArray bytes;
    QStringList selected; // options.txt 顺序，低优先级在前。
    QString problem;
    qsizetype valueOffset = -1;
    qsizetype valueLength = 0;
};
inline Document parse(const QByteArray &bytes) {
    Document out;
    out.bytes = bytes;
    if (bytes.size() > byteLimit) { out.problem = QCoreApplication::translate("ResourcePackOptions", "options.txt 超过 1 MiB，未开放编辑。"); return out; }
    for (qsizetype start = 0; start < bytes.size();) {
        const qsizetype end = bytes.indexOf('\n', start);
        qsizetype contentEnd = end < 0 ? bytes.size() : end;
        if (contentEnd > start && bytes.at(contentEnd - 1) == '\r') --contentEnd;
        const qsizetype bodyStart = start == 0 && bytes.startsWith("\xef\xbb\xbf") ? 3 : start;
        if (bytes.mid(bodyStart, 14) == "resourcePacks:") {
            out.valueOffset = bodyStart + 14;
            out.valueLength = contentEnd - out.valueOffset;
        }
        if (end < 0) break;
        start = end + 1;
    }
    if (out.valueOffset < 0) return out;
    QJsonParseError error;
    const QJsonDocument json = QJsonDocument::fromJson(bytes.mid(out.valueOffset, out.valueLength), &error);
    if (error.error != QJsonParseError::NoError || !json.isArray()) {
        out.problem = QCoreApplication::translate("ResourcePackOptions", "resourcePacks 不是有效的 JSON 数组，未改写原文件。"); return out;
    }
    QSet<QString> seen;
    for (const auto &value : json.array()) {
        if (!value.isString() || value.toString().isEmpty() || seen.contains(value.toString())) {
            out.problem = QCoreApplication::translate("ResourcePackOptions", "资源包列表包含空值、重复项或非文本项，未开放编辑。"); return out;
        }
        const QString id = value.toString();
        if (isLocal(id) && !safeLocal(id)) {
            out.problem = QCoreApplication::translate("ResourcePackOptions", "资源包标识包含不支持的路径，未开放编辑。"); return out;
        }
        seen.insert(id);
        out.selected.append(id);
    }
    return out;
}
inline QStringList builtins(const QStringList &ids) {
    QStringList result;
    for (const auto &id : ids) if (!isLocal(id)) result.append(id);
    return result;
}
inline bool rewrite(const Document &document, const QStringList &ids, QByteArray *output, QString *problem) {
    if (problem) problem->clear();
    const auto fail = [problem](const QString &why) { if (problem) *problem = why; return false; };
    if (!document.problem.isEmpty()) return fail(document.problem);
    if (builtins(document.selected) != builtins(ids))
        return fail(QCoreApplication::translate("ResourcePackOptions", "内置或模组注入资源包必须保留，且不能改变它们彼此的顺序。"));
    QSet<QString> seen;
    QJsonArray array;
    for (const auto &id : ids) {
        if (id.isEmpty() || seen.contains(id) || (isLocal(id) && !safeLocal(id)))
            return fail(QCoreApplication::translate("ResourcePackOptions", "资源包名单包含无效标识或重复项。"));
        seen.insert(id);
        array.append(id);
    }
    if (ids == document.selected) { if (output) *output = document.bytes; return true; }
    const QByteArray value = QJsonDocument(array).toJson(QJsonDocument::Compact);
    QByteArray bytes = document.bytes;
    if (document.valueOffset >= 0) bytes.replace(document.valueOffset, document.valueLength, value);
    else {
        const QByteArray eol = bytes.contains("\r\n") ? "\r\n" : "\n";
        if (!bytes.isEmpty() && !bytes.endsWith('\n')) bytes += eol;
        bytes += "resourcePacks:" + value + eol;
    }
    if (output) *output = bytes;
    if (problem) problem->clear();
    return true;
}
struct Snapshot {
    Document document;
    bool exists = false;
    QString problem;
};
inline Snapshot read(const QString &path) {
    Snapshot out;
    const QFileInfo info(path);
    out.exists = info.exists();
    if (info.isSymLink() || (out.exists && !info.isFile())) {
        out.problem = QCoreApplication::translate("ResourcePackOptions", "options.txt 不是普通文件，未开放编辑。"); return out;
    }
    if (!out.exists) return out;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { out.problem = file.errorString(); return out; }
    const QByteArray bytes = file.read(byteLimit + 1);
    if (file.error() != QFileDevice::NoError) { out.problem = file.errorString(); return out; }
    out.document = parse(bytes);
    out.problem = out.document.problem;
    return out;
}
// available 是本次重新扫描得到的文件标识；缺失的旧条目可保留，但不能添加不存在的新条目。
inline bool save(const QString &path, const QByteArray &expected, bool expectedExists,
                 const QStringList &ids, const QSet<QString> &available,
                 const QString &backupRoot, QString *problem) {
    if (!WriteDirectoryGuard::acquire(path, problem)) return false;
    if (problem) problem->clear();
    const auto fail = [problem](const QString &why) { if (problem) *problem = why; return false; };
    QLockFile lock(path + QStringLiteral(".wtfml.lock"));
    if (!lock.tryLock(0)) return fail(QCoreApplication::translate("ResourcePackOptions", "无法锁定 options.txt：可能有其他操作正在使用，或目录不可写。"));
    const Snapshot current = read(path);
    if (!current.problem.isEmpty()) return fail(current.problem);
    if (current.exists != expectedExists || current.document.bytes != expected)
        return fail(QCoreApplication::translate("ResourcePackOptions", "options.txt 已被外部修改；本次未覆盖，请刷新后重新操作。"));
    for (const auto &id : ids)
        if (isLocal(id) && !current.document.selected.contains(id) && !available.contains(id))
            return fail(QCoreApplication::translate("ResourcePackOptions", "待启用的资源包已不存在，请刷新后重试。"));
    QByteArray bytes;
    if (!rewrite(current.document, ids, &bytes, problem)) return false;
    if (bytes == current.document.bytes) return true;
    if (current.exists) {
        if (!QDir().mkpath(backupRoot)) return fail(QCoreApplication::translate("ResourcePackOptions", "无法创建资源包设置备份目录，未保存。"));
        const QString name = QStringLiteral("options-%1-%2.txt")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmsszzz")),
                 QUuid::createUuid().toString(QUuid::Id128));
        QSaveFile backup(QDir(backupRoot).filePath(name));
        if (!backup.open(QIODevice::WriteOnly) || backup.write(expected) != expected.size() || !backup.commit())
            return fail(QCoreApplication::translate("ResourcePackOptions", "无法完整保存原 options.txt 的备份，未修改原文件。"));
    }
    // 写盘前再核对一次，避免扫描、备份期间其他程序改动文件。
    const Snapshot latest = read(path);
    if (!latest.problem.isEmpty() || latest.exists != current.exists || latest.document.bytes != expected)
        return fail(QCoreApplication::translate("ResourcePackOptions", "保存期间 options.txt 发生变化，未覆盖原文件。"));
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        return fail(QCoreApplication::translate("ResourcePackOptions", "资源包设置保存失败，原文件未被完整替换。"));
    if (problem) problem->clear();
    return true;
}
} // namespace ResourcePackOptions
