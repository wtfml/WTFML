#include "PresetModInstaller.hpp"
#include <QCoreApplication>
#include "PresetWriteLock.hpp"
#include "PresetCompatibility.hpp"
#include "download/DownloadManager.hpp"
#include "download/ResourceStore.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTimer>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include "manage/InstanceIconLookup.hpp"
#include "util/AppPaths.hpp"
#include "util/DirectorySessionLock.hpp"
#include "controllers/DownloadBatchController.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "download/DownloadCheckpoint.hpp"
#include "PresetModTargets.hpp"

namespace Presets {
namespace {
bool redirected(const QString &path) {
    const QFileInfo info(path);
    return info.isSymLink() || info.isJunction();
}
QSet<QString> failingParents(const QList<ModInstallRow> &rows, QSet<QString> failed) {
    for (int pass = 0; pass < rows.size(); ++pass) {
        bool changed = false;
        for (const auto &row : rows) {
            if (failed.contains(row.key)) continue;
            for (const auto &dependency : row.version.dependencies)
                if (dependency.required() && failed.contains(row.reference.platform + QLatin1Char(':') + dependency.projectId)) {
                    failed.insert(row.key); changed = true; break;
                }
        }
        if (!changed) break;
    }
    return failed;
}
}
PresetModInstaller::PresetModInstaller(QObject *parent, ResourceStore *store) : QObject(parent), m_store(store) {}
PresetModInstaller::~PresetModInstaller() {
    if (m_recovery) m_recovery->shutdown();
    m_scanCancel->store(true, std::memory_order_relaxed);
}

void PresetModInstaller::setRecovery(ActivityCenterController *activity, Tasks::TaskId id, const QJsonObject &intent) {
    m_activity = activity; m_taskId = id; m_continuation = intent;
}
bool PresetModInstaller::pauseDownload() {
    if (m_done || !m_recovery || m_phase != "download" || !m_recovery->pause(m_taskId)) return false;
    if (m_deadline) { m_remainingMs = qMax(1, m_deadline->remainingTime()); m_deadline->stop(); }
    return true;
}
bool PresetModInstaller::resumeDownload() {
    if (m_done || !m_recovery || m_phase != "download" || m_resumeScanActive ||
        !m_download || !m_download->batchPaused()) return false;
    const auto root = m_root;
    const auto expected = m_continuation.value("modTargets").toObject();
    if (expected.isEmpty()) return false;
    m_resumeScanActive = true;
    const auto token = m_activity ? m_activity->tasks()->snapshot(m_taskId).recoveryToken : QString();
    if (m_activity) {
        m_activity->tasks()->setDetail(m_taskId, tr("正在后台检查模组目录，完成后恢复文件下载。"));
        m_activity->tasks()->setRecovery(m_taskId, token, false, tr("正在检查模组目录。"));
    }
    auto *watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher, root, expected, token] {
        const auto current = watcher->result();
        watcher->deleteLater();
        m_resumeScanActive = false;
        if (m_done || m_scanCancel->load(std::memory_order_relaxed)) return;
        if (root != m_root || m_phase != "download" || current.isEmpty() || current != expected ||
            (m_running && m_running()) || (m_planCurrent && !m_planCurrent())) {
            if (m_activity) {
                m_activity->tasks()->setDetail(m_taskId, tr("模组目录或实例状态已被外部修改，请取消并重新预览。"));
                m_activity->tasks()->setRecovery(m_taskId, token, false, tr("请取消并重新预览。"));
            }
            return;
        }
        if (m_activity) m_activity->tasks()->setRecovery(m_taskId, token, true);
        if (m_recovery->resume(m_taskId) && m_deadline) m_deadline->start(m_remainingMs);
    });
    watcher->setFuture(QtConcurrent::run([root, cancel = m_scanCancel] {
        return Presets::snapshotModTargets(root, cancel.get());
    }));
    return true;
}
QString PresetModInstaller::stagedPath(int index) const {
    const auto hash = m_downloadRows.at(index).file.sha1.toLower();
    const bool valid = QRegularExpression(QStringLiteral("^[0-9a-f]{40}$")).match(hash).hasMatch();
    return m_stage->filePath(valid ? hash + ".jar" : "invalid-" + QString::number(index));
}

void PresetModInstaller::start(const QList<ModReference> &references, const QString &root,
    const QString &gameVersion, const QString &loader, const QString &apiKey,
    std::function<bool()> running, std::function<void(Result)> completed) {
    m_root = root;
    m_running = std::move(running);
    m_completed = std::move(completed);
    if (!QDir::isAbsolutePath(root) || !QFileInfo(root).isDir() || redirected(root)) {
        finish(tr("目标实例目录不可用，未开始应用。")); return;
    }
    QString writeProblem;
    m_writeLock = acquirePresetWrite(root, &writeProblem);
    if (!m_writeLock) {
        // The owning run's report must not be overwritten by a rejected contender.
        m_root.clear(); finish(writeProblem); return;
    }
    m_request.gameVersion = gameVersion;
    m_request.loader = normalizeCompatibilityLoader(loader);
    for (const auto &ref : references) {
        ModResolveInput input;
        input.reference = ref;
        input.reference.loader = normalizeCompatibilityLoader(ref.loader);
        if ((!ref.gameVersion.isEmpty() && ref.gameVersion != gameVersion) ||
            (!ref.loader.isEmpty() && input.reference.loader != m_request.loader)) {
            input.queried = true;
            input.queryFailed = true;
            input.queryProblem = tr("预设限定的游戏版本或加载器与目标实例不符，未查询其它版本替代。");
            m_answers.insert(ref.key(), input);
        }
        m_request.references.append(input);
    }
    if (references.isEmpty()) { m_result.ok = true; finish(); return; }
    if (gameVersion.isEmpty() || m_request.loader.isEmpty()) {
        finish(tr("无可确认的游戏版本或模组加载器，未下载模组。")); return;
    }
    if (!m_store) m_store = new ResourceStore(this);
    m_store->setCurseForgeApiKey(apiKey);
    connect(m_store, &ResourceStore::modrinthFilesFinished, this,
        [this](const QString &key, const QHash<QString, QString> &projects,
               const QHash<QString, ResourceCatalog::VersionEntry> &versions, const QString &problem) {
            if (m_done || key != m_waiting) return;
            m_waiting.clear();
            if (!problem.isEmpty()) m_result.lines.append(tr("部分已安装模组身份未确认：%1").arg(problem));
            for (auto &file : m_inventory.files) {
                const QString sha = file.sha1.toLower();
                if (!projects.contains(sha) || !versions.contains(sha)) continue;
                if (!file.projectKey.isEmpty() && !file.projectKey.startsWith(QStringLiteral("modrinth:"))) continue;
                const QString projectKey = QStringLiteral("modrinth:") + projects.value(sha);
                const auto version = versions.value(sha);
                if (!file.projectKey.isEmpty() && (file.projectKey != projectKey
                    || (!file.versionId.isEmpty() && file.versionId != version.id))) {
                    m_hashIdentityConflicts.insert(file.projectKey);
                    m_hashIdentityConflicts.insert(projectKey);
                    m_result.lines.append(tr("文件身份反查与已有记录不一致，未自动改写：%1").arg(QFileInfo(file.path).fileName()));
                    continue;
                }
                file.projectKey = projectKey;
                file.versionId = version.id;
                file.version = version;
            }
            QTimer::singleShot(0, this, [this] { resolveNext(); });
        });
    const auto exactVersionFinished = [this](const QString &key, const QString &project, const ResourceCatalog::VersionEntry &version,
               const QString &problem) {
            if (m_done || key != m_waiting) return;
            const QString platform = key.section(QLatin1Char(':'), 1, 1);
            const QString releaseId = key.section(QLatin1Char(':'), 2);
            m_waiting.clear();
            if (!problem.isEmpty()) {
                m_result.lines.append(tr("无法确认前置发行版 %1：%2").arg(releaseId, problem));
            } else {
                const QString projectKey = platform + QLatin1Char(':') + project;
                if (!m_answers.contains(projectKey) && m_answers.size() >= 256) {
                    finish(tr("模组依赖超过本次安全检查上限（256 项）。")); return;
                }
                // Enrich only identities backed by this exact release response.
                // Keep all release alternatives; the planner still merges exact pins.
                for (auto &answer : m_answers) {
                    for (auto &entry : answer.versions) {
                        for (auto &dependency : entry.dependencies) {
                            if (answer.reference.platform == platform
                                && dependency.projectId.isEmpty() && dependency.versionId == releaseId)
                                dependency.projectId = project;
                        }
                    }
                }
                for (auto &file : m_inventory.files) {
                    if (!file.projectKey.startsWith(platform + QLatin1Char(':'))) continue;
                    for (auto &dependency : file.version.dependencies)
                        if (dependency.projectId.isEmpty() && dependency.versionId == releaseId) dependency.projectId = project;
                }
                auto &resolved = m_answers[projectKey];
                resolved.reference.platform = platform;
                resolved.reference.projectId = project;
                resolved.queried = true;
                resolved.queryFailed = false;
                resolved.queryProblem.clear();
                bool found = false;
                for (const auto &entry : resolved.versions) if (entry.id == version.id) found = true;
                if (!found) resolved.versions.append(version);
            }
            QTimer::singleShot(0, this, [this] { resolveNext(); });
        };
    connect(m_store, &ResourceStore::modrinthVersionFinished, this, exactVersionFinished);
    connect(m_store, &ResourceStore::curseForgeVersionFinished, this, exactVersionFinished);
    connect(m_store, &ResourceStore::versionsFinished, this,
        [this](const QString &key, const QList<ResourceCatalog::VersionEntry> &versions, const QString &problem) {
            if (m_done || key != m_waiting) return;
            auto &answer = m_answers[key];
            answer.queried = true;
            answer.queryFailed = !problem.isEmpty();
            answer.queryProblem = problem;
            answer.versions = versions;
            m_waiting.clear();
            QTimer::singleShot(0, this, [this] { resolveNext(); });
        });
    m_deadline = new QTimer(this); m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this] { if (!m_done) cancel(); });
    m_deadline->start(m_remainingMs);
    if (m_activity && !m_continuation.isEmpty()) {
        m_activity->tasks()->setDetail(m_taskId, tr("正在后台建立模组目录快照…"));
        const auto expected = m_continuation.value("modTargets").toObject();
        auto *watcher = new QFutureWatcher<QJsonObject>(this);
        connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher, expected] {
            const auto targets = watcher->result();
            watcher->deleteLater();
            if (m_done || m_scanCancel->load(std::memory_order_relaxed)) return;
            if (targets.isEmpty() || (!expected.isEmpty() && targets != expected) ||
                (m_running && m_running()) || (m_planCurrent && !m_planCurrent())) {
                finish(tr("无法确认模组目录或预设状态，请重新预览；未继续下载或部署。"));
                return;
            }
            m_continuation.insert("modTargets", targets);
            m_activity->tasks()->setDetail(m_taskId, tr("正在检查已安装模组与预设依赖…"));
            readInventory();
        });
        watcher->setFuture(QtConcurrent::run([root = m_root, cancel = m_scanCancel] {
            return Presets::snapshotModTargets(root, cancel.get());
        }));
    } else readInventory();
}

void PresetModInstaller::readInventory(bool beforeCommit) {
    auto *watcher = new QFutureWatcher<InstalledModInventory>(this);
    connect(watcher, &QFutureWatcher<InstalledModInventory>::finished, this, [this, watcher, beforeCommit] {
        auto inventory = watcher->result();
        watcher->deleteLater();
        if (m_done) return;
        if (!inventory.problem.isEmpty()) { finish(inventory.problem); return; }
        if (beforeCommit) {
            if (!sameInstalledMods(m_inventory, inventory)) {
                finish(tr("已安装模组在检查期间发生变化，未写入；请刷新后重试。")); return;
            }
            commit();
        } else {
            m_inventory = std::move(inventory);
            resolveNext();
        }
    });
    InstanceIconLookup::Options options;
    options.cacheOnly = true;
    options.cacheDir = AppPaths::instanceIconCacheDir();
    options.cacheKey = m_identityKey;
    watcher->setFuture(QtConcurrent::run([root = m_root, cancel = m_scanCancel, options, beforeCommit] {
        auto inventory = scanInstalledMods(root, cancel.get());
        if (beforeCommit || !inventory.problem.isEmpty() || options.cacheKey.isEmpty()) return inventory;
        QStringList paths;
        for (const auto &file : inventory.files) paths.append(file.path);
        const auto cached = InstanceIconLookup::lookup(paths, options);
        QHash<QString, InstanceIconLookup::Found> byPath;
        for (const auto &found : cached.found) byPath.insert(found.path, found);
        for (auto &file : inventory.files) {
            const auto found = byPath.value(file.path);
            if (found.sha1.isEmpty() || found.sha1.compare(file.sha1, Qt::CaseInsensitive) != 0
                || found.hit.projectId.isEmpty()) continue;
            const QString key = ResourceCatalog::sourceKey(found.hit.source) + QLatin1Char(':') + found.hit.projectId;
            if (!file.projectKey.isEmpty() && (file.projectKey != key
                || (!file.versionId.isEmpty() && !found.currentVersion.id.isEmpty() && file.versionId != found.currentVersion.id))) {
                inventory.problem = QCoreApplication::translate("PresetModInstaller", "已安装模组的识别缓存与安装记录冲突，未允许安装。");
                return inventory;
            }
            file.projectKey = key;
            if (!found.currentVersion.id.isEmpty()) file.versionId = found.currentVersion.id;
            file.version = found.currentVersion;
        }
        return inventory;
    }));
}

void PresetModInstaller::updateInstalledIdentities() {
    m_request.installedKeys.clear();
    m_request.installedVersionIds.clear();
    m_request.installedVersions.clear();
    m_duplicateInstalled.clear();
    m_identifiedFiles.clear();
    m_ambiguousInstalledProjects = m_hashIdentityConflicts;
    QHash<QString, QList<QPair<QString, ResourceCatalog::VersionEntry>>> byHash;
    for (auto answer = m_answers.cbegin(); answer != m_answers.cend(); ++answer)
        for (const auto &version : answer->versions)
            for (const auto &file : version.files)
                if (!file.sha1.isEmpty()) byHash[file.sha1.toLower()].append({answer.key(), version});
    for (const auto &observed : m_inventory.files) {
        auto file = observed; // Re-evaluate against all answers; don't pin an early partial match.
        QString key = file.projectKey;
        QString release = file.versionId;
        auto metadata = file.version;
        QHash<QString, ResourceCatalog::VersionEntry> candidates;
        for (const auto &candidate : byHash.value(file.sha1.toLower())) {
            if (!key.isEmpty() && candidate.first != key) continue;
            if (!release.isEmpty() && candidate.second.id != release) continue;
            candidates.insert(candidate.first + QLatin1Char('\n') + candidate.second.id, candidate.second);
        }
        if (candidates.size() == 1) {
            const auto identity = candidates.constBegin();
            key = identity.key().section(QLatin1Char('\n'), 0, 0);
            release = identity.value().id;
            metadata = identity.value();
        } else if (key.isEmpty() && candidates.size() > 1) {
            for (auto candidate = candidates.cbegin(); candidate != candidates.cend(); ++candidate)
                m_ambiguousInstalledProjects.insert(candidate.key().section(QLatin1Char('\n'), 0, 0));
        }
        if (key.isEmpty()) continue; // Unknown is not proof of project absence.
        file.projectKey = key;
        file.versionId = release;
        file.version = metadata;
        m_identifiedFiles.append(file);
        if (m_request.installedKeys.contains(key)) m_duplicateInstalled.insert(key);
        m_request.installedKeys.insert(key);
        if (!release.isEmpty()) m_request.installedVersionIds[key] = release;
        if (metadata.dependencyMetadataKnown) m_request.installedVersions[key] = metadata;
    }
    for (const auto &key : m_duplicateInstalled) {
        m_request.installedVersionIds.remove(key);
        m_request.installedVersions.remove(key);
    }
}

void PresetModInstaller::resolveNext() {
    if (m_done || m_reviewPending) return;
    if (m_planCurrent && !m_planCurrent()) { finish(tr("所选预设内容已变化，请重新预览和确认；未继续应用。")); return; }
    if (m_running && m_running()) { finish(tr("游戏正在运行，已停止应用模组预设。")); return; }
    // Recalculate dependency closure after each answer. Only one platform request is in flight.
    for (auto &input : m_request.references) {
        const auto reference = input.reference;
        if (m_answers.contains(reference.key())) input = m_answers.value(reference.key());
        input.reference = reference; // A dependency lookup must not demote an explicit required root.
    }
    for (auto it = m_answers.cbegin(); it != m_answers.cend(); ++it) {
        if (it->queried && !it->queryFailed) m_request.dependencyAnswers[it.key()] = it->versions;
    }
    updateInstalledIdentities();
    m_plan = planModInstall(m_request);
    // Version-only dependencies are valid Modrinth metadata. Resolve the identity
    // before treating the planner's provisional Unknown as a final failure.
    for (const auto &row : m_plan.rows) {
        const QString platform = row.reference.platform;
        if (platform != QStringLiteral("modrinth") && platform != QStringLiteral("curseforge")) continue;
        for (const auto &dependency : row.version.dependencies) {
            if (!dependency.required() || !dependency.projectId.isEmpty() || dependency.versionId.isEmpty()
                || m_versionLookups.contains(platform + QLatin1Char(':') + dependency.versionId)) continue;
            if (m_versionLookups.size() >= 256) {
                finish(tr("精确前置身份查询超过安全上限（256 项），未安装模组。")); return;
            }
            m_versionLookups.insert(platform + QLatin1Char(':') + dependency.versionId);
            m_waiting = QStringLiteral("preset-version:") + platform + QLatin1Char(':') + dependency.versionId;
            if (platform == QStringLiteral("curseforge")) m_store->fetchCurseForgeVersion(m_waiting, dependency.versionId);
            else m_store->fetchModrinthVersion(m_waiting, dependency.versionId);
            return;
        }
    }
    // Honour exact dependency versions present in the platform answers instead of
    // choosing the newest dependency and then unnecessarily refusing the install.
    for (int pass = 0; pass < 8; ++pass) {
        QHash<QString, QString> pins;
        for (const auto &row : m_plan.rows) {
            if (row.status != ModInstallStatus::Download && row.status != ModInstallStatus::AlreadyInstalled) continue;
            for (const auto &dependency : row.version.dependencies) {
                if (!dependency.required() || dependency.versionId.isEmpty() || dependency.projectId.isEmpty()) continue;
                const QString key = row.reference.platform + QLatin1Char(':') + dependency.projectId;
                if (pins.contains(key) && pins.value(key) != dependency.versionId) {
                    finish(tr("多个模组要求不同的精确前置版本，未安装：%1").arg(dependency.projectId)); return;
                }
                pins.insert(key, dependency.versionId);
            }
        }
        bool changed = false;
        for (const auto &row : m_plan.downloads())
            if (pins.contains(row.key) && row.version.id != pins.value(row.key)) changed = true;
        if (!changed) break;
        for (auto it = pins.cbegin(); it != pins.cend(); ++it) {
            QList<ResourceCatalog::VersionEntry> matching;
            for (const auto &version : m_answers.value(it.key()).versions)
                if (version.id == it.value()) matching.append(version);
            if (m_request.dependencyAnswers.contains(it.key())) m_request.dependencyAnswers[it.key()] = matching;
            for (auto &input : m_request.references)
                if (input.reference.key() == it.key()) input.versions = matching;
        }
        m_plan = planModInstall(m_request);
        if (pass == 7) { finish(tr("精确前置约束未收敛，未安装模组。")); return; }
    }
    for (const auto &row : m_plan.rows) {
        if (row.key.isEmpty() || m_answers.contains(row.key)) continue;
        if (m_answers.size() >= 256) { finish(tr("模组依赖超过本次安全检查上限（256 项）。")); return; }
        ModResolveInput answer;
        answer.reference = row.reference;
        m_answers.insert(row.key, answer);
        ResourceCatalog::Source source;
        if (!ResourceCatalog::sourceFromKey(row.reference.platform, &source)) {
            m_answers[row.key].queried = true;
            m_answers[row.key].queryFailed = true;
            m_answers[row.key].queryProblem = tr("无法识别的模组平台。");
            QTimer::singleShot(0, this, [this] { resolveNext(); }); return;
        }
        m_waiting = row.key;
        m_store->fetchVersions(row.key, row.reference.projectId, source,
            row.reference.gameVersion.isEmpty() ? m_request.gameVersion : row.reference.gameVersion,
            normalizeCompatibilityLoader(row.reference.loader.isEmpty() ? m_request.loader : row.reference.loader));
        return;
    }
    if (queryUnidentifiedFiles()) return;
    // Show unresolved installed identities in the review itself, rather than
    // presenting an all-green plan and refusing only after the user confirms it.
    const auto markUnknown = [&](ModInstallRow &row, const QString &problem) {
        if (row.isFailure()) return;
        if (row.isDownload()) --m_plan.download;
        else if (row.status == ModInstallStatus::AlreadyInstalled) --m_plan.alreadyInstalled;
        row.status = ModInstallStatus::Unknown; row.problem = problem; ++m_plan.unknown;
    };
    for (auto &row : m_plan.rows) {
        if (m_ambiguousInstalledProjects.contains(row.key) || m_duplicateInstalled.contains(row.key))
            markUnknown(row, tr("已有文件的项目 / 发行版身份存在歧义或重复，未自动覆盖。"));
        else if (row.status == ModInstallStatus::AlreadyInstalled && !row.version.dependencyMetadataKnown)
            markUnknown(row, tr("已安装发行版的依赖身份尚未确认，不能当作前置已满足。"));
    }
    for (int pass = 0; pass < m_plan.rows.size(); ++pass) {
        QHash<QString, QString> failures;
        for (const auto &row : m_plan.rows) if (row.isFailure()) failures.insert(row.key, row.problem);
        bool changed = false;
        for (auto &row : m_plan.rows) {
            if (row.isFailure()) continue;
            for (const auto &dependency : row.version.dependencies) {
                const QString key = row.reference.platform + QLatin1Char(':') + dependency.projectId;
                if (dependency.required() && failures.contains(key)) {
                    markUnknown(row, tr("必要前置未解决：%1；%2").arg(key, failures.value(key)));
                    changed = true; break;
                }
            }
        }
        if (!changed) break;
    }
    m_plan.ok = !m_plan.hasRequiredFailure();
    if (m_review && !m_reviewApproved) {
        m_reviewPending = true;
        const QPointer<PresetModInstaller> guard(this);
        m_review(m_plan, [guard](ReviewDecision decision) {
            if (!guard || guard->m_done || !guard->m_reviewPending) return;
            auto *self = guard.data(); self->m_reviewPending = false;
            if (!decision.accepted) { self->cancel(); return; }
            for (auto it = decision.alternatives.cbegin(); it != decision.alternatives.cend(); ++it) {
                auto candidate = it.value();
                bool eligible = false;
                for (const auto &row : self->m_plan.rows) if (row.key == it.key() &&
                    (row.status == ModInstallStatus::Missing || row.status == ModInstallStatus::AlternativeNeedsConfirm)) {
                    candidate.required = row.required; eligible = true;
                }
                if (!eligible || candidate.key().isEmpty() || candidate.key() == it.key()) {
                    self->finish(tr("替代项目无效或原项状态不允许替代，未继续应用。")); return;
                }
                self->m_request.alternativesByKey[it.key()] = {candidate};
                self->m_request.confirmedAlternatives.insert(candidate.key());
                bool exists = false;
                for (const auto &input : self->m_request.references) if (input.reference.key() == candidate.key()) exists = true;
                if (!exists) { ModResolveInput input; input.reference = candidate; self->m_request.references.append(input); }
            }
            self->m_reviewApproved = decision.alternatives.isEmpty() && self->m_plan.ok;
            if (!self->m_reviewApproved && decision.alternatives.isEmpty()) {
                self->finish(tr("必要模组或前置尚未解决，未允许下载。")); return;
            }
            QTimer::singleShot(0, self, [self] { self->resolveNext(); });
        });
        return;
    }
    for (const auto &row : m_plan.rows) if (row.isFailure()) m_result.lines.append(row.title + QCoreApplication::translate("PresetModInstaller", "：") + row.problem);
    const int unidentified = static_cast<int>(m_inventory.files.size() - m_identifiedFiles.size());
    if (unidentified > 0) m_result.lines.append(tr("%1 个启用模组的项目身份尚未确认，未将它们当作前置已满足。") .arg(unidentified));
    m_result.optionalFailures = m_plan.optionalFailures();
    if (!m_plan.ok) { finish(tr("必要模组或前置缺失、未知或冲突；未写入模组文件。")); return; }
    QSet<QString> unsafeOptional;
    for (const auto &row : m_plan.rows) {
        QString problem;
        if (m_ambiguousInstalledProjects.contains(row.key))
            problem = tr("已有模组文件可能对应「%1」的多个项目或发行版，无法确认是否重复，未写入。") .arg(row.title);
        else if (row.status == ModInstallStatus::AlreadyInstalled
                 && (m_duplicateInstalled.contains(row.key) || !row.version.dependencyMetadataKnown))
            problem = tr("已安装「%1」有重复文件或发行版依赖身份尚未确认，未自动覆盖。") .arg(row.title);
        if (problem.isEmpty()) continue;
        if (row.required) { finish(problem); return; }
        if (!unsafeOptional.contains(row.key)) {
            unsafeOptional.insert(row.key);
            ++m_result.optionalFailures;
            m_result.lines.append(problem);
        }
    }
    const auto unsafeComponents = failingParents(m_plan.rows, unsafeOptional);
    for (const auto &row : m_plan.downloads()) {
        if (unsafeComponents.contains(row.key)) {
            if (row.required) { finish(tr("必要前置的已有文件身份不安全，未允许安装。")); return; }
            if (!unsafeOptional.contains(row.key)) ++m_result.optionalFailures;
            continue;
        }
        for (const auto &dependency : row.version.dependencies) {
            if (dependency.required() && !dependency.versionId.isEmpty()) {
                const QString dependencyKey = row.reference.platform + QLatin1Char(':') + dependency.projectId;
                bool pinnedMatch = m_request.installedVersionIds.value(dependencyKey) == dependency.versionId;
                for (const auto &candidate : m_plan.downloads())
                    if (candidate.reference.platform == row.reference.platform &&
                        candidate.reference.projectId == dependency.projectId &&
                        candidate.version.id == dependency.versionId) pinnedMatch = true;
                if (!pinnedMatch) {
                    finish(tr("模组「%1」要求精确前置版本，当前解析结果不满足，未写入模组。") .arg(row.title)); return;
                }
            }
        }
        m_downloadRows.append(row);
    }
    // Deploy dependencies before their parents, so cancelling between complete
    // files does not normally leave a parent installed without its necessary child.
    const auto selected = m_downloadRows;
    QHash<QString, int> byKey;
    for (int i = 0; i < selected.size(); ++i) byKey.insert(selected[i].key, i);
    QSet<QString> visited;
    QList<ModInstallRow> ordered;
    std::function<void(int)> visit = [&](int index) {
        const auto &row = selected.at(index);
        if (visited.contains(row.key)) return;
        visited.insert(row.key);
        for (const auto &dependency : row.version.dependencies) {
            const QString key = row.reference.platform + QLatin1Char(':') + dependency.projectId;
            if (dependency.required() && byKey.contains(key)) visit(byKey.value(key));
        }
        ordered.append(row);
    };
    for (int i = 0; i < selected.size(); ++i) visit(i);
    m_downloadRows = ordered;
    download();
}

bool PresetModInstaller::queryUnidentifiedFiles() {
    QSet<QString> confirmed;
    for (const auto &file : m_identifiedFiles)
        if (file.version.dependencyMetadataKnown && !file.versionId.isEmpty()) confirmed.insert(file.path);
    QSet<QString> pending;
    for (const auto &file : m_inventory.files) {
        const QString sha = file.sha1.toLower();
        if (confirmed.contains(file.path) || m_hashLookups.contains(sha)) continue;
        if (!file.projectKey.isEmpty() && !file.projectKey.startsWith(QStringLiteral("modrinth:"))) continue;
        pending.insert(sha);
        if (pending.size() >= 100) break;
    }
    if (pending.isEmpty()) return false;
    m_hashLookups.unite(pending);
    m_waiting = QStringLiteral("preset-file-identities");
    m_store->fetchModrinthFiles(m_waiting, pending.values());
    return true;
}

void PresetModInstaller::download() {
    m_phase = QStringLiteral("download");
    if (!saveExecutionRecord()) { finish(tr("无法保存下载前执行报告，未开始下载。")); return; }
    if (!QDir::isAbsolutePath(m_root) || !QFileInfo(m_root).isDir() || redirected(m_root) ||
        redirected(QDir(m_root).filePath(QStringLiteral("mods")))) {
        finish(tr("实例或模组目录不安全，未写入文件。")); return;
    }
    const auto stage = QDir(m_root).filePath(".wtfml-preset-downloads");
    if (!DownloadCheckpoint::safePath(stage) || !QDir().mkpath(stage)) { finish(tr("无法创建安全的模组下载暂存目录。")); return; }
    m_stage = std::make_unique<QDir>(stage);
    m_download = new DownloadManager(this);
    m_download->setUseMirror(false);
    m_download->setMaxConcurrentDownloads(4);
    if (m_activity && m_taskId != Tasks::invalidTaskId() && !m_continuation.isEmpty()) {
        m_recovery = new DownloadBatchController(m_activity, m_download, [this] { return m_taskId; }, this);
        m_recovery->setContinuation([this] { return m_continuation; }, {});
    }
    connect(m_download, &DownloadManager::allDownloadsFinished, this, [this](bool) {
        if (!m_done) QTimer::singleShot(0, this, [this] { if (!m_done) readInventory(true); });
    });
    m_download->beginBatch();
    const auto rows = m_downloadRows;
    QSet<QString> staged;
    for (int i = 0; i < rows.size(); ++i) {
        const auto &file = rows[i].file;
        const QUrl url(file.url);
        // A platform filename is untrusted; never use it as a path during download.
        if (!url.isValid() || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http")) ||
            !url.userInfo().isEmpty() ||
            file.filename != QFileInfo(file.filename).fileName() || file.filename.contains(QLatin1Char('\\')) ||
            file.filename.contains(QLatin1Char(':')) || !file.filename.endsWith(QStringLiteral(".jar"), Qt::CaseInsensitive) ||
            !QRegularExpression(QStringLiteral("^[0-9a-fA-F]{40}$")).match(file.sha1).hasMatch()) continue;
        const auto path = stagedPath(i);
        if (!DownloadCheckpoint::safePath(path)) { finish(tr("暂存文件路径已变为链接，未下载或部署。")); return; }
        if (staged.contains(path)) continue;
        staged.insert(path);
        m_download->downloadFile(file.url, path, file.sha1, file.size);
    }
    m_download->endBatch();
}

void PresetModInstaller::commit() {
    if (m_planCurrent && !m_planCurrent()) { finish(tr("所选预设内容已变化，未部署下载文件。")); return; }
    m_phase = QStringLiteral("verify");
    const auto rows = m_downloadRows;
    QSet<QString> skipOptional;
    // Necessary downloads must all verify before any staged file is installed.
    for (int i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        const auto verified = verifyDownloadedFile(stagedPath(i), row.file.size, row.file.sha1);
        if ((!verified.ok || verified.unverified) && row.required) {
            finish(tr("必要模组「%1」未通过下载校验，未写入模组文件。保存文件名或校验信息也可能无效。").arg(row.title)); return;
        }
        if ((!verified.ok || verified.unverified) && !row.required) skipOptional.insert(row.key);
        else if (verified.ok && !verified.unverified) m_fileStates[row.key] = QStringLiteral("downloaded");
    }
    skipOptional = failingParents(rows, skipOptional);
    if (!skipOptional.isEmpty()) {
        m_result.optionalFailures += skipOptional.size();
        m_result.lines.append(tr("可选文件下载失败，跳过对应模组及依赖它的父项：%1；其它独立项继续。")
            .arg(QStringList(skipOptional.values()).join(QCoreApplication::translate("PresetModInstaller", "、"))));
    }
    if ((m_running && m_running()) || redirected(m_root) ||
        redirected(QDir(m_root).filePath(QStringLiteral("mods")))) {
        finish(tr("实例状态已变化，未写入模组。")); return;
    }
    QString lockProblem;
    if (!DirectorySessionLock::acquire(AppPaths::minecraftDir(), &lockProblem)) {
        finish(lockProblem); return;
    }
    QDir mods(QDir(m_root).filePath(QStringLiteral("mods")));
    if (!QDir().mkpath(mods.path())) { finish(tr("无法创建模组目录。")); return; }
    QHash<QString, QString> names;
    for (int i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        if (skipOptional.contains(row.key)) continue;
        if (!QFile::exists(stagedPath(i))) continue;
        const QString name = row.file.filename.toCaseFolded();
        if (names.contains(name) && names.value(name) != row.file.sha1.toLower()) {
            finish(tr("多个模组使用相同文件名但内容不同，未写入：%1").arg(row.file.filename)); return;
        }
        names.insert(name, row.file.sha1.toLower());
        const auto target = mods.filePath(row.file.filename);
        if (QFileInfo(target).isSymLink() ||
            (QFileInfo::exists(target) && !verifyDownloadedFile(target, row.file.size, row.file.sha1).ok)) {
            finish(tr("同名模组已存在且内容不同，未覆盖：%1").arg(row.file.filename)); return;
        }
    }
    m_phase = QStringLiteral("deploy");
    if (!saveExecutionRecord()) { finish(tr("无法保存部署阶段报告，未写入模组。")); return; }
    m_skippedDownloads = skipOptional;
    QTimer::singleShot(0, this, [this] { deployOne(0); });
}

void PresetModInstaller::cancel() {
    if (m_done) return;
    m_result.cancelled = true;
    finish(tr("模组预设应用已取消或超时；基础实例保留。"));
}
bool PresetModInstaller::saveExecutionRecord() {
    // Local audit record only: no launch-time enforcement or automatic reapplication.
    if (QDir::isAbsolutePath(m_root) && QDir(m_root).exists() && !redirected(m_root)) {
        const QString path = QDir(m_root).filePath(QStringLiteral("wtfml.preset-mod-install.json"));
        QJsonArray references, files, lines, execution;
        for (const auto &row : m_plan.rows) execution.append(QJsonObject{
            {"project", row.key}, {"versionId", row.version.id}, {"filename", row.file.filename},
            {"state", row.status == ModInstallStatus::AlreadyInstalled ? QStringLiteral("enabled")
                : m_fileStates.value(row.key, row.isFailure() ? QStringLiteral("blocked") : QStringLiteral("planned"))},
            {"required", row.required}, {"reason", row.problem}, {"sha1", row.file.sha1},
            {"replacesProjects", QJsonArray::fromStringList(row.replacesKeys)},
            {"details", QJsonArray::fromStringList(row.details)}});
        for (const auto &input : m_request.references) references.append(modReferenceToJson(input.reference));
        for (const auto &row : m_downloadRows) files.append(QJsonObject{
            {"project", row.key}, {"versionId", row.version.id}, {"filename", row.file.filename}, {"sha1", row.file.sha1}});
        for (const auto &file : m_identifiedFiles) if (!file.projectKey.isEmpty()) files.append(QJsonObject{
            {"project", file.projectKey}, {"versionId", file.versionId},
            {"filename", QFileInfo(file.path).fileName()}, {"sha1", file.sha1}});
        // A failed/cancelled operation must not erase identities from a previous
        // successful operation. These claims are always rehashed before reuse.
        QFile previous(path);
        if (!QFileInfo(path).isSymLink() && previous.open(QIODevice::ReadOnly) && previous.size() <= 8 * 1024 * 1024) {
            const auto object = QJsonDocument::fromJson(previous.readAll()).object();
            if (object.value(QStringLiteral("format")).toString() == QStringLiteral("wtfml-preset-mod-result/1")) {
                const auto oldFiles = object.value(QStringLiteral("plannedFiles")).toArray();
                QSet<QByteArray> retained;
                for (const auto &file : files) retained.insert(QJsonDocument(file.toObject()).toJson(QJsonDocument::Compact));
                for (const auto &old : oldFiles) {
                    if (files.size() >= 20000) break;
                    const auto identity = QJsonDocument(old.toObject()).toJson(QJsonDocument::Compact);
                    if (!retained.contains(identity)) { files.append(old); retained.insert(identity); }
                }
            }
        }
        previous.close();
        for (const auto &line : m_result.lines) lines.append(line);
        const QByteArray bytes = QJsonDocument(QJsonObject{{"format", "wtfml-preset-mod-result/1"},
            {"updatedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {"finishedAt", m_done ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate) : QString()}, {"gameVersion", m_request.gameVersion},
            {"loader", m_request.loader}, {"selection", m_selectionSnapshot}, {"references", references},
            {"plannedFiles", files}, {"execution", execution},
            {"phase", m_result.ok ? QStringLiteral("complete") : m_phase},
            {"success", m_done && m_result.ok}, {"cancelled", m_result.cancelled},
            {"installed", m_result.installed}, {"optionalFailures", m_result.optionalFailures}, {"lines", lines}}).toJson();
        QSaveFile record(path);
        if (bytes.size() > 8 * 1024 * 1024 || QFileInfo(path).isSymLink() || !record.open(QIODevice::WriteOnly) ||
            record.write(bytes) != bytes.size() || !record.commit()) {
            m_result.ok = false;
            m_result.lines.append(tr("预设执行记录保存失败；请保留本次任务报告。"));
            return false;
        }
    }
    return true;
}

void PresetModInstaller::finish(const QString &problem) {
    if (m_done) return;
    m_done = true;
    if (m_deadline) m_deadline->stop();
    m_scanCancel->store(true, std::memory_order_relaxed);
    if (!problem.isEmpty()) { m_result.ok = false; m_result.lines.prepend(problem); }
    if (!problem.isEmpty() && m_result.installed > 0)
        m_result.lines.append(tr("失败前已写入 %1 个模组；基础实例和已写入文件保留。").arg(m_result.installed));
    if (m_download && (m_result.cancelled || m_download->isBusy())) m_download->cancelBatch();
    saveExecutionRecord();
    m_writeLock.reset();
    if (m_completed) m_completed(m_result);
    deleteLater();
}
}
