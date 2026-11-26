/**
 * @file AgentFileTools.hpp
 * @brief 受控文件工具：fs.list / fs.listNames / fs.read / fs.write（Stage 1B）。
 *
 * 所有路径都是**相对路径**，根是锁定实例目录；实例由会话锁提供，工具不接受实例 id。
 * 写入只开放文本配置白名单，必须带 expectedSha256 + fileIdentityToken；提交前重新核身份，
 * 写前备份，原子替换，写后回读校验。不宣称彻底解决 TOCTOU。
 */

#pragma once

#include "AgentTypes.hpp"
#include "AgentWinPath.hpp"

#include <QByteArray>
#include <QList>
#include <QString>


struct AgentDirMetadata {
    bool exists = false;
    bool isDirectory = false;
    int entryCount = 0;
    int fileCount = 0;
    int directoryCount = 0;
    qint64 totalSize = 0;
    bool truncated = false;
    QString summary;
};

struct AgentFileNameEntry {
    QString name;
    bool directory = false;
    bool reparse = false;
    qint64 size = 0;
};

struct AgentFileReadResult {
    bool ok = false;
    bool binary = false;
    bool truncated = false;
    QByteArray raw;
    QString redactedText;
    QString sha256;
    QString identityToken;
    qint64 size = 0;
    Agent::Error error;
};

struct AgentWritePlan {
    bool ok = false;
    QString relativePath;
    QString oldContent;
    QString newContent;
    QString expectedSha256;
    QString identityToken;
    QString parentIdentityToken;
    QString diff;
    qint64 oldSize = 0;
    bool creatingNew = false;
    Agent::Error error;
};

struct AgentWriteResult {
    bool ok = false;
    bool unchanged = false;
    QString sha256;
    QString identityToken;
    QString backupId;
    QString backupDir;
    qint64 bytesBefore = 0;
    qint64 bytesAfter = 0;
    Agent::Error error;
};

class AgentFileTools {
public:
    /// @p backupBeforeWrite 为真时，覆盖已有文件前先写一份 Files 备份到启动器备份目录。
    explicit AgentFileTools(bool backupBeforeWrite = false) : m_backupBeforeWrite(backupBeforeWrite) {}

    AgentDirMetadata list(const QString &root, const QString &relativePath,
                          QString *problem = nullptr) const;
    QList<AgentFileNameEntry> listNames(const QString &root, const QString &relativePath,
                                        int limit, bool *truncated,
                                        QString *problem = nullptr) const;

    AgentFileReadResult read(const QString &root, const QString &relativePath, int maxBytes) const;

    /// 只准备计划，不写盘。@p expectedSha256/@p identityToken 为空表示新建文件。
    AgentWritePlan prepareWrite(const QString &root, const QString &relativePath,
                                const QString &content, const QString &expectedSha256,
                                const QString &identityToken) const;

    /// 提交计划：重新核身份/hash、备份原文件、原子写、写后回读。@p sourceSessionId 进备份元数据。
    /// @p backup 为假时不备份（写进修复事务工作区：那份副本本身就不是原实例）。
    AgentWriteResult commitWrite(const QString &root, const AgentWritePlan &plan,
                                 const QString &sourceSessionId, const QString &instanceId,
                                 const QString &sessionTitle = QString(), bool backup = true) const;

    static bool isConfigWhitelist(const QString &relativePath);
    static int maxReadBytes();
    static int maxInlineBytes();
    static int maxListEntries();

private:
    bool m_backupBeforeWrite = false;
};
