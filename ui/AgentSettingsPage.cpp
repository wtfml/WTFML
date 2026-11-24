/**
 * @file AgentSettingsPage.cpp
 * @brief API/模型设置整页的控件与校验提示（见 .hpp）。
 */

#include "AgentSettingsPage.hpp"
#include <QCoreApplication>
#include "InputSizing.hpp"

#include "AgentSecretEdit.hpp"
#include "SettingsPageParts.hpp"
#include "Theme.hpp"
#include "ToggleSwitch.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QSpinBox>
#include <QVBoxLayout>

using SettingsParts::ExpandSection;
using SettingsParts::Placement;
using SettingsParts::RowSpec;
using SettingsParts::SettingsGroup;

namespace {

/// 次要说明字（13px、次级墨色），和设置页行下说明同一档。
QLabel *captionLabel(QWidget *parent, const QString &name) {
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setWordWrap(true);
    QFont font = label->font();
    font.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
    label->setFont(font);
    label->setProperty(Theme::textRolePropertyName().toUtf8().constData(), Theme::secondaryTextRoleValue());
    return label;
}

/// Wide 行的控件格：上面一个输入框，下面一行说明（可再挂一颗文字按钮）。
QWidget *fieldWithNote(QWidget *parent, QWidget *field, QLabel *note, QWidget *trailing = nullptr) {
    auto *box = new QWidget(parent);
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);
    field->setParent(box);
    v->addWidget(field);
    auto *line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(8);
    note->setParent(box);
    line->addWidget(note, 1);
    if (trailing != nullptr) {
        trailing->setParent(box);
        line->addWidget(trailing, 0, Qt::AlignVCenter);
    }
    v->addLayout(line);
    return box;
}

}  // namespace

AgentSettingsPage::AgentSettingsPage(QWidget *parent) : QDialog(parent) {
    setObjectName(QStringLiteral("agentSettingsPage"));
    setWindowTitle(QCoreApplication::translate("AgentSettingsPage", "API 与模型设置"));
    setModal(true);
    resize(560, 680);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *scroll = new Theme::GutterScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    // 和设置页同一个摆法：两侧留白放在 holder 上，表单最宽 720、在窗口里居中。
    auto *holder = new QWidget(scroll);
    auto *holderLayout = new QHBoxLayout(holder);
    holderLayout->setContentsMargins(0, 4, 20, 24);
    auto *content = new QWidget(holder);
    content->setMaximumWidth(SettingsParts::kFormMaxWidth);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    holderLayout->addStretch(1);
    holderLayout->addWidget(content, 1000);
    holderLayout->addStretch(1);
    auto *form = new QVBoxLayout(content);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(22);
    scroll->setWidget(holder);
    root->addWidget(scroll, 1);

    const auto addGroup = [&](const QString &title) {
        auto *group = new SettingsGroup(title, content);
        form->addWidget(group);
        return group;
    };
    const auto row = [](SettingsGroup *group, const QString &label, QWidget *control, Placement placement,
                        const QString &help = QString(), const QString &tip = QString()) {
        return group->addRow(RowSpec{label, help, tip, placement}, control);
    };
    const auto toggle = [&](const char *name, bool on) {
        auto *sw = new ToggleSwitch(content);
        sw->setObjectName(QString::fromLatin1(name));
        sw->setChecked(on);
        return sw;
    };

    // ---------- 模型服务 ----------
    SettingsGroup *service = addGroup(QCoreApplication::translate("AgentSettingsPage", "模型服务"));
    m_provider = new QComboBox(content);
    m_provider->setObjectName(QStringLiteral("agentProviderCombo"));
    m_provider->addItem(QCoreApplication::translate("AgentSettingsPage", "OpenAI 兼容接口"), QStringLiteral("openai-compatible"));
    row(service, QCoreApplication::translate("AgentSettingsPage", "服务类型"), m_provider, Placement::Column, QString(),
        QCoreApplication::translate("AgentSettingsPage", "DeepSeek、通义、Kimi、Ollama 等大多数服务都提供这种接口。"));

    m_name = new QLineEdit(content);
    m_name->setObjectName(QStringLiteral("agentProfileNameEdit"));
    m_name->setPlaceholderText(QCoreApplication::translate("AgentSettingsPage", "默认模型服务"));
    InputSizing::shortValue(m_name, InputSizing::kNameChars, InputSizing::Script::Han);
    row(service, QCoreApplication::translate("AgentSettingsPage", "配置名称"), m_name, Placement::Column);

    m_baseUrl = new QLineEdit(content);
    m_baseUrl->setObjectName(QStringLiteral("agentBaseUrlEdit"));
    m_baseUrl->setPlaceholderText(QCoreApplication::translate("AgentSettingsPage", "例如 https://api.example.com/v1"));
    InputSizing::longValue(m_baseUrl, QCoreApplication::translate("AgentSettingsPage", "网址：路径长度不定"));
    m_urlPreview = captionLabel(content, QStringLiteral("agentFinalUrlPreview"));
    m_urlPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row(service, QCoreApplication::translate("AgentSettingsPage", "接口地址"), fieldWithNote(content, m_baseUrl, m_urlPreview), Placement::Wide);

    m_model = new QLineEdit(content);
    m_model->setObjectName(QStringLiteral("agentModelEdit"));
    m_model->setPlaceholderText(QCoreApplication::translate("AgentSettingsPage", "例如 deepseek-chat"));
    InputSizing::shortValue(m_model, 40);  // 模型 ID 如 deepseek-chat、claude-sonnet-4-5-20250929
    row(service, QCoreApplication::translate("AgentSettingsPage", "模型"), m_model, Placement::Column, QString(),
        QCoreApplication::translate("AgentSettingsPage", "服务商文档里的模型 ID，大小写要一致。"));

    // API Key 和地址、模型同组：三样都是从服务商那里抄来的。
    m_secret = new AgentSecretEdit(content);
    m_secret->lineEdit()->setObjectName(QStringLiteral("agentApiKeyEdit"));
    m_secret->revealButton()->setObjectName(QStringLiteral("agentApiKeyRevealButton"));
    m_storage = captionLabel(content, QStringLiteral("agentStorageStatus"));
    m_clearKeyButton = new QPushButton(QCoreApplication::translate("AgentSettingsPage", "清除已存密钥"), content);
    m_clearKeyButton->setObjectName(QStringLiteral("agentClearKeyButton"));
    m_clearKeyButton->setCursor(Qt::PointingHandCursor);
    Theme::markButton(m_clearKeyButton, Theme::ButtonKind::Ghost);
    row(service, QStringLiteral("API Key"), fieldWithNote(content, m_secret, m_storage, m_clearKeyButton),
        Placement::Wide);

    // ---------- 生成 ----------
    SettingsGroup *generation = addGroup(QCoreApplication::translate("AgentSettingsPage", "生成"));
    // 这一组四个数字框同宽（按上下文长度上限 10000000 的 8 位钉），左右沿都对齐。
    constexpr int kGenerationDigits = 8;
    m_contextWindow = new QSpinBox(content);
    InputSizing::shortValue(m_contextWindow, kGenerationDigits);
    m_contextWindow->setObjectName(QStringLiteral("agentContextWindowSpin"));
    m_contextWindow->setRange(1024, 10000000);
    m_contextWindow->setSingleStep(1024);
    m_contextWindow->setValue(32768);
    row(generation, QCoreApplication::translate("AgentSettingsPage", "上下文长度"), m_contextWindow, Placement::Natural, QString(),
        QCoreApplication::translate("AgentSettingsPage", "模型一次最多能读多少 token，按服务商写的填；填大了会被服务商拒绝。"));

    m_maxOutput = new QSpinBox(content);
    InputSizing::shortValue(m_maxOutput, kGenerationDigits);
    m_maxOutput->setObjectName(QStringLiteral("agentMaxOutputSpin"));
    m_maxOutput->setRange(1, 1000000);
    m_maxOutput->setValue(4096);
    row(generation, QCoreApplication::translate("AgentSettingsPage", "单次最多输出"), m_maxOutput, Placement::Natural, QString(),
        QCoreApplication::translate("AgentSettingsPage", "一条回答最多多少 token。"));

    m_temperature = new QDoubleSpinBox(content);
    InputSizing::shortValue(m_temperature, kGenerationDigits);
    m_temperature->setObjectName(QStringLiteral("agentTemperatureSpin"));
    m_temperature->setRange(0.0, 2.0);
    m_temperature->setSingleStep(0.1);
    m_temperature->setValue(0.7);
    row(generation, QCoreApplication::translate("AgentSettingsPage", "温度"), m_temperature, Placement::Natural, QString(),
        QCoreApplication::translate("AgentSettingsPage", "越低回答越稳定，越高越发散。排查问题建议 0.3~0.7。"));

    m_timeout = new QSpinBox(content);
    InputSizing::shortValue(m_timeout, kGenerationDigits);
    m_timeout->setObjectName(QStringLiteral("agentTimeoutSpin"));
    m_timeout->setRange(5, 600);
    m_timeout->setValue(120);
    m_timeout->setSuffix(QCoreApplication::translate("AgentSettingsPage", " 秒"));
    row(generation, QCoreApplication::translate("AgentSettingsPage", "等待超时"), m_timeout, Placement::Natural);

    // ---------- 连接 ----------
    SettingsGroup *connection = addGroup(QCoreApplication::translate("AgentSettingsPage", "连接"));
    m_deployment = new QComboBox(content);
    m_deployment->setObjectName(QStringLiteral("agentDeploymentCombo"));
    m_deployment->addItem(QCoreApplication::translate("AgentSettingsPage", "云端服务"), QStringLiteral("cloud"));
    m_deployment->addItem(QCoreApplication::translate("AgentSettingsPage", "本机服务"), QStringLiteral("local"));
    row(connection, QCoreApplication::translate("AgentSettingsPage", "服务在哪"), m_deployment, Placement::Column,
        QCoreApplication::translate("AgentSettingsPage", "云端：对话会发给第三方服务商。本机：只在这台电脑上处理。"));

    m_localHttp = toggle("agentLocalHttpCheck", false);
    row(connection, QCoreApplication::translate("AgentSettingsPage", "允许本机 HTTP"), m_localHttp, Placement::Natural,
        QCoreApplication::translate("AgentSettingsPage", "只对 127.0.0.1、localhost 生效；本机跑的模型服务常用。"));

    // ---------- 隐私 ----------
    SettingsGroup *privacy = addGroup(QCoreApplication::translate("AgentSettingsPage", "隐私"));
    m_strict = toggle("agentEgressStrictCheck", false);
    row(privacy, QCoreApplication::translate("AgentSettingsPage", "严格模式"), m_strict, Placement::Natural,
        QCoreApplication::translate("AgentSettingsPage", "关闭自动读取，发送前显示将发送的内容。"));
    m_privacy = captionLabel(content, QStringLiteral("agentPrivacyText"));
    row(privacy, QCoreApplication::translate("AgentSettingsPage", "会发出去什么"), m_privacy, Placement::Wide);

    // ---------- 高级：协议适配（默认收起） ----------
    m_advanced = new ExpandSection(QCoreApplication::translate("AgentSettingsPage", "高级"), content);
    m_advanced->setObjectName(QStringLiteral("agentAdvancedGroup"));
    // ---------- 联网与诊断（Stage 3）----------
    SettingsGroup *tools = addGroup(QStringLiteral("联网与诊断"));
    m_searxng = new QLineEdit(content);
    m_searxng->setObjectName(QStringLiteral("agentSearxngEdit"));
    m_searxng->setPlaceholderText(QStringLiteral("https://searx.example.com"));
    InputSizing::longValue(m_searxng, "网址：路径长度不定");
    row(tools, QStringLiteral("SearXNG 地址"), m_searxng, Placement::Wide, QString(),
        QStringLiteral("填了你自己的 SearXNG 实例地址才能联网搜索；搜索词会发给这个实例。"));
    m_diagnoseToggle = toggle("agentDiagnoseCommandsToggle", false);
    row(tools, QStringLiteral("允许诊断命令"), m_diagnoseToggle, Placement::Column,
        QStringLiteral("默认关闭。打开后可以执行固定的诊断命令（java -version、ping、nslookup、"
                       "netstat -ano 等），每次都要确认；任意命令始终不开放。"));

    form->addWidget(m_advanced);
    auto *protocol = new SettingsGroup(QCoreApplication::translate("AgentSettingsPage", "协议适配"), m_advanced);
    m_advanced->bodyLayout()->addWidget(protocol);
    m_authMode = new QComboBox(content);
    m_authMode->setObjectName(QStringLiteral("agentAuthModeCombo"));
    m_authMode->addItem(QCoreApplication::translate("AgentSettingsPage", "Bearer 令牌"), QStringLiteral("bearer"));
    m_authMode->addItem(QCoreApplication::translate("AgentSettingsPage", "api-key 请求头"), QStringLiteral("api-key-header"));
    m_authMode->addItem(QCoreApplication::translate("AgentSettingsPage", "不认证（仅本机）"), QStringLiteral("none"));
    row(protocol, QCoreApplication::translate("AgentSettingsPage", "认证方式"), m_authMode, Placement::Column, QString(),
        QCoreApplication::translate("AgentSettingsPage", "大多数服务用 Bearer；Azure 一类用 api-key 请求头。"));
    m_stream = toggle("agentStreamCheck", true);
    row(protocol, QCoreApplication::translate("AgentSettingsPage", "边生成边显示"), m_stream, Placement::Natural, QString(),
        QCoreApplication::translate("AgentSettingsPage", "流式输出（SSE）。服务不支持时关掉。"));
    m_maxTokensField = new QComboBox(content);
    m_maxTokensField->setObjectName(QStringLiteral("agentMaxTokensFieldCombo"));
    m_maxTokensField->addItem(QStringLiteral("max_tokens"), QStringLiteral("max_tokens"));
    m_maxTokensField->addItem(QStringLiteral("max_completion_tokens"), QStringLiteral("max_completion_tokens"));
    row(protocol, QCoreApplication::translate("AgentSettingsPage", "输出上限字段"), m_maxTokensField, Placement::Column, QString(),
        QCoreApplication::translate("AgentSettingsPage", "新版 OpenAI 推理模型要用 max_completion_tokens。"));
    m_systemRole = new QComboBox(content);
    m_systemRole->setObjectName(QStringLiteral("agentSystemRoleCombo"));
    m_systemRole->addItem(QStringLiteral("system"), QStringLiteral("system"));
    m_systemRole->addItem(QStringLiteral("developer"), QStringLiteral("developer"));
    row(protocol, QCoreApplication::translate("AgentSettingsPage", "系统提示角色"), m_systemRole, Placement::Column);
    m_reasoningEcho = new QComboBox(content);
    m_reasoningEcho->setObjectName(QStringLiteral("agentReasoningEchoCombo"));
    m_reasoningEcho->addItem(QCoreApplication::translate("AgentSettingsPage", "只显示"), QStringLiteral("display-only"));
    m_reasoningEcho->addItem(QCoreApplication::translate("AgentSettingsPage", "回传给后续请求"), QStringLiteral("send-back"));
    row(protocol, QCoreApplication::translate("AgentSettingsPage", "思考过程"), m_reasoningEcho, Placement::Column, QString(),
        QCoreApplication::translate("AgentSettingsPage", "模型返回的思考过程要不要带进下一轮请求。"));
    m_sendTemperature = toggle("agentSendTemperatureCheck", true);
    row(protocol, QCoreApplication::translate("AgentSettingsPage", "发送温度参数"), m_sendTemperature, Placement::Natural, QString(),
        QCoreApplication::translate("AgentSettingsPage", "有些推理模型不接受 temperature，报错时关掉。"));
    m_sendToolChoice = toggle("agentSendToolChoiceCheck", true);
    row(protocol, QCoreApplication::translate("AgentSettingsPage", "发送工具选择参数"), m_sendToolChoice, Placement::Natural, QString(),
        QCoreApplication::translate("AgentSettingsPage", "有些服务不接受 tool_choice，报错时关掉。"));

    // ---------- 删除配置：危险操作单独一组，不和「保存」挤在一行 ----------
    SettingsGroup *manage = addGroup(QCoreApplication::translate("AgentSettingsPage", "当前配置"));
    m_deleteProfile = new QPushButton(QCoreApplication::translate("AgentSettingsPage", "删除"), content);
    m_deleteProfile->setObjectName(QStringLiteral("agentDeleteProfileButton"));
    m_deleteProfile->setCursor(Qt::PointingHandCursor);
    Theme::markButton(m_deleteProfile, Theme::ButtonKind::Danger);
    row(manage, QCoreApplication::translate("AgentSettingsPage", "删除此配置"), m_deleteProfile, Placement::Natural,
        QCoreApplication::translate("AgentSettingsPage", "连同已存的密钥一起删掉；会话记录保留。"));
    form->addStretch(1);

    // ---------- 底栏：测试结果 + 状态 + 测试连接 / 保存 ----------
    auto *footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("agentSettingsFooter"));
    auto *footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(0, 10, 20, 16);
    footerLayout->setSpacing(6);
    m_testResult = captionLabel(footer, QStringLiteral("agentTestResultLabel"));
    m_testResult->hide();
    footerLayout->addWidget(m_testResult);
    m_status = captionLabel(footer, QStringLiteral("agentSettingsStatus"));
    m_status->hide();
    footerLayout->addWidget(m_status);
    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(10);
    m_test = new QPushButton(QCoreApplication::translate("AgentSettingsPage", "测试连接"), footer);
    m_test->setObjectName(QStringLiteral("agentTestConnectionButton"));
    Theme::markButton(m_test, Theme::ButtonKind::Neutral);
    m_save = new QPushButton(QCoreApplication::translate("AgentSettingsPage", "保存"), footer);
    m_save->setObjectName(QStringLiteral("agentSaveButton"));
    m_save->setDefault(true);
    Theme::markButton(m_save, Theme::ButtonKind::Primary);
    m_save->setMinimumWidth(96);
    buttons->addWidget(m_test);
    m_mcp = new QPushButton(QStringLiteral("MCP 服务器…"), this);
    m_mcp->setObjectName(QStringLiteral("agentSettingsMcpButton"));
    buttons->addWidget(m_mcp);
    buttons->addStretch(1);
    buttons->addWidget(m_save);
    footerLayout->addLayout(buttons);
    // 底栏和表单同宽、同样居中（宽窗时「保存」落在表单右缘，不跑到窗口最右）。
    // 右边让出滚动条那一格：表单在视口里居中，视口比页窄这一格。
    footer->setMaximumWidth(SettingsParts::kFormMaxWidth + 20);
    auto *footerRow = new QHBoxLayout;
    footerRow->setContentsMargins(0, 0, Theme::scrollBarThickness(), 0);
    footerRow->addStretch(1);
    footerRow->addWidget(footer, 1000);
    footerRow->addStretch(1);
    root->addLayout(footerRow);

    connect(m_baseUrl, &QLineEdit::textChanged, this, [this] { updateUrlPreview(); });
    connect(m_strict, &QAbstractButton::toggled, this, [this] { updatePrivacyText(); });
    connect(m_save, &QPushButton::clicked, this, &AgentSettingsPage::saveRequested);
    connect(m_test, &QPushButton::clicked, this, &AgentSettingsPage::testRequested);
    connect(m_mcp, &QPushButton::clicked, this, &AgentSettingsPage::mcpSettingsRequested);
    connect(m_clearKeyButton, &QPushButton::clicked, this, [this] {
        m_clearKey = true;
        m_secret->clear();
        m_secret->setStoredKey(false);
        m_storage->setText(QCoreApplication::translate("AgentSettingsPage", "保存后清除已存密钥。"));
        m_clearKeyButton->hide();
        setStatus(QCoreApplication::translate("AgentSettingsPage", "点了保存后才会真正清除已存凭据。"));
        emit clearKeyRequestedSignal();
    });
    connect(m_secret, &AgentSecretEdit::textChanged, this, [this] {
        if (!m_secret->text().isEmpty()) m_clearKey = false;
    });
    connect(m_deleteProfile, &QPushButton::clicked, this, &AgentSettingsPage::deleteProfileRequested);
    connect(m_authMode, &QComboBox::currentIndexChanged, this, [this] { updateKeyNote(); });
    m_secret->setStoredKey(false);
    m_clearKeyButton->hide();
    updateUrlPreview();
    updatePrivacyText();
}

void AgentSettingsPage::updateKeyNote() {
    // 已存 / 待清除那两句照旧；只有「还没存」时按认证方式说：不认证时根本用不到密钥，
    // 再说「还没保存密钥，填好后点保存」会让人以为非填不可。
    if (m_clearKey || m_secret->hasStoredKey()) return;
    const bool none = m_authMode->currentData().toString() == QLatin1String("none");
    m_storage->setText(none ? QCoreApplication::translate("AgentSettingsPage", "认证方式是「不认证」，用不到 API Key，留空即可。")
                            : QCoreApplication::translate("AgentSettingsPage", "尚未保存密钥。填写后选择「保存」。"));
    m_secret->lineEdit()->setPlaceholderText(none ? QCoreApplication::translate("AgentSettingsPage", "不认证，可留空")
                                                  : QCoreApplication::translate("AgentSettingsPage", "尚未保存 API Key"));
}

void AgentSettingsPage::updatePrivacyText() {
    if (!m_privacyOverride.isEmpty()) {
        m_privacy->setText(m_privacyOverride);
        return;
    }
    // 边界照 docs/features/AGENT.md §5.6.2（D11）：标准模式自动读脱敏日志 / 最新崩溃报告 / 关联实例的版本信息，
    // 其余文件逐次确认；严格模式把自动那一列也改成逐次确认。凭据两种模式都不发。
    const QString what = m_strict->isChecked()
        ? QCoreApplication::translate("AgentSettingsPage", "仅发送输入的文字。读取日志、崩溃报告或文件前，须逐次确认。发送前会显示内容。")
        : QCoreApplication::translate("AgentSettingsPage", "发送输入的文字，以及关联实例的日志末尾、最新崩溃报告和版本信息。发送前移除账号信息。读取配置及其他文件前须确认。");
    m_privacy->setText(what + QLatin1Char('\n') +
                       QCoreApplication::translate("AgentSettingsPage", "账号令牌、API 密钥这类凭据永远不会发送。云端服务商怎么保存对话由服务商决定；" "WTFML 自己不收集使用数据。"));
}

void AgentSettingsPage::updateUrlPreview() {
    QString base = m_baseUrl->text().trimmed();
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    m_urlPreview->setText(base.isEmpty()
                              ? QCoreApplication::translate("AgentSettingsPage", "填服务商给的地址，一般以 /v1 结尾；实际请求发到 <地址>/chat/completions")
                              : QCoreApplication::translate("AgentSettingsPage", "实际请求发到 %1/chat/completions").arg(base));
}

void AgentSettingsPage::setProfile(const AgentView::ProviderDraft &profile, bool hasStoredKey,
                                   const QString &storageBackend) {
    m_profileId = profile.id;
    m_name->setText(profile.name);
    m_baseUrl->setText(profile.baseUrl);
    m_model->setText(profile.model);
    m_contextWindow->setValue(profile.contextWindow);
    m_maxOutput->setValue(profile.maxOutput);
    m_temperature->setValue(profile.temperature);
    m_timeout->setValue(profile.timeoutSec);
    m_deployment->setCurrentIndex(qMax(0, m_deployment->findData(profile.deployment)));
    m_localHttp->setChecked(profile.localHttpAllowed);
    m_authMode->setCurrentIndex(qMax(0, m_authMode->findData(profile.authMode)));
    m_stream->setChecked(profile.stream);
    m_maxTokensField->setCurrentIndex(qMax(0, m_maxTokensField->findData(profile.maxTokensField)));
    m_systemRole->setCurrentIndex(qMax(0, m_systemRole->findData(profile.systemRole)));
    m_reasoningEcho->setCurrentIndex(qMax(0, m_reasoningEcho->findData(profile.reasoningEcho)));
    m_sendTemperature->setChecked(profile.sendTemperature);
    m_sendToolChoice->setChecked(profile.sendToolChoice);
    m_secret->clear();
    m_secret->setStoredKey(hasStoredKey);
    m_clearKey = false;
    m_storage->setText(hasStoredKey
                           ? (storageBackend == QLatin1String("dpapi-fallback")
                                  ? QCoreApplication::translate("AgentSettingsPage", "已加密保存在本机，只有当前 Windows 账户能读；不会显示出来。")
                                  : QCoreApplication::translate("AgentSettingsPage", "已保存在 Windows 凭据管理器，只有当前账户能读；不会显示出来。"))
                           : QCoreApplication::translate("AgentSettingsPage", "尚未保存密钥。填写后选择「保存」。"));
    m_clearKeyButton->setVisible(hasStoredKey);
    updateKeyNote();
    updateUrlPreview();
}

AgentView::ProviderDraft AgentSettingsPage::profile() const {
    AgentView::ProviderDraft profile;
    profile.id = m_profileId.isEmpty() ? QStringLiteral("default") : m_profileId;
    profile.name = m_name->text().trimmed().isEmpty() ? QCoreApplication::translate("AgentSettingsPage", "默认模型服务")
                                                      : m_name->text().trimmed();
    profile.baseUrl = m_baseUrl->text().trimmed();
    profile.model = m_model->text().trimmed();
    profile.contextWindow = m_contextWindow->value();
    profile.maxOutput = m_maxOutput->value();
    profile.temperature = m_temperature->value();
    profile.timeoutSec = m_timeout->value();
    profile.deployment = m_deployment->currentData().toString();
    profile.localHttpAllowed = m_localHttp->isChecked();
    profile.authMode = m_authMode->currentData().toString();
    profile.stream = m_stream->isChecked();
    profile.maxTokensField = m_maxTokensField->currentData().toString();
    profile.systemRole = m_systemRole->currentData().toString();
    profile.reasoningEcho = m_reasoningEcho->currentData().toString();
    profile.sendTemperature = m_sendTemperature->isChecked();
    profile.sendToolChoice = m_sendToolChoice->isChecked();
    return profile;
}

QString AgentSettingsPage::apiKey() const { return m_secret->text().trimmed(); }

void AgentSettingsPage::setStatus(const QString &text, bool error) {
    m_status->setText(text);
    // 出错用主题危险色（原先写死的 #b3261e 在深色底上几乎看不见）；平常是次级说明字。
    QPalette pal = palette();
    if (error) pal.setColor(QPalette::WindowText, Theme::dangerColor(palette()));
    m_status->setPalette(pal);
    m_status->setProperty(Theme::textRolePropertyName().toUtf8().constData(),
                          error ? QVariant() : QVariant(Theme::secondaryTextRoleValue()));
    m_status->setVisible(!text.isEmpty());
}

void AgentSettingsPage::setTestResult(const QString &text, bool ok) {
    m_testResult->setText(text);
    QPalette pal = palette();
    pal.setColor(QPalette::WindowText, ok ? Theme::okColor(palette()) : Theme::dangerColor(palette()));
    m_testResult->setPalette(pal);
    m_testResult->setProperty(Theme::textRolePropertyName().toUtf8().constData(), QVariant());
    m_testResult->setVisible(!text.isEmpty());
}

void AgentSettingsPage::setPrivacyText(const QString &text) {
    m_privacyOverride = text;
    updatePrivacyText();
}

void AgentSettingsPage::setWebSearchEndpoint(const QString &endpoint) {
    if (m_searxng != nullptr) m_searxng->setText(endpoint);
}

QString AgentSettingsPage::webSearchEndpoint() const {
    return m_searxng != nullptr ? m_searxng->text().trimmed() : QString();
}

void AgentSettingsPage::setDiagnoseCommands(bool enabled) {
    if (m_diagnoseToggle != nullptr) m_diagnoseToggle->setChecked(enabled);
}

bool AgentSettingsPage::diagnoseCommands() const {
    return m_diagnoseToggle != nullptr && m_diagnoseToggle->isChecked();
}

void AgentSettingsPage::setStrictEgress(bool strict) { m_strict->setChecked(strict); }
bool AgentSettingsPage::strictEgress() const { return m_strict->isChecked(); }
