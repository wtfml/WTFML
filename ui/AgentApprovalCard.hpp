/**
 * @file AgentApprovalCard.hpp
 * @brief 写/外发审批卡：目标、摘要、预览/diff、备份提示、允许/拒绝。
 *
 * `sessionAllowAvailable` 只对允许会话放行的工具（`web.search` / `web.fetch`）为真；
 * 其它工具的按钮一直隐藏。
 */

#pragma once

#include <QJsonObject>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;

class AgentApprovalCard : public QWidget {
    Q_OBJECT

public:
    explicit AgentApprovalCard(QWidget *parent = nullptr);

    void setRequest(const QJsonObject &request);
    QString approvalId() const { return m_approvalId; }

signals:
    void allowed(const QString &approvalId);
    void denied(const QString &approvalId);
    /// 本会话允许：这一次照常执行，同一来源在本会话内不再逐次确认。
    void sessionAllowed(const QString &approvalId);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updatePreviewHeight();
    void updatePreviewToggleStyle();

    QString m_approvalId;
    QLabel *m_title = nullptr;
    QLabel *m_target = nullptr;
    QLabel *m_summary = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QPushButton *m_previewToggle = nullptr;
    QLabel *m_backup = nullptr;
    QLabel *m_egress = nullptr;
    QPushButton *m_allow = nullptr;
    QPushButton *m_deny = nullptr;
    QPushButton *m_sessionAllow = nullptr;
    bool m_previewExpanded = false;
    bool m_fittingPreview = false;
};
