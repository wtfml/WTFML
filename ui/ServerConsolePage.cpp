/**
 * @file ServerConsolePage.cpp
 * @brief 服务端控制台那一页的实现（形状与取舍见头文件）。
 */

#include "ServerConsolePage.hpp"
#include <QCoreApplication>
#include "InputSizing.hpp"

#include <QComboBox>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCursor>  // setLog() 末尾那一下"落到最底下"
#include <QVBoxLayout>

#include "MainWindowUI.hpp"
#include "Theme.hpp"

namespace {

/// 一行小字（状态那句）。和 `NewServerPage.cpp` 里那个 `makeMutedLabel()` 同一套做法
/// —— 全仓"比正文浅一档"的取法只有这一种（从自己的调色板算，不写死颜色）。
QLabel *makeMutedLabel(QWidget *parent, const QString &objectName) {
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    QPalette palette = label->palette();
    QColor muted = palette.color(QPalette::WindowText);
    muted.setAlpha(150);
    palette.setColor(QPalette::WindowText, muted);
    label->setPalette(palette);
    return label;
}

}  // namespace

ServerConsolePage::ServerConsolePage(QWidget *parent) : QDialog(parent) {
    setObjectName(MainWindowUI::serverConsolePageName());
    setWindowTitle(windowTitle());
    // 只影响"单独拿出来用"（验证工程那几处）：嵌进覆盖层时这一页拿的是内容区的整幅宽。
    resize(880, 620);

    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(12, 12, 12, 12);
    box->setSpacing(10);

    // ---------- 顶上：哪一个服务端 + 它现在什么状态 ----------
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        m_titleLabel = new QLabel(row);
        m_titleLabel->setObjectName(MainWindowUI::serverConsoleTitleName());
        QFont titleFont = m_titleLabel->font();
        titleFont.setBold(true);
        m_titleLabel->setFont(titleFont);
        // 名字可能很长（用户起的）：不许它把状态那半截挤出去。**这一句要在 setFont 之后**
        // （尺寸策略跟字体无关，但下面取调色板那一步跟字体有关，这里保持一致）。
        m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(m_titleLabel, 1);
        m_statusLabel = makeMutedLabel(row, MainWindowUI::serverConsoleStatusName());
        layout->addWidget(m_statusLabel, 0);
        box->addWidget(row);
    }

    // ---------- 搜索 / 显示多少行 / 导出 ----------
    //
    // 这三件和游戏日志窗**共用同一份模型与文案**（`RuntimeOutputModel`）：搜索是大小写
    // 不分的子串，显示上限只影响视图，导出拿的永远是整份日志。
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        m_searchEdit = new QLineEdit(row);
        m_searchEdit->setObjectName(searchEditName());
        m_searchEdit->setPlaceholderText(searchPlaceholderText());
        m_searchEdit->setClearButtonEnabled(true);
        InputSizing::longValue(m_searchEdit, QCoreApplication::translate("ServerConsolePage", "搜索框：筛选日志行，关键词长度不定"));
        m_searchEdit->setToolTip(
            QCoreApplication::translate("ServerConsolePage", "只筛选当前显示的行；「导出日志…」仍导出完整日志。"));
        connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
            m_model.setSearchText(text);
            rebuildView();
        });
        layout->addWidget(m_searchEdit, 1);

        m_lineLimitCombo = new QComboBox(row);
        m_lineLimitCombo->setObjectName(lineLimitComboName());
        for (int limit : RuntimeOutputModel::lineLimitChoices()) {
            m_lineLimitCombo->addItem(RuntimeOutputModel::lineLimitText(limit), limit);
        }
        m_lineLimitCombo->setToolTip(
            QCoreApplication::translate("ServerConsolePage", "只影响显示，日志本身一行都不会少。"));
        m_lineLimitCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_lineLimitCombo->setMinimumContentsLength(8);
        // 滚轮防护：这是一页"滚着看输出"的地方，鼠标划过下拉框的机会不少。
        WheelGuard::install(m_lineLimitCombo);
        for (int i = 0; i < m_lineLimitCombo->count(); ++i) {
            if (m_lineLimitCombo->itemData(i).toInt() == m_model.displayLimit()) {
                m_lineLimitCombo->setCurrentIndex(i);
                break;
            }
        }
        connect(m_lineLimitCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
            setDisplayLineLimit(RuntimeOutputModel::lineLimitOfIndex(index));
        });
        layout->addWidget(m_lineLimitCombo, 0);

        m_exportButton = new QPushButton(QCoreApplication::translate("ServerConsolePage", "导出日志…"), row);
        m_exportButton->setObjectName(exportLogButtonName());
        m_exportButton->setToolTip(
            QCoreApplication::translate("ServerConsolePage", "把控制台全部输出存成文本文件（含被搜掉的行）。"));
        // 只读、低频 ⇒ Ghost（和游戏日志窗那两颗导出键同一档）。
        Theme::markButton(m_exportButton, Theme::ButtonKind::Ghost);
        connect(m_exportButton, &QPushButton::clicked, this,
                &ServerConsolePage::exportLogRequested);
        layout->addWidget(m_exportButton, 0);
        box->addWidget(row);
    }

    // ---------- 中间那一大片：它的输出 ----------
    {
        m_logView = new QPlainTextEdit(this);
        m_logView->setObjectName(MainWindowUI::serverConsoleLogName());
        m_logView->setReadOnly(true);
        // 等宽字体：服务端那些日志是对着列的（时间戳 / 级别 / 正文），比例字体一读就歪。
        // 问 `QFontDatabase` 要系统那一个，不写死字体名 —— 写死的话换台机器就退回去了。
        QFont logFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        m_logView->setFont(logFont);
        // 不许折行：折了之后一行日志占两行、时间戳和正文错开，反而更难读（横向滚就是了）。
        m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
        // NOTE: 这里**不做**选中复制那一套特别设置 —— QPlainTextEdit 默认就能选中复制，
        // 而这一页正是"用户要拿一行去搜"的地方。
        box->addWidget(m_logView, 1);
    }

    // ---------- 底下：往里敲一行 ----------
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        m_commandEdit = new QLineEdit(row);
        m_commandEdit->setObjectName(MainWindowUI::serverConsoleCommandEditName());
        InputSizing::longValue(m_commandEdit, QCoreApplication::translate("ServerConsolePage", "命令输入：像聊天框一样整行"));
        m_commandEdit->setPlaceholderText(
            QCoreApplication::translate("ServerConsolePage", "往这个服务端敲一条命令（比如 say 大家好），回车发送"));
        layout->addWidget(m_commandEdit, 1);
        m_sendButton = new QPushButton(MainWindowUI::serverConsoleSendText(), row);
        m_sendButton->setObjectName(MainWindowUI::serverConsoleSendButtonName());
        // 送一行命令 = 这一页的主操作，走主题色那一档（和「创建」「新建服务端…」同一档）。
        Theme::markButton(m_sendButton, Theme::ButtonKind::Accent);
        layout->addWidget(m_sendButton, 0);
        box->addWidget(row);

        // 回车 = 发送（和按那颗按钮一回事）。
        connect(m_commandEdit, &QLineEdit::returnPressed, m_sendButton, &QPushButton::click);
        connect(m_sendButton, &QPushButton::clicked, this, [this] {
            const QString text = m_commandEdit->text().trimmed();
            if (text.isEmpty()) {
                return;  // 空行不发（服务端那边收到一个空命令只会回一句没用的报错）
            }
            // 命令历史：**发出去的那一刻**记，连续重复的不记（按两下回车
            // 不该在历史里留下两条一模一样的）。
            if (m_history.isEmpty() || m_history.last() != text) {
                m_history.append(text);
                while (m_history.size() > maxHistoryEntries()) {
                    m_history.removeFirst();
                }
            }
            m_historyCursor = m_history.size();
            m_historyDraft.clear();
            emit commandSubmitted(text);
            m_commandEdit->clear();
        });
        // 上/下键走历史（在输入框上装一个事件过滤器，见 eventFilter()）。
        m_commandEdit->installEventFilter(this);
        // 用户自己动手改了字 ⇒ 退出"历史浏览"状态（下一次按上键从最新一条开始）。
        connect(m_commandEdit, &QLineEdit::textEdited, this, [this](const QString &text) {
            m_historyCursor = m_history.size();
            m_historyDraft = text;
        });
    }

    // ---------- 最下面：停它 ----------
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        layout->addStretch(1);
        m_forceKillButton = new QPushButton(MainWindowUI::serverConsoleForceKillText(), row);
        m_forceKillButton->setObjectName(MainWindowUI::serverConsoleForceKillButtonName());
        // 强杀 = 可能丢存档 ⇒ 危险那一档的红（和「删除这个服务端」同一档）。
        Theme::markButton(m_forceKillButton, Theme::ButtonKind::Danger);
        m_forceKillButton->setToolTip(
            QCoreApplication::translate("ServerConsolePage", "不等待保存就强制结束进程，可能丢失存档。"));
        layout->addWidget(m_forceKillButton, 0);
        m_stopButton = new QPushButton(MainWindowUI::serverConsoleStopText(), row);
        m_stopButton->setObjectName(MainWindowUI::serverConsoleStopButtonName());
        Theme::markButton(m_stopButton, Theme::ButtonKind::Neutral);
        m_stopButton->setToolTip(QCoreApplication::translate("ServerConsolePage", "让它自己存盘、收尾，然后退出（推荐）。"));
        layout->addWidget(m_stopButton, 0);
        box->addWidget(row);

        connect(m_stopButton, &QPushButton::clicked, this, &ServerConsolePage::stopRequested);
        connect(m_forceKillButton, &QPushButton::clicked, this,
                &ServerConsolePage::forceKillRequested);
    }

    // 起手是"什么都没在跑"那一档（`MainWindow` 开页时会立刻喂一遍真状态）。
    setStopAvailable(false);
    setForceKillAvailable(false);
    setCommandAvailable(false);
}

void ServerConsolePage::setServerName(const QString &name) {
    if (m_titleLabel != nullptr) {
        m_titleLabel->setText(name);
    }
    setWindowTitle(name.isEmpty() ? windowTitle() : name);
}

void ServerConsolePage::setStatusText(const QString &text) {
    if (m_statusLabel != nullptr) {
        m_statusLabel->setText(text);
    }
}

void ServerConsolePage::setStopAvailable(bool available) {
    if (m_stopButton != nullptr) {
        m_stopButton->setEnabled(available);
    }
}

void ServerConsolePage::setForceKillAvailable(bool available) {
    if (m_forceKillButton != nullptr) {
        m_forceKillButton->setEnabled(available);
    }
}

void ServerConsolePage::setCommandAvailable(bool available) {
    if (m_commandEdit != nullptr) {
        m_commandEdit->setEnabled(available);
    }
    if (m_sendButton != nullptr) {
        m_sendButton->setEnabled(available);
    }
}

void ServerConsolePage::setMaxLines(int lines) {
    // 保留上限钉在**共用模型**上；视图的块数上限跟着它（或者跟着显示上限）走。
    m_model.setCapacity(qMax(1, lines));
    if (m_logView != nullptr) {
        m_logView->setMaximumBlockCount(viewBlockLimit());
    }
}

int ServerConsolePage::viewBlockLimit() const {
    const int shown = m_model.displayLimit();
    if (shown > 0) {
        return qMin(shown, m_model.capacity());
    }
    return m_model.capacity();
}

void ServerConsolePage::rebuildView() {
    if (m_logView == nullptr) {
        return;
    }
    QStringList shown;
    for (const RuntimeOutputModel::VisibleLine &line : m_model.visibleLines()) {
        shown.append(line.text);
    }
    m_logView->setPlainText(shown.join(QLatin1Char('\n')));
    // 落到最底下：用户开这一页十有八九是来看"刚才那几行"的。
    m_logView->moveCursor(QTextCursor::End);
    m_logView->ensureCursorVisible();

    // 空的时候说清**为什么**空（三句话和三件事对应，和游戏日志窗那边同一套）。
    if (m_model.lines().isEmpty()) {
        m_logView->setPlaceholderText(QCoreApplication::translate("ServerConsolePage", "等待服务端输出日志…"));
    } else if (m_model.matchingLineCount() == 0) {
        m_logView->setPlaceholderText(
            m_model.searchText().isEmpty()
                ? QCoreApplication::translate("ServerConsolePage", "当前筛选下没有日志。")
                : QCoreApplication::translate("ServerConsolePage", "没有匹配的日志。换个关键词，或清空搜索框。"));
    } else {
        m_logView->setPlaceholderText(QString());
    }
}

void ServerConsolePage::setLog(const QStringList &lines) {
    if (m_logView == nullptr) {
        return;
    }
    m_model.clear();
    for (const QString &line : lines) {
        m_model.addLine(line);
    }
    rebuildView();
}

void ServerConsolePage::appendLine(const QString &line) {
    if (m_logView == nullptr) {
        return;
    }
    const RuntimeOutputModel::Level level = m_model.addLine(line);
    // 被搜索筛掉的行不进视图（模型里照样留着）；这一行进了视图时，`appendPlainText()`
    // 会自己补换行并把滚动条钉到底 —— 那正是"一条新消息来了"的意思。
    if (m_model.matches(level, line)) {
        m_logView->appendPlainText(line);
    }
}

QStringList ServerConsolePage::logLines() const {
    QStringList lines;
    lines.reserve(m_model.lineCount());
    for (const RuntimeOutputModel::Line &line : m_model.lines()) {
        lines.append(line.text);
    }
    return lines;
}

QStringList ServerConsolePage::visibleLogLines() const {
    QStringList lines;
    for (const RuntimeOutputModel::VisibleLine &line : m_model.visibleLines()) {
        lines.append(line.text);
    }
    return lines;
}

QString ServerConsolePage::allLogText() const {
    return m_model.allText();
}

void ServerConsolePage::setSearchText(const QString &text) {
    if (m_searchEdit != nullptr) {
        m_searchEdit->setText(text);  // 走 textChanged → rebuildView()
        return;
    }
    m_model.setSearchText(text);
}

QString ServerConsolePage::searchText() const {
    return m_searchEdit != nullptr ? m_searchEdit->text() : m_model.searchText();
}

void ServerConsolePage::setDisplayLineLimit(int limit) {
    if (limit == m_model.displayLimit()) {
        return;
    }
    m_model.setDisplayLimit(limit);
    if (m_lineLimitCombo != nullptr) {
        const QSignalBlocker blocker(m_lineLimitCombo);
        for (int i = 0; i < m_lineLimitCombo->count(); ++i) {
            if (m_lineLimitCombo->itemData(i).toInt() == limit) {
                m_lineLimitCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    if (m_logView != nullptr) {
        m_logView->setMaximumBlockCount(viewBlockLimit());
    }
    rebuildView();
}

QString ServerConsolePage::searchPlaceholderText() {
    return QCoreApplication::translate("ServerConsolePage", "在这个控制台里找…（大小写不分）");
}

bool ServerConsolePage::stepHistory(int delta) {
    if (m_commandEdit == nullptr || m_history.isEmpty() || delta == 0) {
        return false;
    }
    // 第一次按上键时把手上那半截先收着；从历史里一路翻下去再按下来能回到它。
    if (m_historyCursor == m_history.size()) {
        m_historyDraft = m_commandEdit->text();
    }
    const int next = qBound(0, m_historyCursor + delta, m_history.size());
    if (next == m_historyCursor) {
        return false;
    }
    m_historyCursor = next;
    const QString text =
        next == m_history.size() ? m_historyDraft : m_history.at(next);
    m_commandEdit->setText(text);
    m_commandEdit->setCursorPosition(text.size());
    return true;
}

bool ServerConsolePage::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_commandEdit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Up && key->modifiers() == Qt::NoModifier) {
            return stepHistory(-1);
        }
        if (key->key() == Qt::Key_Down && key->modifiers() == Qt::NoModifier) {
            return stepHistory(+1);
        }
    }
    return QDialog::eventFilter(watched, event);
}

QString ServerConsolePage::commandText() const {
    return m_commandEdit != nullptr ? m_commandEdit->text() : QString();
}
