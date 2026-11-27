/**
 * @file AgentApprovalCard.cpp
 * @brief 审批卡渲染（见 .hpp）。
 */

#include "AgentApprovalCard.hpp"
#include "AgentPanelPaint.hpp"
#include "AutoHeight.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBlock>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

namespace {

constexpr int kPreviewCollapsedLines = 8;

/// 预览框无内部竖滚动条，滚轮一律交回外层对话流滚动区（#136）。
class PreviewEdit final : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;

protected:
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }
};

}  // namespace

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
    m_preview = new PreviewEdit(this);
    m_preview->setObjectName(QStringLiteral("agentEgressPreviewCard"));
    m_preview->setReadOnly(true);
    m_preview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_preview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_preview->installEventFilter(this);
    layout->addWidget(m_preview);
    m_previewToggle = new QPushButton(tr("展开全文"), this);
    m_previewToggle->setObjectName(QStringLiteral("agentApprovalPreviewToggle"));
    m_previewToggle->setCursor(Qt::PointingHandCursor);
    m_previewToggle->setFocusPolicy(Qt::StrongFocus);
    m_previewToggle->setAutoDefault(false);
    m_previewToggle->setDefault(false);
    m_previewToggle->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    Theme::markIconButton(m_previewToggle);
    m_previewToggle->installEventFilter(this);
    m_previewToggle->hide();
    updatePreviewToggleStyle();
    Theme::follow(m_previewToggle, [this] { updatePreviewToggleStyle(); });
    connect(m_previewToggle, &QPushButton::clicked, this, [this] {
        m_previewExpanded = !m_previewExpanded;
        updatePreviewToggleStyle();
        updatePreviewHeight();
        if (!m_previewExpanded) {
            QTimer::singleShot(0, this, [this] {
                if (m_previewToggle != nullptr && m_previewToggle->isVisible()) {
                    AutoHeight::reveal(m_previewToggle, m_previewToggle->rect());
                }
            });
        }
    });
    layout->addWidget(m_previewToggle, 0, Qt::AlignLeft);
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
    m_previewExpanded = false;
    m_preview->setPlainText(diff.isEmpty() ? preview : diff);
    m_preview->setVisible(!diff.isEmpty() || !preview.isEmpty());
    updatePreviewToggleStyle();
    updatePreviewHeight();
    QTimer::singleShot(0, this, &AgentApprovalCard::updatePreviewHeight);
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

void AgentApprovalCard::updatePreviewToggleStyle() {
    if (m_previewToggle == nullptr) return;
    const Theme::Tokens t = Theme::tokens();
    const QColor ink = Theme::textTone(t.accentInk, t.deep);
    m_previewToggle->setText(m_previewExpanded ? tr("收起") : tr("展开全文"));
    m_previewToggle->setIcon(m_previewExpanded ? VersionIcons::chevronUp(16, ink)
                                               : VersionIcons::chevron(true, 16, ink));
    m_previewToggle->setIconSize(QSize(16, 16));
    m_previewToggle->setStyleSheet(QStringLiteral(
        "QPushButton { border: none; border-radius: 6px; padding: 3px 6px; background: transparent;"
        " color: %1; text-align: left; }"
        "QPushButton:hover { background: %2; }"
        "QPushButton[wtfmlKeyboardFocus=\"true\"]:focus { border: 2px solid %3; padding: 1px 4px; }")
                                       .arg(ink.name(QColor::HexArgb),
                                            t.hover.name(QColor::HexArgb),
                                            Theme::focusRingColor(palette()).name(QColor::HexArgb)));
}

void AgentApprovalCard::updatePreviewHeight() {
    if (m_preview == nullptr || m_previewToggle == nullptr || m_fittingPreview) return;
    if (m_preview->isHidden()) {
        m_previewToggle->setVisible(false);
        return;
    }
    m_fittingPreview = true;
    m_preview->ensurePolished();
    const int chrome = m_preview->isVisible() && m_preview->viewport()->height() > 0
                           ? qMax(2 * m_preview->frameWidth(),
                                  m_preview->height() - m_preview->viewport()->height())
                           : 2 * m_preview->frameWidth();
    const int lineSpacing = qMax(1, m_preview->fontMetrics().lineSpacing());
    const qreal docMargin = m_preview->document()->documentMargin();
    qreal full = 2.0 * docMargin;
    // QPlainTextEdit 首块从视口 y=0 起绘（上下留白由 QSS padding 在 chrome 里给），
    // 收起态只取前 8 个视觉行的精确高度，避免第 9 行顶部露进视口。
    qreal firstEight = 0.0;
    int visualLines = 0;
    auto *docLayout = m_preview->document()->documentLayout();
    for (QTextBlock block = m_preview->document()->begin(); block.isValid(); block = block.next()) {
        const qreal blockHeight = docLayout != nullptr ? docLayout->blockBoundingRect(block).height() : 0.0;
        const int lines = qMax(1, block.lineCount());
        const qreal effective = blockHeight > 0.0 ? blockHeight : qreal(lines * lineSpacing);
        if (visualLines < kPreviewCollapsedLines) {
            const int take = qMin(lines, kPreviewCollapsedLines - visualLines);
            firstEight += effective * (qreal(take) / qreal(lines));
        }
        visualLines += lines;
        full += effective;
    }
    const int fullContent = int(std::ceil(full)) + 1;
    const int collapsedContent = int(std::floor(qMin(firstEight, qreal(lineSpacing * kPreviewCollapsedLines))));
    const bool overflow = visualLines > kPreviewCollapsedLines;
    if (!overflow) {
        m_previewExpanded = false;
        m_previewToggle->setVisible(false);
    } else {
        m_previewToggle->setVisible(true);
    }
    const int targetContent = (!overflow || m_previewExpanded)
                                  ? qMax(lineSpacing + qRound(2.0 * docMargin) + 1, fullContent)
                                  : collapsedContent;
    const int height = targetContent + chrome;
    if (m_preview->minimumHeight() != height || m_preview->maximumHeight() != height) {
        m_preview->setFixedHeight(height);
    }
    m_preview->verticalScrollBar()->setValue(0);
    m_fittingPreview = false;
}

bool AgentApprovalCard::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_preview) {
        switch (event->type()) {
        case QEvent::Resize:
            QTimer::singleShot(0, this, &AgentApprovalCard::updatePreviewHeight);
            break;
        case QEvent::Show:
        case QEvent::FontChange:
        case QEvent::StyleChange:
        case QEvent::LanguageChange:
            updatePreviewHeight();
            QTimer::singleShot(0, this, &AgentApprovalCard::updatePreviewHeight);
            break;
        default:
            break;
        }
    } else if (watched == m_previewToggle) {
        if (event->type() == QEvent::ShortcutOverride) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                event->accept();
                return true;
            }
        } else if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                m_previewToggle->click();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AgentApprovalCard::paintEvent(QPaintEvent *) {
    QColor tint = Theme::tokens().warnSoft;
    // 浅色的 warnSoft 是棕色，透明度给高了整张卡发重（#136 批 4）；两档都约为 token 原透明度的 1.6 倍。
    tint.setAlpha(Theme::isDark() ? 60 : 48);
    paintAgentSoftPanel(this, 12, tint);
}
