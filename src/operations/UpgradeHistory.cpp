/**
 * @file UpgradeHistory.cpp
 * @brief 见头文件：每个实例一份的升级历史（追加、限量、原子写、读不懂不覆盖）。
 */
#include "UpgradeHistory.hpp"
#include <QCoreApplication>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace UpgradeHistory {
namespace {

QString keyOf(const char *key) { return QString::fromLatin1(key); }

QJsonObject toJson(const Entry &entry) {
    QJsonObject out;
    out.insert(keyOf("transactionId"), entry.transactionId);
    out.insert(keyOf("sourceId"), entry.sourceId);
    out.insert(keyOf("sourceName"), entry.sourceName);
    out.insert(keyOf("targetId"), entry.targetId);
    out.insert(keyOf("targetName"), entry.targetName);
    out.insert(keyOf("fromGameVersion"), entry.fromGameVersion);
    out.insert(keyOf("toGameVersion"), entry.toGameVersion);
    out.insert(keyOf("fromLoader"), entry.fromLoader);
    out.insert(keyOf("toLoader"), entry.toLoader);
    out.insert(keyOf("startedAt"), entry.startedAt);
    out.insert(keyOf("finishedAt"), entry.finishedAt);
    out.insert(keyOf("ok"), entry.ok);
    out.insert(keyOf("cancelled"), entry.cancelled);
    if (!entry.problem.isEmpty()) {
        out.insert(keyOf("problem"), entry.problem);
    }
    out.insert(keyOf("categories"), QJsonArray::fromStringList(entry.categories));
    out.insert(keyOf("copiedFiles"), entry.copiedFiles);
    out.insert(keyOf("skippedFiles"), entry.skippedFiles);
    out.insert(keyOf("failedFiles"), entry.failedFiles);
    if (!entry.snapshotName.isEmpty()) {
        out.insert(keyOf("snapshotName"), entry.snapshotName);
    }
    out.insert(keyOf("rollbackAvailable"), entry.rollbackAvailable);
    return out;
}

Entry fromJson(const QJsonObject &object) {
    Entry entry;
    entry.transactionId = object.value(keyOf("transactionId")).toString();
    entry.sourceId = object.value(keyOf("sourceId")).toString();
    entry.sourceName = object.value(keyOf("sourceName")).toString();
    entry.targetId = object.value(keyOf("targetId")).toString();
    entry.targetName = object.value(keyOf("targetName")).toString();
    entry.fromGameVersion = object.value(keyOf("fromGameVersion")).toString();
    entry.toGameVersion = object.value(keyOf("toGameVersion")).toString();
    entry.fromLoader = object.value(keyOf("fromLoader")).toString();
    entry.toLoader = object.value(keyOf("toLoader")).toString();
    entry.startedAt = object.value(keyOf("startedAt")).toString();
    entry.finishedAt = object.value(keyOf("finishedAt")).toString();
    entry.ok = object.value(keyOf("ok")).toBool(false);
    entry.cancelled = object.value(keyOf("cancelled")).toBool(false);
    entry.problem = object.value(keyOf("problem")).toString();
    const QJsonArray categories = object.value(keyOf("categories")).toArray();
    for (const QJsonValue &value : categories) {
        entry.categories.append(value.toString());
    }
    entry.copiedFiles = object.value(keyOf("copiedFiles")).toInt();
    entry.skippedFiles = object.value(keyOf("skippedFiles")).toInt();
    entry.failedFiles = object.value(keyOf("failedFiles")).toInt();
    entry.snapshotName = object.value(keyOf("snapshotName")).toString();
    entry.rollbackAvailable = object.value(keyOf("rollbackAvailable")).toBool(false);
    return entry;
}

}  // namespace

QString historyPath(const QString &instanceDir) {
    return QDir(instanceDir).filePath(QStringLiteral("wtfml.upgrade-history.json"));
}

QList<Entry> read(const QString &instanceDir, QString *problem) {
    if (problem != nullptr) {
        problem->clear();
    }
    QFile file(historyPath(instanceDir));
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("UpgradeHistory", "无法读取升级历史：%1").arg(file.errorString());
        }
        return {};
    }
    if (file.size() > 4 * 1024 * 1024) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("UpgradeHistory", "升级历史文件过大，无法作为记录解析。");
        }
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("UpgradeHistory", "升级历史无法解析（不是 JSON 对象）。");
        }
        return {};
    }
    const QJsonArray entries = document.object().value(keyOf("entries")).toArray();
    QList<Entry> out;
    for (const QJsonValue &value : entries) {
        if (value.isObject()) {
            out.append(fromJson(value.toObject()));
        }
    }
    return out;
}

bool append(const QString &instanceDir, const Entry &entry, QString *problem) {
    if (problem != nullptr) {
        problem->clear();
    }
    QList<Entry> existing;
    QJsonObject root;
    if (QFile::exists(historyPath(instanceDir))) {
        QString readProblem;
        existing = read(instanceDir, &readProblem);
        if (!readProblem.isEmpty()) {
            if (problem != nullptr) {
                *problem = readProblem + QCoreApplication::translate("UpgradeHistory", "，此条未写入。");
            }
            return false;
        }
        // 盘上那份的**其它键**原样留着（以后加了新字段，老版本写回时不该把它抹掉）。
        QFile raw(historyPath(instanceDir));
        if (raw.open(QIODevice::ReadOnly) && raw.size() <= 4 * 1024 * 1024) {
            root = QJsonDocument::fromJson(raw.readAll()).object();
        }
    }
    existing.prepend(entry);
    while (existing.size() > kMaxEntries) {
        existing.removeLast();
    }
    QJsonArray array;
    for (const Entry &one : existing) {
        array.append(toJson(one));
    }
    root.insert(keyOf("schema"), root.value(keyOf("schema")).toInt(1));
    root.insert(keyOf("entries"), array);
    QSaveFile file(historyPath(instanceDir));
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("UpgradeHistory", "无法保存升级历史：%1").arg(file.errorString());
        }
        return false;
    }
    return true;
}

}  // namespace UpgradeHistory


