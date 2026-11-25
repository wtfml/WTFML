/**
 * @file AgentApprovalCard.cpp
 * @brief 审批卡渲染（见 .hpp）。
 */

#include "AgentApprovalCard.hpp"
#include "AgentPanelPaint.hpp"

#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

AgentApprovalCard::AgentApprovalCard(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("agentApprovalCard"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(6);
    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("agentApprovalTitle"));
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    layout->addWidget(m_title);
    m_target = new QLabel(this);
    m_target->setObjectName(QStringLiteral("agentApprovalTarget"));
    m_target->setWordWrap(true);
    layout->addWidget(m_target);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("agentApprovalSummary"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    m_preview = new QPlainTextEdit(this);
    m_preview->setObjectName(QStringLiteral("agentEgressPreviewCard"));
    m_preview->setReadOnly(true);
    m_preview->setMaximumHeight(160);
    layout->addWidget(m_preview);
    m_backup = new QLabel(this);
    m_backup->setObjectName(QStringLiteral("agentApprovalBackup"));
    m_backup->setWordWrap(true);
    m_backup->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(m_backup);
    m_egress = new QLabel(this);
    m_egress->setObjectName(QStringLiteral("agentApprovalEgress"));
    m_egress->setWordWrap(true);
    m_egress->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(m_egress);
    auto *buttons = new QHBoxLayout;
    // 只有联网工具会让这个按钮出现，所以文案说“来源”而不是“目录”。
    m_sessionAllow = new QPushButton(tr("本会话允许此来源"), this);
    m_sessionAllow->setObjectName(QStringLiteral("agentApprovalSessionAllow"));
    m_sessionAllow->hide();
    m_allow = new QPushButton(tr("允许本次"), this);
    m_allow->setObjectName(QStringLiteral("agentApprovalAllow"));
    m_deny = new QPushButton(tr("拒绝"), this);
    m_deny->setObjectName(QStringLiteral("agentApprovalDeny"));
    m_deny->setDefault(true);
    buttons->addWidget(m_sessionAllow);
    buttons->addStretch(1);
    buttons->addWidget(m_deny);
    buttons->addWidget(m_allow);
    layout->addLayout(buttons);
    connect(m_allow, &QPushButton::clicked, this, [this] {
        if (!m_approvalId.isEmpty()) emit allowed(m_approvalId);
    });
    connect(m_deny, &QPushButton::clicked, this, [this] {
        if (!m_approvalId.isEmpty()) emit denied(m_approvalId);
    });
    connect(m_sessionAllow, &QPushButton::clicked, this, [this] {
        if (!m_approvalId.isEmpty()) emit sessionAllowed(m_approvalId);
    });
}

void AgentApprovalCard::setRequest(const QJsonObject &request) {
    m_approvalId = request.value(QStringLiteral("approvalId")).toString();
    m_title->setText(tr("需要确认：%1")
                         .arg(request.value(QStringLiteral("displayName")).toString()));
    m_target->setText(tr("目标：%1").arg(
        request.value(QStringLiteral("target")).toString()));
    m_summary->setText(request.value(QStringLiteral("summary")).toString());
    const QString diff = request.value(QStringLiteral("diff")).toString();
    const QString preview = request.value(QStringLiteral("preview")).toString();
    m_preview->setPlainText(diff.isEmpty() ? preview : diff);
    m_preview->setVisible(!diff.isEmpty() || !preview.isEmpty());
    const QString backup = request.value(QStringLiteral("backupHint")).toString();
    m_backup->setText(backup);
    m_backup->setVisible(!backup.isEmpty());
    const QJsonObject egress = request.value(QStringLiteral("egress")).toObject();
    m_egress->setText(tr("外发：%1 · %2")
                          .arg(egress.value(QStringLiteral("kind")).toString(),
                               egress.value(QStringLiteral("reason")).toString()));
    m_sessionAllow->setVisible(
        request.value(QStringLiteral("sessionAllowAvailable")).toBool(false));
}

void AgentApprovalCard::paintEvent(QPaintEvent *) {
    QColor tint = Theme::tokens().warnSoft;
    tint.setAlpha(Theme::isDark() ? 60 : 150);
    paintAgentSoftPanel(this, 12, tint);
}
