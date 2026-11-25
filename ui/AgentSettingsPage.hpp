/**
 * @file AgentSettingsPage.hpp
 * @brief 智能体的“API 与模型设置”整页（OverlayHost::showPage 里开，不另开窗口）。
 *
 * 版式和设置页同一套（SettingsParts：分组淡底 + 左标签右控件、窄时上下叠放、开关用 ToggleSwitch）：
 * 模型服务 / API 密钥 / 生成 / 连接 / 隐私 五组，协议适配收在默认折叠的「高级」里，
 * 删除配置单独一组放最下面；底栏只留「测试连接」和「保存」。MCP 一栏不显示。
 * 密钥：已保存时留空、占位文字说明；保存前不发任何网络请求。
 */

#pragma once

#include "AgentViewTypes.hpp"

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class ToggleSwitch;
class QPushButton;
class QSpinBox;
class AgentSecretEdit;
class ToggleSwitch;
namespace SettingsParts {
class ExpandSection;
}

class AgentSettingsPage : public QDialog {
    Q_OBJECT

public:
    explicit AgentSettingsPage(QWidget *parent = nullptr);

    void setProfile(const AgentView::ProviderDraft &profile, bool hasStoredKey,
                    const QString &storageBackend);
    AgentView::ProviderDraft profile() const;
    QString apiKey() const;
    bool clearKeyRequested() const { return m_clearKey; }
    void setStatus(const QString &text, bool error = false);
    void setTestResult(const QString &text, bool ok);
    void setPrivacyText(const QString &text);
    void setStrictEgress(bool strict);
    bool strictEgress() const;
    /// Stage 3：联网搜索与诊断命令。
    void setWebSearchEndpoint(const QString &endpoint);
    QString webSearchEndpoint() const;
    void setDiagnoseCommands(bool enabled);
    bool diagnoseCommands() const;
    /// P2 修复病例库开关（默认关闭）。
    void setRepairCases(bool enabled);
    bool repairCases() const;

signals:
    void saveRequested();
    void testRequested();
    /// 「MCP 服务器…」：打开 MCP 服务器列表（阶段 2A）。
    void mcpSettingsRequested();
    void clearKeyRequestedSignal();
    void deleteProfileRequested();

private:
    void updateUrlPreview();
    void updateKeyNote();
    void updatePrivacyText();

    QComboBox *m_provider = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_baseUrl = nullptr;
    QLineEdit *m_model = nullptr;
    QSpinBox *m_contextWindow = nullptr;
    QSpinBox *m_maxOutput = nullptr;
    QDoubleSpinBox *m_temperature = nullptr;
    QSpinBox *m_timeout = nullptr;
    QComboBox *m_deployment = nullptr;
    ToggleSwitch *m_localHttp = nullptr;
    SettingsParts::ExpandSection *m_advanced = nullptr;
    QComboBox *m_authMode = nullptr;
    ToggleSwitch *m_stream = nullptr;
    QComboBox *m_maxTokensField = nullptr;
    QComboBox *m_systemRole = nullptr;
    QComboBox *m_reasoningEcho = nullptr;
    ToggleSwitch *m_sendTemperature = nullptr;
    ToggleSwitch *m_sendToolChoice = nullptr;
    AgentSecretEdit *m_secret = nullptr;
    QLabel *m_storage = nullptr;
    QLabel *m_urlPreview = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_testResult = nullptr;
    QLabel *m_privacy = nullptr;
    ToggleSwitch *m_strict = nullptr;
    QPushButton *m_test = nullptr;
    QPushButton *m_mcp = nullptr;
    QLineEdit *m_searxng = nullptr;
    ToggleSwitch *m_diagnoseToggle = nullptr;
    ToggleSwitch *m_repairCasesToggle = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_clearKeyButton = nullptr;
    QPushButton *m_deleteProfile = nullptr;
    QString m_privacyOverride;  ///< setPrivacyText() 给的整段说明；空 = 按严格模式开关现写
    QString m_profileId;
    bool m_clearKey = false;
};
