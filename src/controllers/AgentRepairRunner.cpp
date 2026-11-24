/**
 * @file AgentRepairRunner.cpp
 * @brief 修复事务实现（见 .hpp）。
 */

#include "AgentRepairRunner.hpp"

#include "MainWindow.hpp"

#include "agent/AgentPaths.hpp"
#include "launcher/LauncherManager.hpp"
#include "manage/VersionManager.hpp"
#include "util/AppPaths.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QTimer>
#include <QtConcurrent>

#include <memory>

namespace {

constexpr qint64 kTransactionLimitBytes = 16LL * 1024 * 1024 * 1024;
constexpr qint64 kTransactionHeadroomBytes = 1024LL * 1024 * 1024;

QString sha256Of(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool safeRelative(const QString &path) {
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.startsWith(QChar(0x5c))) {
        return false;
    }
    if (path.contains(QStringLiteral("..")) || path.contains(QLatin1Char(':'))) return false;
    return true;
}

bool copyFile(const QString &from, const QString &to) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    QFile source(from);
    if (!source.open(QIODevice::ReadOnly)) return false;
    QSaveFile target(to);
    if (!target.open(QIODevice::WriteOnly)) return false;
    if (!target.write(source.readAll())) return false;
    return target.commit();
}

}  // namespace

AgentRepairRunner::AgentRepairRunner(MainWindow *owner, ApplySetting applySetting,
                                     ReadSetting readSetting, QObject *parent)
    : QObject(parent),
      m_owner(owner),
      m_applySetting(std::move(applySetting)),
      m_readSetting(std::move(readSetting)),
      m_workspaces(),
      m_transactions() {}

AgentRepairRunner::~AgentRepairRunner() {
    if (m_cancel) m_cancel->store(true);
    if (m_beginWatcher != nullptr) {
        m_beginWatcher->waitForFinished();
        m_beginWatcher = nullptr;
    }
    if (m_finishWatcher != nullptr) {
        m_finishWatcher->waitForFinished();
        m_finishWatcher = nullptr;
    }
}

bool AgentRepairRunner::begin(const QString &instanceId, const QStringList &scopes,
                              const AgentRepairHost::BeginCompletion &done, QString *problem) {
    if (m_active || m_beginWatcher != nullptr || m_finishWatcher != nullptr) {
        if (problem != nullptr) *problem = QStringLiteral("已有事务操作在运行。");  // i18n-ignore
        return false;
    }
    if (m_owner == nullptr || instanceId.isEmpty()) {
        if (problem != nullptr) *problem = QStringLiteral("主窗口或实例未接入。");  // i18n-ignore
        return false;
    }
    const QString sourceRoot = AppPaths::versionDir(instanceId);
    if (!QDir(sourceRoot).exists()) {
        if (problem != nullptr) *problem = QStringLiteral("实例目录不存在。");  // i18n-ignore
        return false;
    }
    if (m_owner->m_launcherManager->isVersionRunning(instanceId) ||
        m_owner->installingVersionIds().contains(instanceId)) {
        if (problem != nullptr) *problem = QStringLiteral("实例正在运行或安装，不能开始事务。");  // i18n-ignore
        return false;
    }
    QString txProblem;
    if (m_transactions.activeForInstance(instanceId, nullptr, &txProblem)) {
        if (problem != nullptr) *problem = QStringLiteral("这个实例已有活动事务。");  // i18n-ignore
        return false;
    }

    m_beginTransactionId = AgentRepairTransactionStore::newId();
    m_beginWorkspaceId = AgentWorkspaceStore::newId();
    m_beginVersionId = AgentWorkspaceStore::newVersionId(m_beginWorkspaceId);
    m_beginSourceRoot = sourceRoot;
    m_beginWorkspaceRoot = m_workspaces.workspaceRoot(m_beginWorkspaceId);
    m_beginScopes = scopes;
    m_beginDone = done;
    m_cancel = std::make_shared<std::atomic_bool>(false);

    AgentWorkspaceRecord workspace;
    workspace.id = m_beginWorkspaceId;
    workspace.sourceInstanceId = instanceId;
    workspace.versionId = m_beginVersionId;
    workspace.rootPath = m_beginWorkspaceRoot;
    workspace.status = QStringLiteral("copying");
    QString workspaceProblem;
    if (!m_workspaces.create(workspace, &workspaceProblem)) {
        if (problem != nullptr) *problem = workspaceProblem;
        return false;
    }
    AgentRepairTransactionRecord transaction;
    transaction.id = m_beginTransactionId;
    transaction.instanceId = instanceId;
    transaction.workspaceId = m_beginWorkspaceId;
    transaction.state = QStringLiteral("preparing");
    transaction.scopes = scopes;
    QString transactionProblem;
    if (!m_transactions.create(transaction, &transactionProblem)) {
        m_workspaces.remove(m_beginWorkspaceId, true, nullptr);
        if (problem != nullptr) *problem = transactionProblem;
        return false;
    }

    m_active = true;
    VersionManager *versions = m_owner->m_versionManager;
    const QString source = sourceRoot;
    const QString workspaceRoot = m_beginWorkspaceRoot;
    const QString versionId = m_beginVersionId;
    const std::shared_ptr<std::atomic_bool> cancel = m_cancel;
    m_beginWatcher = new QFutureWatcher<BeginPrepared>(this);
    connect(m_beginWatcher, &QFutureWatcher<BeginPrepared>::finished, this, [this, cancel] {
        QFutureWatcher<BeginPrepared> *watcher = m_beginWatcher;
        m_beginWatcher = nullptr;
        const BeginPrepared prepared = watcher->result();
        watcher->deleteLater();
        if (!m_active) return;

        AgentRepairTransactionRecord record;
        QString loadProblem;
        if (!m_transactions.load(m_beginTransactionId, &record, &loadProblem)) {
            m_active = false;
            if (m_beginDone) m_beginDone(AgentRepairBeginResult{false, {}, {}, 0, loadProblem});
            return;
        }
        if (!prepared.ok || cancel->load()) {
            record.state = QStringLiteral("failed");
            record.problem = prepared.problem;
            m_transactions.save(record, nullptr);
            m_workspaces.updateStatus(m_beginWorkspaceId, QStringLiteral("failed"), nullptr);
            m_active = false;
            const AgentRepairHost::BeginCompletion callback = m_beginDone;
            m_beginDone = {};
            if (callback) {
                callback(AgentRepairBeginResult{false, record.id, record.workspaceId, 0,
                                                prepared.problem});
            }
            return;
        }
        record.baseline = prepared.baseline;
        record.state = QStringLiteral("ready");
        record.problem.clear();
        m_transactions.save(record, nullptr);
        m_workspaces.updateStatus(m_beginWorkspaceId, QStringLiteral("ready"), nullptr);
        m_active = false;
        const AgentRepairHost::BeginCompletion callback = m_beginDone;
        m_beginDone = {};
        if (callback) {
            callback(AgentRepairBeginResult{true, record.id, record.workspaceId, prepared.bytes,
                                            QString()});
        }
    });
    m_beginWatcher->setFuture(QtConcurrent::run([=] {
        BeginPrepared prepared;
        qint64 bytes = 0;
        QDirIterator it(source, QDir::Files | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            bytes += it.fileInfo().size();
            if (bytes > kTransactionLimitBytes) break;
        }
        if (bytes > kTransactionLimitBytes) {
            prepared.problem = QStringLiteral("实例超过 16 GB 事务上限。");  // i18n-ignore
            return prepared;
        }
        const QStorageInfo storage(workspaceRoot);
        if (storage.isValid() && storage.bytesAvailable() < bytes + kTransactionHeadroomBytes) {
            prepared.problem = QStringLiteral("事务工作区所在磁盘空间不足。");  // i18n-ignore
            return prepared;
        }
        QString copyProblem;
        if (!versions->copyInstanceFrom(source, QFileInfo(source).fileName(), workspaceRoot,
                                        versionId, &copyProblem)) {
            prepared.problem = copyProblem;
            return prepared;
      
        }
        if (cancel->load()) {
            prepared.problem = QStringLiteral("事务创建已取消。");  // i18n-ignore
            return prepared;
        }
        QString baselineProblem;
        prepared.baseline = AgentRepairTransactionStore::captureBaseline(source, scopes,
                                                                          &baselineProblem);
        if (!baselineProblem.isEmpty()) {
            prepared.problem = baselineProblem;
            return prepared;
        }
        prepared.bytes = bytes;
        prepared.ok = true;
        return prepared;
    }));
    return true;
}

bool AgentRepairRunner::finish(const QString &transactionId, bool commit,
                               const AgentRepairHost::FinishCompletion &done, QString *problem) {
    if (m_active || m_beginWatcher != nullptr || m_finishWatcher != nullptr) {
        if (problem != nullptr) *problem = QStringLiteral("已有事务操作在运行。");  // i18n-ignore
        return false;
    }
    AgentRepairTransactionRecord record;
    QString txProblem;
    if (!m_transactions.load(transactionId, &record, &txProblem)) {
        if (problem != nullptr) *problem = txProblem;
        return false;
    }
    if (!record.active()) {
        if (problem != nullptr) *problem = QStringLiteral("事务已经结束。");  // i18n-ignore
        return false;
    }
    if (commit && !record.verification.value(QStringLiteral("ok")).toBool()) {
        if (problem != nullptr) *problem = QStringLiteral("事务还没有通过的验证，不能提交。");  // i18n-ignore
        return false;
    }
    m_finishTransactionId = transactionId;
    m_finishCommit = commit;
    m_finishDone = done;
    m_active = true;

    if (!commit) {
        QString restoreProblem;
        QSet<QString> changed;
        AgentWorkspaceRecord workspace;
        const QString sourceRoot = AppPaths::versionDir(record.instanceId);
        const QString backupRoot = AgentPaths::transactionsDir() + QLatin1Char('/') + record.id +
                                   QStringLiteral("/backups");
        if (!record.baseline.isEmpty() || QDir(backupRoot).exists()) {
            if (m_workspaces.load(record.workspaceId, &workspace, &restoreProblem)) {
                const QString workspaceVersion = workspace.rootPath +
                                                 QStringLiteral("/versions/") +
                                                 workspace.versionId;
                QString diffProblem;
                const QList<AgentRepairFileChange> changes = AgentRepairTransactionStore::diff(
                    workspaceVersion, record.baseline, record.scopes, &diffProblem);
                if (!diffProblem.isEmpty()) {
                    restoreProblem = diffProblem;
                } else {
                    for (const auto &change : changes) {
                        if (!safeRelative(change.relativePath)) continue;
                        const QString sourcePath =
                            sourceRoot + QLatin1Char('/') + change.relativePath;
                        const QString backupPath =
                            backupRoot + QLatin1Char('/') + change.relativePath;
                        if (QFileInfo(backupPath).isFile()) {
                            if (!copyFile(backupPath, sourcePath)) {
                                restoreProblem = QStringLiteral("回滚文件失败：%1")  // i18n-ignore
                                                     .arg(change.relativePath);  // i18n-ignore
                                break;
                            }
                            changed.insert(change.relativePath);
                        } else if (change.kind == AgentRepairFileChange::Kind::Added) {
                            QFile::remove(sourcePath);
                            changed.insert(change.relativePath);
                        }
                    }
                }
            } else {
                restoreProblem = QStringLiteral("找不到事务工作区登记。");  // i18n-ignore
            }
        }
        for (auto it = record.settings.begin(); it != record.settings.end(); ++it) {
            const QJsonObject entry = it.value().toObject();
            if (entry.contains(QStringLiteral("old")) && m_applySetting) {
                m_applySetting(it.key(), entry.value(QStringLiteral("old")), nullptr);
            }
        }
        record.state = restoreProblem.isEmpty() ? QStringLiteral("rolled_back")
                                                : QStringLiteral("failed");
        record.problem = restoreProblem;
        m_transactions.save(record, nullptr);
        m_workspaces.updateStatus(record.workspaceId,
                                  restoreProblem.isEmpty() ? QStringLiteral("rolled_back")
                                                           : QStringLiteral("failed"),
                                  nullptr);
        m_active = false;
        QTimer::singleShot(0, this, [this, record, restoreProblem, changed] {
            const AgentRepairHost::FinishCompletion callback = m_finishDone;
            m_finishDone = {};
            if (callback) {
                AgentRepairFinishResult result;
                result.ok = restoreProblem.isEmpty();
                result.committed = false;
                result.transactionId = record.id;
                for (const QString &path : changed) result.changedFiles.append(path);
                result.problem = restoreProblem;
                callback(result);
            }
        });
        return true;
    }

    m_transactions.updateState(record.id, QStringLiteral("committing"), QString(), nullptr);
    QList<QPair<QString, QJsonValue>> appliedSettings;
    for (auto it = record.settings.begin(); it != record.settings.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        if (!entry.contains(QStringLiteral("old")) || !entry.contains(QStringLiteral("new"))) {
            continue;
        }
        const QJsonValue oldValue = entry.value(QStringLiteral("old"));
        const QJsonValue newValue = entry.value(QStringLiteral("new"));
        if (m_readSetting && m_readSetting(it.key()) != oldValue) {
            if (problem != nullptr) *problem = QStringLiteral("设置 %1 已被外部修改。").arg(it.key());  // i18n-ignore
            m_transactions.updateState(record.id, QStringLiteral("failed"), *problem, nullptr);
            m_active = false;
            return false;
        }
        QString applyProblem;
        if (!m_applySetting || !m_applySetting(it.key(), newValue, &applyProblem)) {
            for (const auto &applied : appliedSettings) {
                m_applySetting(applied.first, applied.second, nullptr);
            }
            if (problem != nullptr) {
                *problem = applyProblem.isEmpty() ? QStringLiteral("设置提交失败。") : applyProblem;  // i18n-ignore
            }
            m_transactions.updateState(record.id, QStringLiteral("failed"), *problem, nullptr);
            m_active = false;
            return false;
        }
        appliedSettings.append({it.key(), oldValue});
    }

    AgentWorkspaceRecord workspace;
    if (!m_workspaces.load(record.workspaceId, &workspace, &txProblem)) {
        for (const auto &applied : appliedSettings) {
            m_applySetting(applied.first, applied.second, nullptr);
        }
        if (problem != nullptr) *problem = txProblem;
        m_transactions.updateState(record.id, QStringLiteral("failed"), txProblem, nullptr);
        m_active = false;
        return false;
    }
    const QString sourceRoot = AppPaths::versionDir(record.instanceId);
    const QString workspaceVersion = workspace.rootPath + QStringLiteral("/versions/") +
                                     workspace.versionId;
    const QString backupRoot = AgentPaths::transactionsDir() + QLatin1Char('/') + record.id +
                               QStringLiteral("/backups");

    m_finishWatcher = new QFutureWatcher<FinishPrepared>(this);
    connect(m_finishWatcher, &QFutureWatcher<FinishPrepared>::finished, this,
            [this, record, appliedSettings] {
                QFutureWatcher<FinishPrepared> *watcher = m_finishWatcher;
                m_finishWatcher = nullptr;
                const FinishPrepared prepared = watcher->result();
                watcher->deleteLater();
                if (!m_active) return;
                if (prepared.ok) {
                    AgentRepairTransactionRecord updated;
                    if (m_transactions.load(record.id, &updated, nullptr)) {
                        updated.state = QStringLiteral("committed");
                        updated.problem.clear();
                        m_transactions.save(updated, nullptr);
                    }
                    m_workspaces.updateStatus(record.workspaceId, QStringLiteral("committed"),
                                              nullptr);
                } else {
                    for (const auto &applied : appliedSettings) {
                        if (m_applySetting) m_applySetting(applied.first, applied.second, nullptr);
                    }
                    m_transactions.updateState(record.id, QStringLiteral("rolled_back"),
                                               prepared.problem, nullptr);
                }
                m_active = false;
                const AgentRepairHost::FinishCompletion callback = m_finishDone;
                m_finishDone = {};
                if (callback) {
                    AgentRepairFinishResult result;
                    result.ok = prepared.ok;
                    result.committed = prepared.ok;
                    result.transactionId = record.id;
                    result.changedFiles = prepared.changed;
                    result.backupIds = prepared.backups;
                    result.problem = prepared.problem;
                    callback(result);
                }
            });
    m_finishWatcher->setFuture(QtConcurrent::run([=] {
        FinishPrepared prepared;
        QString diffProblem;
        const QList<AgentRepairFileChange> changes = AgentRepairTransactionStore::diff(
            workspaceVersion, record.baseline, record.scopes, &diffProblem);
        if (!diffProblem.isEmpty()) {
            prepared.problem = diffProblem;
            return prepared;
        }
        if (changes.size() > 2000) {
            prepared.problem = QStringLiteral("事务差异超过 2000 个文件。");  // i18n-ignore
            return prepared;
        }
        qint64 backupBytes = 0;
        for (const AgentRepairFileChange &change : changes) {
            if (change.kind == AgentRepairFileChange::Kind::Added) continue;
            const QString path = sourceRoot + QLatin1Char('/') + change.relativePath;
            backupBytes += QFileInfo(path).size();
            if (backupBytes > 512LL * 1024 * 1024) {
                prepared.problem = QStringLiteral("事务备份超过 512 MB。");  // i18n-ignore
                return prepared;
            }
        }
        QList<AgentRepairFileChange> applied;
        for (const AgentRepairFileChange &change : changes) {
            if (!safeRelative(change.relativePath)) {
                prepared.problem = QStringLiteral("事务差异里有不安全路径。");  // i18n-ignore
                break;
            }
            const QString sourcePath = sourceRoot + QLatin1Char('/') + change.relativePath;
            const QString workspacePath = workspaceVersion + QLatin1Char('/') + change.relativePath;
            const QString backupPath = backupRoot + QLatin1Char('/') + change.relativePath;
            applied.append(change);
            if (change.kind != AgentRepairFileChange::Kind::Added) {
                if (!QFileInfo(sourcePath).isFile() ||
                    sha256Of(sourcePath) != change.beforeSha256) {
                    prepared.problem = QStringLiteral("源文件在提交前被修改：%1")  // i18n-ignore
                                           .arg(change.relativePath);  // i18n-ignore
                    break;
                }
                if (!copyFile(sourcePath, backupPath)) {
                    prepared.problem = QStringLiteral("备份源文件失败：%1")  // i18n-ignore
                                           .arg(change.relativePath);  // i18n-ignore
                    break;
                }
                prepared.backups.append(record.id + QStringLiteral("/backups/") +
                                        change.relativePath);
            }
            if (change.kind == AgentRepairFileChange::Kind::Deleted) {
                if (!QFile::remove(sourcePath)) {
                    prepared.problem = QStringLiteral("删除源文件失败：%1")  // i18n-ignore
                                           .arg(change.relativePath);  // i18n-ignore
                    break;
                }
            } else {
                if (!QFileInfo(workspacePath).isFile()) {
                    prepared.problem = QStringLiteral("工作区文件不存在：%1")  // i18n-ignore
                                           .arg(change.relativePath);  // i18n-ignore
                    break;
                }
                QFile source(workspacePath);
                if (!source.open(QIODevice::ReadOnly)) {
                    prepared.problem = QStringLiteral("读工作区文件失败：%1")  // i18n-ignore
                                           .arg(change.relativePath);  // i18n-ignore
                    break;
                }
                const QByteArray bytes = source.readAll();
                const QString afterHash = QString::fromLatin1(
                    QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
                if (afterHash != change.afterSha256) {
                    prepared.problem = QStringLiteral("工作区文件在提交前变化：%1")  // i18n-ignore
                                           .arg(change.relativePath);  // i18n-ignore
                    break;
                }
                QSaveFile target(sourcePath);
                if (!target.open(QIODevice::WriteOnly) || !target.write(bytes) ||
                    !target.commit()) {
                    prepared.problem = QStringLiteral("写入源文件失败：%1")  // i18n-ignore
                                           .arg(change.relativePath);  // i18n-ignore
                    break;
                }
                if (sha256Of(sourcePath) != change.afterSha256) {
                    prepared.problem = QStringLiteral("提交后哈希不匹配：%1")  // i18n-ignore
                                           .arg(change.relativePath);  // i18n-ignore
                    break;
                }
            }
            prepared.changed.append(change.relativePath);
        }
        if (!prepared.problem.isEmpty()) {
            for (int i = applied.size() - 1; i >= 0; --i) {
                const AgentRepairFileChange &change = applied.at(i);
                const QString sourcePath = sourceRoot + QLatin1Char('/') + change.relativePath;
                const QString backupPath = backupRoot + QLatin1Char('/') + change.relativePath;
                if (QFileInfo(backupPath).isFile()) {
                    copyFile(backupPath, sourcePath);
                } else if (change.kind == AgentRepairFileChange::Kind::Added) {
                    QFile::remove(sourcePath);
                }
            }
            return prepared;
        }
        prepared.ok = true;
        prepared.committed = true;
        return prepared;
    }));
    return true;
}
