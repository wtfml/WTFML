/**
 * @file ServerSettingsPage.cpp
 * @brief 「服务端设置」那一整页的实现（形状与取舍见头文件）。
 */

#include "ServerSettingsPage.hpp"

#include <QComboBox>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "MainWindowUI.hpp"
#include "InstanceSettingsDialog.hpp"
#include "RowActivation.hpp"
// MOTD 那块预览：`§` 码那套解析在 `MotdText` 里（ui 层自己的东西，反过来 include src
// 才是犯规）。这里只做"段 → 富文本"的最后一步。
#include "MotdText.hpp"
#include "Theme.hpp"
#include "ToggleSwitch.hpp"
#include "VersionTreeDelegate.hpp"

namespace {

/// 表单那一行里，行首那个名字占多宽。比 `NewServerPage` 的 96 宽一档：这一页的名字里
/// 「允许飞行」「生成怪物」这种四字词不少，96 会把右边那行小字顶到半截上。
constexpr int kFieldLabelWidth = 132;
/// 表单那一行的**控件**占多宽（开关那档除外，它按 `ToggleSwitch::sizeHint()`）。
/// 钉死是为了让右边那列小字**对成一竖** —— 每行控件的宽度不一样的话，小字的起点会一行一个样。
constexpr int kFieldEditorWidth = 200;

/// 一行小字（说明 / 状态）。全仓"比正文浅一档"的取法只有这一种（从自己的调色板算，
/// 不写死颜色）—— 和 `NewServerPage.cpp` / `ServerConsolePage.cpp` 那两个是同一套做法。
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

/// 表单行右边那行小字（**没有 objectName**：一页里有十几条，名字只会是噪音；要认得它们
/// 就走它左边那一格的 `serverSettingsFieldName()`）。
QLabel *makeHintLabel(QWidget *parent, const QString &text) {
    auto *label = new QLabel(text, parent);
    QPalette palette = label->palette();
    QColor muted = palette.color(QPalette::WindowText);
    muted.setAlpha(140);
    palette.setColor(QPalette::WindowText, muted);
    label->setPalette(palette);
    label->setWordWrap(true);
    label->setToolTip(text);
    return label;
}

/// 一行小字（正文那档，不是"浅一档"）。备份 / 体检两页顶上那句说明用它。
QLabel *makeNoteLabel(QWidget *parent, const QString &text) {
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // 折行 QLabel 的 sizeHint 是"整句排成一行"的宽度 —— 纯说明不许参与横向度量（同一条规矩）。
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

/**
 * @brief 把一串 MOTD（带 `§` 码）变成 QLabel 吃的那份富文本。
 *
 * `MotdText::parse()` 已经把"哪一段是什么颜色 / 什么字形"拆好了（见那个文件头：色号、
 * `§l/m/n/o`、`§r` 复位、`§x` 十六进制都认）。这一步只剩三件事：
 *
 * - **按行拼 `<br>`**（游戏那边 MOTD 就是两行，`parse(..., 2)` 第 3 行起就丢了）；
 * - **颜色照游戏那 16 个色号**（`pieceColor()`），不按我们的主题走 —— 这一块画的是
 * "别的玩家在服务器列表里看到的样子"，改成我们的主题色就不像了；
 * - **空格换成 `&nbsp;`**：HTML 会把行首空格和连续空格吃掉，而 MOTD 里那几种空格正是
 * 服务器拿来对齐的。
 */
QString motdPreviewHtml(const QString &raw) {
    const QVector<MotdPiece> pieces = MotdText::parse(raw, 2);
    if (pieces.isEmpty()) {
        return QStringLiteral("<span style=\"color:#9a9a9a;\">%1</span>")
            .arg(MainWindowUI::serverSettingsMotdEmptyText().toHtmlEscaped());
    }
    QString html;
    int line = 0;
    for (const MotdPiece &piece : pieces) {
        while (line < piece.line) {
            html += QStringLiteral("<br>");
            ++line;
        }
        QString text = piece.text.toHtmlEscaped();
        text.replace(QLatin1Char(' '), QStringLiteral("&nbsp;"));
        QStringList styles;
        styles << QStringLiteral("color:%1").arg(MotdText::pieceColor(piece).name());
        if (piece.bold) styles << QStringLiteral("font-weight:bold");
        if (piece.italic) styles << QStringLiteral("font-style:italic");
        QStringList decorations;
        if (piece.underline) decorations << QStringLiteral("underline");
        if (piece.strike) decorations << QStringLiteral("line-through");
        if (!decorations.isEmpty()) {
            styles << QStringLiteral("text-decoration:%1").arg(decorations.join(QLatin1Char(' ')));
        }
        html += QStringLiteral("<span style=\"%1\">%2</span>").arg(styles.join(QLatin1Char(';')),
                                                                    text);
    }
    return html;
}

/// 往右栏那一叠里加一页。**每一页都活在一层滚动区里**（和实例设置那几页同一个形状）：
/// 内容比视口高的时候自己能滚；换页动画抬的是视口（见构造末尾那条 `currentChanged`）。
///
/// @param objectName 里面那一页的名字（没名字就传空串 —— 表里一页一张，名字只能是噪音）。
/// @return 那一页自己的竖排布局（往它里面加东西）。
QVBoxLayout *addSectionPage(QStackedWidget *stack, const QString &objectName) {
    auto *scroll = new Theme::GutterScrollArea(stack);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto *page = new QWidget(scroll);
    if (!objectName.isEmpty()) {
        page->setObjectName(objectName);
    }
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    scroll->setWidget(page);
    stack->addWidget(scroll);
    return layout;
}

}  // namespace

ServerSettingsPage::ServerSettingsPage(QWidget *parent) : QDialog(parent) {
    setObjectName(MainWindowUI::serverSettingsPageName());
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
        m_titleLabel->setObjectName(MainWindowUI::serverSettingsTitleName());
        QFont titleFont = m_titleLabel->font();
        titleFont.setBold(true);
        titleFont = Theme::textFont(titleFont, Theme::TextRole::Heading);
        m_titleLabel->setFont(titleFont);
        // 名字可能很长（用户起的）：不许它把状态那半截挤出去。
        m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(m_titleLabel, 1);
        m_statusLabel = makeMutedLabel(row, MainWindowUI::serverSettingsStatusName());
        layout->addWidget(m_statusLabel, 0);
        box->addWidget(row);
    }

    // ---------- 两栏：左栏四页 + 右栏一个堆叠页 ----------
    auto *columns = new QHBoxLayout;
    columns->setSpacing(12);

    m_sectionList = new QTreeWidget(this);
    m_sectionList->setObjectName(MainWindowUI::serverSettingsSectionListName());
    m_sectionList->setHeaderHidden(true);
    m_sectionList->setUniformRowHeights(false);
    m_sectionList->setIndentation(16);
    m_sectionList->setExpandsOnDoubleClick(false);
    m_sectionList->setFrameShape(QFrame::NoFrame);
    m_sectionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sectionList->setFixedWidth(InstanceSettingsDialog::navigationTreeWidth());
    // 悬停底色那一套由代理画（`Theme::rowHoverFill()`），但它要的 `State_MouseOver` 是
    // **视图**填的 —— 少了下一句，代理里那一段一个字都不会生效（同实例设置那一列）。
    m_sectionList->setMouseTracking(true);
    // 与实例设置的树导航共用标准行样式，避免下载列表代理再绘制一枚右侧箭头。
    QFont navFont = m_sectionList->font();
    navFont = Theme::textFont(navFont, Theme::TextRole::Body);
    m_sectionList->setFont(navFont);
    for (int i = 0; i < MainWindowUI::serverSettingsSectionCount(); ++i) {
        auto *item = new QTreeWidgetItem(m_sectionList, {MainWindowUI::serverSettingsSectionTitle(i)});
        item->setData(0, Qt::UserRole, i);
        if (i == static_cast<int>(Section::Players)) {
            for (int j = 0; j < MainWindowUI::serverSettingsPlayerListCount(); ++j) {
                auto *child = new QTreeWidgetItem(item, {MainWindowUI::serverSettingsPlayerListTitle(j)});
                child->setData(0, Qt::UserRole, i);
                child->setData(0, Qt::UserRole + 1, j);
            }
        } else if (i == static_cast<int>(Section::Content)) {
            for (int j = 0; j < MainWindowUI::serverSettingsContentKindCount(); ++j) {
                auto *child = new QTreeWidgetItem(item, {MainWindowUI::serverSettingsContentKindTitle(j)});
                child->setData(0, Qt::UserRole, i);
                child->setData(0, Qt::UserRole + 1, j);
            }
        }
        item->setSizeHint(0, QSize(0, item->childCount() > 0 ? 32 : 36));
        for (int j = 0; j < item->childCount(); ++j)
            item->child(j)->setSizeHint(0, QSize(0, 36));
    }
    {
        auto *column = new QWidget(this);
        auto *layout = new QVBoxLayout(column);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(m_sectionList);
        column->setFixedWidth(InstanceSettingsDialog::navigationTreeWidth());
        columns->addWidget(column);
    }

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(MainWindowUI::serverSettingsStackName());
    columns->addWidget(m_stack, 1);
    // 换页动效不用接：`QStackedWidget` 换页默认就有 `pageAppear()`（`ui/DesignDefaults.hpp`）。
    box->addLayout(columns, 1);

    // ---------- ① 常用设置（表单）----------
    {
        m_formLayout = addSectionPage(m_stack, MainWindowUI::serverSettingsFormName());
        m_formBody = m_formLayout->parentWidget();
        // 弹簧**摆在收尾**：新加的行一律插在它前面（`addFieldRow()` 那一下）。摆中间的话，
        // 重摆表单时得先把它认出来、再想好插哪儿（同实例设置那条规矩）。
        m_formLayout->addStretch(1);
    }

    // ---------- ② 原始文本 ----------
    {
        auto *layout = addSectionPage(m_stack, QString());
        m_rawNote = makeNoteLabel(nullptr, MainWindowUI::serverSettingsRawNoteText());
        m_rawNote->setObjectName(MainWindowUI::serverSettingsRawNoteName());
        layout->addWidget(m_rawNote);

        m_rawEdit = new QPlainTextEdit();
        m_rawEdit->setObjectName(MainWindowUI::serverSettingsRawEditName());
        // 等宽字体：这一份文件是对着列的（`key=value` + 一大段 `#` 注释），比例字体一读就歪。
        // 问 `QFontDatabase` 要系统那一个，不写死字体名 —— 写死的话换台机器就退回去了。
        m_rawEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        // 不折行：折了之后一行配置占两行、"这是第几行"就对不上了（横向滚就是了）。
        m_rawEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_rawEdit->setTabStopDistance(
            4.0 * QFontMetricsF(m_rawEdit->font()).horizontalAdvance(QLatin1Char(' ')));
        connect(m_rawEdit, &QPlainTextEdit::textChanged, this, [this] {
            // 程序摆进来的那一份不算"用户动过"（`setRawText()` 那一段）。
            if (m_settingRaw) {
                return;
            }
            m_rawEdited = true;
            refreshSaveBar();
            emit edited();
        });
        layout->addWidget(m_rawEdit, 1);
    }

    // ---------- ③ 内容（模组 / 数据包）----------
    //
    // 两类内容通过左栏子项选择，中间显示文件列表（名字 / 大小 / 状态），底下
    // 四颗按钮（加 / 启用禁用 / 打开文件夹 / 刷新）。**这一页不碰盘**：列目录、改名、拷
    // 文件全在 `MainWindow`（`ui/` 不许 include `src/`，判据在 `ServerContent` 那一层）。
    {
        auto *layout = addSectionPage(m_stack, QString());
        m_contentNote = makeNoteLabel(nullptr, QString());
        m_contentNote->setObjectName(MainWindowUI::serverSettingsContentNoteName());
        layout->addWidget(m_contentNote);

        m_contentTree = new QTreeWidget();
        m_contentTree->setObjectName(MainWindowUI::serverSettingsContentTreeName());
        m_contentTree->setColumnCount(MainWindowUI::serverSettingsContentColumns().size());
        m_contentTree->setHeaderLabels(MainWindowUI::serverSettingsContentColumns());
        m_contentTree->setRootIsDecorated(false);
        m_contentTree->setUniformRowHeights(true);
        m_contentTree->setAllColumnsShowFocus(true);
        m_contentTree->setSelectionMode(QAbstractItemView::SingleSelection);
        m_contentTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_contentTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_contentTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_contentTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        connect(m_contentTree, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem *, QTreeWidgetItem *) { refreshContentActions(); });
        layout->addWidget(m_contentTree, 1);

        {
            auto *row = new QWidget();
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);
            m_contentAddButton = new QPushButton(MainWindowUI::serverSettingsContentAddText(), row);
            m_contentAddButton->setObjectName(MainWindowUI::serverSettingsContentAddButtonName());
            Theme::markButton(m_contentAddButton, Theme::ButtonKind::Accent);
            m_contentAddButton->setCursor(Qt::PointingHandCursor);
            connect(m_contentAddButton, &QPushButton::clicked, this,
                    &ServerSettingsPage::contentAddRequested);
            rowLayout->addWidget(m_contentAddButton);
            rowLayout->addStretch(1);
            m_contentToggleButton =
                new QPushButton(MainWindowUI::serverSettingsContentDisableText(), row);
            m_contentToggleButton->setObjectName(
                MainWindowUI::serverSettingsContentToggleButtonName());
            // 改名而已，不做危险动作那一档（文件一个字节都不动，随时能改回来）。
            Theme::markButton(m_contentToggleButton, Theme::ButtonKind::Neutral);
            m_contentToggleButton->setCursor(Qt::PointingHandCursor);
            connect(m_contentToggleButton, &QPushButton::clicked, this, [this] {
                const QString name = selectedContentName();
                if (name.isEmpty()) {
                    return;
                }
                // 禁用着的按下去就是"启用"，反之是"禁用"。
                emit contentToggleRequested(name, selectedContentDisabled());
            });
            rowLayout->addWidget(m_contentToggleButton);
            m_contentOpenFolderButton =
                new QPushButton(MainWindowUI::serverSettingsContentOpenFolderText(), row);
            m_contentOpenFolderButton->setObjectName(
                MainWindowUI::serverSettingsContentOpenFolderButtonName());
            // 看一眼而已 ⇒ 一般性那一档；服务端在跑的时候照样能按。
            Theme::markButton(m_contentOpenFolderButton, Theme::ButtonKind::Ghost);
            m_contentOpenFolderButton->setCursor(Qt::PointingHandCursor);
            connect(m_contentOpenFolderButton, &QPushButton::clicked, this,
                    &ServerSettingsPage::contentOpenFolderRequested);
            rowLayout->addWidget(m_contentOpenFolderButton);
            m_contentRefreshButton =
                new QPushButton(MainWindowUI::serverSettingsContentRefreshText(), row);
            m_contentRefreshButton->setObjectName(
                MainWindowUI::serverSettingsContentRefreshButtonName());
            Theme::markButton(m_contentRefreshButton, Theme::ButtonKind::Ghost);
            m_contentRefreshButton->setCursor(Qt::PointingHandCursor);
            connect(m_contentRefreshButton, &QPushButton::clicked, this,
                    &ServerSettingsPage::contentRefreshRequested);
            rowLayout->addWidget(m_contentRefreshButton);
            layout->addWidget(row);
        }
    }

    // ---------- ④ 世界备份 ----------
    {
        auto *layout = addSectionPage(m_stack, QString());
        m_backupNote = makeNoteLabel(nullptr, QString());
        m_backupNote->setObjectName(MainWindowUI::serverSettingsBackupNoteName());
        layout->addWidget(m_backupNote);

        m_backupList = new QTreeWidget();
        m_backupList->setObjectName(MainWindowUI::serverSettingsBackupListName());
        m_backupList->setColumnCount(MainWindowUI::serverSettingsBackupColumns().size());
        m_backupList->setHeaderLabels(MainWindowUI::serverSettingsBackupColumns());
        m_backupList->setRootIsDecorated(false);
        m_backupList->setUniformRowHeights(true);
        m_backupList->setAllColumnsShowFocus(true);
        m_backupList->setSelectionMode(QAbstractItemView::SingleSelection);
        m_backupList->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_backupList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_backupList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_backupList->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        connect(m_backupList, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem *, QTreeWidgetItem *) { refreshBackupActions(); });
        connect(m_backupList, &QTreeWidget::itemDoubleClicked, this,
                [this](QTreeWidgetItem *item, int) {
                    if (m_editable && item != nullptr) {
                        emit restoreBackupRequested(item->data(0, Qt::UserRole).toString());
                    }
                });
        layout->addWidget(m_backupList, 1);

        auto *row = new QWidget();
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        m_backupNowButton = new QPushButton(MainWindowUI::serverSettingsBackupNowText(), row);
        m_backupNowButton->setObjectName(MainWindowUI::serverSettingsBackupNowButtonName());
        // 做一份备份 = 这一页的确认性动作，走主题色那一档。
        Theme::markButton(m_backupNowButton, Theme::ButtonKind::Accent);
        m_backupNowButton->setCursor(Qt::PointingHandCursor);
        connect(m_backupNowButton, &QPushButton::clicked, this,
                &ServerSettingsPage::backupNowRequested);
        rowLayout->addWidget(m_backupNowButton);
        m_openBackupFolderButton =
            new QPushButton(MainWindowUI::serverSettingsOpenBackupFolderText(), row);
        m_openBackupFolderButton->setObjectName(
            MainWindowUI::serverSettingsOpenBackupFolderButtonName());
        // 看一眼而已 ⇒ 一般性那一档；服务端在跑的时候它照样能按（开个文件夹不碰盘上那些
        // 正被写着的文件）。
        Theme::markButton(m_openBackupFolderButton, Theme::ButtonKind::Ghost);
        m_openBackupFolderButton->setCursor(Qt::PointingHandCursor);
        connect(m_openBackupFolderButton, &QPushButton::clicked, this,
                &ServerSettingsPage::openBackupFolderRequested);
        rowLayout->addWidget(m_openBackupFolderButton);
        rowLayout->addStretch(1);
        m_deleteBackupButton = new QPushButton(MainWindowUI::serverSettingsDeleteBackupText(), row);
        m_deleteBackupButton->setObjectName(MainWindowUI::serverSettingsDeleteBackupButtonName());
        // 删了就拿不回来 ⇒ 危险那一档的红。
        Theme::markButton(m_deleteBackupButton, Theme::ButtonKind::Danger);
        m_deleteBackupButton->setCursor(Qt::PointingHandCursor);
        connect(m_deleteBackupButton, &QPushButton::clicked, this, [this] {
            const QString path = selectedBackup();
            if (!path.isEmpty()) {
                emit deleteBackupRequested(path);
            }
        });
        rowLayout->addWidget(m_deleteBackupButton);
        m_restoreButton = new QPushButton(MainWindowUI::serverSettingsRestoreText(), row);
        m_restoreButton->setObjectName(MainWindowUI::serverSettingsRestoreButtonName());
        // 把世界换回旧那一份 —— 现在这一份会被先存成一张安全快照（`BackupFiles::restoreBackup`
        // 那五步），所以它不是"删了拿不回来"那一档。
        Theme::markButton(m_restoreButton, Theme::ButtonKind::Accent);
        m_restoreButton->setCursor(Qt::PointingHandCursor);
        connect(m_restoreButton, &QPushButton::clicked, this, [this] {
            const QString path = selectedBackup();
            if (!path.isEmpty()) {
                emit restoreBackupRequested(path);
            }
        });
        rowLayout->addWidget(m_restoreButton);
        layout->addWidget(row);
    }

    // ---------- ⑤ 玩家名单 ----------
    //
    // 四份文件（`whitelist.json` / `ops.json` / 两份封禁）在盘上的形状都是 JSON 数组，
    // 左栏子项选择名单，右侧显示记录与操作。**这一页不认
    // JSON**（`ui/` 不许 include `src/`）—— 解析、UUID、写盘全在 `MainWindow` 那一边，
    // 这里只把 `PlayerRow` 摆出来、把"用户点了加/移除"报出去。
    {
        auto *layout = addSectionPage(m_stack, QString());
        m_playerNote = makeNoteLabel(nullptr, QString());
        m_playerNote->setObjectName(MainWindowUI::serverSettingsPlayerNoteName());
        layout->addWidget(m_playerNote);

        m_playerTree = new QTreeWidget();
        m_playerTree->setObjectName(MainWindowUI::serverSettingsPlayerTreeName());
        m_playerTree->setColumnCount(MainWindowUI::serverSettingsPlayerColumns().size());
        m_playerTree->setHeaderLabels(MainWindowUI::serverSettingsPlayerColumns());
        m_playerTree->setRootIsDecorated(false);
        m_playerTree->setUniformRowHeights(true);
        m_playerTree->setAllColumnsShowFocus(true);
        m_playerTree->setSelectionMode(QAbstractItemView::SingleSelection);
        m_playerTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_playerTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_playerTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        m_playerTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        connect(m_playerTree, &QTreeWidget::currentItemChanged, this,
                [this](QTreeWidgetItem *, QTreeWidgetItem *) { refreshPlayerActions(); });
        layout->addWidget(m_playerTree, 1);

        {
            auto *row = new QWidget();
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);
            m_playerAddButton = new QPushButton(
                MainWindowUI::serverSettingsPlayerAddText(m_playerListIndex), row);
            m_playerAddButton->setObjectName(MainWindowUI::serverSettingsPlayerAddButtonName());
            Theme::markButton(m_playerAddButton, Theme::ButtonKind::Accent);
            m_playerAddButton->setCursor(Qt::PointingHandCursor);
            connect(m_playerAddButton, &QPushButton::clicked, this,
                    &ServerSettingsPage::playerAddRequested);
            rowLayout->addWidget(m_playerAddButton);
            rowLayout->addStretch(1);
            m_playerRemoveButton =
                new QPushButton(MainWindowUI::serverSettingsPlayerRemoveText(), row);
            m_playerRemoveButton->setObjectName(
                MainWindowUI::serverSettingsPlayerRemoveButtonName());
            // 从名单上拿掉一个人 = 他会进不来 / 不再是管理员 —— 危险那一档。
            Theme::markButton(m_playerRemoveButton, Theme::ButtonKind::Danger);
            m_playerRemoveButton->setCursor(Qt::PointingHandCursor);
            connect(m_playerRemoveButton, &QPushButton::clicked, this, [this] {
                const QString identity = selectedPlayerIdentity();
                if (!identity.isEmpty()) emit playerRemoveRequested(identity);
            });
            rowLayout->addWidget(m_playerRemoveButton);
            // 「踢出…」：**只在服务端跑着的时候才有意义**（把一条 kick 命令送进它 stdin，
            // 结果回显在控制台里）。所以它和上面两颗正好相反：不是跟着
            // "能不能改文件"走，而是跟着"进程在不在"走（见 `setPlayerKickAvailable()`）。
            m_playerKickButton = new QPushButton(MainWindowUI::serverSettingsPlayerKickText(), row);
            m_playerKickButton->setObjectName(
                MainWindowUI::serverSettingsPlayerKickButtonName());
            Theme::markButton(m_playerKickButton, Theme::ButtonKind::Neutral);
            m_playerKickButton->setCursor(Qt::PointingHandCursor);
            connect(m_playerKickButton, &QPushButton::clicked, this, [this] {
                const QString name = selectedPlayerName();
                if (!name.isEmpty()) emit playerKickRequested(name);
            });
            rowLayout->addWidget(m_playerKickButton);
            layout->addWidget(row);
        }
    }

    // ---------- ⑥ 体检 ----------
    {
        auto *layout = addSectionPage(m_stack, QString());
        m_checkSummary = makeNoteLabel(nullptr, QString());
        m_checkSummary->setObjectName(MainWindowUI::serverSettingsCheckSummaryName());
        {
            QFont font = m_checkSummary->font();
            font.setBold(true);
            m_checkSummary->setFont(font);
        }
        layout->addWidget(m_checkSummary);

        m_checkList = new QTreeWidget();
        m_checkList->setObjectName(MainWindowUI::serverSettingsCheckListName());
        m_checkList->setColumnCount(MainWindowUI::serverSettingsCheckColumns().size());
        m_checkList->setHeaderLabels(MainWindowUI::serverSettingsCheckColumns());
        m_checkList->setRootIsDecorated(false);
        m_checkList->setAllColumnsShowFocus(true);
        m_checkList->setSelectionMode(QAbstractItemView::NoSelection);
        m_checkList->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_checkList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_checkList->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        layout->addWidget(m_checkList, 1);

        auto *row = new QWidget();
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        // 出路按钮活在这一格里面（`refreshCheckActions()` 按记号重摆）。
        m_checkActions = new QWidget(row);
        m_checkActions->setObjectName(MainWindowUI::serverSettingsCheckActionsName());
        auto *actionsLayout = new QHBoxLayout(m_checkActions);
        actionsLayout->setContentsMargins(0, 0, 0, 0);
        actionsLayout->setSpacing(8);
        rowLayout->addWidget(m_checkActions, 1);
        m_recheckButton = new QPushButton(MainWindowUI::serverSettingsRecheckText(), row);
        m_recheckButton->setObjectName(MainWindowUI::serverSettingsRecheckButtonName());
        // 重查一遍 = 一般性那一档；在跑的时候也留着（它只读、不改任何东西）。
        Theme::markButton(m_recheckButton, Theme::ButtonKind::Ghost);
        m_recheckButton->setCursor(Qt::PointingHandCursor);
        connect(m_recheckButton, &QPushButton::clicked, this, &ServerSettingsPage::recheckRequested);
        rowLayout->addWidget(m_recheckButton, 0);
        layout->addWidget(row);
    }

    // ---------- 底下那条保存带 ----------
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        m_saveNote = makeMutedLabel(row, MainWindowUI::serverSettingsSaveNoteName());
        m_saveNote->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(m_saveNote, 1);
        m_revertButton = new QPushButton(MainWindowUI::serverSettingsRevertText(), row);
        m_revertButton->setObjectName(MainWindowUI::serverSettingsRevertButtonName());
        Theme::markButton(m_revertButton, Theme::ButtonKind::Neutral);
        m_revertButton->setCursor(Qt::PointingHandCursor);
        connect(m_revertButton, &QPushButton::clicked, this, &ServerSettingsPage::revertRequested);
        m_saveButton = new QPushButton(MainWindowUI::serverSettingsSaveText(), row);
        m_saveButton->setObjectName(MainWindowUI::serverSettingsSaveButtonName());
        Theme::markButton(m_saveButton, Theme::ButtonKind::Neutral);
        m_saveButton->setCursor(Qt::PointingHandCursor);
        connect(m_saveButton, &QPushButton::clicked, this, &ServerSettingsPage::saveRequested);
        auto *navigation = qobject_cast<QVBoxLayout *>(m_sectionList->parentWidget()->layout());
        navigation->setSpacing(8);
        navigation->addWidget(m_revertButton);
        navigation->addWidget(m_saveButton);
        m_startButton = new QPushButton(tr("启动服务端"), m_sectionList->parentWidget());
        m_startButton->setObjectName(QStringLiteral("serverSettingsStart"));
        Theme::markButton(m_startButton, Theme::ButtonKind::Primary);
        m_startButton->setCursor(Qt::PointingHandCursor);
        connect(m_startButton, &QPushButton::clicked, this, &ServerSettingsPage::startRequested);
        navigation->addWidget(m_startButton);
        box->addWidget(row);
    }

    // 起手那一档：没改过什么（`MainWindow` 开页时会立刻喂一遍真状态）。
    m_saveNoteText = MainWindowUI::serverSettingsCleanNote();
    refreshSaveBar();
    refreshBackupActions();
    refreshCheckActions();
    refreshContentActions();
    refreshPlayerActions();

    connect(m_sectionList, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_settingSection || item == nullptr) {
            return;
        }
        const int row = item->data(0, Qt::UserRole).toInt();
        if (item->parent() != nullptr) {
            const int index = item->data(0, Qt::UserRole + 1).toInt();
            if (row == static_cast<int>(Section::Players)) {
                setPlayerList(index);
                emit playerListChanged(index);
            } else if (row == static_cast<int>(Section::Content)) {
                setContentKind(index);
                emit contentKindChanged(index);
            }
        }
        // **切页之前**发：`MainWindow` 要借这一下把"表单补丁"和"原始文本"合起来
        // （见 .hpp 文件头那段），合完再让右栏翻过去。
        emit sectionChanged(row);
        m_stack->setCurrentIndex(row);
    });
    connectRowActivation(m_sectionList, [](QTreeWidgetItem *item) {
        if (item->childCount() > 0) item->setExpanded(!item->isExpanded());
    }, [](const QTreeWidgetItem *item) { return item && item->childCount() > 0; });
    m_sectionList->setCurrentItem(m_sectionList->topLevelItem(static_cast<int>(Section::Common)));
}

// ---------------------------------------------------------------------------
// 顶上那两行
// ---------------------------------------------------------------------------

void ServerSettingsPage::setServerName(const QString &name) {
    if (m_titleLabel != nullptr) {
        m_titleLabel->setText(name);
    }
    setWindowTitle(name.isEmpty() ? windowTitle() : name);
}

void ServerSettingsPage::setStatusText(const QString &text) {
    if (m_statusLabel != nullptr) {
        m_statusLabel->setText(text);
    }
}

// ---------------------------------------------------------------------------
// ① 常用设置
// ---------------------------------------------------------------------------

void ServerSettingsPage::setFields(const QList<FieldView> &fields) {
    if (m_formLayout == nullptr) {
        return;
    }
    // 清掉上一次那几行（弹簧留着：它永远在收尾）。
    // 这里用 `delete` 而不是仓库里常见的那一发 `deleteLater()`：行里的控件名字是
    // **按 key 拼的**（`serverSettingsFieldName()`），`deleteLater()` 会让新旧两批同名
    // 控件同时活在树上，谁先被 `findChild()` 找着就看运气了（验证工程会当场踩到）。
    while (m_formLayout->count() > 1) {
        QLayoutItem *item = m_formLayout->takeAt(0);
        QWidget *row = item->widget();
        delete item;
        delete row;
    }
    m_editors.clear();
    m_baseline.clear();
    // 预览那两块跟着行一起被 `delete` 掉了 —— 手边这两个指针必须同时作废，不然它们是
    // 悬空的（`refreshSaveBar()` 那一路上还会用到 "有没有预览" 这个判断）。
    m_motdPreview = nullptr;
    m_motdPreviewNote = nullptr;
    m_zipPickButton = nullptr;
    m_fields = fields;
    for (const FieldView &field : fields) {
        addFieldRow(field);
    }
    refreshSaveBar();
}

void ServerSettingsPage::setValues(const Values &values) {
    QHash<QString, QString> given;
    for (const QPair<QString, QString> &pair : values) {
        given.insert(pair.first, pair.second);
    }
    m_settingValues = true;
    // 按键找回每一格（**认键不认次序**：喂进来的那一份和表里的次序不必一样）。
    for (const FieldView &field : m_fields) {
        if (given.contains(field.key)) {
            setEditorValue(field, given.value(field.key));
        }
    }
    m_settingValues = false;
    // 摆进来的这一份就是**新的基线**（"改过没有"是拿它比的）—— `setValues()` 自己
    // 就带着 `clearChanged()` 的语义，别指望调用方记得再喊一次。
    clearChanged();
}

void ServerSettingsPage::setFieldValue(const QString &key, const QString &value) {
    for (const FieldView &field : m_fields) {
        if (field.key != key) {
            continue;
        }
        // **不**进 `m_settingValues` 那一段：这一发是"用户选了东西之后的产物"，要按
        // 改动算（保存带上那句"改过 1 项"、保存时会写回盘上）。控件自己的 `textChanged`
        // 会把 `edited()` 发出去 —— 那正是这里要的（`setValues()` 那一趟才是基线）。
        setEditorValue(field, value);
        refreshSaveBar();
        return;
    }
}

ServerSettingsPage::Values ServerSettingsPage::values() const {
    Values out;
    for (const FieldView &field : m_fields) {
        out.append(qMakePair(field.key, editorValue(field)));
    }
    return out;
}

ServerSettingsPage::Values ServerSettingsPage::changedOnly() const { return dirtyValues(); }

void ServerSettingsPage::clearChanged() {
    m_baseline.clear();
    for (const FieldView &field : m_fields) {
        m_baseline.insert(field.key, editorValue(field));
    }
    refreshSaveBar();
}

void ServerSettingsPage::setEditable(bool editable, const QString &reason) {
    m_editable = editable;
    m_notEditableReason = reason;
    for (QWidget *editor : m_editors) {
        if (editor != nullptr) {
            editor->setEnabled(editable);
        }
    }
    if (m_rawEdit != nullptr) {
        // 只读而不是禁用：禁用的文本框连"选中复制"都不给了，而"服务端在跑"的时候
        // 用户多半正是要来抄一行配置出去。
        m_rawEdit->setReadOnly(!editable);
    }
    if (m_backupNowButton != nullptr) {
        m_backupNowButton->setEnabled(editable);
    }
    // 「选择本地 zip…」只为填那一格服务 —— 那一格不能改的时候它也不该能按。
    if (m_zipPickButton != nullptr) {
        m_zipPickButton->setEnabled(editable);
    }
    // 「打开备份文件夹」「重新检查」**留着能按**：它们只读、不改任何东西。
    refreshBackupActions();
    refreshCheckActions();
    refreshContentActions();
    refreshPlayerActions();
    refreshSaveBar();
}

// ---------------------------------------------------------------------------
// ② 原始文本
// ---------------------------------------------------------------------------

void ServerSettingsPage::setRawText(const QString &text) {
    if (m_rawEdit == nullptr) {
        return;
    }
    // 行尾记在手边：文本框只认 `\n`（见 `m_rawCrlf` 那段），盘上那份是 CRLF ——
    // 回来的时候要按它自己的规矩补回去，不然"只改了一个端口"会把整份文件的行尾翻掉。
    m_rawCrlf = text.contains(QStringLiteral("\r\n"));
    QString body = text;
    if (m_rawCrlf) {
        body.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    }
    m_settingRaw = true;
    m_rawEdit->setPlainText(body);
    m_settingRaw = false;
    // 程序摆进来的这一份不算"用户动过"（这也正是"开页 / 放弃改动"要的语义）。
    m_rawEdited = false;
    refreshSaveBar();
}

QString ServerSettingsPage::rawText() const {
    if (m_rawEdit == nullptr) {
        return QString();
    }
    QString body = m_rawEdit->toPlainText();
    if (m_rawCrlf) {
        body.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    }
    return body;
}

bool ServerSettingsPage::rawEdited() const { return m_rawEdited; }

void ServerSettingsPage::markRawEdited() {
    // 摆进来的那份**内容**是外面合好的（表单补丁已经并进去了），所以这里只动记号 ——
    // 顺带把保存带重刷一遍（那句话要跟着变），但**不发 `edited()`**：调用方（`MainWindow`）
    // 自己就知道这一下该重算，再发一次只会让它多算一遍。
    m_rawEdited = true;
    refreshSaveBar();
}

// ---------------------------------------------------------------------------
// ③ 内容（模组 / 数据包）
// ---------------------------------------------------------------------------

int ServerSettingsPage::contentKind() const {
    return m_contentKindIndex;
}

void ServerSettingsPage::setContentKind(int kindIndex) {
    if (kindIndex < 0 || kindIndex >= MainWindowUI::serverSettingsContentKindCount()) return;
    // 程序摆的那一下**不算用户换类**（`MainWindow` 会紧接着自己刷一遍行）。
    m_contentKindIndex = kindIndex;
    if (m_stack && m_stack->currentIndex() == static_cast<int>(Section::Content)) {
        const QSignalBlocker blocker(m_sectionList);
        auto *group = m_sectionList->topLevelItem(static_cast<int>(Section::Content));
        group->setExpanded(true);
        m_sectionList->setCurrentItem(group->child(kindIndex));
    }
    refreshContentActions();
}

void ServerSettingsPage::setContentRows(const QList<ContentRow> &rows) {
    if (m_contentTree == nullptr) return;
    const QSignalBlocker blocker(m_contentTree);
    m_contentTree->clear();
    const QColor muted = [this] {
        QColor color = m_contentTree->palette().color(QPalette::Text);
        color.setAlpha(150);
        return color;
    }();
    for (const ContentRow &row : rows) {
        auto *item = new QTreeWidgetItem(m_contentTree);
        item->setText(0, row.name);
        item->setText(1, row.size);
        item->setText(2, MainWindowUI::serverSettingsContentStateText(row.disabled));
        // 认人认文件名（带不带 `.disabled` 都照原样），不认这行字。
        item->setData(0, Qt::UserRole, row.name);
        // 禁用着没有 —— 摆进来那一份说了算（**不在这里反推文件名尾巴**：`.disabled` 那条
        // 规矩的出处是 `src/server/ServerContent`，这一层不该抄一份）。
        item->setData(0, Qt::UserRole + 1, row.disabled);
        item->setToolTip(0, row.name);
        if (row.disabled) {
            // 禁用着的那一行整行画灰 + 斜体：它还在文件夹里，但服务端不会加载它。
            for (int column = 0; column < 3; ++column) {
                QFont font = item->font(column);
                font.setItalic(true);
                item->setFont(column, font);
                item->setForeground(column, muted);
            }
        }
    }
    refreshContentActions();
}

void ServerSettingsPage::setContentNote(const QString &text) {
    if (m_contentNote != nullptr) m_contentNote->setText(text);
}

QString ServerSettingsPage::selectedContentName() const {
    if (m_contentTree == nullptr) return QString();
    const QTreeWidgetItem *item = m_contentTree->currentItem();
    return item != nullptr ? item->data(0, Qt::UserRole).toString() : QString();
}

bool ServerSettingsPage::selectedContentDisabled() const {
    if (m_contentTree == nullptr) return false;
    const QTreeWidgetItem *item = m_contentTree->currentItem();
    return item != nullptr && item->data(0, Qt::UserRole + 1).toBool();
}

// ---------------------------------------------------------------------------
// ④ 世界备份
// ---------------------------------------------------------------------------

void ServerSettingsPage::setBackupRows(const QList<BackupRow> &rows) {
    if (m_backupList == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_backupList);  // 重灌这一列不算"换了一份"
    m_backupList->clear();
    const QColor muted = [this] {
        QColor color = m_backupList->palette().color(QPalette::Text);
        color.setAlpha(150);
        return color;
    }();
    for (const BackupRow &row : rows) {
        auto *item = new QTreeWidgetItem(m_backupList);
        item->setText(0, row.when);
        item->setText(1, row.size);
        item->setText(2, row.scope);
        // 身份进 userData（**认路径，不认这行字**：时间是会随文案/格式改的）。
        item->setData(0, Qt::UserRole, row.path);
        if (row.safety) {
            // "还原之前自动做的那一份"：斜体 + 浅一档 —— 不另编一句话，`scope` 那格由
            // 翻译点说清它是什么（见 BackupRow::safety）。
            for (int column = 0; column < 3; ++column) {
                QFont font = item->font(column);
                font.setItalic(true);
                item->setFont(column, font);
                item->setForeground(column, muted);
            }
        }
        item->setToolTip(0, row.path);
    }
    refreshBackupActions();
}

QString ServerSettingsPage::selectedBackup() const {
    if (m_backupList == nullptr) {
        return QString();
    }
    const QTreeWidgetItem *item = backupItemAt(m_backupList->currentIndex().row());
    return item != nullptr ? item->data(0, Qt::UserRole).toString() : QString();
}

void ServerSettingsPage::setBackupNote(const QString &text) {
    if (m_backupNote != nullptr) {
        m_backupNote->setText(text);
    }
}

// ---------------------------------------------------------------------------
// ④ 玩家名单
// ---------------------------------------------------------------------------

int ServerSettingsPage::playerList() const {
    return m_playerListIndex;
}

void ServerSettingsPage::setPlayerList(int listIndex) {
    if (listIndex < 0 || listIndex >= MainWindowUI::serverSettingsPlayerListCount()) return;
    // 程序摆的那一下**不算用户换页**（`MainWindow` 会紧接着自己刷一遍行）。
    m_playerListIndex = listIndex;
    if (m_stack && m_stack->currentIndex() == static_cast<int>(Section::Players)) {
        const QSignalBlocker blocker(m_sectionList);
        auto *group = m_sectionList->topLevelItem(static_cast<int>(Section::Players));
        group->setExpanded(true);
        m_sectionList->setCurrentItem(group->child(listIndex));
    }
    if (m_playerAddButton != nullptr) {
        m_playerAddButton->setText(MainWindowUI::serverSettingsPlayerAddText(listIndex));
    }
    refreshPlayerActions();
}

void ServerSettingsPage::setPlayerRows(const QList<PlayerRow> &rows) {
    if (m_playerTree == nullptr) return;
    const QSignalBlocker blocker(m_playerTree);
    m_playerTree->clear();
    const QColor muted = [this] {
        QColor color = m_playerTree->palette().color(QPalette::Text);
        color.setAlpha(150);
        return color;
    }();
    for (const PlayerRow &row : rows) {
        auto *item = new QTreeWidgetItem(m_playerTree);
        item->setText(0, row.name);
        item->setText(1, row.detail);
        item->setText(2, row.note);
        // 认人认身份键（uuid / ip），不认这行字。
        item->setData(0, Qt::UserRole, row.identity);
        // 「踢出」用的是玩家名（控制台 `kick <名字>`），不是 uuid。
        item->setData(0, Qt::UserRole + 1, row.kickName);
        item->setToolTip(1, row.detail);
        if (row.expired) {
            // 已经过期的那条：整行画灰 + 斜体 —— 服务端下次启动会自己清掉它。
            for (int column = 0; column < 3; ++column) {
                QFont font = item->font(column);
                font.setItalic(true);
                item->setFont(column, font);
                item->setForeground(column, muted);
            }
        }
    }
    refreshPlayerActions();
}

void ServerSettingsPage::setPlayerNote(const QString &text) {
    if (m_playerNote != nullptr) m_playerNote->setText(text);
}

QString ServerSettingsPage::selectedPlayerIdentity() const {
    if (m_playerTree == nullptr) return QString();
    const QTreeWidgetItem *item = m_playerTree->currentItem();
    return item != nullptr ? item->data(0, Qt::UserRole).toString() : QString();
}

QString ServerSettingsPage::selectedPlayerName() const {
    if (m_playerTree == nullptr) return QString();
    const QTreeWidgetItem *item = m_playerTree->currentItem();
    return item != nullptr ? item->data(0, Qt::UserRole + 1).toString() : QString();
}

void ServerSettingsPage::setPlayerKickAvailable(bool available) {
    m_playerKickAllowed = available;
    refreshPlayerActions();
}

// ---------------------------------------------------------------------------
// ⑤ 体检
// ---------------------------------------------------------------------------

void ServerSettingsPage::setCheckRows(const QList<CheckRow> &rows) {
    if (m_checkList == nullptr) {
        return;
    }
    m_checkRows = rows;
    m_checkList->clear();
    const QPalette palette = m_checkList->palette();
    for (const CheckRow &row : rows) {
        auto *item = new QTreeWidgetItem(m_checkList);
        item->setText(0, row.title);
        item->setText(1, row.detail);
        item->setToolTip(0, row.title);
        item->setToolTip(1, row.detail);
        // 颜色由 severity 决定（`ServerCheck.hpp` 文件头那段：次序已经把它排好，颜色是
        // 唯一那个"这一条多要紧"的记号）。
        QColor ink;
        QFont font = item->font(0);
        switch (row.severity) {
        case 0:  // 要处理
            ink = Theme::dangerColor(palette);
            font.setBold(true);
            break;
        case 1:  // 提醒
            ink = Theme::warningColor(palette);
            break;
        default: {  // 说明
            ink = palette.color(QPalette::Text);
            ink.setAlpha(150);
            break;
        }
        }
        item->setForeground(0, ink);
        item->setFont(0, font);
    }
    refreshCheckActions();
}

void ServerSettingsPage::setCheckSummary(const QString &text) {
    if (m_checkSummary != nullptr) {
        m_checkSummary->setText(text);
    }
}

// ---------------------------------------------------------------------------
// 底下那条保存带
// ---------------------------------------------------------------------------

void ServerSettingsPage::setSaveState(bool enabled, const QString &note) {
    m_saveAllowed = enabled;
    m_saveNoteText = note;
    refreshSaveBar();
}

void ServerSettingsPage::setSection(Section section) {
    if (m_stack == nullptr || m_sectionList == nullptr) {
        return;
    }
    const int index = static_cast<int>(section);
    if (index < 0 || index >= m_stack->count()) {
        return;
    }
    // **不发 `sectionChanged`**：这是程序摆的（"开页时摆到某一页"/"从体检的出路按钮跳
    // 过去"），不是用户点的那一发 —— 发出去的话 `MainWindow` 会再合一次两边的编辑。
    m_settingSection = true;
    auto *item = m_sectionList->topLevelItem(index);
    if (section == Section::Players) {
        item->setExpanded(true);
        item = item->child(m_playerListIndex);
    } else if (section == Section::Content) {
        item->setExpanded(true);
        item = item->child(m_contentKindIndex);
    }
    m_sectionList->setCurrentItem(item);
    m_settingSection = false;
    m_stack->setCurrentIndex(index);
}

ServerSettingsPage::Section ServerSettingsPage::section() const {
    const int index = m_stack == nullptr ? 0 : m_stack->currentIndex();
    return static_cast<Section>(qMax(0, index));
}

// ---------------------------------------------------------------------------
// 里边那几件小事
// ---------------------------------------------------------------------------

void ServerSettingsPage::addFieldRow(const FieldView &field) {
    if (m_formLayout == nullptr || m_formBody == nullptr) {
        return;
    }
    // 用户在**这一行**里动了东西 ⇒ 重刷保存带 + 报给翻译点（那几句话是产品措辞，
    // 不在这一层；见 `edited()` 那段）。程序摆值那一路（`m_settingValues`）要丢掉。
    const auto changed = [this] {
        if (m_settingValues) {
            return;
        }
        refreshSaveBar();
        emit edited();
    };

    auto *row = new QWidget(m_formBody);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto *label = new QLabel(field.label, row);
    label->setFixedWidth(kFieldLabelWidth);
    layout->addWidget(label);

    QWidget *editor = nullptr;
    switch (field.kind) {
    case Kind::Text: {
        auto *edit = new QLineEdit(row);
        edit->setFixedWidth(kFieldEditorWidth);
        // 接在**摆默认值之后**：`addItem` / `setRange` 这一类也会发信号，接早了会在
        // 搭界面这一路就把 `edited()` 发出去（那时页面还没搭完）。
        connect(edit, &QLineEdit::textChanged, this, [changed](const QString &) { changed(); });
        editor = edit;
        break;
    }
    case Kind::Number: {
        auto *spin = new QSpinBox(row);
        spin->setRange(field.min, field.max);
        spin->setFixedWidth(kFieldEditorWidth);
        connect(spin, &QSpinBox::valueChanged, this, [changed](int) { changed(); });
        editor = spin;
        break;
    }
    case Kind::Switch: {
        auto *toggle = new ToggleSwitch(row);
        connect(toggle, &ToggleSwitch::toggled, this, [changed](bool) { changed(); });
        editor = toggle;
        break;
    }
    case Kind::Choice: {
        auto *combo = new QComboBox(row);
        combo->setFixedWidth(kFieldEditorWidth);
        for (int i = 0; i < field.choiceTokens.size(); ++i) {
            combo->addItem(field.choiceNames.value(i), field.choiceTokens.at(i));
        }
        connect(combo, &QComboBox::currentIndexChanged, this, [changed](int) { changed(); });
        editor = combo;
        break;
    }
    }
    // 认人走这个名字（前缀 + `server.properties` 里那个键），不认行号、也不认那一行的字。
    editor->setObjectName(MainWindowUI::serverSettingsFieldName(field.key));
    // 挂着预览的那一格：**敲一个字画一次**（预览就是给人边改边看的）。程序摆值那一趟
    // （`setEditorValue()` 走 `setText()`）也走这里 —— 那也是"这一格现在写着什么"。
    if (field.preview) {
        if (auto *edit = qobject_cast<QLineEdit *>(editor)) {
            connect(edit, &QLineEdit::textChanged, this, [this](const QString &text) {
                if (m_motdPreview != nullptr) {
                    m_motdPreview->setText(motdPreviewHtml(text));
                }
            });
        }
    }
    if (field.kind == Kind::Switch) {
        layout->addWidget(editor, 0, Qt::AlignLeft);
    } else {
        layout->addWidget(editor, 0);
    }

    // 「选择本地 zip…」：只为那一格服务。选文件、验 zip、算 SHA-1 全在 `MainWindow`
    // （`ui/` 不 include `src/`，那条线见文件头）。
    if (field.zipPick) {
        auto *pick = new QPushButton(MainWindowUI::serverSettingsPickZipText(), row);
        pick->setObjectName(MainWindowUI::serverSettingsPickZipButtonName());
        pick->setEnabled(m_editable);
        connect(pick, &QPushButton::clicked, this,
                [this] { emit resourcePackPickRequested(); });
        layout->addWidget(pick, 0);
        m_zipPickButton = pick;
    }

    if (field.hint.isEmpty()) {
        layout->addStretch(1);
    } else {
        auto *hint = makeHintLabel(row, field.hint);
        // 折行的小字**不许**参与横向度量：它一长，整行就被抻宽，右栏就出横向滚动条了。
        hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(hint, 1);
    }

    m_editors.insert(field.key, editor);
    // 弹簧在收尾 ⇒ 新行插在它**前面**。
    m_formLayout->insertWidget(m_formLayout->count() - 1, row);

    // ---------- 这一行下面那块"游戏里长什么样"（MOTD 提供格式化预览）----------
    //
    // 只有 `FieldView::preview` 为真的那一行才有（目前是 `motd`）。它**紧跟在那一行后面**
    // 插进同一竖排，左边留出和编辑栏同一个起点 —— 看起来是"这一格的注释"，而不是另一项设置。
    if (field.preview) {
        auto *holder = new QWidget(m_formBody);
        auto *holderLayout = new QVBoxLayout(holder);
        holderLayout->setContentsMargins(kFieldLabelWidth + 12, 0, 0, 0);
        holderLayout->setSpacing(4);

        auto *plate = new QLabel(holder);
        plate->setObjectName(MainWindowUI::serverSettingsMotdPreviewName());
        plate->setTextFormat(Qt::RichText);
        plate->setWordWrap(true);
        plate->setTextInteractionFlags(Qt::TextSelectableByMouse);
        // 这一块画的是**游戏那一边**的样子（深底 + 游戏那 16 色），不跟我们的主题走 ——
        // 一行固定的深色就够了，别去动调色板（`setStyleSheet()` 只作用在它自己身上）。
        plate->setStyleSheet(QStringLiteral("QLabel { background-color: #202020;"
                                            " border: none; border-radius: 4px;"
                                            " padding: 6px 10px; }"));
        plate->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        holderLayout->addWidget(plate);

        QLabel *note = makeMutedLabel(holder, MainWindowUI::serverSettingsMotdNoteName());
        note->setText(MainWindowUI::serverSettingsMotdNote());
        note->setWordWrap(true);
        note->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        holderLayout->addWidget(note);

        m_motdPreview = plate;
        m_motdPreviewNote = note;
        // 摆进来的那一份也要画出来（开页时它是盘上那份 MOTD，不是空的）。
        if (const auto *edit = qobject_cast<const QLineEdit *>(editor)) {
            plate->setText(motdPreviewHtml(edit->text()));
        }
        m_formLayout->insertWidget(m_formLayout->count() - 1, holder);
    }
}

QString ServerSettingsPage::editorValue(const FieldView &field) const {
    const QWidget *editor = fieldEditor(field.key);
    if (editor == nullptr) {
        return QString();
    }
    switch (field.kind) {
    case Kind::Text: {
        const auto *edit = qobject_cast<const QLineEdit *>(editor);
        return edit != nullptr ? edit->text() : QString();
    }
    case Kind::Number: {
        const auto *spin = qobject_cast<const QSpinBox *>(editor);
        return spin != nullptr ? QString::number(spin->value()) : QString();
    }
    case Kind::Switch: {
        const auto *toggle = qobject_cast<const ToggleSwitch *>(editor);
        return toggle != nullptr && toggle->isChecked() ? QStringLiteral("true")
                                                        : QStringLiteral("false");
    }
    case Kind::Choice: {
        const auto *combo = qobject_cast<const QComboBox *>(editor);
        if (combo == nullptr || combo->currentIndex() < 0) {
            return QString();
        }
        return combo->currentData().toString();
    }
    }
    return QString();
}

void ServerSettingsPage::setEditorValue(const FieldView &field, const QString &value) {
    QWidget *editor = fieldEditor(field.key);
    if (editor == nullptr) {
        // 表里没有这一格的控件（`setFields()` 还没喂过 / 这一项不在表里）—— 什么都不做，
        // 别为它现造一个（"摆值"和"摆表"是两件事，次序由调用方负责）。
        return;
    }
    switch (field.kind) {
    case Kind::Text: {
        if (auto *edit = qobject_cast<QLineEdit *>(editor)) {
            edit->setText(value);
        }
        break;
    }
    case Kind::Number: {
        if (auto *spin = qobject_cast<QSpinBox *>(editor)) {
            // 读不动的值（`1e3` / `10.0` / `0x10` / 空串）**不帮它圆**，一律落到框的下限，
            // 而且**这一格紧接着会被当成基线**（见 `clearChanged()`）—— 于是它不会被写回
            // 盘上（"没改过"）。文件里那个不合法的值原样留着，由体检那一页报出来、
            // 由用户决定怎么办（同 `ServerPropertiesForm::accepts()` 那条规矩）。
            bool ok = false;
            const int parsed = value.toInt(&ok);
            spin->setValue(ok ? parsed : spin->minimum());
        }
        break;
    }
    case Kind::Switch: {
        if (auto *toggle = qobject_cast<ToggleSwitch *>(editor)) {
            // 认不出来的值（比如手写了个 `yes`）按"开"算 —— `setChecked` 那一发不会把值
            // 写回去（见上面 Number 那一段的同一条理由）。
            const QString lowered = value.trimmed().toLower();
            const bool on = !(lowered == QStringLiteral("false") || lowered == QStringLiteral("0")
                              || lowered == QStringLiteral("no") || lowered == QStringLiteral("off")
                              || lowered.isEmpty());
            toggle->setChecked(on);
        }
        break;
    }
    case Kind::Choice: {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (combo == nullptr) {
            break;
        }
        int index = combo->findData(value);
        if (index < 0) {
            // 文件里这一项是个我们表上没有的词：**临时**给它一格（值原样带着），于是它
            // 原样显示、原样算作"没改过"（体检那一页会报"这一项的值不对"，出路是让用户
            // 自己挑一个）。挑完就换回正经的那几格 —— 别把它留在表里。
            combo->addItem(value, value);
            index = combo->count() - 1;
        }
        combo->setCurrentIndex(index);
        break;
    }
    }
}

ServerSettingsPage::Values ServerSettingsPage::dirtyValues() const {
    Values out;
    for (const FieldView &field : m_fields) {
        const QString now = editorValue(field);
        if (m_baseline.value(field.key) != now) {
            out.append(qMakePair(field.key, now));
        }
    }
    return out;
}

void ServerSettingsPage::refreshSaveBar() {
    if (m_saveNote == nullptr) {
        return;
    }
    if (!m_editable) {
        // 在跑的时候那句原因盖过一切（用户现在最该知道的是"为什么按不动"）。
        m_saveNote->setText(m_notEditableReason);
    } else {
        m_saveNote->setText(m_saveNoteText);
    }
    // 「保存」的灰亮由这一层**再兜一道**：`MainWindow` 那一边说"允许"之外，还得真的
    // 改过点什么 —— 没改过就按下去是白按（而且会把"保存"这两个字的份量花掉）。
    // 在跑的时候一律点不动。
    const bool dirty = !dirtyValues().isEmpty() || m_rawEdited;
    if (m_saveButton != nullptr) {
        m_saveButton->setEnabled(m_editable && m_saveAllowed && dirty);
    }
    if (m_startButton != nullptr) {
        m_startButton->setEnabled(m_editable && !dirty);
        m_startButton->setToolTip(!m_editable ? tr("服务端正在运行")
                                            : dirty ? tr("请先保存或放弃修改") : QString());
    }
    if (m_revertButton != nullptr) {
        m_revertButton->setEnabled(m_editable && dirty);
    }
}

void ServerSettingsPage::refreshBackupActions() {
    const bool picked = !selectedBackup().isEmpty();
    // 恢复 / 删除都是"动盘上那些文件"的事 ⇒ 服务端在跑的时候一并点不动（它正开着世界里的
    // 文件在写）。
    if (m_restoreButton != nullptr) {
        m_restoreButton->setEnabled(m_editable && picked);
    }
    if (m_deleteBackupButton != nullptr) {
        m_deleteBackupButton->setEnabled(m_editable && picked);
    }
}

void ServerSettingsPage::refreshCheckActions() {
    if (m_checkActions == nullptr) {
        return;
    }
    auto *layout = qobject_cast<QHBoxLayout *>(m_checkActions->layout());
    if (layout == nullptr) {
        return;
    }
    while (QLayoutItem *item = layout->takeAt(0)) {
        QWidget *button = item->widget();
        delete item;
        delete button;
    }
    // 按记号**去重**、顺序按第一次出现 —— 一页上最多就是那五种出路，一条一个按钮。
    QStringList seen;
    for (const CheckRow &row : m_checkRows) {
        if (row.actionToken.isEmpty() || seen.contains(row.actionToken)) {
            continue;
        }
        seen.append(row.actionToken);
        const QString text = MainWindowUI::serverCheckActionText(row.actionToken);
        if (text.isEmpty()) {
            // 认不出来的记号：这一条照旧摆出来，只是不配按钮（比编一个"按一下就好了"
            // 出来强 —— 见 `serverCheckActionText()` 那段）。
            continue;
        }
        auto *button = new QPushButton(text, m_checkActions);
        button->setCursor(Qt::PointingHandCursor);
        // 看一眼 / 挪个地方那两颗（"去「常用设置」""去「世界备份」"）服务端在跑的时候也
        // 留着能按 —— 只翻页、不改任何东西；其余三颗（重装 / 同意 EULA / 改回缺省值）
        // 都要动盘，跟表单一起被锁上。
        const bool viewOnly = row.actionToken == QStringLiteral("open-settings")
                              || row.actionToken == QStringLiteral("open-backups");
        Theme::markButton(button, viewOnly ? Theme::ButtonKind::Neutral
                                           : Theme::ButtonKind::Accent);
        button->setEnabled(m_editable || viewOnly);
        const QString token = row.actionToken;
        connect(button, &QPushButton::clicked, this,
                [this, token] { emit actionRequested(token); });
        layout->addWidget(button);
    }
    layout->addStretch(1);
}

void ServerSettingsPage::refreshContentActions() {
    // 「添加…」只往文件夹里拷文件 ⇒ 服务端在跑的时候也先不给按（它下次启动才加载，
    // 现在拷进去只会让人以为"已经生效了"；等它停下来）。
    if (m_contentAddButton != nullptr) {
        m_contentAddButton->setEnabled(m_editable);
    }
    const bool picked = !selectedContentName().isEmpty();
    if (m_contentToggleButton != nullptr) {
        m_contentToggleButton->setEnabled(m_editable && picked);
        // 那颗按钮上的字跟着选中那一行翻：禁用着的写「启用」，反过来写「禁用」。
        m_contentToggleButton->setText(
            picked && selectedContentDisabled() ? MainWindowUI::serverSettingsContentEnableText()
                                                 : MainWindowUI::serverSettingsContentDisableText());
    }
    // 「打开文件夹」「刷新」一直能按（只读，不改任何东西）。
}

void ServerSettingsPage::refreshPlayerActions() {
    // 「加」那一颗：只读时点不动（服务端退出时会把它内存里那份名单写回来，见 `setEditable`）。
    if (m_playerAddButton != nullptr) {
        m_playerAddButton->setEnabled(m_editable);
    }
    // 「移除」还要选中一行才有意义。
    if (m_playerRemoveButton != nullptr) {
        m_playerRemoveButton->setEnabled(
            m_editable && m_playerTree != nullptr && m_playerTree->currentItem() != nullptr);
    }
    // 「踢出」跟着**进程在不在**走（和上面两颗相反 —— 见 `setPlayerKickAvailable()`），
    // 再有就是得选中一行、而且那一行得有名字（封禁 IP 那一档没有玩家名可踢）。
    if (m_playerKickButton != nullptr) {
        m_playerKickButton->setEnabled(
            m_playerKickAllowed && m_playerTree != nullptr
            && m_playerTree->currentItem() != nullptr
            && !selectedPlayerName().isEmpty());
    }
}

QWidget *ServerSettingsPage::fieldEditor(const QString &key) const {
    return m_editors.value(key, nullptr);
}

QTreeWidgetItem *ServerSettingsPage::backupItemAt(int row) const {
    if (m_backupList == nullptr || row < 0 || row >= m_backupList->topLevelItemCount()) {
        return nullptr;
    }
    return m_backupList->topLevelItem(row);
}
