/**
 * @file AgentController.cpp
 * @brief 智能体页的接线与状态流转（见 .hpp）。
 */

#include "AgentController.hpp"
#include <QCoreApplication>

#include "agent/AgentPaths.hpp"
#include "util/AppPaths.hpp"
#include "agent/AgentProviderAdapters.hpp"
#include "agent/AgentRedaction.hpp"
#include "agent/AgentSecretStore.hpp"
#include "agent/AgentSkill.hpp"
#include "agent/McpTypes.hpp"

#include "AgentMcpPage.hpp"
#include "AgentViewTypes.hpp"
#include "InlineMessage.hpp"
#include "OverlayHost.hpp"
#include "AgentComposer.hpp"
#include "AgentMessageView.hpp"
#include "AgentPage.hpp"
#include "AgentSettingsPage.hpp"
#include "AgentToolCard.hpp"
#include "InlineMessage.hpp"
#include "agent/OpenAiCompatibleClient.hpp"
#include "OverlayHost.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QUuid>


namespace {

AgentView::MessageStatus viewStatus(Agent::MessageStatus status) {
    switch (status) {
    case Agent::MessageStatus::Streaming: return AgentView::MessageStatus::Streaming;
    case Agent::MessageStatus::Interrupted: return AgentView::MessageStatus::Interrupted;
    case Agent::MessageStatus::Error: return AgentView::MessageStatus::Error;
    case Agent::MessageStatus::ToolPending: return AgentView::MessageStatus::ToolPending;
    case Agent::MessageStatus::AwaitingApproval: return AgentView::MessageStatus::AwaitingApproval;
    case Agent::MessageStatus::Complete: break;
    }
    return AgentView::MessageStatus::Complete;
}

AgentView::Message viewMessage(const Agent::Message &message) {
    AgentView::Message view;
    view.id = message.id;
    view.user = message.role == Agent::Role::User;
    view.content = message.content;
    view.reasoning = message.reasoning;
    view.status = viewStatus(message.status);
    view.errorText = message.errorText;
    return view;
}

QList<AgentView::Message> viewMessages(const QList<Agent::Message> &messages) {
    QList<AgentView::Message> views;
    views.reserve(messages.size());
    for (const Agent::Message &message : messages) {
        if (message.role == Agent::Role::Tool) continue;  // 工具结果用 tool card 呈现
        views.append(viewMessage(message));
    }
    return views;
}

AgentView::SessionMeta viewMeta(const AgentSessionMeta &meta) {
    AgentView::SessionMeta view;
    view.id = meta.id;
    view.title = meta.title;
    view.updatedAt = meta.updatedAt;
    view.pinned = meta.pinned;
    view.encrypted = meta.encrypted;
    view.corrupt = meta.corrupt;
    return view;
}

QList<AgentView::SessionMeta> viewMetas(const QList<AgentSessionMeta> &metas) {
    QList<AgentView::SessionMeta> views;
    views.reserve(metas.size());
    for (const AgentSessionMeta &meta : metas) views.append(viewMeta(meta));
    return views;
}

AgentView::ProviderDraft viewProfile(const AgentProviderProfile &profile) {
    AgentView::ProviderDraft draft;
    draft.id = profile.id;
    draft.name = profile.name;
    draft.baseUrl = profile.baseUrl;
    draft.model = profile.model;
    draft.contextWindow = profile.contextWindow;
    draft.maxOutput = profile.maxOutput;
    draft.temperature = profile.temperature;
    draft.timeoutSec = profile.timeoutSec;
    draft.deployment = profile.deployment;
    draft.authMode = profile.authMode;
    draft.stream = profile.stream;
    draft.maxTokensField = profile.maxTokensField;
    draft.systemRole = profile.systemRole;
    draft.reasoningEcho = profile.reasoningEcho;
    draft.sendTemperature = profile.sendTemperature;
    draft.sendToolChoice = profile.sendToolChoice;
    draft.localHttpAllowed = profile.localHttpAllowed;
    return draft;
}

AgentProviderProfile coreProfile(const AgentView::ProviderDraft &draft) {
    AgentProviderProfile profile;
    profile.id = draft.id;
    profile.name = draft.name;
    profile.baseUrl = draft.baseUrl;
    profile.model = draft.model;
    profile.contextWindow = draft.contextWindow;
    profile.maxOutput = draft.maxOutput;
    profile.temperature = draft.temperature;
    profile.timeoutSec = draft.timeoutSec;
    profile.deployment = draft.deployment;
    profile.authMode = draft.authMode;
    profile.stream = draft.stream;
    profile.maxTokensField = draft.maxTokensField;
    profile.systemRole = draft.systemRole;
    profile.reasoningEcho = draft.reasoningEcho;
    profile.sendTemperature = draft.sendTemperature;
    profile.sendToolChoice = draft.sendToolChoice;
    profile.localHttpAllowed = draft.localHttpAllowed;
    return profile;
}

}  // namespace

AgentController::AgentController(AgentPage *page, OverlayHost *overlay, QObject *parent)
    : QObject(parent),
      m_page(page),
      m_overlay(overlay),
      m_store(),
      m_artifacts(),
      m_backups(),
      m_files(&m_backups) {
    QString problem;
    if (!AgentPaths::ensureDataDir(&problem) && m_page != nullptr) {
        m_page->showNotice(problem);
    }
    m_ownedClient = new OpenAiCompatibleClient(this);
    m_client = m_ownedClient;
    m_loop = new AgentLoop(m_client, this);
    m_toolEngine = new AgentFileToolEngine(
        &m_files, &m_artifacts, [this](QString *problem) { return toolContext(problem); });
    m_toolEngine->setStrictEgress(AgentSettings::strictEgress());
    // Stage 2：文件工具 + 启动器工具 + 工作流工具合成一个引擎交给 Loop。技能白名单与
    // “写步骤必须在已批准计划里”这两道闸在复合层，子引擎不知道当前是哪个模式。
    m_workflow.setSkill(AgentSkillRegistry::find(QStringLiteral("general")));
    m_launcherTools = new AgentLauncherTools(
        nullptr, [this](QString *problem) { return toolContext(problem); });
    m_launcherTools->setStrictEgress(AgentSettings::strictEgress());
    m_workflowEngine = new AgentWorkflowEngine(&m_workflow);
    // Stage 2A：MCP。确认闸在 McpTrustStore，生命周期在 McpManager；这里的引擎只管
    // 逐次审批与异步交付。配置读不出来就当作没有服务器，不阻断其它功能。
    QString mcpProblem;
    m_mcpManager = new McpManager(&m_mcpStore, this);
    if (!m_mcpManager->reload(&mcpProblem) && m_page != nullptr) {
        m_page->showNotice(QStringLiteral("MCP 配置读取失败：%1").arg(mcpProblem));
    }
    m_mcpTools = new AgentMcpToolEngine(
        m_mcpManager, [this](QString *problem) { return toolContext(problem); }, this);
    // Stage 3：联网与固定诊断命令。联网工具强制直连 + pinned-IP；诊断命令默认关闭。
    m_webTransport = new QtAgentWebTransport(this);
    m_webTools = new AgentWebTools(
        m_webTransport, [this](QString *problem) { return toolContext(problem); }, this);
    m_commandTools = new AgentCommandTools(
        [this](QString *problem) { return toolContext(problem); }, this);
    m_compositeEngine = new AgentCompositeToolEngine;
    m_compositeEngine->addEngine(m_toolEngine);
    m_compositeEngine->addEngine(m_launcherTools);
    m_compositeEngine->addEngine(m_workflowEngine);
    m_compositeEngine->addEngine(m_mcpTools);
    m_compositeEngine->addEngine(m_webTools);
    m_compositeEngine->addEngine(m_commandTools);
    m_compositeEngine->setWorkflow(&m_workflow);
    m_loop->setToolEngine(m_compositeEngine);
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(2000);
    connect(m_saveTimer, &QTimer::timeout, this, [this] {
        if (m_savePending) flushSave(false);
    });

    const QList<AgentProviderProfile> profiles = AgentSettings::profiles();
    const QString active = AgentSettings::activeProfileId();
    for (const AgentProviderProfile &profile : profiles) {
        if (profile.id == active) {
            m_profile = profile;
            break;
        }
    }
    if (!m_profile.isValid() && !profiles.isEmpty()) {
        m_profile = profiles.constFirst();
    }
    if (m_profile.id.isEmpty()) {
        m_profile.id = QStringLiteral("default");
        m_profile.name = QCoreApplication::translate("AgentController", "默认模型服务");
        m_profile.baseUrl = QStringLiteral("https://api.openai.com/v1");
        m_profile.model = QStringLiteral("gpt-4o-mini");
    }

    wirePage();
    connect(m_loop, &AgentLoop::stateChanged, this, [this](AgentLoop::State state) {
        const bool busy = state == AgentLoop::State::Preparing ||
                          state == AgentLoop::State::Requesting;
        if (m_page != nullptr) m_page->setStreaming(busy);
        if (busy) refreshApiState();
    });
    connect(m_loop, &AgentLoop::assistantUpdated, this,
            [this](const Agent::Message &message) { upsertAssistant(message); });
    connect(m_loop, &AgentLoop::turnFinished, this, [this](const Agent::Message &message) {
        upsertAssistant(message);
        m_session.pendingApproval = {};
        m_saveTimer->stop();
        flushSave(true);
        if (m_page != nullptr) {
            m_page->setStreaming(false);
            m_page->hideApproval();
            // 出错的那句已经在消息行上（带「重试」），不再弹居中提示条重复一遍；
            // 只有这条消息没 id、进不了消息列（upsertAssistant 直接丢）时才用提示条兜底。
            if (message.id.isEmpty() && !message.errorText.isEmpty()) m_page->showNotice(message.errorText);
        }
        refreshSessions();
    });
    connect(m_loop, &AgentLoop::turnFailed, this, [this](const Agent::Error &error) {
        m_session.pendingApproval = {};
        if (m_page != nullptr) {
            m_page->setStreaming(false);
            m_page->hideApproval();
            m_page->showNotice(error.message);
        }
    });
    connect(m_loop, &AgentLoop::usageUpdated, this, [this](const Agent::Usage &usage) {
        m_session.usage = usage;
    });
    connect(m_loop, &AgentLoop::messageAdded, this, [this](const Agent::Message &message) {
        if (message.role == Agent::Role::Tool) {
            m_session.messages.append(message);
            scheduleSave();
        }
    });
    connect(m_loop, &AgentLoop::toolCallStarted, this, [this](const Agent::ToolCall &call) {
        if (m_page == nullptr) return;
        AgentView::ToolCard card;
        card.id = call.id;
        card.title = call.name;
        card.summary = QCoreApplication::translate("AgentController", "参数已接收");
        card.detail = QString::fromUtf8(
            QJsonDocument(call.arguments).toJson(QJsonDocument::Indented));
        card.status = AgentView::ToolCardStatus::Running;
        m_page->upsertToolCard(card);
    });
    connect(m_loop, &AgentLoop::toolCallFinished, this,
            [this](const Agent::ToolCall &call, const AgentToolResult &result) {
                if (m_page == nullptr) return;
                if (result.data.value(QStringLiteral("autoReadNotice")).toBool(false)) {
                    const QString host = QUrl(m_profile.baseUrl).host();
                    if (m_autoNoticeShownFor != host) {
                        m_autoNoticeShownFor = host;
                        m_page->showNotice(
                            QCoreApplication::translate("AgentController", "自动读取的以下日志或元数据可能发送给当前模型服务：%1")
                                .arg(host));
                    }
                }
                AgentView::ToolCard card;
                card.id = call.id;
                card.title = call.name;
                card.summary = result.summary;
                card.detail = QString::fromUtf8(
                    QJsonDocument(result.data).toJson(QJsonDocument::Indented));
                card.status = result.status == AgentToolResult::Status::Ok
                                  ? AgentView::ToolCardStatus::Done
                                  : AgentView::ToolCardStatus::Error;
                m_page->upsertToolCard(card);
            });
    connect(m_loop, &AgentLoop::approvalRequested, this,
            [this](const QString &approvalId, const Agent::ToolCall &call,
                   const QJsonObject &request) {
                Q_UNUSED(call);
                m_session.pendingApproval = request;
                m_session.pendingApproval.insert(QStringLiteral("approvalId"), approvalId);
                flushSave(true);
                if (m_page != nullptr) m_page->showApproval(m_session.pendingApproval);
            });

    loadInitialSession();
    refreshApiState();
}

AgentController::~AgentController() { shutdown(); }

void AgentController::wirePage() {
    if (m_page == nullptr) return;
    connect(m_page, &AgentPage::newSessionRequested, this, &AgentController::onNewSession);
    connect(m_page, &AgentPage::sessionSelected, this, &AgentController::selectSession);
    connect(m_page, &AgentPage::sendRequested, this, &AgentController::onSend);
    connect(m_page, &AgentPage::stopRequested, this, &AgentController::onStop);
    connect(m_page, &AgentPage::settingsRequested, this, &AgentController::openSettings);
    connect(m_page, &AgentPage::retryRequested, this, &AgentController::onRetry);
    connect(m_page, &AgentPage::sessionRenameRequested, this, &AgentController::onRename);
    connect(m_page, &AgentPage::sessionPinRequested, this, &AgentController::onPin);
    connect(m_page, &AgentPage::sessionDeleteRequested, this, &AgentController::onDelete);
    connect(m_page, &AgentPage::encryptToggled, this, &AgentController::onEncryptToggled);
    connect(m_page, &AgentPage::approvalAllowed, this, [this](const QString &) { onApproval(true); });
    connect(m_page, &AgentPage::approvalDenied, this, [this](const QString &) { onApproval(false); });
    connect(m_page, &AgentPage::instanceChipRequested, this, &AgentController::onInstanceMenuRequested);
    connect(m_page, &AgentPage::instanceChosen, this, &AgentController::onInstanceChosen);
    connect(m_page, &AgentPage::sessionExportRequested, this, &AgentController::onExport);
    connect(m_page, &AgentPage::skillChosen, this, &AgentController::onSkillChosen);
    connect(m_loop, &AgentLoop::toolCallFinished, this,
            [this](const Agent::ToolCall &call, const AgentToolResult &result) {
                Q_UNUSED(call);
                Q_UNUSED(result);
                if (m_page != nullptr) m_page->setWorkflowStatus(m_workflow.statusLine());
            });
}

void AgentController::shutdown() {
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    if (m_saveTimer != nullptr) m_saveTimer->stop();
    if (m_loop != nullptr && m_loop->busy()) {
        m_loop->cancel();
    }
    if (!m_session.meta.id.isEmpty()) {
        m_store.save(m_session, false, nullptr);
    }
    if (m_settingsPage != nullptr) {
        m_settingsPage->close();
    }
    stopMcpServers();
    if (m_toolEngine != nullptr) {
        m_toolEngine->cancelPending();
        if (m_loop != nullptr) m_loop->setToolEngine(nullptr);
        delete m_compositeEngine;
        m_compositeEngine = nullptr;
        delete m_launcherTools;
        m_launcherTools = nullptr;
        delete m_workflowEngine;
        m_workflowEngine = nullptr;
        delete m_mcpTools;
        m_mcpTools = nullptr;
        delete m_webTools;
        m_webTools = nullptr;
        delete m_commandTools;
        m_commandTools = nullptr;
        delete m_toolEngine;
        m_toolEngine = nullptr;
    }
}

void AgentController::setModelClientForTest(AgentModelClient *client) {
    if (client == nullptr || client == m_client) return;
    if (m_loop->busy()) m_loop->cancel();
    if (m_ownedClient != nullptr) {
        m_ownedClient->deleteLater();
        m_ownedClient = nullptr;
    }
    m_client = client;
    m_loop->setClient(client);
}

void AgentController::applyProfileForTest(const AgentProviderProfile &profile,
                                          const QString &apiKey) {
    m_profile = profile;
    m_testApiKey = apiKey;
    refreshApiState();
}

QString AgentController::currentApiKey() const {
    if (!m_testApiKey.isEmpty()) return m_testApiKey;
    if (m_profile.id.isEmpty()) return {};
    return AgentSecretStore::read(m_profile.id).secret;
}

void AgentController::refreshApiState() {
    QString status;
    if (!m_profile.isValid()) {
        m_configured = false;
        status = QCoreApplication::translate("AgentController", "API 未配置");
    } else {
        QString problem;
        const bool valid = AgentProviderAdapters::validateProfile(m_profile, &problem);
        const QString key = currentApiKey();
        const bool keyOk = !key.isEmpty() || m_profile.authMode == QLatin1String("none");
        m_configured = valid && keyOk;
        if (!valid) {
            status = QCoreApplication::translate("AgentController", "配置无效：%1").arg(problem);
        } else if (!keyOk) {
            status = QCoreApplication::translate("AgentController", "API 未配置（缺少 API Key）");
        } else if (m_profile.authMode == QLatin1String("none")) {
            status = QCoreApplication::translate("AgentController", "本地服务（无认证）");
        } else {
            status = QCoreApplication::translate("AgentController", "API 已配置");
        }
    }
    if (m_page != nullptr) {
        m_page->setConfigured(m_configured, status);
        const QString modelLabel =
            m_profile.isValid() ? QStringLiteral("%1 · %2").arg(m_profile.name, m_profile.model)
                                : QCoreApplication::translate("AgentController", "未配置模型");
        m_page->setModelLabel(modelLabel);
    }
}

void AgentController::loadInitialSession() {
    QStringList problems;
    m_store.finalizePendingDeletions(&problems);
    QList<AgentSessionMeta> metas;
    m_store.list(&metas, nullptr);
    if (!problems.isEmpty() && m_page != nullptr) {
        m_page->showNotice(problems.join(QLatin1Char('\n')));
    }
    if (metas.isEmpty()) {
        m_session = {};
        if (m_page != nullptr) {
            m_page->setSessions({});
            m_page->setCurrentSession(QString());
            m_page->showEmptyState(true);
        }
        return;
    }
    if (m_page != nullptr) m_page->setSessions(viewMetas(metas));
    selectSession(metas.constFirst().id);
}

/// 从一条没动过的空「新对话」切走时把它丢掉：「+」复用空会话只管「别再建」，这里管「别攒下」
/// （r104：重启后列表里留了两条空「新对话」）。改过名 / 固定 / 加密 / 关联了实例的都算动过，留着。
/// 认标题一律用 `AgentSessionTitle::isDefault`：这里原来写的是 `QLatin1String("新对话")`，
/// UTF-8 字节被当成 Latin-1，跟盘上读回的标题永远不相等，条件一次都没成立过（r109）。
void AgentController::discardBlankCurrent(const QString &nextId) {
    const AgentSessionMeta &meta = m_session.meta;
    if (meta.id.isEmpty() || meta.id == nextId || !m_session.messages.isEmpty()) return;
    if (meta.pinned || meta.encrypted || meta.instanceLock.isValid() ||
        !AgentSessionTitle::isDefault(meta.title)) {
        return;
    }
    const QString id = meta.id;
    QString problem;
    if (m_store.requestDeletion(id, &problem)) m_store.completeDeletion(id, &problem, nullptr);
    m_session = {};
    m_savePending = false;
    m_saveTimer->stop();
    refreshSessions();
}

void AgentController::refreshSessions() {
    QList<AgentSessionMeta> metas;
    m_store.list(&metas, nullptr);
    if (m_page != nullptr) m_page->setSessions(viewMetas(metas));
}

void AgentController::selectSession(const QString &sessionId) {
    if (sessionId.isEmpty()) return;
    if (m_loop->busy()) {
        m_loop->cancel();
        flushSave(true);
    }
    discardBlankCurrent(sessionId);
    AgentSession loaded;
    QString problem;
    if (!m_store.load(sessionId, &loaded, &problem)) {
        if (m_page != nullptr) m_page->showNotice(problem);
        refreshSessions();
        return;
    }
    m_session = loaded;
    if (!m_session.pendingApproval.isEmpty()) {
        // 重启/切回后旧审批不能自动执行，只能作废并重新请求。
        m_session.pendingApproval = {};
        m_store.save(m_session, false, nullptr);
        if (m_page != nullptr) {
            m_page->showNotice(QCoreApplication::translate("AgentController", "上次未完成的审批已作废；需要重新请求并确认。"));
        }
    }
    m_savePending = false;
    m_saveTimer->stop();
    updateHeader();
    // 会话里记着它上一轮用哪个模式；换会话时工作流从头开始，旧计划不留。
    m_workflow.setSkill(AgentSkillRegistry::find(m_session.meta.skill));
    refreshSkillUi();
}

void AgentController::updateHeader() {
    if (m_page == nullptr) return;
    m_page->setCurrentSession(m_session.meta.id);
    m_page->setTitle(m_session.meta.title);
    m_page->setMessages(viewMessages(m_session.messages));
    m_page->setToolCards(toolCardsFromHistory());
    if (!m_session.pendingApproval.isEmpty()) {
        m_page->showApproval(m_session.pendingApproval);
    } else {
        m_page->hideApproval();
    }
    m_page->setEncryptionChecked(m_session.meta.encrypted);
    m_page->setStreaming(m_loop->busy());
    updateInstanceChip();
}

void AgentController::updateConfiguredState() {
    refreshApiState();
    if (m_page != nullptr) {
        m_page->setConfigured(m_configured, m_configured ? QCoreApplication::translate("AgentController", "API 已配置")
                                                        : QCoreApplication::translate("AgentController", "API 未配置"));
    }
}

void AgentController::scheduleSave() {
    m_savePending = true;
    if (m_saveTimer != nullptr && !m_saveTimer->isActive()) m_saveTimer->start();
}

void AgentController::flushSave(bool snapshot) {
    if (m_session.meta.id.isEmpty()) return;
    m_saveTimer->stop();
    m_savePending = false;
    QString problem;
    if (!m_store.save(m_session, snapshot, &problem)) {
        if (m_page != nullptr) m_page->showNotice(problem);
    }
}

void AgentController::upsertAssistant(const Agent::Message &message) {
    if (message.id.isEmpty()) return;
    bool replaced = false;
    for (Agent::Message &existing : m_session.messages) {
        if (existing.id == message.id) {
            existing = message;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        m_session.messages.append(message);
    }
    if (m_page != nullptr) m_page->upsertMessage(viewMessage(message));
    scheduleSave();
}

void AgentController::removeTrailingFailedAssistant() {
    while (!m_session.messages.isEmpty() &&
           m_session.messages.constLast().role == Agent::Role::Assistant &&
           m_session.messages.constLast().status != Agent::MessageStatus::Complete) {
        const QString id = m_session.messages.constLast().id;
        m_session.messages.removeLast();
        if (m_page != nullptr) m_page->removeMessage(id);
    }
}

QString AgentController::configurationProblem() const {
    QString problem;
    if (!AgentProviderAdapters::validateProfile(m_profile, &problem)) {
        return problem;
    }
    if (currentApiKey().isEmpty() && m_profile.authMode != QLatin1String("none")) {
        return QCoreApplication::translate("AgentController", "没有可用的 API Key，请先在设置里配置。");
    }
    return {};
}

void AgentController::onNewSession() {
    if (m_loop->busy()) {
        m_loop->cancel();
        flushSave(true);
    }
    // 空会话复用：当前这条还没内容就继续用它；否则盘上已有现成的空「新对话」就切过去，
    // 不再多建一条同名空会话。
    if (m_session.meta.id.isEmpty() || !m_session.messages.isEmpty() || m_session.meta.instanceLock.isValid()) {
        QString reuseId;
        QList<AgentSessionMeta> metas;
        m_store.list(&metas, nullptr);
        for (const AgentSessionMeta &meta : metas) {
            if (meta.messageCount == 0 && !meta.corrupt && !meta.archived && !meta.encrypted &&
                meta.deletionState == QLatin1String("none") && !meta.instanceLock.isValid() &&
                AgentSessionTitle::isDefault(meta.title)) {
                reuseId = meta.id;
                break;
            }
        }
        if (!reuseId.isEmpty()) {
            selectSession(reuseId);
            if (m_session.meta.id == reuseId) {
                refreshSessions();
                refreshApiState();
                return;
            }
        }
        AgentSession created;
        QString problem;
        if (!m_store.create(&created, &problem)) {
            if (m_page != nullptr) m_page->showNotice(problem);
            return;
        }
        m_session = created;
    }
    refreshSessions();
    updateHeader();
    refreshApiState();
}

void AgentController::onSend(const QString &text) {
    if (m_loop->busy()) return;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return;

    refreshApiState();
    const QString problem = configurationProblem();
    if (!problem.isEmpty()) {
        if (m_page != nullptr) m_page->showNotice(problem);
        openSettings();
        return;
    }
    if (m_session.meta.id.isEmpty()) {
        AgentSession created;
        QString createProblem;
        if (!m_store.create(&created, &createProblem)) {
            if (m_page != nullptr) m_page->showNotice(createProblem);
            return;
        }
        m_session = created;
    }

    removeTrailingFailedAssistant();
    const AgentRedaction::Result redacted = AgentRedaction::redact(trimmed);
    if (redacted.containsCredentials()) {
        if (m_page != nullptr) {
            m_page->showNotice(QCoreApplication::translate("AgentController", "检测到疑似凭据（%1），已在本机遮挡后再发送。")
                                   .arg(redacted.kinds.join(QCoreApplication::translate("AgentController", "、"))));
        }
    }
    const QString content = redacted.text;
    if (content.trimmed().isEmpty()) {
        if (m_page != nullptr) m_page->showNotice(QCoreApplication::translate("AgentController", "消息只包含凭据，已取消发送。"));
        return;
    }

    Agent::Message user;
    user.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    user.role = Agent::Role::User;
    user.content = content;
    user.status = Agent::MessageStatus::Complete;
    user.createdAt = QDateTime::currentDateTimeUtc();
    m_session.messages.append(user);
    if (m_session.meta.title.isEmpty() || m_session.meta.title == QCoreApplication::translate("AgentController", "新对话")) {
        m_session.meta.title = AgentSessionTitle::fromMessage(content);
    }
    m_session.meta.provider = QJsonObject{{QStringLiteral("profileId"), m_profile.id},
                                          {QStringLiteral("model"), m_profile.model},
                                          {QStringLiteral("deployment"), m_profile.deployment}};
    if (m_page != nullptr) {
        m_page->upsertMessage(viewMessage(user));
        m_page->setTitle(m_session.meta.title);
        m_page->composer()->clear();
        m_page->showNotice(QString());
    }
    flushSave(true);
    refreshSessions();

    applySkillToLoop();
    m_loop->setHistory(m_session.messages);
    m_loop->start();
}

void AgentController::onStop() {
    if (!m_loop->busy()) return;
    m_loop->cancel();
    m_saveTimer->stop();
    flushSave(true);
    if (m_page != nullptr) {
        m_page->setStreaming(false);
        m_page->showNotice(QCoreApplication::translate("AgentController", "已停止生成，已保留收到的部分。"));
    }
    refreshSessions();
}

void AgentController::onRetry(const QString &messageId) {
    if (m_loop->busy()) return;
    if (m_session.messages.isEmpty()) return;
    const Agent::Message last = m_session.messages.constLast();
    if (last.id != messageId || last.role != Agent::Role::Assistant) return;
    removeTrailingFailedAssistant();
    flushSave(false);
    applySkillToLoop();
    m_loop->setHistory(m_session.messages);
    m_loop->start();
}

void AgentController::onRename(const QString &sessionId, const QString &title) {
    QString problem;
    if (!m_store.rename(sessionId, title, &problem)) {
        if (m_page != nullptr) m_page->showNotice(problem);
        return;
    }
    if (m_session.meta.id == sessionId) {
        m_session.meta.title = title.simplified().isEmpty() ? QCoreApplication::translate("AgentController", "新对话")
                                                            : title.simplified().left(80);
        if (m_page != nullptr) m_page->setTitle(m_session.meta.title);
    }
    refreshSessions();
}

void AgentController::onPin(const QString &sessionId, bool pinned) {
    QString problem;
    if (!m_store.setPinned(sessionId, pinned, &problem)) {
        if (m_page != nullptr) m_page->showNotice(problem);
        return;
    }
    if (m_session.meta.id == sessionId) m_session.meta.pinned = pinned;
    refreshSessions();
}

void AgentController::onDelete(const QString &sessionId) {
    if (sessionId.isEmpty()) return;
    const int choice = InlineMessage::ask(
        m_overlay, MessageKind::Question, QCoreApplication::translate("AgentController", "删除会话"),
        QCoreApplication::translate("AgentController", "只会删除会话正文、恢复快照和本会话 artifacts；" "文件维护备份会保留。确定删除吗？"),
        QStringList{QCoreApplication::translate("AgentController", "删除"), QCoreApplication::translate("AgentController", "取消")}, 1, 0);
    if (choice != 0) return;

    if (m_session.meta.id == sessionId && m_loop->busy()) {
        m_loop->cancel();
        if (m_compositeEngine != nullptr && m_compositeEngine->committing()) {
            if (m_page != nullptr) {
                m_page->showNotice(QCoreApplication::translate("AgentController", "文件正在提交，无法安全停止，暂不删除会话。"));
            }
            return;
        }
    }
    QString problem;
    if (!m_store.requestDeletion(sessionId, &problem) ||
        !m_store.completeDeletion(sessionId, &problem, nullptr)) {
        if (m_page != nullptr) m_page->showNotice(problem);
        return;
    }
    if (m_session.meta.id == sessionId) {
        m_session = {};
        m_savePending = false;
        m_saveTimer->stop();
    }
    QList<AgentSessionMeta> metas;
    m_store.list(&metas, nullptr);
    if (metas.isEmpty()) {
        if (m_page != nullptr) {
            m_page->setSessions({});
            m_page->setCurrentSession(QString());
            m_page->clearMessages();
            m_page->setTitle(QCoreApplication::translate("AgentController", "新对话"));
        }
    } else {
        selectSession(metas.constFirst().id);
    }
    refreshSessions();
}

void AgentController::onEncryptToggled(bool encrypted) {
    if (m_session.meta.id.isEmpty()) return;
    QString problem;
    if (!m_store.setEncrypted(m_session.meta.id, encrypted, &problem)) {
        if (m_page != nullptr) {
            m_page->showNotice(problem);
            m_page->setEncryptionChecked(m_session.meta.encrypted);
        }
        return;
    }
    AgentSession reloaded;
    if (m_store.load(m_session.meta.id, &reloaded, &problem)) {
        m_session = reloaded;
    }
    if (m_page != nullptr) {
        m_page->setEncryptionChecked(encrypted);
        m_page->showNotice(encrypted ? QCoreApplication::translate("AgentController", "本会话已加密存储（绑定当前 Windows 用户）")
                                     : QCoreApplication::translate("AgentController", "此会话改为明文存储"));
    }
    refreshSessions();
}

bool AgentController::deleteSessionForTest(const QString &sessionId) {
    if (sessionId.isEmpty()) return false;
    if (m_loop->busy()) m_loop->cancel();
    if (m_compositeEngine != nullptr && m_compositeEngine->committing()) return false;
    QString problem;
    if (!m_store.requestDeletion(sessionId, &problem) ||
        !m_store.completeDeletion(sessionId, &problem, nullptr)) {
        return false;
    }
    if (m_session.meta.id == sessionId) {
        m_session = {};
        m_savePending = false;
        m_saveTimer->stop();
    }
    QList<AgentSessionMeta> metas;
    m_store.list(&metas, nullptr);
    if (metas.isEmpty()) {
        if (m_page != nullptr) {
            m_page->setSessions({});
            m_page->setCurrentSession(QString());
            m_page->clearMessages();
        }
    } else {
        selectSession(metas.constFirst().id);
    }
    refreshSessions();
    return true;
}

void AgentController::setInstanceContextProvider(InstanceProvider provider) {
    m_instanceProvider = std::move(provider);
    m_useTestInstance = false;
    updateInstanceChip();
}

void AgentController::setInstanceListProvider(InstanceListProvider provider) {
    m_instanceListProvider = std::move(provider);
}

void AgentController::setInstanceContextForTest(const QString &rootPath,
                                                const AgentInstanceInfo &info) {
    m_testRootPath = rootPath;
    m_testInstance = info;
    m_useTestInstance = true;
    updateInstanceChip();
}

void AgentController::approveToolForTest(bool approved) { onApproval(approved); }

AgentToolContext AgentController::toolContext(QString *problem) {
    AgentToolContext context;
    context.sessionId = m_session.meta.id;
    context.providerHost = QUrl(m_profile.baseUrl).host();
    context.strictEgress = AgentSettings::strictEgress();
    if (context.sessionId.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentController", "先新建会话再使用文件工具。");
        return context;
    }
    const AgentInstanceInfo current = homeInstance();
    if (!m_session.meta.instanceLock.isValid()) {
        if (!current.isValid() || !current.exists) {
            if (problem != nullptr) {
                *problem = QCoreApplication::translate("AgentController", "NeedsInstanceLock：先在主页选择一个实例；不会自动挑一个。");
            }
            return context;
        }
        QString lockProblem;
        if (!AgentInstanceLocks::lock(&m_session, current, &lockProblem)) {
            if (problem != nullptr) *problem = lockProblem;
            return context;
        }
        QString saveProblem;
        if (!m_store.save(m_session, true, &saveProblem)) {
            m_session.meta.instanceLock = {};
            if (problem != nullptr) *problem = QCoreApplication::translate("AgentController", "保存实例锁定设置失败：%1").arg(saveProblem);
            return context;
        }
        if (m_page != nullptr) {
            m_page->showNotice(QCoreApplication::translate("AgentController", "本会话已锁定实例：%1").arg(current.name));
        }
        refreshSessions();
        updateInstanceChip();
    }
    if (!AgentInstanceLocks::isUsable(m_session.meta.instanceLock, lockedInstance())) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentController", "InstanceUnavailable：锁定实例不在当前游戏目录或已被删除。");
        }
        return context;
    }
    context.instanceId = m_session.meta.instanceLock.id;
    context.instanceName = m_session.meta.instanceLock.name;
    context.gameRootId = m_session.meta.instanceLock.gameRootId;
    context.rootPath = m_useTestInstance ? m_testRootPath : AppPaths::versionDir(context.instanceId);
    if (!QFileInfo::exists(context.rootPath)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentController", "InstanceUnavailable：锁定实例目录不存在。");
    }
    return context;
}

AgentInstanceInfo AgentController::homeInstance() const {
    if (m_useTestInstance) return m_testInstance;
    return m_instanceProvider ? m_instanceProvider() : AgentInstanceInfo{};
}

QList<AgentInstanceInfo> AgentController::instances() const {
    if (!m_useTestInstance && m_instanceListProvider) return m_instanceListProvider();
    // 没接实例清单（测试 / 早期构造）时，可选的只有主页当前那一个。
    const AgentInstanceInfo home = homeInstance();
    return home.isValid() ? QList<AgentInstanceInfo>{home} : QList<AgentInstanceInfo>{};
}

AgentInstanceInfo AgentController::lockedInstance() const {
    const AgentInstanceLock &lock = m_session.meta.instanceLock;
    for (const AgentInstanceInfo &info : instances())
        if (info.id == lock.id) return info;
    return {};
}

void AgentController::updateInstanceChip() {
    if (m_page == nullptr) return;
    const AgentInstanceLock &lock = m_session.meta.instanceLock;
    m_page->setInstanceChip(AgentInstanceLocks::chipText(lock, lock.isValid() ? lockedInstance() : homeInstance()),
                            lock.isValid());
}

void AgentController::onInstanceMenuRequested() {
    if (m_page == nullptr) return;
    QList<AgentView::InstanceOption> options;
    for (const AgentInstanceInfo &info : instances())
        if (info.isValid() && info.exists) options.append({info.id, info.name.isEmpty() ? info.id : info.name});
    m_page->showInstanceMenu(options, m_session.meta.instanceLock.id);
}

void AgentController::onInstanceChosen(const QString &instanceId) {
    if (m_page == nullptr) return;
    if (m_loop->busy() || !m_session.pendingApproval.isEmpty()) {
        m_page->showNotice(QCoreApplication::translate("AgentController", "有待确认或执行中的操作，暂不能切换实例。"));
        return;
    }
    const AgentInstanceLock old = m_session.meta.instanceLock;
    if (instanceId == old.id) return;
    AgentInstanceInfo target;
    if (!instanceId.isEmpty()) {
        for (const AgentInstanceInfo &info : instances())
            if (info.id == instanceId) target = info;
        if (!target.isValid() || !target.exists) {
            m_page->showNotice(QCoreApplication::translate("AgentController", "实例「%1」已不在当前游戏目录。").arg(instanceId));
            return;
        }
    }
    // 已关联过实例、对话里也有了内容：旧实例上的证据和计划不再适用，建议另开会话（D8：切换须显式确认）。
    if (old.isValid() && !m_session.messages.isEmpty() && m_overlay != nullptr) {
        const int choice = InlineMessage::ask(
            m_overlay, MessageKind::Question, QCoreApplication::translate("AgentController", "切换实例"),
            QCoreApplication::translate("AgentController", "本会话的记录属于「%1」，切换后旧计划作废，建议新建会话。").arg(old.name),
            QStringList{QCoreApplication::translate("AgentController", "新建会话"), QCoreApplication::translate("AgentController", "在本会话切换"), QCoreApplication::translate("AgentController", "取消")}, 2, 0);
        if (choice == 0) onNewSession();
        else if (choice != 1) return;
    }
    if (m_session.meta.id.isEmpty()) {
        AgentSession created;
        QString createProblem;
        if (!m_store.create(&created, &createProblem)) {
            m_page->showNotice(createProblem);
            return;
        }
        m_session = created;
    }
    QString problem;
    if (target.isValid()) {
        if (!AgentInstanceLocks::switchTo(&m_session, target, &problem)) {
            m_page->showNotice(problem);
            return;
        }
    } else {
        m_session.meta.instanceLock = {};  // 跟随主页：下一次实例操作时再锁定主页当前实例
    }
    m_session.pendingApproval = {};
    flushSave(true);
    refreshSessions();
    updateHeader();
    m_page->showNotice(target.isValid() ? QCoreApplication::translate("AgentController", "本会话已关联实例：%1").arg(target.name)
                                        : QCoreApplication::translate("AgentController", "本会话改为跟随主页实例"));
}

void AgentController::onExport(const QString &sessionId) {
    if (m_page == nullptr || sessionId.isEmpty()) return;
    AgentSession session = m_session;
    QString problem;
    if (session.meta.id != sessionId && !m_store.load(sessionId, &session, &problem)) {
        InlineMessage::warning(m_overlay, QCoreApplication::translate("AgentController", "导出失败"), problem);
        return;
    }
    QString name = session.meta.title.isEmpty() ? QCoreApplication::translate("AgentController", "新对话") : session.meta.title;
    name.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("_"));
    const QString path = QFileDialog::getSaveFileName(
        m_page->window(), QCoreApplication::translate("AgentController", "导出会话"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).filePath(name + QStringLiteral(".md")),
        QCoreApplication::translate("AgentController", "Markdown (*.md);;所有文件 (*)"));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(AgentSessionExport::toMarkdown(session).toUtf8()) < 0 || !file.commit()) {
        InlineMessage::warning(m_overlay, QCoreApplication::translate("AgentController", "导出失败"),
                               QCoreApplication::translate("AgentController", "无法写入 %1：%2。请换一个位置再试。")
                                   .arg(QDir::toNativeSeparators(path), file.errorString()));
        return;
    }
    m_page->showNotice(QCoreApplication::translate("AgentController", "会话已导出：%1").arg(QDir::toNativeSeparators(path)));
}

void AgentController::stopMcpServers() {
    if (m_mcpManager != nullptr) m_mcpManager->stopAll();
}

AgentView::McpServerDraft AgentController::draftFrom(const McpServerConfig &server) const {
    AgentView::McpServerDraft draft;
    draft.id = server.id;
    draft.name = server.name;
    draft.command = server.command;
    draft.arguments = server.arguments.join(QChar('\n'));
    draft.workingDirectory = server.workingDirectory;
    draft.environmentNames = server.environmentNames.join(QStringLiteral(", "));
    draft.enabled = server.enabled;
    return draft;
}

McpServerConfig AgentController::serverFrom(const AgentView::McpServerDraft &draft) const {
    McpServerConfig server;
    server.id = draft.id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : draft.id;
    server.name = draft.name;
    server.command = draft.command;
    for (const QString &line : draft.arguments.split(QChar('\n'), Qt::SkipEmptyParts)) {
        const QString argument = line.trimmed();
        if (!argument.isEmpty()) server.arguments.append(argument);
    }
    server.workingDirectory = draft.workingDirectory;
    const QStringList names =
        draft.environmentNames.split(QRegularExpression(QStringLiteral("[,;\\s]+")),
                                     Qt::SkipEmptyParts);
    for (const QString &name : names) {
        if (!name.trimmed().isEmpty()) server.environmentNames.append(name.trimmed());
    }
    server.enabled = draft.enabled;
    return server;
}

void AgentController::refreshMcpPage() {
    if (m_mcpPage == nullptr || m_mcpManager == nullptr) return;
    QList<AgentView::McpServerRow> rows;
    for (const McpServerConfig &server : m_mcpManager->servers()) {
        AgentView::McpServerRow row;
        row.id = server.id;
        row.name = server.name;
        row.command = server.command;
        row.enabled = server.enabled;
        row.state = McpClient::stateName(m_mcpManager->state(server.id));
        row.detail = m_mcpManager->stateDetail(server.id);
        McpConfirmation confirmation;
        if (m_mcpStore.confirmation(server.id, &confirmation) &&
            confirmation.configHash == server.configHash()) {
            row.identityShort = confirmation.identity.sha256.left(12);
        }
        for (const McpToolDescriptor &tool : m_mcpManager->tools()) {
            if (tool.serverId == server.id) ++row.toolCount;
        }
        rows.append(row);
    }
    m_mcpPage->setServers(rows);
}

bool AgentController::confirmAndStart(const QString &serverId, bool reconnect) {
    if (m_mcpManager == nullptr) return false;
    McpTrustStore::GateResult gate;
    QString problem;
    const McpManager::StartResult result =
        reconnect ? m_mcpManager->reconnect(serverId, &gate, &problem)
                  : m_mcpManager->start(serverId, &gate, &problem);
    if (result == McpManager::StartResult::Blocked) {
        if (m_page != nullptr) m_page->showNotice(problem);
        if (m_mcpPage != nullptr) m_mcpPage->setLog(problem);
        refreshMcpPage();
        return false;
    }
    if (result == McpManager::StartResult::Started) {
        refreshMcpPage();
        return true;
    }
    QString text = QStringLiteral("服务器：%1\n%2\n\n参数：%3\n工作目录：%4\n环境变量：%5\n\n"
                                  "该服务器以当前 Windows 用户权限运行，可能读写本机文件和网络；"
                                  "只添加你信任的服务器。")
                       .arg(gate.server.name, gate.identity.summary(),
                            gate.server.arguments.isEmpty()
                                ? QStringLiteral("（无）")
                                : gate.server.arguments.join(QStringLiteral("  ")),
                            gate.server.workingDirectory.isEmpty()
                                ? QStringLiteral("（agent 数据目录）")
                                : gate.server.workingDirectory,
                            gate.server.environmentNames.isEmpty()
                                ? QStringLiteral("（不继承）")
                                : gate.server.environmentNames.join(QStringLiteral("、")));
    if (!gate.reason.isEmpty()) text = gate.reason + QStringLiteral("\n\n") + text;
    const int choice = InlineMessage::ask(
        m_overlay, MessageKind::Question,
        reconnect ? QStringLiteral("重新连接 MCP 服务器") : QStringLiteral("启动 MCP 服务器"),
        text, QStringList{QStringLiteral("确认并连接"), QStringLiteral("取消")}, 1, 1);
    if (choice != 0) {
        if (m_mcpPage != nullptr) m_mcpPage->setLog(QStringLiteral("已取消：没有启动服务器。"));
        refreshMcpPage();
        return false;
    }
    McpTrustStore::GateResult confirmed;
    if (!m_mcpStore.confirm(serverId,
                            reconnect ? QStringLiteral("reconnect") : QStringLiteral("start"),
                            &confirmed, &problem)) {
        if (m_page != nullptr) m_page->showNotice(problem);
        refreshMcpPage();
        return false;
    }
    const McpManager::StartResult started = m_mcpManager->startConfirmed(serverId, &problem);
    if (started != McpManager::StartResult::Started) {
        if (m_page != nullptr) m_page->showNotice(problem);
        if (m_mcpPage != nullptr) m_mcpPage->setLog(problem);
    }
    refreshMcpPage();
    return started == McpManager::StartResult::Started;
}

void AgentController::onMcpConnect(const QString &serverId) {
    const bool wasConnected = m_mcpManager != nullptr &&
                              m_mcpManager->state(serverId) != McpClient::State::Idle;
    confirmAndStart(serverId, wasConnected);
}

void AgentController::onMcpDisconnect(const QString &serverId) {
    if (m_mcpManager == nullptr) return;
    m_mcpManager->stop(serverId);
    if (m_mcpPage != nullptr) m_mcpPage->setLog(QStringLiteral("已断开。重新连接前会再次要求确认。"));
    refreshMcpPage();
}

void AgentController::onMcpAdd() {
    if (m_mcpManager == nullptr || m_mcpPage == nullptr) return;
    AgentView::McpServerDraft draft;
    draft.enabled = true;
    if (!AgentMcpPage::editDraft(m_mcpPage, QStringLiteral("添加 MCP 服务器"), &draft)) return;
    const McpServerConfig server = serverFrom(draft);
    const QString problem = McpTrustStore::validateConfig(server);
    if (!problem.isEmpty()) {
        m_mcpPage->setLog(problem);
        return;
    }
    QString saveProblem;
    if (!m_mcpStore.upsert(server, &saveProblem)) {
        m_mcpPage->setLog(saveProblem);
        return;
    }
    m_mcpManager->reload(&saveProblem);
    m_mcpPage->setLog(QStringLiteral("已保存「%1」，首次启动前会确认可执行文件身份。")
                          .arg(server.name));
    refreshMcpPage();
}

void AgentController::onMcpEdit(const QString &serverId) {
    if (m_mcpManager == nullptr || m_mcpPage == nullptr) return;
    McpServerConfig existing;
    for (const McpServerConfig &server : m_mcpManager->servers()) {
        if (server.id == serverId) existing = server;
    }
    if (!existing.isValid()) return;
    AgentView::McpServerDraft draft = draftFrom(existing);
    if (!AgentMcpPage::editDraft(m_mcpPage, QStringLiteral("编辑 MCP 服务器"), &draft)) return;
    const McpServerConfig updated = serverFrom(draft);
    const QString problem = McpTrustStore::validateConfig(updated);
    if (!problem.isEmpty()) {
        m_mcpPage->setLog(problem);
        return;
    }
    if (m_mcpManager->state(serverId) != McpClient::State::Idle) {
        // 改配置就断开：重启必须重新确认，不能拿旧确认继续跑。
        m_mcpManager->stop(serverId);
    }
    QString saveProblem;
    if (!m_mcpStore.upsert(updated, &saveProblem)) {
        m_mcpPage->setLog(saveProblem);
        return;
    }
    m_mcpManager->reload(&saveProblem);
    m_mcpPage->setLog(QStringLiteral("已保存「%1」；配置变了，下次启动要重新确认。")
                          .arg(updated.name));
    refreshMcpPage();
}

void AgentController::onMcpRemove(const QString &serverId) {
    if (m_mcpManager == nullptr || m_mcpPage == nullptr) return;
    const int choice = InlineMessage::ask(
        m_overlay, MessageKind::Question, QStringLiteral("删除 MCP 服务器"),
        QStringLiteral("删除这个服务器的配置和确认记录；只删配置，不删你机器上的程序。"),
        QStringList{QStringLiteral("删除"), QStringLiteral("取消")}, 1, 1);
    if (choice != 0) return;
    m_mcpManager->stop(serverId);
    QString problem;
    if (!m_mcpStore.remove(serverId, &problem)) {
        m_mcpPage->setLog(problem);
        return;
    }
    m_mcpManager->reload(&problem);
    m_mcpPage->setLog(QStringLiteral("已删除。"));
    refreshMcpPage();
}

void AgentController::openMcpSettings() {
    if (m_overlay == nullptr || m_mcpManager == nullptr) return;
    if (m_mcpPage == nullptr) {
        auto *page = new AgentMcpPage(nullptr);
        m_mcpPage = page;
        connect(page, &AgentMcpPage::addRequested, this, &AgentController::onMcpAdd);
        connect(page, &AgentMcpPage::editRequested, this, &AgentController::onMcpEdit);
        connect(page, &AgentMcpPage::removeRequested, this, &AgentController::onMcpRemove);
        connect(page, &AgentMcpPage::connectRequested, this, &AgentController::onMcpConnect);
        connect(page, &AgentMcpPage::disconnectRequested, this, &AgentController::onMcpDisconnect);
        connect(page, &AgentMcpPage::refreshRequested, this, &AgentController::refreshMcpPage);
        connect(m_mcpManager, &McpManager::serversChanged, this, &AgentController::refreshMcpPage);
        connect(m_mcpManager, &McpManager::toolsChanged, this, &AgentController::refreshMcpPage);
        connect(m_mcpManager, &McpManager::serverStateChanged, this,
                [this](const QString &, const QString &, const QString &detail) {
                    if (m_mcpPage != nullptr && !detail.isEmpty()) m_mcpPage->setLog(detail);
                    refreshMcpPage();
                });
        connect(m_mcpManager, &McpManager::logLine, this,
                [this](const QString &serverId, const QString &text) {
                    if (m_mcpPage != nullptr) {
                        m_mcpPage->setLog(QStringLiteral("%1：%2").arg(serverId, text));
                    }
                });
    }
    m_mcpManager->reload(nullptr);
    refreshMcpPage();
    m_overlay->showPage(m_mcpPage, QStringLiteral("MCP 服务器"));
}

void AgentController::setLauncherHost(AgentLauncherHost *host) {
    m_launcherHost = host;
    if (m_launcherTools != nullptr) m_launcherTools->setHost(host);
}

void AgentController::applySkillToLoop() {
    AgentLoop::Config config = m_loop->config();
    config.profile = m_profile;
    config.apiKey = currentApiKey();
    QString prompt = m_workflow.skill().systemPrompt;
    const QString status = m_workflow.statusLine();
    if (!status.isEmpty()) {
        prompt += QStringLiteral("\n\n当前工作流状态（本地记账，不要复述给用户）：%1。").arg(status);
    }
    config.systemPrompt = prompt;
    m_loop->setConfig(config);
}

void AgentController::refreshSkillUi() {
    if (m_page == nullptr) return;
    QList<AgentView::SkillOption> options;
    for (const QString &id : AgentSkillRegistry::orderedIds()) {
        const AgentSkill skill = AgentSkillRegistry::find(id);
        if (skill.id.isEmpty()) continue;
        options.append(AgentView::SkillOption{skill.id, skill.name, skill.description});
    }
    m_page->setSkills(options, m_workflow.skill().id);
}

void AgentController::onSkillChosen(const QString &skillId) {
    const AgentSkill skill = AgentSkillRegistry::find(skillId);
    if (skill.id.isEmpty()) return;
    if (m_loop->busy()) {
        if (m_page != nullptr) m_page->showNotice(QStringLiteral("先停止当前回复再切换模式。"));
        return;
    }
    if (m_workflow.skill().id == skill.id) return;
    m_workflow.setSkill(skill);
    if (!m_session.meta.id.isEmpty()) {
        m_session.meta.skill = skill.id;
        flushSave(false);
        refreshSessions();
    }
    applySkillToLoop();
    refreshSkillUi();
    if (m_page != nullptr) {
        m_page->showNotice(skill.id == QStringLiteral("general")
                               ? QStringLiteral("已回到自由对话。")
                               : QStringLiteral("已切到「%1」：%2").arg(skill.name, skill.description));
    }
}

void AgentController::onApproval(bool approved) {
    if (m_session.meta.id.isEmpty() || m_session.pendingApproval.isEmpty()) return;
    const QString approvalId =
        m_session.pendingApproval.value(QStringLiteral("approvalId")).toString();
    m_session.pendingApproval = {};
    flushSave(true);
    if (m_page != nullptr) m_page->hideApproval();
    m_loop->resolveApproval(approved);
}

QList<AgentView::ToolCard> AgentController::toolCardsFromHistory() const {
    QList<AgentView::ToolCard> cards;
    for (const Agent::Message &message : m_session.messages) {
        if (message.role != Agent::Role::Tool) continue;
        AgentView::ToolCard card;
        card.id = message.toolCallId.isEmpty() ? message.id : message.toolCallId;
        card.title = message.toolName;
        QJsonObject body = QJsonDocument::fromJson(message.content.toUtf8()).object();
        card.summary = body.value(QStringLiteral("summary")).toString();
        const QJsonObject data = body.value(QStringLiteral("data")).toObject();
        card.detail = QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented));
        const bool ok = body.value(QStringLiteral("ok")).toBool(false);
        card.status = ok ? AgentView::ToolCardStatus::Done : AgentView::ToolCardStatus::Error;
        cards.append(card);
    }
    return cards;
}

void AgentController::showToolCardsFromHistory() {
    if (m_page != nullptr) m_page->setToolCards(toolCardsFromHistory());
}

void AgentController::onPageHidden() {
    if (m_loop->busy()) onStop();
}

void AgentController::onPageShown() { updateInstanceChip(); }

void AgentController::openSettings() {
    if (m_overlay == nullptr || m_page == nullptr) return;
    // 覆盖层被别的页占着（典型：在别的选项卡上开了页再切过来，那页收着、仍占着这一位）时 showPage() 直接返回
    // false，按钮就「点了没反应」。用户亲手点的入口按全窗规矩（MainWindow::runWhenOverlayFree）：静默收掉占着的
    // 那一层、排到下一轮再开；这一层只认自己的 overlay，不依赖 MainWindow，所以在这里做同一件事。
    if (m_overlay->isOpen()) {
        if (!m_settingsOpenQueued && m_overlay->evict()) {
            m_settingsOpenQueued = true;
            QTimer::singleShot(0, this, [this] {
                m_settingsOpenQueued = false;
                openSettings();
            });
        }
        return;
    }
    if (m_settingsPage != nullptr) {
        // 已经开着/收着：把值重摆一遍再让它露头。
        m_settingsPage->setProfile(viewProfile(m_profile), AgentSecretStore::hasSecret(m_profile.id),
                                   AgentSecretStore::read(m_profile.id).backend ==
                                           AgentSecretStore::Backend::DpapiFallback
                                       ? QStringLiteral("dpapi-fallback")
                                       : QStringLiteral("credential-manager"));
        m_settingsPage->setStrictEgress(AgentSettings::strictEgress());
        m_overlay->showPage(m_settingsPage, QCoreApplication::translate("AgentController", "API 与模型设置"));
        return;
    }
    auto *page = new AgentSettingsPage(nullptr);
    m_settingsPage = page;
    const AgentSecretStore::ReadResult secret = AgentSecretStore::read(m_profile.id);
    page->setProfile(viewProfile(m_profile), secret.ok,
                     secret.backend == AgentSecretStore::Backend::DpapiFallback
                         ? QStringLiteral("dpapi-fallback")
                         : QStringLiteral("credential-manager"));
    page->setStrictEgress(AgentSettings::strictEgress());
    page->setWebSearchEndpoint(AgentSettings::webSearchEndpoint());
    page->setDiagnoseCommands(AgentSettings::diagnoseCommandsEnabled());
    connect(page, &AgentSettingsPage::saveRequested, this, &AgentController::onSettingsSave);
    connect(page, &AgentSettingsPage::testRequested, this, &AgentController::onSettingsTest);
    connect(page, &AgentSettingsPage::mcpSettingsRequested, this,
            &AgentController::openMcpSettings);
    connect(page, &AgentSettingsPage::deleteProfileRequested, this,
            &AgentController::onSettingsDeleteProfile);
    connect(page, &AgentSettingsPage::clearKeyRequestedSignal, this, [page] {
        page->setStatus(QCoreApplication::translate("AgentController", "保存后清除；不会再回显旧 Key。"));
    });
    if (!m_overlay->showPage(page, QCoreApplication::translate("AgentController", "API 与模型设置"))) {
        page->deleteLater();
        m_settingsPage = nullptr;
    }
}

void AgentController::onSettingsSave() {
    AgentSettingsPage *page = m_settingsPage;
    if (page == nullptr) return;
    AgentProviderProfile profile = coreProfile(page->profile());
    profile.id = m_profile.id.isEmpty() ? QStringLiteral("default") : m_profile.id;
    QString problem;
    if (!AgentProviderAdapters::validateProfile(profile, &problem) ||
        !profile.validateEnumFields(&problem)) {
        page->setStatus(problem, true);
        return;
    }

    const QString newKey = page->apiKey();
    if (!newKey.isEmpty()) {
        AgentSecretStore::Backend backend = AgentSecretStore::Backend::Unavailable;
        QString keyProblem;
        if (!AgentSecretStore::write(profile.id, newKey, &backend, &keyProblem)) {
            // 两条路都失败：保留旧 key，也不保存这次改动，避免出现“配置变了但 key 没变”的错觉。
            page->setStatus(keyProblem, true);
            return;
        }
        profile.storageBackend = AgentSecretStore::backendKey(backend);
        m_testApiKey.clear();
    } else if (page->clearKeyRequested()) {
        QString keyProblem;
        if (!AgentSecretStore::clear(profile.id, &keyProblem)) {
            page->setStatus(keyProblem, true);
            return;
        }
        m_testApiKey.clear();
    }

    if (!AgentSettings::saveProfile(profile, &problem)) {
        page->setStatus(problem, true);
        return;
    }
    AgentSettings::setActiveProfileId(profile.id);
    AgentSettings::setStrictEgress(page->strictEgress());
    // Stage 3：SearXNG 地址必须是 http/https；诊断命令开关原样保存（默认关闭）。
    const QString endpoint = page->webSearchEndpoint();
    if (!endpoint.isEmpty()) {
        const QUrl url(endpoint);
        if (!url.isValid() || (url.scheme() != QStringLiteral("http") &&
                               url.scheme() != QStringLiteral("https"))) {
            page->setStatus(QStringLiteral("SearXNG 地址必须是 http/https。"), true);
            return;
        }
    }
    AgentSettings::setWebSearchEndpoint(endpoint);
    AgentSettings::setDiagnoseCommandsEnabled(page->diagnoseCommands());
    if (m_toolEngine != nullptr) m_toolEngine->setStrictEgress(page->strictEgress());
    if (m_launcherTools != nullptr) m_launcherTools->setStrictEgress(page->strictEgress());
    m_profile = profile;
    refreshApiState();
    refreshSessions();
    page->setStatus(QCoreApplication::translate("AgentController", "已保存。"), false);
    page->accept();
}

void AgentController::onSettingsTest() {
    AgentSettingsPage *page = m_settingsPage;
    if (page == nullptr) return;
    AgentProviderProfile profile = coreProfile(page->profile());
    profile.id = m_profile.id.isEmpty() ? QStringLiteral("default") : m_profile.id;
    QString problem;
    if (!AgentProviderAdapters::validateProfile(profile, &problem)) {
        page->setTestResult(problem, false);
        return;
    }
    QString key = page->apiKey();
    if (key.isEmpty()) {
        key = AgentSecretStore::read(profile.id).secret;
    }
    if (key.isEmpty() && profile.authMode != QLatin1String("none")) {
        page->setTestResult(QCoreApplication::translate("AgentController", "没有可用的 API Key；先填写或保存后再测试。"), false);
        return;
    }
    const QString requestId = QStringLiteral("test-") +
                              QUuid::createUuid().toString(QUuid::WithoutBraces);
    page->setTestResult(QCoreApplication::translate("AgentController", "正在测试连接…"), true);
    connect(m_client, &AgentModelClient::connectionTested, page,
            [page, requestId](const QString &id, bool ok, int status, const QString &summary) {
                if (id != requestId) return;
                Q_UNUSED(status);
                page->setTestResult(summary, ok);
            },
            Qt::SingleShotConnection);
    Agent::ModelRequest request;
    request.requestId = requestId;
    request.baseUrl = profile.baseUrl;
    request.model = profile.model;
    request.apiKey = key;
    request.authMode = profile.authMode;
    request.localHttpAllowed = profile.localHttpAllowed;
    request.timeoutSec = profile.timeoutSec;
    m_client->testConnection(request);
}

void AgentController::onSettingsDeleteProfile() {
    if (m_overlay == nullptr) return;
    const int choice = InlineMessage::ask(
        m_overlay, MessageKind::Question, QCoreApplication::translate("AgentController", "删除模型配置"),
        QCoreApplication::translate("AgentController", "将删除此模型配置及其 API 密钥，保留会话记录。确认删除？"),
        QStringList{QCoreApplication::translate("AgentController", "删除"), QCoreApplication::translate("AgentController", "取消")}, 1, 0);
    if (choice != 0) return;
    QString problem;
    if (!AgentSecretStore::clear(m_profile.id, &problem) ||
        !AgentSettings::removeProfile(m_profile.id, &problem)) {
        if (m_settingsPage != nullptr) m_settingsPage->setStatus(problem, true);
        return;
    }
    m_profile = AgentProviderProfile{};
    m_testApiKey.clear();
    refreshApiState();
    if (m_settingsPage != nullptr) {
        const AgentSecretStore::ReadResult secret = AgentSecretStore::read(m_profile.id);
        m_settingsPage->setProfile(viewProfile(m_profile), secret.ok, QString());
        m_settingsPage->setStatus(QCoreApplication::translate("AgentController", "配置已删除。"), false);
        m_settingsPage->accept();
    }
}
