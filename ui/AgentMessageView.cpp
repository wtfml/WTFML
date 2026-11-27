/**
 * @file AgentMessageView.cpp
 * @brief 消息气泡的渲染（Markdown 全部走 AgentMarkdown 的白名单）。
 */

#include "AgentMessageView.hpp"

#include "AgentMarkdown.hpp"
#include "Theme.hpp"

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTextDocument>

#include <cmath>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

AgentMessageView::AgentMessageView(const AgentView::Message &message, QWidget *parent)
    : QWidget(parent), m_message(message) {
    setObjectName(QStringLiteral("agentMessageView"));
    // 整行铺满对话列；自己的话是右边一块定宽气泡（宽 = 正文一行排开的宽，最多 520），
    // 这样高度按气泡宽折行，不会被父布局按整列宽量矮了截字。
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    m_bubble = new QWidget(this);
    if (message.user) row->addStretch(1);
    row->addWidget(m_bubble, message.user ? 0 : 1);
    auto *layout = new QVBoxLayout(m_bubble);
    layout->setContentsMargins(message.user ? QMargins(16, 10, 16, 10) : QMargins());
    layout->setSpacing(4);

    m_role = new QLabel(this);
    m_role->setObjectName(QStringLiteral("agentMessageRole"));
    QFont roleFont = m_role->font();
    roleFont.setBold(true);
    m_role->setFont(Theme::textFont(roleFont, Theme::TextRole::Caption));
    m_role->setForegroundRole(QPalette::PlaceholderText);
    m_role->hide();
    layout->addWidget(m_role);

    m_reasoningToggle = new QToolButton(this);
    m_reasoningToggle->setObjectName(QStringLiteral("agentReasoningToggle"));
    m_reasoningToggle->setText(tr("思考过程"));
    m_reasoningToggle->setCheckable(true);
    m_reasoningToggle->setChecked(false);
    m_reasoningToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    // 扁平：不留系统 QToolButton 那圈方框（悬停 / 勾上才出底）。
    m_reasoningToggle->setAutoRaise(true);
    layout->addWidget(m_reasoningToggle);
    m_reasoning = new QLabel(this);
    m_reasoning->setObjectName(QStringLiteral("agentReasoningLabel"));
    m_reasoning->setWordWrap(true);
    m_reasoning->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_reasoning->setForegroundRole(QPalette::PlaceholderText);
    m_reasoning->hide();
    // 点思考正文就折叠 / 展开（和 pi 那个手感一样）：光标变手形，悬停提示写清点一下会怎样。
    // 拖选文字不算点击 —— 按下与松开隔得太远就不动那一位（见 eventFilter）。
    m_reasoning->setCursor(Qt::PointingHandCursor);
    m_reasoning->setToolTip(tr("点一下折叠或展开思考过程"));
    m_reasoning->installEventFilter(this);
    layout->addWidget(m_reasoning);
    connect(m_reasoningToggle, &QToolButton::toggled, this, [this](bool checked) {
        m_reasoning->setVisible(checked);
        refreshReasoningToggle();
    });

    m_body = new QLabel(this);
    m_body->setObjectName(QStringLiteral("agentMessageBody"));
    m_body->setWordWrap(true);
    m_body->setTextFormat(Qt::RichText);
    m_body->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    m_body->setOpenExternalLinks(false);
    layout->addWidget(m_body);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("agentMessageStatus"));
    m_status->setForegroundRole(QPalette::PlaceholderText);
    footer->addWidget(m_status);
    footer->addStretch(1);
    m_retry = new QPushButton(tr("重试"), this);
    m_retry->setObjectName(QStringLiteral("agentRetryButton"));
    m_retry->hide();
    footer->addWidget(m_retry);
    layout->addLayout(footer);
    connect(m_retry, &QPushButton::clicked, this, [this] {
        emit retryRequested(m_message.id);
    });

    refresh();
}

void AgentMessageView::setMessage(const AgentView::Message &message) {
    m_message = message;
    refresh();
}

void AgentMessageView::refreshReasoningToggle() {
    if (m_reasoningToggle == nullptr) return;
    const bool expanded = m_reasoningToggle->isChecked();
    const int chars = m_message.reasoning.size();
    // 收起时把长度写进那行按钮里：折叠之后仍然看得出"这里有这么一段、有多长"。
    m_reasoningToggle->setText(expanded || chars == 0
                                   ? tr("思考过程")
                                   : tr("思考过程（已折叠 · %1 字）").arg(chars));
    m_reasoningToggle->setToolTip(expanded ? tr("点一下折叠思考过程") : tr("点一下展开思考过程"));
}

bool AgentMessageView::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_reasoning && m_reasoningToggle != nullptr) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *press = static_cast<QMouseEvent *>(event);
            if (press->button() == Qt::LeftButton) {
                m_reasoningPressed = true;
                m_reasoningPressPos = press->pos();
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto *release = static_cast<QMouseEvent *>(event);
            const bool wasPress = m_reasoningPressed;
            m_reasoningPressed = false;
            // 挪开超过 4 px 当成拖选文字，不折叠（否则选字永远选不完）。
            if (wasPress && release->button() == Qt::LeftButton &&
                (release->pos() - m_reasoningPressPos).manhattanLength() <= 4) {
                m_reasoningToggle->toggle();
                return true;
            }
        } else if (event->type() == QEvent::Hide) {
            m_reasoningPressed = false;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AgentMessageView::refresh() {
    const bool user = m_message.user;
    m_role->setText(user ? tr("你") : tr("助手"));
    setAccessibleName(m_role->text());
    m_body->setText(AgentMarkdown::toHtml(m_message.content));
    if (user) {
        QTextDocument doc;
        doc.setDefaultFont(m_body->font());
        doc.setDocumentMargin(0);
        doc.setHtml(m_body->text());
        m_bubble->setFixedWidth(qMin(520, int(std::ceil(doc.idealWidth())) + 32 + 4));
    }
    const bool hasReasoning = !m_message.reasoning.trimmed().isEmpty();
    m_reasoningToggle->setVisible(hasReasoning);
    m_reasoning->setText(AgentMarkdown::toHtml(m_message.reasoning));
    if (!hasReasoning) m_reasoning->hide();
    refreshReasoningToggle();

    QString status;
    switch (m_message.status) {
    case AgentView::MessageStatus::Streaming: status = tr("正在生成…"); break;
    case AgentView::MessageStatus::Interrupted: status = tr("已中断"); break;
    case AgentView::MessageStatus::Error: status = tr("失败"); break;
    case AgentView::MessageStatus::ToolPending: status = tr("等待工具…"); break;
    case AgentView::MessageStatus::AwaitingApproval: status = tr("等待确认…"); break;
    case AgentView::MessageStatus::Complete: status.clear(); break;
    }
    if (!m_message.errorText.isEmpty()) {
        status = m_message.errorText;
    }
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    const bool canRetry = !user && (m_message.status == AgentView::MessageStatus::Error ||
                                    m_message.status == AgentView::MessageStatus::Interrupted);
    m_retry->setVisible(canRetry);
}

void AgentMessageView::paintEvent(QPaintEvent *) {
    if (!m_message.user) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(m_bubble->geometry());
    // 圆角 16、右下角 4：气泡「指」向说话的一侧。
    const qreal big = 16;
    const qreal small = 4;
    QPainterPath path;
    path.moveTo(r.left() + big, r.top());
    path.arcTo(QRectF(r.right() - 2 * big, r.top(), 2 * big, 2 * big), 90, -90);
    path.arcTo(QRectF(r.right() - 2 * small, r.bottom() - 2 * small, 2 * small, 2 * small), 0, -90);
    path.arcTo(QRectF(r.left(), r.bottom() - 2 * big, 2 * big, 2 * big), 270, -90);
    path.arcTo(QRectF(r.left(), r.top(), 2 * big, 2 * big), 180, -90);
    path.closeSubpath();
    p.fillPath(path, Theme::tokens().subtle);
}
