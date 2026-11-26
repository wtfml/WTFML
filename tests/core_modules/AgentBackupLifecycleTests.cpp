/**
 * @file AgentBackupLifecycleTests.cpp
 * @brief 维护备份元数据、owner/source/sourceSessionId、来源 Session 删除后仍可读。
 */

#include "Support.hpp"

#include "agent/AgentBackupIndex.hpp"
#include "agent/AgentSessionStore.hpp"
#include "util/AppPaths.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

void runAgentBackupLifecycleTests() {
    QTemporaryDir fixture;
    AgentBackupIndex backups(fixture.path() + QStringLiteral("/backups"));
    QString backupId;
    QString problem;
    const QByteArray original("maxMemory = 4096\n");
    check(backups.createFromBytes(QStringLiteral("session-1"), QStringLiteral("inst-1"),
                                  QStringLiteral("config/a.toml"), original,
                                  QStringLiteral("fs.write 前备份"), &backupId, &problem),
          "备份：创建", problem);
    check(!backupId.isEmpty(), "备份：生成 id");
    QList<AgentBackupEntry> entries;
    check(backups.list(&entries, &problem) && entries.size() == 1, "备份：列表");
    const AgentBackupEntry entry = entries.constFirst();
    check(entry.owner == QStringLiteral("agent") &&
              entry.source == QStringLiteral("agent-session") &&
              entry.storage == QStringLiteral("agent-maintenance"),
          "备份：owner/source/storage 正确");
    check(entry.sourceSessionId == QStringLiteral("session-1"),
          "备份：保留来源会话 id");
    check(entry.instanceId == QStringLiteral("inst-1"), "备份：保留实例 id");
    check(entry.sha256.size() == 64 && entry.size == original.size(),
          "备份：记录 sha256/大小");
    check(entry.originalPaths.contains(QStringLiteral("config/a.toml")),
          "备份：记录来源相对路径");
    QByteArray restored;
    check(backups.readOriginal(backupId, &restored, &problem) && restored == original,
          "备份：内容可解出", problem);

    // 元数据明文不含内容。
    QFile meta(backups.dirFor(backupId) + QStringLiteral("/meta.json"));
    check(meta.open(QIODevice::ReadOnly), "备份：读元数据");
    const QByteArray metaBytes = meta.readAll();
    check(!metaBytes.contains("maxMemory = 4096"), "备份：元数据不含文件内容");
    const QJsonObject metaObject = QJsonDocument::fromJson(metaBytes).object();
    meta.close();
    check(!metaObject.value(QStringLiteral("approved")).toBool(false),
          "备份：元数据不存审批结果");

    // 删除来源会话不删备份。
    AgentSessionStore sessions(fixture.path());
    AgentSession session;
    sessions.create(&session, &problem);
    check(sessions.requestDeletion(session.meta.id, &problem) &&
              sessions.completeDeletion(session.meta.id, &problem, nullptr),
          "备份：来源会话可删除");
    check(QFileInfo::exists(backups.dirFor(backupId) + QStringLiteral("/meta.json")),
          "备份：删除会话后备份仍在");
    entries.clear();
    backups.list(&entries, nullptr);
    check(!entries.isEmpty() && entries.constFirst().sourceSessionId == QStringLiteral("session-1"),
          "备份：来源会话删除后元数据仍可读");

    // 只允许清理 owner=agent。
    check(backups.removeOwnedBackup(backupId, &problem), "备份：清理 owner=agent 成功", problem);
    check(!QFileInfo::exists(backups.dirFor(backupId)), "备份：清理后目录消失");

    // 伪造 owner=user 的元数据，清理必须拒绝。
    const QString foreignId = QStringLiteral("foreign-1");
    QDir().mkpath(backups.dirFor(foreignId));
    QJsonObject foreign{{QStringLiteral("schema"), 1},
                        {QStringLiteral("backupId"), foreignId},
                        {QStringLiteral("owner"), QStringLiteral("user")},
                        {QStringLiteral("source"), QStringLiteral("user-export")}};
    QFile foreignMeta(backups.dirFor(foreignId) + QStringLiteral("/meta.json"));
    check(foreignMeta.open(QIODevice::WriteOnly), "备份：写一份外部来源的元数据", foreignMeta.fileName());
    foreignMeta.write(QJsonDocument(foreign).toJson());
    foreignMeta.close();
    check(!backups.removeOwnedBackup(foreignId, &problem), "备份：owner=user 不允许 Agent 清理");
    check(QFileInfo::exists(backups.dirFor(foreignId) + QStringLiteral("/meta.json")),
          "备份：共享/外部备份未被误删");

    // ---------- 多文件备份（Stage 2 的 backup.create）----------
    QList<QPair<QString, QByteArray>> files{
        {QStringLiteral("config/a.toml"), QByteArray("maxMemory = 4096\n")},
        {QStringLiteral("mods/sodium.jar"), QByteArray::fromHex("504b0304")},
        {QStringLiteral("servers.dat"), QByteArray::fromHex("0a0d0a00")}};
    QString multiId;
    check(backups.createFromFiles(QStringLiteral("session-1"), QStringLiteral("1.20.1"),
                                  files, QStringLiteral("backup.create：config+mods"), &multiId,
                                  &problem),
          "备份：多文件备份写入成功", problem);
    QList<AgentBackupEntry> multiEntries;
    backups.list(&multiEntries, nullptr);
    bool foundMulti = false;
    for (const AgentBackupEntry &entry : multiEntries) {
        if (entry.backupId != multiId) continue;
        foundMulti = entry.originalPaths.size() == 3 && entry.owner == QStringLiteral("agent");
    }
    check(foundMulti, "备份：多文件备份的元数据记下三条相对路径");
    QList<QPair<QString, QByteArray>> restoredFiles;
    check(backups.readFiles(multiId, &restoredFiles, &problem), "备份：多文件备份可读回", problem);
    check(restoredFiles.size() == 3, "备份：读回三个文件", QString::number(restoredFiles.size()));
    check(restoredFiles.constFirst().second == QByteArray("maxMemory = 4096\n"),
          "备份：文本内容逐字节一致");
    check(restoredFiles.at(1).second == QByteArray::fromHex("504b0304"),
          "备份：二进制内容逐字节一致");

    // 单文件备份（createFromBytes）也要能被 readFiles 读回（前面那份已经被清理过）。
    QString singleId;
    check(backups.createFromBytes(QStringLiteral("session-1"), QStringLiteral("1.20.1"),
                                  QStringLiteral("options.txt"), QByteArray("fov:0.0\n"),
                                  QStringLiteral("单元测试"), &singleId, &problem),
          "备份：单文件备份写入成功", problem);
    QList<QPair<QString, QByteArray>> single;
    check(backups.readFiles(singleId, &single, &problem), "备份：单文件备份也能走 readFiles",
          problem);
    check(single.size() == 1, "备份：单文件备份读回一条");

    // ---------- #129：旧库冻结，但照旧列得出、还原得了 ----------
    // 新备份落在当前游戏文件夹（cache/backups/files）；旧库里的条目按原 id 一起列出。
    const QString originalGameDir = AppPaths::minecraftDir();
    AppPaths::setMinecraftDir(fixture.path() + QStringLiteral("/game"));
    QString writeId;
    check(backups.createFromBytes(QStringLiteral("session-9"), QStringLiteral("1.20.1"),
                                  QStringLiteral("options.txt"), QByteArray("fov:70\n"),
                                  QStringLiteral("写入文件前的备份"), &writeId, &problem),  // i18n-ignore
          "备份：旧库写入一份 fs.write 备份", problem);
    const QList<AgentBackups::Item> legacyItems = AgentBackups::list(QStringLiteral("1.20.1"), &backups);
    const auto itemOf = [&legacyItems](const QString &id) {
        for (const AgentBackups::Item &item : legacyItems) {
            if (item.backupId == id) return item;
        }
        return AgentBackups::Item();
    };
    check(itemOf(writeId).legacy() && itemOf(writeId).trigger == AgentBackups::kTriggerWrite &&
              itemOf(writeId).sessionId == QStringLiteral("session-9") &&
              itemOf(multiId).trigger == AgentBackups::kTriggerCreate &&
              itemOf(multiId).scope == QStringLiteral("config+mods"),
          "备份：旧库条目按原 id 列出，按 reason 认出来源");
    check(AgentBackups::list(QStringLiteral("other-instance"), &backups).isEmpty(),
          "备份：旧库条目只出现在自己的实例下");
    const QString instanceDir = fixture.path() + QStringLiteral("/game/versions/1.20.1");
    QDir().mkpath(instanceDir);
    QFile current(instanceDir + QStringLiteral("/options.txt"));
    check(current.open(QIODevice::WriteOnly), "备份：造当前的 options.txt", current.fileName());
    current.write("fov:110\n");
    current.close();
    QList<BackupFiles::FilePayload> legacyFiles;
    check(AgentBackups::readFiles(itemOf(writeId), &backups, &legacyFiles, &problem) &&
              legacyFiles.size() == 1,
          "备份：旧库条目照旧解密读回", problem);
    BackupFiles::Manifest safety;
    safety.root = QStringLiteral("files");
    safety.label = QStringLiteral("1.20.1");
    safety.sourceInstanceId = QStringLiteral("1.20.1");
    const BackupFiles::RestoreResult restoredLegacy =
        BackupFiles::restoreFiles(instanceDir, legacyFiles, safety);
    QFile after(instanceDir + QStringLiteral("/options.txt"));
    after.open(QIODevice::ReadOnly);
    check(restoredLegacy.ok && after.readAll() == QByteArray("fov:70\n"),
          "备份：旧库条目还原成功", restoredLegacy.problem);
    after.close();
    const QList<AgentBackups::Item> afterRestore = AgentBackups::list(QStringLiteral("1.20.1"), &backups);
    // 新格式时间只到秒，和同一秒写的旧条目谁先谁后不定：按内容找，不按位置。
    check(std::any_of(afterRestore.cbegin(), afterRestore.cend(),
                      [](const AgentBackups::Item &item) {
                          return !item.legacy() &&
                                 item.trigger == AgentBackups::kTriggerBeforeRestore;
                      }),
          "备份：还原前快照写进新格式，旧库不再增加");
    QList<AgentBackupEntry> frozen;
    backups.list(&frozen, nullptr);
    check(std::none_of(frozen.cbegin(), frozen.cend(), [](const AgentBackupEntry &entry) {
              return entry.reason.contains(QStringLiteral("恢复"));  // i18n-ignore
          }),
          "备份：旧库没有新写入");
    AppPaths::setMinecraftDir(originalGameDir);
}
