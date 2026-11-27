/**
 * @file AgentPage.hpp
 * @brief 智能体页：全宽聊天区 + 左缘玻璃会话抽屉（#136 消除常驻双滚动条）。
 *
 * 顶行（拖动区下沿再空 12px）：「会话列表」抽屉钮 · 「新建对话」钮 · 会话标题（点击改名）·
 * API 未配置警示胶囊 · 「实例」下拉胶囊 · ⋯ 菜单（重命名 / 固定 / 加密存储 / 导出 / 删除会话）·
 * ⚙ API 设置。点「会话列表」从页面左缘滑出玻璃抽屉（宽 236，窗口窄于 1024 时 216），
 * 盖在对话区上不推开内容，内含「会话」标题、搜索框和按「固定 / 今天 / 更早」分组的会话列表。
 * 下面是对话流（最宽 760，系统提示以居中淡色一行 + 两侧细线插在流里）、空状态，最底是磨砂输入岛。
 *
 * 纯 QWidget：只认值、信号和槽；不 include `src/`，不读盘、不联网。
 */

#pragma once

#include "AgentViewTypes.hpp"
#include "ThemeBackground.hpp"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPointer>

class QListWidgetItem;
class QPoint;
class QHBoxLayout;
class QHideEvent;
class QVariantAnimation;

class QCheckBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class AgentApprovalCard;
class AgentComposer;
class AgentMessageView;
class AgentToolCard;

class AgentPage : public ThemeBackground {
    Q_OBJECT

public:
    explicit AgentPage(QWidget *parent = nullptr);
    ~AgentPage() override;

    static int sidebarWidth();
    static int sidebarNarrowWidth();
    static QString relativeTime(const QDateTime &when, const QDateTime &now);

    bool isSessionDrawerOpen() const { return m_drawerOpen; }
    void openSessionDrawer();
    void closeSessionDrawer(bool restoreFocus = true);
    void toggleSessionDrawer();

    void setSessions(const QList<AgentView::SessionMeta> &sessions);
    void setCurrentSession(const QString &sessionId);
    QString currentSessionId() const { return m_currentSessionId; }

    void setTitle(const QString &title);
    void setMessages(const QList<AgentView::Message> &messages);
    void upsertMessage(const AgentView::Message &message);
    void removeMessage(const QString &messageId);
    void clearMessages();
    void setToolCards(const QList<AgentView::ToolCard> &cards);
    void upsertToolCard(const AgentView::ToolCard &card);
    void clearToolCards();
    void showApproval(const QJsonObject &request);
    void hideApproval();

    void setStreaming(bool streaming);
    /// @param statusText 未配置时进警示胶囊的悬停提示，已配置时进 ⚙ 的悬停提示。
    void setConfigured(bool configured, const QString &statusText = QString());
    /// 当前模型（「配置名 · 模型 ID」），写进 ⚙ 的悬停提示。
    void setModelLabel(const QString &label);
    /// 模式卡片（自由对话 / 体检 / 崩溃急救 / 升级 / 模组维护）。@p currentId 是选中的那个。
    void setSkills(const QList<AgentView::SkillOption> &skills, const QString &currentId);
    /// 工作流的阶段/计划进度那一行（空串收起）。
    void setWorkflowStatus(const QString &text);
    /// Auto-safe 开关（S5.7）：只在崩溃急救/模组维护显示；打开后由控制器授权事务。
    void setAutoSafeVisible(bool visible);
    void setAutoSafeEnabled(bool enabled);
    bool autoSafeEnabled() const;
    QString autoSafeText() const;
    /// 「实例」胶囊右半边的值：已关联的实例名，未关联时是主页当前实例或「无」。
    void setInstanceChip(const QString &text, bool locked);
    /// 弹出实例下拉：第一行「跟随主页」，下面每个实例一行；@p lockedId 为空表示跟随主页。
    void showInstanceMenu(const QList<AgentView::InstanceOption> &instances, const QString &lockedId);
    void setEncryptionChecked(bool checked);
    /// 系统提示：对话流末尾居中一行淡字 + 两侧细线；空串收起。
    void showNotice(const QString &text);
    void showEmptyState(bool empty);

    AgentComposer *composer() const { return m_composer; }
    QListWidget *sessionList() const { return m_sessionList; }
    AgentApprovalCard *approvalCard() const { return m_approvalCard; }
    /// ⋯ 菜单与实例下拉（测试和验收图用；没弹过时实例下拉为空）。
    QWidget *moreMenu() const { return m_moreMenu; }
    QWidget *instanceMenu() const { return m_instanceMenu; }
    void openMoreMenu();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void newSessionRequested();
    void sessionSelected(const QString &sessionId);
    void sessionRenameRequested(const QString &sessionId, const QString &title);
    void sessionPinRequested(const QString &sessionId, bool pinned);
    void sessionDeleteRequested(const QString &sessionId);
    void sessionExportRequested(const QString &sessionId);
    void sendRequested(const QString &text);
    void stopRequested();
    void settingsRequested();
    void retryRequested(const QString &messageId);
    void encryptToggled(bool encrypted);
    void textChanged();
    /// 点了「实例」胶囊：控制器回一句 showInstanceMenu()。
    void instanceChipRequested();
    /// 在实例下拉里选了一项；空串 = 跟随主页。
    void instanceChosen(const QString &instanceId);
    void approvalAllowed(const QString &approvalId);
    void approvalDenied(const QString &approvalId);
    void approvalSessionAllowed(const QString &approvalId);
    /// 点了模式卡片。
    void skillChosen(const QString &skillId);
    /// 点了 Auto-safe 开关。
    void autoSafeToggled(bool enabled);

private:
    /// 强制回到底部并恢复"跟随最新"（切会话、发消息、Ctrl+End）。
    void scrollToLatest();
    /// 只在用户本来就贴着底时才跟随（流式正文/工具卡/提示的自动更新走这一条）。
    void scrollToLatestIfSticky();
    bool atBottom() const;
    void rebuildSessionRow(QListWidgetItem *item, const AgentView::SessionMeta &meta);
    void updateSendEnabled();
    void updateEmptyState();
    void updateComposerHint();
    void insertStreamWidget(QWidget *widget);
    AgentMessageView *viewFor(const QString &messageId);
    /// 长会话窗口：只渲染末尾一段，前面的用「显示更早的消息」按需补。
    int messageIndex(const QString &messageId) const;
    int streamStartIndex() const;
    void renderMessage(const AgentView::Message &message, int layoutIndex);
    void trimVisibleWindow();
    void updateLoadEarlier();
    void loadEarlier();
    void showSessionMenu(const QPoint &position);
    /// 「重命名」：当前会话就地改顶行标题，别的会话就地改列表里那一行。
    void promptRename(const QString &sessionId);
    void beginTitleRename();
    void beginListRename(QListWidgetItem *item);
    void placeSessionDrawer();

    QWidget *m_sidebar = nullptr;
    QWidget *m_header = nullptr;
    QPushButton *m_sessionsButton = nullptr;
    QLineEdit *m_search = nullptr;
    QListWidget *m_sessionList = nullptr;
    QLabel *m_title = nullptr;
    QPushButton *m_newSession = nullptr;
    QPointer<QVariantAnimation> m_drawerAnim;
    bool m_drawerOpen = false;
    QPushButton *m_apiWarn = nullptr;
    QPushButton *m_settingsButton = nullptr;
    QPushButton *m_instanceChip = nullptr;
    QLabel *m_instanceValue = nullptr;
    QPushButton *m_moreButton = nullptr;
    QFrame *m_moreMenu = nullptr;
    QFrame *m_instanceMenu = nullptr;
    QPushButton *m_renameAction = nullptr;
    QPushButton *m_pinAction = nullptr;
    QPushButton *m_exportAction = nullptr;
    QPushButton *m_deleteAction = nullptr;
    QCheckBox *m_encryptCheck = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_messageHost = nullptr;
    QVBoxLayout *m_messageLayout = nullptr;
    QWidget *m_emptyState = nullptr;
    QLabel *m_emptyTitle = nullptr;
    QLabel *m_emptyText = nullptr;
    QPushButton *m_emptyAction = nullptr;
    QWidget *m_notice = nullptr;
    QLabel *m_noticeText = nullptr;
    QWidget *m_skillBar = nullptr;
    QHBoxLayout *m_skillLayout = nullptr;
    QLabel *m_workflowStatus = nullptr;
    QPushButton *m_autoSafeButton = nullptr;
    AgentComposer *m_composer = nullptr;
    QLabel *m_keyHint = nullptr;
    QPushButton *m_send = nullptr;
    QPushButton *m_stop = nullptr;
    QPushButton *m_loadEarlier = nullptr;
    /// 会话里的全部消息；`m_views` 只是 `m_allMessages[m_visibleFrom..]` 那一段的视图。
    QList<AgentView::Message> m_allMessages;
    int m_visibleFrom = 0;
    QHash<QString, AgentMessageView *> m_views;
    QHash<QString, AgentToolCard *> m_toolCards;
    AgentApprovalCard *m_approvalCard = nullptr;
    QHash<QString, AgentView::SessionMeta> m_metaById;
    QString m_currentSessionId;
    QString m_modelLabel;
    QString m_statusText;
    bool m_streaming = false;
    bool m_configured = false;
    /// 用户是不是贴着底：贴底时新内容跟随，上翻后不抢位置（Ctrl+End 或滚回底部恢复）。
    bool m_stickyBottom = true;
};
