#include "PresetController.hpp"
#include "PresetEditorPage.hpp"
#include "manage/ModpackExport.hpp"
#include "manage/VersionManager.hpp"
#include "presets/GenericFiles.hpp"
#include "presets/ModSourceResolution.hpp"
#include "presets/PresetCapture.hpp"
#include "presets/PresetModTargets.hpp"
#include "presets/PresetWorkspace.hpp"
#include "util/AppPaths.hpp"
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QLayout>
#include <QPushButton>
#include <QSet>
#include <QTemporaryDir>
#include <QtConcurrent/QtConcurrentRun>

void PresetController::openEditor(const QString &id, bool draft) {
    if (m_editor && !leaveEditor())
        return;
    buildPage();
    QJsonObject state;
    if (!id.isEmpty()) {
        const bool isDraft = draft || id.startsWith("draft:");
        const auto key = id.startsWith("draft:") ? id.mid(6) : id;
        bool ok = false;
        const auto root = isDraft ? presetsRoot() + "/drafts" : presetsRoot();
        auto manifest = Presets::loadManifest(root, key, &ok);
        if (!ok) {
            m_page->showStatus(tr("无法读取预设，原文件保留。"));
            return;
        }
        state = Presets::editorDocument(manifest);
        state["_filesRoot"] = Presets::filesDir(root, key);
    } else {
        state["id"] = Presets::newPresetId();
        state["name"] = QString();
    }
    QSet<QString> editableSections;
    for (const auto &v : state.value("sections").toArray()) {
        const auto section = v.toObject();
        const auto kind = section.value("kind").toString();
        QString key;
        if (kind=="vanillaOptions" || kind=="vanillaKeys") key=kind;
        else if (kind=="genericFiles" && !section.value("items").toArray().isEmpty()) key=kind+"/"+section.value("items").toArray().first().toObject().value("path").toString().section('/',0,0);
        if (key.isEmpty()) continue;
        if (editableSections.contains(key)) {
            m_page->showStatus(tr("此旧预设含多段同类内容，暂不能在统一编辑器中合并编辑。原内容保留，可从实例重新提取一份草稿。"));
            return;
        }
        editableSections.insert(key);
    }
    if (!m_host.showPage) {
        if (m_page) m_page->showStatus(tr("当前界面不能打开预设编辑器。"));
        return;
    }
    // 编辑器是**整页**（和实例设置同一条路）：铺在内容区上，页眉带返回箭头。摆上去就
    // 返回 —— 收页由覆盖层自己管，收掉之后才叫下面那条回调（见 `Host::showPage`）。
    auto *editor = new PresetEditorPage(state);
    editor->saveRequested = [this](bool draft) { saveEditor(draft); };
    editor->backRequested = [this] { leaveEditor(); };
    editor->confirmReset = [this](const QString &text) {
        return m_editorPrompt &&
               m_editorPrompt(tr("放弃修改"), text, {tr("确认"), tr("取消")}) == 0;
    };
    m_editor = editor;
    const bool shown = m_host.showPage(
        editor, id.isEmpty() ? tr("新建预设") : tr("编辑预设"), -1,
        [this, editor](int) {
            // 走到这儿 = 那一页已经收掉了（用户自己走 / 存完自己走 / 这一层被别处收掉）。
            // 收尾只做一件事：把这一页还回去。列表刷新在 closeEditor() 里做过一遍了。
            if (m_editor == editor) {
                m_editor = nullptr;
                if (m_page) refreshList(m_currentId);
            }
            editor->hide();
            editor->deleteLater();
        });
    if (!shown) {
        m_editor = nullptr;
        editor->hide();
        editor->deleteLater();
        if (m_page) m_page->showStatus(tr("当前界面不能打开预设编辑器。"));
    }
}

void PresetController::closeEditor() {
    if (!m_editor)
        return;
    auto *editor = m_editor.data();
    m_editor = nullptr;
    refreshList(m_currentId);
    // 收页 = accept()：覆盖层收到 finished 就拆这一层，随后 `pageClosed()` 叫到
    // openEditor() 里那条回调（reject() 已经被页面接管成"问要不要保存"，见
    // ui/PresetEditorPage 的 .hpp）。真正的 deleteLater 在那边做。
    editor->accept();
}
void PresetController::saveEditor(bool draft, const std::function<void(bool)> &done) {
    if (!m_editor || m_editorSaving) {
        if (done)
            done(false);
        return;
    }
    m_editorSaving = true;
    m_editor->setEnabled(false);
    m_editor->showProblem(tr("正在保存…"));
    const auto editor = m_editor;
    const auto document = editor->document();
    const auto root = presetsRoot();
    auto *watcher = new QFutureWatcher<Presets::EditorSaveResult>(this);
    connect(watcher, &QFutureWatcher<Presets::EditorSaveResult>::finished, this,
            [this, watcher, editor, draft, done] {
                auto result = watcher->result();
                watcher->deleteLater();
                m_editorSaving = false;
                if (editor) {
                    editor->setEnabled(true);
                    if (result.ok) {
                        editor->markSaved(result.document);
                        editor->showProblem(draft ? tr("草稿已保存") : tr("预设已保存"));
                        m_currentId = draft ? QStringLiteral("draft:")+result.manifest.id : result.manifest.id;
                        refreshList(m_currentId);
                    } else
                        editor->showProblem(result.problem);
                }
                if (done)
                    done(result.ok);
            });
    watcher->setFuture(QtConcurrent::run(
        [root, document, draft] { return Presets::saveEditorDocument(root, document, draft); }));
}
bool PresetController::leaveEditor() {
    if (!m_editor)
        return true;
    if (m_editorSaving || m_editorPrompting)
        return false;
    if (!m_editor->isDirty()) {
        closeEditor();
        return true;
    }
    if (!m_editorPrompt)
        return false;
    m_editorPrompting = true;
    const int choice =
        m_editorPrompt(tr("未保存的修改"), tr("如何处理当前修改？"),
                       {tr("保存为草稿"), tr("保存为预设"), tr("不保存"), tr("取消")});
    m_editorPrompting = false;
    if (choice == 2) {
        closeEditor();
        return true;
    }
    if (choice != 0 && choice != 1)
        return false;
    QEventLoop loop;
    bool saved = false;
    saveEditor(choice == 0, [&](bool ok) {
        saved = ok;
        loop.quit();
    });
    if (m_editorSaving)
        loop.exec();
    if (!saved)
        return false;
    closeEditor();
    return true;
}

namespace {
/// 提取出来的一个模组：认出来的带着平台和项目号，认不出来的两格都空（编辑页里那行显示成
/// 「未知来源（请选择）」，等用户补）。
struct CapturedMod {
    QString platform;
    QString projectId;
    QString name;
};
struct EditorCapture {
    Presets::CaptureReport report;
    QList<CapturedMod> mods;
    /// 来源问询自己出的问题（没连上、接口回的东西读不懂）。只作提示，不影响"认出来几个"。
    QStringList lookupProblems;
};
} // namespace
void PresetController::captureForEditor(const QString &instanceId) {
    if (instanceId.isEmpty() || !leaveEditor())
        return;
    if ((m_host.isRunning && m_host.isRunning(instanceId)) ||
        (m_host.isInstanceBusy && m_host.isInstanceBusy(instanceId))) {
        if (m_page)
            m_page->showStatus(tr("请先停止实例及其文件操作。"));
        return;
    }
    const auto id = Presets::newPresetId();
    const auto root = presetsRoot() + "/drafts";
    Presets::CaptureRequest request;
    request.instanceId = instanceId;
    request.instanceRoot = instanceRoot(instanceId);
    request.presetFilesRoot = Presets::filesDir(root, id);
    request.selection.config = false;
    request.selection.defaultConfigs = false;
    request.selection.malilib = false;
    request.selection.resourcePacks = true;
    request.selection.shaderPacks = true;
    request.selection.dataPacks = true;
    // 两个来源的设置在这儿就抄成值：工人里现建一台 `VersionManager`，不去碰控制器、
    // 页面和它们手上那些对象（和导出那条路同一个理由，见 ModpackExportController）。
    // NOTE: 两句"问询失败"的话也在主线程翻好带进去 —— 工人里调不到页面的 `tr()`
    // （那是个成员函数，lambda 没捕获 `this`）。
    const QString modrinthBase = m_versions ? m_versions->modrinthApiRoot() : QString();
    const QString curseForgeKey = m_presetApiKey;
    const QString curseForgeBase =
        m_versions ? m_versions->curseForgeResolveSettings().apiBaseOverride : QString();
    const QString modrinthFailedText = tr("查询 Modrinth 失败：%1");
    const QString curseForgeFailedText = tr("查询 CurseForge 失败：%1");
    auto *watcher = new QFutureWatcher<EditorCapture>(this);
    if (m_page) {
        m_page->setBusy(true);
        m_page->showStatus(tr("正在提取实例内容并识别模组来源…"));
    }
    connect(
        watcher, &QFutureWatcher<EditorCapture>::finished, this,
        [this, watcher, id, root, instanceId] {
            auto capture = watcher->result();
            watcher->deleteLater();
            if (m_page)
                m_page->setBusy(false);
            if (!capture.report.ok && capture.mods.isEmpty()) {
                if (m_page)
                    m_page->showStatus(capture.report.problems.join('\n'));
                return;
            }
            Presets::PresetManifest draft;
            draft.id = id;
            draft.name = displayName(instanceId);
            draft.sections = capture.report.sections;
            draft.sourceInstanceId = instanceId;
            draft.createdAt = QDateTime::currentDateTime();
            draft.updatedAt = draft.createdAt;
            auto document = Presets::editorDocument(draft);
            QJsonArray refs;
            int unresolved = 0;
            for (const CapturedMod &mod : capture.mods) {
                if (mod.projectId.isEmpty())
                    ++unresolved;
                refs.append(QJsonObject{
                    {"platform", mod.platform},
                    {"projectId", mod.projectId},
                    {"name", mod.name},
                    {"required", true}});
            }
            document["modReferences"] = refs;
            draft.editorState = document;
            if (!Presets::saveManifest(root, draft)) {
                if (m_page)
                    m_page->showStatus(tr("无法保存提取的草稿。"));
                return;
            }
            openEditor(id, true);
            if (m_editor) {
                QStringList notes = capture.report.problems;
                if (unresolved)
                    notes.prepend(
                        tr("Modrinth 和 CurseForge 均未识别 %1 个模组。可在模组页筛选来源未知的项目，补充或移除它们，也可在概览中停用模组部分。")
                            .arg(unresolved));
                notes.append(capture.lookupProblems);
                m_editor->showProblem(notes.join('\n'));
            }
        });
    watcher->setFuture(QtConcurrent::run(
        [request, modrinthBase, curseForgeKey, curseForgeBase, modrinthFailedText,
         curseForgeFailedText] {
            EditorCapture result;
            result.report = Presets::captureFromInstance(request);

            // ---- 一个 jar 一条：sha1 和 CurseForge 指纹（`hashFile` 两趟读一次算齐） ----
            //
            // 读不动的那一份留成"没认出来"，不让整趟提取失败：一个锁着的 jar 不该把
            // 其它几十个已经认出来的模组一起拖下水。
            struct LocalMod {
                QString sha1;
                QString name;
                quint32 fingerprint = 0;
                bool fingerprintOk = false;
            };
            QList<LocalMod> mods;
            const auto entries =
                QDir(request.instanceRoot + "/mods")
                    .entryInfoList({"*.jar"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
            mods.reserve(entries.size());
            for (const auto &entry : entries) {
                ModpackExport::FileHashes hashes;
                QString why;
                if (!ModpackExport::hashFile(entry.filePath(), true, &hashes, &why))
                    continue;
                mods.append({hashes.sha1, entry.completeBaseName(), hashes.fingerprint,
                             hashes.fingerprintOk});
            }
            if (mods.isEmpty())
                return result;

            // ---- 两个来源各问一次（都是批量；失败就整条停，见两个 lookup 的注释） ----
            VersionManager worker;
            worker.setModrinthApiBaseOverride(modrinthBase);
            worker.setCurseForgeApiKey(curseForgeKey);
            worker.setCurseForgeApiBaseOverride(curseForgeBase);
            QStringList sha1s;
            sha1s.reserve(mods.size());
            for (const LocalMod &mod : mods)
                sha1s.append(mod.sha1);
            QHash<QString, ModpackExport::ModrinthHit> modrinthHits;
            const QString modrinthProblem = worker.lookupModrinthVersionFiles(sha1s, &modrinthHits);
            if (!modrinthProblem.isEmpty())
                result.lookupProblems.append(modrinthFailedText.arg(modrinthProblem));

            // CurseForge 只问 Modrinth 没认出来的那几条：两个平台都有的模组跟 Modrinth 走
            // （判断在 `Presets::resolveModSources()` 一处）。同一个指纹只问一次 ——
            // 内容相同的两个 jar 在接口那边也是同一条。
            QHash<quint32, QString> curseForgeProjects;
            QList<quint32> fingerprints;
            QSet<quint32> asked;
            for (const LocalMod &mod : mods) {
                if (!mod.fingerprintOk || modrinthHits.contains(mod.sha1) ||
                    asked.contains(mod.fingerprint))
                    continue;
                asked.insert(mod.fingerprint);
                fingerprints.append(mod.fingerprint);
            }
            if (!fingerprints.isEmpty()) {
                QHash<quint32, ModpackExport::CurseForgeHit> hits;
                const QString curseForgeProblem =
                    worker.lookupCurseForgeFingerprints(fingerprints, &hits);
                if (!curseForgeProblem.isEmpty())
                    result.lookupProblems.append(curseForgeFailedText.arg(curseForgeProblem));
                for (auto it = hits.constBegin(); it != hits.constEnd(); ++it) {
                    // 接口的 `modId` 是 int：0 及以下说明这一条少了项目身份，别拿它当号。
                    if (it.value().projectId > 0)
                        curseForgeProjects.insert(it.key(), QString::number(it.value().projectId));
                }
            }

            QHash<QString, QString> modrinthProjects;
            for (auto it = modrinthHits.constBegin(); it != modrinthHits.constEnd(); ++it)
                modrinthProjects.insert(it.key(), it.value().projectId);
            QList<Presets::LocalModIdentity> identities;
            identities.reserve(mods.size());
            for (const LocalMod &mod : mods)
                identities.append({mod.sha1, mod.fingerprint, mod.fingerprintOk});
            const QList<Presets::ResolvedModSource> resolved =
                Presets::resolveModSources(identities, modrinthProjects, curseForgeProjects);
            result.mods.reserve(mods.size());
            for (qsizetype i = 0; i < mods.size(); ++i)
                result.mods.append({resolved.at(i).platform, resolved.at(i).projectId,
                                    mods.at(i).name});
            return result;
        }));
}
