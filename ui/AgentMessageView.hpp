/**
 * @file AgentMessageView.hpp
 * @brief 一条消息的显示：受控 Markdown 正文、折叠思考、状态、错误与重试。
 *
 * 自己说的话是靠右的一块淡底气泡（最宽 520，圆角 16 / 右下 4），助手的话直接铺在玻璃上
 * （样张 r2 agent .msg.me / .msg.ai）。角色不再单写一行，读屏靠无障碍名。
 *
 * 纯界面：只认 `AgentView::Message` 值，不读盘、不联网。
 */

#pragma once

#include "AgentViewTypes.hpp"

#include <QPoint>
#include <QWidget>

class QLabel;
class QPushButton;
class QToolButton;

class AgentMessageView : public QWidget {
    Q_OBJECT

public:
    explicit AgentMessageView(const AgentView::Message &message, QWidget *parent = nullptr);

    QString messageId() const { return m_message.id; }
    void setMessage(const AgentView::Message &message);
    AgentView::Message message() const { return m_message; }

signals:
    void retryRequested(const QString &messageId);

protected:
    void paintEvent(QPaintEvent *event) override;
    /// 思考过程那一块点一下就折叠/展开（pi 那个手感）：按下时记位置，松开时位置没挪
    /// 才算「点」，拖选文字不会误触。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh();
    /// 「思考过程」那颗按钮的字与提示：收起时把字数写进去（看得出有多长）。
    void refreshReasoningToggle();

    AgentView::Message m_message;
    QWidget *m_bubble = nullptr;  ///< 内容那一块；自己的话时它靠右、定宽，底色画在它下面
    QLabel *m_role = nullptr;
    QLabel *m_body = nullptr;
    QLabel *m_reasoning = nullptr;
    QToolButton *m_reasoningToggle = nullptr;
    /// 按在思考正文上的位置；松开时还在附近才算一次点击。
    QPoint m_reasoningPressPos;
    bool m_reasoningPressed = false;
    QLabel *m_status = nullptr;
    QPushButton *m_retry = nullptr;
};
