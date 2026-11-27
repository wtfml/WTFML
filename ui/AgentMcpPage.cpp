/**
 * @file AgentMcpPage.cpp
 * @brief MCP 服务器设置页（见 .hpp）。
 */

#include "AgentMcpPage.hpp"

#include "ChromeDialog.hpp"
#include "Theme.hpp"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {

QLabel *caption(QWidget *parent, const QString &name) {
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setWordWrap(true);
    QFont font = label->font();
    font.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
    label->setFont(font);
    label->setProperty(Theme::textRolePropertyName().toUtf8().constData(),
                       Theme::secondaryTextRoleValue());
    return label;
}

QString stateText(const AgentView::McpServerRow &row) {
    if (!row.enabled) return QCoreApplication::translate("AgentMcpPage", "未启用");
    if (row.state == QStringLiteral("ready")) {
        return QCoreApplication::translate("AgentMcpPage", "已连接（%1 个工具）").arg(row.toolCount);
    }
    if (row.state == QStringLiteral("idle") || row.state.isEmpty()) {
        return QCoreApplication::translate("AgentMcpPage", "未连接");
    }
    if (row.state == QStringLiteral("failed")) return QCoreApplication::translate("AgentMcpPage", "失败");
    return QCoreApplication::translate("AgentMcpPage", "连接中…");
}

}  // namespace

AgentMcpPage::AgentMcpPage(QWidget *parent) : QDialog(parent) {
    setProperty("wtfmlSelfScrollingPage", true);
    setObjectName(QStringLiteral("agentMcpPage"));
    setWindowTitle(tr("MCP 服务器"));
    setModal(true);
    resize(620, 560);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);

    auto *title = new QLabel(tr("MCP 服务器（stdio）"), this);
    title->setObjectName(QStringLiteral("agentMcpTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(Theme::textSize(Theme::TextRole::Heading));
    title->setFont(titleFont);
    root->addWidget(title);

    m_note = caption(this, QStringLiteral("agentMcpNote"));
    m_note->setText(tr("MCP 服务器以当前 Windows 用户权限运行，可读写本机文件与网络。仅添加可信服务器；首次启动、修改配置、重新连接或可执行文件变更前均须确认。"));
    root->addWidget(m_note);

    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("agentMcpScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *host = new QWidget(scroll);
    host->setObjectName(QStringLiteral("agentMcpHost"));
    m_listLayout = new QVBoxLayout(host);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(8);
    m_empty = caption(host, QStringLiteral("agentMcpEmpty"));
    m_empty->setText(tr("暂无服务器。"));
    m_listLayout->addWidget(m_empty);
    m_listLayout->addStretch(1);
    scroll->setWidget(host);
    root->addWidget(scroll, 1);

    m_log = caption(this, QStringLiteral("agentMcpLog"));
    m_log->hide();
    root->addWidget(m_log);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    auto *add = new QPushButton(tr("添加服务器…"), this);
    add->setObjectName(QStringLiteral("agentMcpAdd"));
    auto *refresh = new QPushButton(tr("刷新"), this);
    refresh->setObjectName(QStringLiteral("agentMcpRefresh"));
    auto *close = new QPushButton(tr("关闭"), this);
    close->setObjectName(QStringLiteral("agentMcpClose"));
    buttons->addWidget(add);
    buttons->addWidget(refresh);
    buttons->addStretch(1);
    buttons->addWidget(close);
    root->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, &AgentMcpPage::addRequested);
    connect(refresh, &QPushButton::clicked, this, &AgentMcpPage::refreshRequested);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
}

void AgentMcpPage::setServers(const QList<AgentView::McpServerRow> &servers) {
    m_rows = servers;
    rebuild();
}

void AgentMcpPage::setNote(const QString &text) {
    if (m_note != nullptr) m_note->setText(text);
}

void AgentMcpPage::setLog(const QString &text) {
    if (m_log == nullptr) return;
    m_log->setText(text);
    m_log->setVisible(!text.isEmpty());
}

void AgentMcpPage::rebuild() {
    if (m_listLayout == nullptr) return;
    // 只清服务器行：小标题、空态与末尾弹簧按名字/位置保留。
    for (int i = m_listLayout->count() - 1; i >= 0; --i) {
        QLayoutItem *item = m_listLayout->itemAt(i);
        if (item == nullptr) continue;
        QWidget *widget = item->widget();
        if (widget == nullptr) continue;
        if (widget->objectName() != QStringLiteral("agentMcpRow")) continue;
        m_listLayout->takeAt(i);
        widget->hide();
        widget->setParent(nullptr);
        widget->deleteLater();
        delete item;
    }
    m_empty->setVisible(m_rows.isEmpty());
    int insertAt = m_listLayout->count() > 0 ? m_listLayout->count() - 1 : 0;
    for (const AgentView::McpServerRow &row : m_rows) {
        auto *frame = new QFrame(m_empty->parentWidget());
        frame->setObjectName(QStringLiteral("agentMcpRow"));
        frame->setProperty("serverId", row.id);
        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(4);
        auto *head = new QHBoxLayout;
        head->setContentsMargins(0, 0, 0, 0);
        auto *name = new QLabel(row.name, frame);
        name->setObjectName(QStringLiteral("agentMcpRowName"));
        QFont nameFont = name->font();
        nameFont.setBold(true);
        name->setFont(nameFont);
        head->addWidget(name);
        auto *state = caption(frame, QStringLiteral("agentMcpRowState"));
        state->setText(stateText(row));
        head->addStretch(1);
        head->addWidget(state);
        layout->addLayout(head);
        auto *command = caption(frame, QStringLiteral("agentMcpRowCommand"));
        command->setText(row.command);
        command->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(command);
        auto *identity = caption(frame, QStringLiteral("agentMcpRowIdentity"));
        identity->setText(row.identityShort.isEmpty()
                              ? tr("尚未确认（首次启动前须确认可执行文件身份）")
                              : tr("SHA-256：%1…").arg(row.identityShort));
        layout->addWidget(identity);
        if (!row.detail.isEmpty()) {
            auto *detail = caption(frame, QStringLiteral("agentMcpRowDetail"));
            detail->setText(row.detail);
            layout->addWidget(detail);
        }
        auto *rowButtons = new QHBoxLayout;
        rowButtons->setContentsMargins(0, 0, 0, 0);
        auto *connectButton = new QPushButton(
            row.state == QStringLiteral("ready") ? tr("重新连接") : tr("连接"),
            frame);
        connectButton->setObjectName(QStringLiteral("agentMcpConnect_") + row.id);
        auto *disconnectButton = new QPushButton(tr("断开"), frame);
        disconnectButton->setObjectName(QStringLiteral("agentMcpDisconnect_") + row.id);
        auto *editButton = new QPushButton(tr("编辑…"), frame);
        editButton->setObjectName(QStringLiteral("agentMcpEdit_") + row.id);
        auto *removeButton = new QPushButton(tr("删除"), frame);
        removeButton->setObjectName(QStringLiteral("agentMcpRemove_") + row.id);
        rowButtons->addStretch(1);
        rowButtons->addWidget(connectButton);
        rowButtons->addWidget(disconnectButton);
        rowButtons->addWidget(editButton);
        rowButtons->addWidget(removeButton);
        layout->addLayout(rowButtons);
        const QString id = row.id;
        connect(connectButton, &QPushButton::clicked, this,
                [this, id] { emit connectRequested(id); });
        connect(disconnectButton, &QPushButton::clicked, this,
                [this, id] { emit disconnectRequested(id); });
        connect(editButton, &QPushButton::clicked, this,
                [this, id] { emit editRequested(id); });
        connect(removeButton, &QPushButton::clicked, this,
                [this, id] { emit removeRequested(id); });
        m_listLayout->insertWidget(insertAt++, frame);
    }
}

bool AgentMcpPage::editDraft(QWidget *parent, const QString &title,
                             AgentView::McpServerDraft *draft) {
    if (draft == nullptr) return false;
    ChromeDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("agentMcpEditDialog"));
    dialog.setWindowTitle(title);
    dialog.setModal(true);
    dialog.resize(600, 420);
    auto *layout = new QVBoxLayout(dialog.body());
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(draft->name, &dialog);
    name->setObjectName(QStringLiteral("agentMcpNameEdit"));
    auto *command = new QLineEdit(draft->command, &dialog);
    command->setObjectName(QStringLiteral("agentMcpCommandEdit"));
    command->setPlaceholderText(QCoreApplication::translate("AgentMcpPage", "绝对路径，例如 C:\\Servers\\mcp\\server.exe"));
    auto *arguments = new QPlainTextEdit(draft->arguments, &dialog);
    arguments->setObjectName(QStringLiteral("agentMcpArgumentsEdit"));
    arguments->setFrameShape(QFrame::NoFrame);
    arguments->setMaximumHeight(90);
    arguments->setPlaceholderText(QCoreApplication::translate("AgentMcpPage", "一行一个参数，不经过 shell"));
    auto *workingDirectory = new QLineEdit(draft->workingDirectory, &dialog);
    workingDirectory->setObjectName(QStringLiteral("agentMcpWorkdirEdit"));
    auto *environment = new QLineEdit(draft->environmentNames, &dialog);
    environment->setObjectName(QStringLiteral("agentMcpEnvEdit"));
    environment->setPlaceholderText(QCoreApplication::translate("AgentMcpPage", "透传的环境变量名，逗号分隔（仅保存名称）"));
    auto *enabled = new QCheckBox(QCoreApplication::translate("AgentMcpPage", "启用服务器"), &dialog);
    enabled->setObjectName(QStringLiteral("agentMcpEnabledEdit"));
    Theme::markSwitch(enabled);
    enabled->setChecked(draft->enabled);
    form->addRow(QCoreApplication::translate("AgentMcpPage", "名称"), name);
    form->addRow(QCoreApplication::translate("AgentMcpPage", "可执行文件"), command);
    form->addRow(QCoreApplication::translate("AgentMcpPage", "参数"), arguments);
    form->addRow(QCoreApplication::translate("AgentMcpPage", "工作目录"), workingDirectory);
    form->addRow(QCoreApplication::translate("AgentMcpPage", "环境变量"), environment);
    form->addRow(QString(), enabled);
    layout->addLayout(form);
    auto *note = caption(&dialog, QStringLiteral("agentMcpEditNote"));
    note->setText(QCoreApplication::translate("AgentMcpPage", "保存配置后须重新确认；不会自动下载或安装 MCP 服务器。"));
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return false;
    draft->name = name->text().trimmed();
    draft->command = command->text().trimmed();
    draft->arguments = arguments->toPlainText();
    draft->workingDirectory = workingDirectory->text().trimmed();
    draft->environmentNames = environment->text().trimmed();
    draft->enabled = enabled->isChecked();
    return true;
}
