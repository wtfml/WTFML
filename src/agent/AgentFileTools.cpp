/**
 * @file AgentFileTools.cpp
 * @brief 受控文件读写实现（见 .hpp 与 docs/features/AGENT.md §9.3）。
 */

#include "AgentFileTools.hpp"

#include "AgentBackupIndex.hpp"
#include "AgentRedaction.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace {
QString sha256(const QByteArray &bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

Agent::Error makeError(Agent::ErrorKind kind, const QString &message) {
    Agent::Error error;
    error.kind = kind;
    error.message = message;
    return error;
}

QString lineDiff(const QString &oldText, const QString &newText) {
    if (oldText == newText) return {};
    QString diff = QStringLiteral("--- %1\n+++ %2\n")
                       .arg(QCoreApplication::translate("AgentFileTools", "当前内容"),
                            QCoreApplication::translate("AgentFileTools", "写入内容"));
    for (const QString &line : oldText.split(QLatin1Char('\n'))) diff += QStringLiteral("- ") + line + QLatin1Char('\n');
    for (const QString &line : newText.split(QLatin1Char('\n'))) diff += QStringLiteral("+ ") + line + QLatin1Char('\n');
    if (diff.size() > 20000) diff = diff.left(20000) + QCoreApplication::translate("AgentFileTools", "\n…（diff 已截断）");
    return diff;
}
}  // namespace

AgentFileTools::AgentFileTools(AgentBackupIndex *backups) : m_backups(backups) {}

int AgentFileTools::maxReadBytes() { return 1024 * 1024; }
int AgentFileTools::maxInlineBytes() { return 256 * 1024; }
int AgentFileTools::maxListEntries() { return 2000; }

bool AgentFileTools::isConfigWhitelist(const QString &relativePath) {
    QString normalized;
    if (!AgentWinPath::normalizeRelative(relativePath, &normalized, nullptr)) return false;
    return normalized == QStringLiteral("options.txt") ||
           normalized.startsWith(QStringLiteral("config/")) ||
           normalized.startsWith(QStringLiteral("defaultconfigs/"));
}

AgentDirMetadata AgentFileTools::list(const QString &root, const QString &relativePath,
                                      QString *problem) const {
    AgentDirMetadata result;
    AgentWinPath::Info info;
    const bool isRoot = relativePath.trimmed().isEmpty() || relativePath == QStringLiteral(".");
    if (isRoot ? !AgentWinPath::resolveRoot(root, &info, problem)
               : !AgentWinPath::resolveExisting(root, relativePath, true, &info, problem)) {
        return result;
    }
    result.exists = true;
    result.isDirectory = info.isDirectory;
    QDir dir(info.finalPath);
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                    QDir::NoSymLinks,
                                                    QDir::Name);
    result.entryCount = entries.size();
    if (entries.size() > maxListEntries()) result.truncated = true;
    const int count = qMin(entries.size(), maxListEntries());
    for (int i = 0; i < count; ++i) {
        const QFileInfo &entry = entries.at(i);
        if (entry.isDir()) {
            ++result.directoryCount;
        } else {
            ++result.fileCount;
            result.totalSize += entry.size();
        }
    }
    result.summary = (result.truncated
                          ? QCoreApplication::translate("AgentFileTools", "目录：%1 个文件，%2 个子目录，合计 %3 字节（超过 2000 项，统计不完整）")
                          : QCoreApplication::translate("AgentFileTools", "目录：%1 个文件，%2 个子目录，合计 %3 字节"))
                         .arg(result.fileCount)
                         .arg(result.directoryCount)
                         .arg(result.totalSize);
    return result;
}

QList<AgentFileNameEntry> AgentFileTools::listNames(const QString &root,
                                                    const QString &relativePath, int limit,
                                                    bool *truncated, QString *problem) const {
    QList<AgentFileNameEntry> result;
    if (truncated != nullptr) *truncated = false;
    AgentWinPath::Info info;
    const bool isRoot = relativePath.trimmed().isEmpty() || relativePath == QStringLiteral(".");
    if (isRoot ? !AgentWinPath::resolveRoot(root, &info, problem)
               : !AgentWinPath::resolveExisting(root, relativePath, true, &info, problem)) {
        return result;
    }
    const int cap = qBound(1, limit, maxListEntries());
    QDir dir(info.finalPath);
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                                                    QDir::Name);
    if (entries.size() > cap && truncated != nullptr) *truncated = true;
    for (int i = 0; i < qMin(entries.size(), cap); ++i) {
        const QFileInfo &entry = entries.at(i);
        AgentFileNameEntry item;
        item.name = entry.fileName();
        item.directory = entry.isDir();
        item.reparse = entry.isSymLink() || entry.isJunction();
        item.size = entry.isFile() ? entry.size() : 0;
        result.append(item);
    }
    return result;
}

AgentFileReadResult AgentFileTools::read(const QString &root, const QString &relativePath,
                                         int maxBytes) const {
    AgentFileReadResult result;
    AgentWinPath::Info info;
    QString problem;
    if (!AgentWinPath::resolveExisting(root, relativePath, false, &info, &problem)) {
        result.error = makeError(Agent::ErrorKind::ToolError, problem);
        return result;
    }
    if (info.isDirectory) {
        result.error = makeError(Agent::ErrorKind::ToolError,
                                 QCoreApplication::translate("AgentFileTools", "目标是目录，不能按文件读取。"));
        return result;
    }
    QFile file(info.finalPath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = makeError(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentFileTools", "无法读取文件。"));
        return result;
    }
    const int limit = qBound(1024, maxBytes <= 0 ? maxInlineBytes() : maxBytes, maxReadBytes());
    result.raw = file.read(qMin<qint64>(limit, info.size));
    result.truncated = info.size > result.raw.size();
    result.size = info.size;
    result.sha256 = sha256(result.raw);
    result.identityToken = AgentWinPath::identityToken(info);
    if (AgentWinPath::looksBinary(result.raw)) {
        result.binary = true;
        result.error = makeError(Agent::ErrorKind::EgressBlocked,
                                 QCoreApplication::translate("AgentFileTools", "检测到二进制内容，只能读取文件名、哈希等元数据。"));
        return result;
    }
    const QString text = QString::fromUtf8(result.raw);
    result.redactedText = AgentRedaction::redact(text).text;
    result.ok = true;
    return result;
}

AgentWritePlan AgentFileTools::prepareWrite(const QString &root, const QString &relativePath,
                                            const QString &content,
                                            const QString &expectedSha256,
                                            const QString &identityToken) const {
    AgentWritePlan plan;
    plan.relativePath = relativePath;
    plan.newContent = content;
    if (!isConfigWhitelist(relativePath)) {
        plan.error = makeError(Agent::ErrorKind::EgressBlocked,
                               QCoreApplication::translate("AgentFileTools", "只允许写入 config/**、defaultconfigs/** 和 options.txt。"));
        return plan;
    }
    if (content.toUtf8().size() > 1024 * 1024) {
        plan.error = makeError(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentFileTools", "写入内容超过 1 MB。"));
        return plan;
    }
    QString normalized;
    QString problem;
    if (!AgentWinPath::normalizeRelative(relativePath, &normalized, &problem)) {
        plan.error = makeError(Agent::ErrorKind::ToolError, problem);
        return plan;
    }
    AgentWinPath::Info parent;
    if (!AgentWinPath::resolveParent(root, normalized, &parent, &problem)) {
        plan.error = makeError(Agent::ErrorKind::ToolError, problem);
        return plan;
    }
    plan.parentIdentityToken = AgentWinPath::identityToken(parent);
    AgentWinPath::Info current;
    if (!AgentWinPath::resolveExisting(root, normalized, false, &current, &problem)) {
        if (!expectedSha256.isEmpty() || !identityToken.isEmpty()) {
            plan.error = makeError(Agent::ErrorKind::ToolError,
                                   QCoreApplication::translate("AgentFileTools", "目标不存在，但调用提供了 expectedSha256 或 fileIdentityToken。"));
            return plan;
        }
        plan.creatingNew = true;
        plan.oldSize = 0;
        plan.diff = lineDiff(QString(), content);
        plan.ok = true;
        return plan;
    }
    if (current.isDirectory) {
        plan.error = makeError(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentFileTools", "目标是目录，不能写入。"));
        return plan;
    }
    if (expectedSha256.isEmpty() || identityToken.isEmpty()) {
        plan.error = makeError(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentFileTools", "覆盖已有文件必须提供 expectedSha256 和 fileIdentityToken。"));
        return plan;
    }
    if (identityToken != AgentWinPath::identityToken(current)) {
        plan.error = makeError(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentFileTools", "文件身份已变化，请重新读取后再试。"));
        return plan;
    }
    QFile file(current.finalPath);
    if (!file.open(QIODevice::ReadOnly)) {
        plan.error = makeError(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentFileTools", "无法读取原文件。"));
        return plan;
    }
    const QByteArray oldBytes = file.readAll();
    if (sha256(oldBytes) != expectedSha256) {
        plan.error = makeError(Agent::ErrorKind::ToolError,
                               QCoreApplication::translate("AgentFileTools", "文件内容已变化，请重新读取后再试。"));
        return plan;
    }
    plan.oldContent = QString::fromUtf8(oldBytes);
    plan.oldSize = oldBytes.size();
    plan.expectedSha256 = expectedSha256;
    plan.identityToken = identityToken;
    plan.diff = lineDiff(plan.oldContent, content);
    plan.ok = true;
    return plan;
}

AgentWriteResult AgentFileTools::commitWrite(const QString &root, const AgentWritePlan &plan,
                                             const QString &sourceSessionId,
                                             const QString &instanceId) const {
    AgentWriteResult result;
    if (!plan.ok) {
        result.error = plan.error;
        return result;
    }
    QString normalized;
    QString problem;
    if (!AgentWinPath::normalizeRelative(plan.relativePath, &normalized, &problem)) {
        result.error = makeError(Agent::ErrorKind::ToolError, problem);
        return result;
    }
    AgentWinPath::Info parent;
    if (!AgentWinPath::resolveParent(root, normalized, &parent, &problem) ||
        AgentWinPath::identityToken(parent) != plan.parentIdentityToken) {
        result.error = makeError(Agent::ErrorKind::ToolError,
                                 QCoreApplication::translate("AgentFileTools", "父目录身份已变化，已拒绝提交。"));
        return result;
    }
    AgentWinPath::Info current;
    const bool exists = AgentWinPath::resolveExisting(root, normalized, false, &current, &problem);
    if (plan.creatingNew) {
        if (exists) {
            result.error = makeError(Agent::ErrorKind::ToolError,
                                     QCoreApplication::translate("AgentFileTools", "待新建的文件在提交前已存在，已拒绝覆盖。"));
            return result;
        }
    } else {
        if (!exists || current.isDirectory) {
            result.error = makeError(Agent::ErrorKind::ToolError,
                                     QCoreApplication::translate("AgentFileTools", "目标在提交前消失或被替换。"));
            return result;
        }
        if (AgentWinPath::identityToken(current) != plan.identityToken) {
            result.error = makeError(Agent::ErrorKind::ToolError,
                                     QCoreApplication::translate("AgentFileTools", "目标在提交前被替换或修改，已拒绝写入。"));
            return result;
        }
        QFile oldFile(current.finalPath);
        if (!oldFile.open(QIODevice::ReadOnly)) {
            result.error = makeError(Agent::ErrorKind::ToolError, QCoreApplication::translate("AgentFileTools", "无法读取目标。"));
            return result;
        }
        const QByteArray oldBytes = oldFile.readAll();
        if (sha256(oldBytes) != plan.expectedSha256) {
            result.error = makeError(Agent::ErrorKind::ToolError,
                                     QCoreApplication::translate("AgentFileTools", "目标内容在提交前已变化，已拒绝写入。"));
            return result;
        }
        result.bytesBefore = oldBytes.size();
        if (m_backups != nullptr) {
            QString backupProblem;
            if (!m_backups->createFromBytes(sourceSessionId, instanceId, normalized, oldBytes,
                                            QCoreApplication::translate("AgentFileTools", "写入文件前的备份"), &result.backupId,
                                            &backupProblem)) {
                result.error = makeError(Agent::ErrorKind::ToolError,
                                         QCoreApplication::translate("AgentFileTools", "写入前备份失败：%1").arg(backupProblem));
                return result;
            }
            result.backupDir = m_backups->dirFor(result.backupId);
        }
    }

    const QString finalPath = QDir::cleanPath(root + QLatin1Char('/') + normalized);
    QSaveFile file(finalPath);
    const QByteArray bytes = plan.newContent.toUtf8();
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = makeError(Agent::ErrorKind::ToolError,
                                 QCoreApplication::translate("AgentFileTools", "无法创建临时文件：%1").arg(file.errorString()));
        return result;
    }
    if (file.write(bytes) != bytes.size()) {
        result.error = makeError(Agent::ErrorKind::ToolError,
                                 QCoreApplication::translate("AgentFileTools", "写入临时文件失败：%1").arg(file.errorString()));
        file.cancelWriting();
        return result;
    }
    if (!file.commit()) {
        result.error = makeError(Agent::ErrorKind::ToolError,
                                 QCoreApplication::translate("AgentFileTools", "无法提交写入：%1。原文件和备份均已保留。")
                                     .arg(file.errorString()));
        return result;
    }
    AgentWinPath::Info after;
    QString verifyProblem;
    if (!AgentWinPath::resolveExisting(root, normalized, false, &after, &verifyProblem)) {
        result.error = makeError(Agent::ErrorKind::ToolError,
                                 QCoreApplication::translate("AgentFileTools", "写入后无法回读：%1").arg(verifyProblem));
        return result;
    }
    QFile verify(finalPath);
    if (!verify.open(QIODevice::ReadOnly) || verify.readAll() != bytes) {
        result.error = makeError(Agent::ErrorKind::ToolError,
                                 QCoreApplication::translate("AgentFileTools", "写入后内容校验失败，备份已保留。"));
        return result;
    }
    result.ok = true;
    result.sha256 = sha256(bytes);
    result.identityToken = AgentWinPath::identityToken(after);
    result.bytesAfter = bytes.size();
    result.unchanged = !plan.creatingNew && bytes == plan.oldContent.toUtf8();
    return result;
}
