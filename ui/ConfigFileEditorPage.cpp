/**
 * @file ConfigFileEditorPage.cpp
 * @brief ConfigFileEditorPage 的实现。
 * @note 见 .hpp 那一段（为什么是文本编辑器、为什么它是 QWidget 不是 QDialog、
 * 为什么这一层自己算要写的字节）。这里只补两处实现上的说明：行号槽那一套
 * （本文件里唯一一处自己画的控件），以及 Esc 那一颗得在这一层接住的原因。
 */
#include "ConfigFileEditorPage.hpp"
#include <QCoreApplication>

#include <QDir>
#include <QFontDatabase>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTime>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <functional>

#include "Animations.hpp"
#include "ConfigEntries.hpp"
#include "Theme.hpp"   // 上面那四颗按钮的身份（一般性 = 黑字黑边）
#include "ElidedLabel.hpp"

namespace {

/// 编辑框至少这么高。这一页是嵌在「实例设置」那层滚动区里的（见 .hpp），
/// 而 `QPlainTextEdit` 自己那个 sizeHint 很小 —— 不顶一格的话，用户看到的是一条
/// 三行高的缝，"编辑一个文件"这件事就没地方下手了。
constexpr int kEditorMinHeight = 280;
/// 行号那一列数字右边留的空白（数字和正文之间那一条缝）。
constexpr int kGutterRightPad = 10;
/// 行号槽至少这么宽（行数只有个位数的时候也不至于贴着边）。
constexpr int kGutterMinWidth = 30;
/// 左边「条目」那一格多宽。**固定宽**（不做可拖的分隔条）：这一格是"帮你找那一行"的
/// 索引，不是主角，让它随窗口变宽变窄只会把右边那个真正在改的编辑框挤来挤去。
constexpr int kIndexWidth = 240;
/// 改完字之后等这么久再重认一遍。**手停下来才重算**：`textChanged` 是每敲一个字一发，
/// 一份几万行的配置上"每敲一个字就扫全文 + 重建整棵树"是看得见的卡。
constexpr int kIndexRebuildDelayMs = 250;

class CodeEditor;

/// 行号槽那一格。**是一个真控件，不是画在编辑框上的** —— 它得自己占掉编辑框左侧那一条
/// 视口宽度（`setViewportMargins`），正文才会从它右边开始、滚到哪儿都不重叠。
class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor *editor);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    CodeEditor *m_editor = nullptr;
};

/// 带行号的纯文本编辑框。
///
/// NOTE: 用 `QPlainTextEdit` 而不是 `QTextEdit` —— 前者就是为**纯文本**设计的（没有富文本
/// 那一套文档结构），长行和大文件都稳；而这一页要的正好是纯文本。
/// NOTE: **不换行**（`NoWrap`）。这一条不只是"看着像编辑器"，它还是行号槽成立的前提：
/// 一个块正好占一个视觉行，所以第 n 个块的行号就画在第 n 行上。一换行，一个块占好几
/// 个视觉行，行号就得跟着算折行 —— 那是另一件事，也更容易画错。
class CodeEditor : public QPlainTextEdit {
public:
    explicit CodeEditor(QWidget *parent = nullptr) : QPlainTextEdit(parent) {
        m_gutter = new LineNumberArea(this);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        // 等宽字体：配置文件是缩进敏感的东西（TOML / JSON），比例字体下对齐全乱。
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        // 制表符按 **4 个空格宽**算。NeoForge 生成的那批 `.toml` 就是拿制表符缩进的
        // （`carryon-common.toml` 里 `[settings]` 底下那一层全是 `\t`），照 Qt 默认那个
        // 8 格宽画出来，同一份文件的缩进会比在别处看着深一倍。
        setTabStopDistance(4 * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
        // 这一页是**编辑器**：Tab 得是缩进，不是"焦点跳到下一个控件"。QPlainTextEdit
        // 默认就是这样（tabChangesFocus 默认 false）；写出来是因为这一条哪天被改掉，
        // 症状是"按 Tab 焦点跑了"，而用户只是想缩进一格。
        setTabChangesFocus(false);

        connect(this, &QPlainTextEdit::blockCountChanged, this,
                [this](int count) { updateLineNumberAreaWidth(count); });
        // 滚了、改了、视口变了：行号槽整个重画一遍。行号是"贴"在行上的，行一动它就得跟着动
        // （这也是 Qt 自带那份 line numbers 例子的做法）。
        connect(this, &QPlainTextEdit::updateRequest, this,
                [this](const QRect &rect, int dy) { updateLineNumberArea(rect, dy); });
        updateLineNumberAreaWidth(blockCount());
    }

    /// 行号槽要多宽：数字的位数决定。**`blockCount()` 一变大它就变宽**，而
    /// `updateLineNumberAreaWidth()` 会把这个宽度让给槽（`setViewportMargins`）。
    int gutterWidth() const {
        int digits = 1;
        int max = qMax(1, blockCount());
        while (max >= 10) {
            max /= 10;
            ++digits;
        }
        const int digitWidth = fontMetrics().horizontalAdvance(QLatin1Char('9'));
        return kGutterRightPad + qMax(kGutterMinWidth, digitWidth * digits + digitWidth);
    }

    void paintGutter(QPaintEvent *event) {
        QPainter painter(m_gutter);
        // 底色取 `AlternateBase`：那是调色板里"和正文底色差一档"的那一格，深浅两个主题下
        // 都成立（和 TitleBar 里 `Window.darker(105)` 是同一个意思，但这一格调色板里
        // 本来就有）。
        painter.fillRect(event->rect(), palette().color(QPalette::AlternateBase));
        painter.setPen(palette().color(QPalette::PlaceholderText));

        QTextBlock block = firstVisibleBlock();
        int blockNumber = block.blockNumber();
        int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        int bottom = top + qRound(blockBoundingRect(block).height());
        while (block.isValid() && top <= event->rect().bottom()) {
            if (block.isVisible() && bottom >= event->rect().top()) {
                painter.drawText(0, top, m_gutter->width() - kGutterRightPad,
                                 fontMetrics().height(), Qt::AlignRight,
                                 QString::number(blockNumber + 1));
            }
            block = block.next();
            top = bottom;
            bottom = top + qRound(blockBoundingRect(block).height());
            ++blockNumber;
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QPlainTextEdit::resizeEvent(event);
        const QRect cr = contentsRect();
        m_gutter->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
    }

private:
    void updateLineNumberAreaWidth(int /*newBlockCount*/) {
        // 正文左边让出这么宽 —— `setViewportMargins` 说的就是"这一块是留给别人的"，
        // 于是正文从槽右边开始，滚到哪儿都不重叠。
        setViewportMargins(gutterWidth(), 0, 0, 0);
    }

    void updateLineNumberArea(const QRect &rect, int dy) {
        if (dy != 0) {
            m_gutter->scroll(0, dy);
        } else {
            m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
        }
        if (rect.contains(viewport()->rect())) {
            updateLineNumberAreaWidth(blockCount());
        }
    }

    LineNumberArea *m_gutter = nullptr;
};

LineNumberArea::LineNumberArea(CodeEditor *editor) : QWidget(editor), m_editor(editor) {}

QSize LineNumberArea::sizeHint() const { return QSize(m_editor->gutterWidth(), 0); }

void LineNumberArea::paintEvent(QPaintEvent *event) { m_editor->paintGutter(event); }

/// 比正文浅一档的那个颜色。**全仓唯一去处是 `QPalette::PlaceholderText`**（和
/// InstanceFilesPage.cpp 里那个同名的小工具同一个角色、同一个出处）。
void makeSecondary(QLabel *label) {
    if (label == nullptr) {
        return;
    }
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    label->setPalette(palette);
}

}  // namespace

ConfigFileEditorPage::ConfigFileEditorPage(QWidget *parent) : QWidget(parent) {
    setObjectName(pageName());
    // 这一页**自己要拿到焦点**（用户进来就是要敲键盘改字），而它是嵌在两格子里被切出来
    // 的 —— 见 .hpp 里 showEvent() 那一 NOTE。
    setFocusPolicy(Qt::StrongFocus);

    auto *outer = new QVBoxLayout(this);
    // 边上不留白：这一块是**页里面的一格**（「配置文件」那一页的第二个格子），四周的留白
    // 由外面那一页给。留了的话，切过来切过去会看见内容整体缩一下 —— 而两格讲的是同一张
    // 列表的两种看法，位置该稳住。
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(10);

    // ---------- 上面一行：返回 + 这是哪个文件 + 打开文件夹 ----------
    auto *topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);

    // 这一颗是必须的：覆盖层那颗返回箭头顶上顶着的是**整个「实例设置」**（点一下这一趟
    // 就没了），而用户在这儿的下一步多半是"看另一个文件"。所以这一页自己带一颗。
    m_back = new QPushButton(backText(), this);
    m_back->setObjectName(backButtonName());
    // 「← 返回」、「条目」、「还原上一次」、「打开文件夹」四颗都是**看一眼或者挪个地方**
    // （一般性）；只有右下角那颗「保存」是确认性。
    // NOTE：「还原上一次」**不是**危险那一档 —— 它只把那份备份摆进框里
    // （下面那条注释说得很死：“取回不许是一次悄悄的写入”），真写盘是点「保存」。
    Theme::markButton(m_back, Theme::ButtonKind::Neutral);
    connect(m_back, &QPushButton::clicked, this, [this] { emit backRequested(); });
    topRow->addWidget(m_back);

    m_pathLabel = new ElidedLabel(this);
    m_pathLabel->setObjectName(pathLabelName());
    // 从**中间**截：这一串两头都有用 —— 前头是哪个实例、尾巴上才是文件名。
    //
    // NOTE: 这一格的真身份是 `ElidedLabel`，而 .hpp 里成员声明的是 `QLabel`（对外只要
    // setText / text 那两下，和 InstanceFilesPage::pathLabel() 同一条规矩：别把
    // ElidedLabel.hpp 拖进每一个用到这一页的地方）。所以在这儿转一次。
    static_cast<ElidedLabel *>(m_pathLabel)->setElideMode(Qt::ElideMiddle);
    makeSecondary(m_pathLabel);
    topRow->addWidget(m_pathLabel, 1);

    // 「条目」那一颗：左边那一格的开合（见 .hpp 那段）。它不是在切"看什么文件"，
    // 而是在切"这一格里看什么" —— 所以和「返回」摆在一头，不跟文件那两颗挤在另一头。
    m_indexToggle = new QPushButton(indexToggleText(), this);
    m_indexToggle->setObjectName(indexToggleName());
    Theme::markButton(m_indexToggle, Theme::ButtonKind::Ghost);
    m_indexToggle->setCheckable(true);
    m_indexToggle->setChecked(true);
    connect(m_indexToggle, &QPushButton::toggled, this, [this] { refreshIndexVisibility(); });
    topRow->addWidget(m_indexToggle);

    // 「还原上一次」：每存一次**之前**都会把盘上那一份原样留一份（见 ConfigFileText 那一
    // 节），这一颗就是把最近那一份取回来。它摆在这一头、和「条目」挨着 —— 那两颗干的都是
    // "看着这一格"的事，不是"换一份文件"。
    //
    // 取回来**只是摆进框里**：底下那行字会说清它是哪来的、还没写回盘上，用户按「保存」
    // 才真的落到盘上。**取回不许是一次悄悄的写入** —— 那样用户按一下就叫"还原"，
    // 而盘上那份已经被盖掉了，再想回到"刚才改的那一份"就没有路（它没被备份过：
    // 备份留的永远是"写之前的那一份"）。
    m_restore = new QPushButton(restoreText(), this);
    m_restore->setObjectName(restoreButtonName());
    Theme::markButton(m_restore, Theme::ButtonKind::Neutral);
    connect(m_restore, &QPushButton::clicked, this, [this] {
        if (!m_hasFile) {
            return;
        }
        // 这一层**不自己读那份备份**：读盘是读盘，而它连 AppPaths 都够不着（见文件头
        // "这一层不碰盘"）。所以只把"用户要这个文件的上一次"这件事发出去。
        emit restoreRequested(m_path);
    });
    topRow->addWidget(m_restore);
    m_history = new QPushButton(tr("历史版本"), this);
    m_history->setObjectName(QStringLiteral("configFileHistory"));
    Theme::markButton(m_history, Theme::ButtonKind::Neutral);
    m_history->setToolTip(tr("按时间选择这个文件的备份；载入后需点击保存才会写回。"));
    connect(m_history, &QPushButton::clicked, this, [this] {
        if (m_hasFile && m_backupAvailable && isEditable()) emit historyRequested(m_path);
    });
    topRow->addWidget(m_history);

    m_openFolder = new QPushButton(openFolderText(), this);
    m_openFolder->setObjectName(openFolderButtonName());
    Theme::markButton(m_openFolder, Theme::ButtonKind::Ghost);
    // 这一层不自己开：开目录是 QDesktopServices 的事，而失败还要弹一句 —— 那得走覆盖层
    // （见文件头那段"这一层不碰盘"）。
    connect(m_openFolder, &QPushButton::clicked, this,
            [this] { emit openFolderRequested(m_path); });
    topRow->addWidget(m_openFolder);
    outer->addLayout(topRow);

    // ---------- 中间：左边那一格「条目」+ 编辑框 ----------
    auto *middle = new QHBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0);
    middle->setSpacing(10);

    m_indexPanel = new QWidget(this);
    m_indexPanel->setObjectName(indexPanelName());
    m_indexPanel->setFixedWidth(kIndexWidth);
    auto *panelColumn = new QVBoxLayout(m_indexPanel);
    panelColumn->setContentsMargins(0, 0, 0, 0);
    panelColumn->setSpacing(6);

    m_indexFilter = new QLineEdit(m_indexPanel);
    m_indexFilter->setObjectName(indexFilterName());
    m_indexFilter->setPlaceholderText(indexFilterPlaceholder());
    m_indexFilter->setClearButtonEnabled(true);
    // 筛选**立刻**生效（不走那台 250 ms 的延时器）：它只是在树上藏几行，不改任何正文，
    // 也就没有"每敲一个字扫全文"那个代价。
    connect(m_indexFilter, &QLineEdit::textChanged, this, [this] { applyIndexFilter(); });
    panelColumn->addWidget(m_indexFilter);

    m_index = new QTreeWidget(m_indexPanel);
    m_index->setObjectName(indexTreeName());
    m_index->setColumnCount(2);
    m_index->setHeaderLabels({QCoreApplication::translate("ConfigFileEditorPage", "条目"), QCoreApplication::translate("ConfigFileEditorPage", "值")});
    m_index->setRootIsDecorated(true);
    // 一行一个键，行高一致 ⇒ 让视图按第一行算高度就够了（长列表上省一大笔）。
    m_index->setUniformRowHeights(true);
    m_index->setSelectionMode(QAbstractItemView::SingleSelection);
    // 索引是给人点着看的，不是给人改的 —— 双击那一行不许变成"编辑这一格文字"。
    m_index->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_index->header()->setStretchLastSection(true);
    m_index->setColumnWidth(0, 140);
    // 点一条就跳过去。**只在点叶子的时候有位置可跳**（分组那几行没有值的区间，
    // `jumpToEntry()` 自己会认出来）。
    connect(m_index, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int /*column*/) { jumpToEntry(item); });
    panelColumn->addWidget(m_index, 1);
    middle->addWidget(m_indexPanel);
    // 展开/折叠动画（规则见 ui/Animations.hpp）。
    Anim::animateExpansion(m_index);

    m_editor = new CodeEditor(this);
    m_editor->setObjectName(editorName());
    m_editor->setPlaceholderText(tr("（这个文件是空的）"));
    m_editor->setMinimumHeight(kEditorMinHeight);
    middle->addWidget(m_editor, 1);
    outer->addLayout(middle, 1);

    // 改字之后把索引重算一遍：**区间是拿正文算出来的，正文一动它就旧了**。
    // 延时 250 ms（见 kIndexRebuildDelayMs）—— 不延时的话，一次保存型的大改动
    // （全选粘贴）会连着触发几十次重建。
    m_indexTimer = new QTimer(this);
    m_indexTimer->setSingleShot(true);
    m_indexTimer->setInterval(kIndexRebuildDelayMs);
    connect(m_indexTimer, &QTimer::timeout, this, [this] { rebuildIndex(); });
    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        if (m_loading || m_indexTimer == nullptr) {
            return;
        }
        m_indexTimer->start();
    });

    // ---------- 底下：一句话 + 保存 ----------
    auto *bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(8);

    m_note = new QLabel(this);
    m_note->setObjectName(noteLabelName());
    m_note->setWordWrap(true);
    makeSecondary(m_note);
    bottomRow->addWidget(m_note, 1);

    m_save = new QPushButton(saveText(), this);
    m_save->setObjectName(saveButtonName());
    connect(m_save, &QPushButton::clicked, this, [this] {
        if (!m_hasFile) {
            return;
        }
        // 要写哪些字节**在这儿算**（行尾、BOM 都还完了，见 ConfigFileText::encode）——
        // 接的人只负责把它写到 m_path 上，不用再拼第二遍。
        emit saveRequested(m_path, ConfigFileText::encode(m_loaded, text()));
    });
    bottomRow->addWidget(m_save);
    outer->addLayout(bottomRow);

    // "还没摆过文件"那一档：编辑框只读、保存不画（真摆文件时 setFile 会把这两样按那一份
    // 文件重算一遍）。
    m_editor->setReadOnly(true);
    m_save->setVisible(false);
    m_restore->setVisible(false);
    m_history->setVisible(false);
    m_history->setEnabled(false);
    refreshNote();
    refreshIndexVisibility();
}

void ConfigFileEditorPage::setFile(const QString &path, const ConfigFileText::Loaded &loaded,
                                   const ConfigFileText::Stamp &stamp) {
    m_path = path;
    m_loaded = loaded;
    m_hasFile = true;
    m_stamp = stamp;
    // 换了一份文件 ⇒ **上一条的那个答案不作数了**：那个文件留过备份没有、框里是不是从
    // 备份取回来的，都是"对某一份文件"说的。不清的话，用户从 A（有备份）切到 B（没备份）
    // 会看见 B 上挂着一颗「还原上一次」，点下去取回来的是 A 的备份 —— 而且它看起来
    // 完全合理。调用方紧接着会用 setBackupAvailable() 把这一份的真实答案摆上。
    m_backupAvailable = false;
    m_restoredFrom.clear();

    m_pathLabel->setText(QDir::toNativeSeparators(path));
    m_pathLabel->setToolTip(QDir::toNativeSeparators(path));

    const bool editable = loaded.editable();
    m_editor->setReadOnly(!editable);
    // 摆正文：`setPlainText()` 会把"改过没有"重置回"没改"（Qt 的文档模型就是这么做的）
    // —— 不过 isDirty() 压根不看那一位，它是拿 m_loaded.text 现比的，所以这一句与它无关。
    //
    // 这一句会发 textChanged，而"改字了 ⇒ 索引得重算"那台延时器接的就是它 —— 不挡住
    // 的话，刚摆完一份文件又自己扫一遍全文（白扫，而且会把下面刚摆好的索引再清一次）。
    m_loading = true;
    m_editor->setPlainText(loaded.text);
    m_loading = false;
    // 换了一份文件 ⇒ 筛选框清空、索引**立刻**重算（不等那 250 ms：用户这时候正盯着它）。
    if (m_indexTimer != nullptr) {
        m_indexTimer->stop();
    }
    if (m_indexFilter != nullptr) {
        m_indexFilter->clear();  // 它自己会叫 applyIndexFilter()
    }
    rebuildIndex();
    // 从**顶上**开始（不留在上一次翻到的地方），并把光标摆在开头。
    m_editor->moveCursor(QTextCursor::Start);
    m_editor->ensureCursorVisible();

    // 只读时「保存」**不画**（不是画成灰的：灰按钮说的是"还没资格点"，而这里的问题是
    // "点了也没有意义"）。可编辑时才摆出来。
    m_save->setVisible(editable);
    m_save->setEnabled(true);
    refreshRestore();
    refreshNote();
}

void ConfigFileEditorPage::setBackupAvailable(bool available) {
    m_backupAvailable = available;
    refreshRestore();
}

void ConfigFileEditorPage::refreshRestore() {
    if (m_restore == nullptr) {
        return;
    }
    // 只读的（不是 UTF-8 / 太大）取回来也写不回去 —— 摆一颗按下去没结果的按钮，
    // 和画一颗灰按钮一样，都是"点了也没意义"（见 .hpp 里「保存」那一段）。
    m_restore->setVisible(m_backupAvailable && isEditable());
    m_restore->setEnabled(true);
    if (m_history != nullptr) {
        m_history->setVisible(m_backupAvailable && isEditable());
        m_history->setEnabled(m_backupAvailable && isEditable());
    }
}

void ConfigFileEditorPage::restoreContent(const QString &text, const QString &whenText) {
    if (!m_hasFile) {
        return;
    }
    // 只摆正文（见 .hpp）：`m_loaded` **一个字节都不动**。于是
    // - `isDirty()` 自己就变 true（它拿的是 m_loaded.text 比的）⇒「返回」会问一句、
    // 「保存」才真的写盘 —— 还原不是一次悄悄的写入；
    // - 存的时候行尾/BOM 用的是**这个文件现在**的那些（`m_loaded.ends`）—— 备份里那几行
    // 要是赶上不同的行尾/编码，写回去也还是这份文件本来的样子。
    m_loading = true;
    m_editor->setPlainText(text);
    m_loading = false;
    if (m_indexTimer != nullptr) {
        m_indexTimer->stop();
    }
    rebuildIndex();  // 正文整份换掉了，索引的区间全不作数了 —— 立刻重认
    m_editor->moveCursor(QTextCursor::Start);
    m_editor->ensureCursorVisible();
    m_restoredFrom = whenText;
    refreshNote();
}

QString ConfigFileEditorPage::text() const {
    return m_editor == nullptr ? QString() : m_editor->toPlainText();
}

bool ConfigFileEditorPage::isDirty() const { return m_hasFile && text() != m_loaded.text; }

bool ConfigFileEditorPage::isEditable() const { return m_hasFile && m_loaded.editable(); }

QString ConfigFileEditorPage::path() const { return m_path; }

QPlainTextEdit *ConfigFileEditorPage::editor() const { return m_editor; }

int ConfigFileEditorPage::gutterWidth() const {
    // 行号槽是 CodeEditor 自己的事（见上面那个类）。`m_editor` 是它的基类指针，这一页
    // 只知道"它是个 QPlainTextEdit"（那样 .hpp 不必把 CodeEditor 拖进去），所以这儿
    // 按基类指针往下转一次。**这个向下转型一定成立**：构造函数里 new 的就是它。
    const auto *editor = static_cast<const CodeEditor *>(m_editor);
    return editor == nullptr ? 0 : editor->gutterWidth();
}

void ConfigFileEditorPage::markSaved() {
    if (!m_hasFile) {
        return;
    }
    // 存过之后"读过那一份"就得换成**刚写下去的那一份**：`isDirty()` 是拿它比的，
    // 不换的话用户存完再点「返回」，还会被问一句"改了没保存"——而它明明刚存过。
    //
    // 换的东西就是把刚才那份字节解回来（`saveRequested` 里算的是同一份：同样的
    // `m_loaded` + 同样的正文，`encode()` 是纯函数），所以 `raw` 是盘上真实的那一份，
    // `text` 也逐字等于框里的正文 ⇒ `isDirty()` 归零。
    m_loaded = ConfigFileText::decode(ConfigFileText::encode(m_loaded, text()));
    // 刚写下去的这一份现在**就是**盘上那一份了，"框里是从备份取回来的"这件事到此为止
    // （不抹掉的话，下一次 refreshNote() 还会说"取回来的、按保存才写回盘上"——而它已经
    // 写回去了）。
    m_restoredFrom.clear();
    m_note->setText(savedPrefix() + QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) +
                    QCoreApplication::translate("ConfigFileEditorPage", "）"));
}

void ConfigFileEditorPage::refreshNote() {
    makeSecondary(m_note);
    if (!m_hasFile) {
        m_note->setText(QString());
        return;
    }
    if (!m_loaded.utf8Ok) {
        m_note->setText(readOnlyPrefix() +
                        tr("文件使用非 UTF-8 编码。为保护内容，已按只读方式打开。"));
        return;
    }
    if (m_loaded.tooBig) {
        m_note->setText(readOnlyPrefix() +
                        tr("文件超过 %1 MB 的编辑上限，已按只读方式打开。")
                            .arg(ConfigFileText::editableByteLimit() / (1024 * 1024)));
        return;
    }
    // 取回来那一份得**说明白**：框里的内容和刚打开时不一样了，而盘上还是旧的。不说的话，
    // 用户会以为"还原"已经落盘了（那是这一页唯一一处"正文变了但盘上没变"的情形）。
    if (!m_restoredFrom.isEmpty()) {
        m_note->setText(restoredPrefix() + m_restoredFrom + QCoreApplication::translate("ConfigFileEditorPage", "）") +
                        tr("，选择「保存」后写入文件。"));
        return;
    }
    m_note->setText(hintText());
}

void ConfigFileEditorPage::discardChanges() {
    if (!m_hasFile) {
        return;
    }
    // 退回读进来那一份 ⇒ isDirty() 变 false ⇒ 调用方接着切回列表。**改过的东西不许悄悄
    // 没掉**（见 .hpp）：那一问由 MainWindow 弹，用户答"不保存"才走到这儿。
    m_loading = true;
    m_editor->setPlainText(m_loaded.text);
    m_loading = false;
    // 正文退回读进来那一份 ⇒ "框里是从备份取回来的"这件事也没了（用户刚说了这些改动不要）。
    m_restoredFrom.clear();
    // 正文回到原样了，索引也得跟着回去（不然它指着的是"用户改过的那一份"的位置）。
    if (m_indexTimer != nullptr) {
        m_indexTimer->stop();
    }
    rebuildIndex();
    refreshNote();
}

int ConfigFileEditorPage::rebuildIndex() {
    if (m_index == nullptr) {
        return 0;
    }
    // 重建之后原来选中的那一条还得选回来：改一个字就会重算索引，而用户手里往往正握着
    // 一条（"找到它了、还没动手改"）—— 每一次重算都把它跳回顶层，用起来就是"索引自己
    // 在动"。
    QString keepPath;
    if (QTreeWidgetItem *current = m_index->currentItem()) {
        keepPath = current->data(0, entryPathRole()).toString();
    }

    // 方言按**文件的路径**认（`.cfg` / `.properties` / `.ini` / `.conf` 那一族的值读到
    // 行尾，逗号不是分隔符）—— 这一层是唯一知道"正在编的是哪一份文件"的地方。
    const QList<ConfigEntries::Entry> entries =
        ConfigEntries::scan(text(), ConfigEntries::dialectForPath(m_path));
    m_indexEntries = entries.size();
    m_index->clear();

    QHash<QString, QTreeWidgetItem *> groups;  // 路径前缀 → 那一行分组
    QTreeWidgetItem *keepItem = nullptr;
    for (const ConfigEntries::Entry &entry : entries) {
        QTreeWidgetItem *parent = nullptr;
        QString prefix;
        for (int i = 0; i + 1 < entry.segments.size(); ++i) {
            prefix = prefix.isEmpty() ? entry.segments.at(i)
                                      : prefix + QLatin1Char('.') + entry.segments.at(i);
            QTreeWidgetItem *node = groups.value(prefix);
            if (node == nullptr) {
                node = parent == nullptr ? new QTreeWidgetItem(m_index)
                                         : new QTreeWidgetItem(parent);
                node->setText(0, entry.segments.at(i));
                // 分组那一行**不给选中**：它不是一个设置项，选中它没有可跳的地方。
                node->setFlags(Qt::ItemIsEnabled);
                groups.insert(prefix, node);
            }
            parent = node;
        }
        auto *leaf = parent == nullptr ? new QTreeWidgetItem(m_index)
                                       : new QTreeWidgetItem(parent);
        // 行上是 `键 = 值` 那种一一对应：第一格是键（这棵树里就是它最后那一段），
        // 第二格是**值原文**（缩进、引号、大小写都和文件里一模一样）。
        leaf->setText(0, entry.key);
        leaf->setText(1, entry.value);
        leaf->setData(0, valueBeginRole(), entry.valueBegin);
        leaf->setData(0, valueEndRole(), entry.valueEnd);
        leaf->setData(0, entryPathRole(), entry.path);
        // 光标停在那一条上时，那一堆"这个键上面写着什么"就浮出来 —— 而那段注释常常是这份
        // 配置唯一的一份说明书（Forge 生成的那批 TOML 连范围都写在里面）。
        QStringList tip;
        tip << entry.path;
        if (!entry.range.isEmpty()) {
            tip << QCoreApplication::translate("ConfigFileEditorPage", "范围：") + entry.range;
        }
        if (!entry.note.isEmpty()) {
            tip << entry.note;
        }
        tip << QCoreApplication::translate("ConfigFileEditorPage", "%1 / 第 %2 行")
                   .arg(ConfigEntries::kindName(entry.kind))
                   .arg(entry.line + 1);
        leaf->setToolTip(0, tip.join(QLatin1Char('\n')));
        leaf->setToolTip(1, tip.join(QLatin1Char('\n')));
        if (!keepPath.isEmpty() && entry.path == keepPath) {
            keepItem = leaf;
        }
    }

    // 分组默认全展开：这一格是"一眼看全这份文件能改什么"，一进来就让人一个个点开分组
    // 反而多一层。条目太多的时候有筛选框（那才是长列表的正门）。
    m_index->expandAll();
    applyIndexFilter();
    if (keepItem != nullptr) {
        m_index->setCurrentItem(keepItem);
    }
    refreshIndexVisibility();
    return entries.size();
}

int ConfigFileEditorPage::indexEntryCount() const {
    if (m_index == nullptr) {
        return 0;
    }
    int leaves = 0;
    std::function<void(QTreeWidgetItem *)> count = [&](QTreeWidgetItem *item) {
        if (item->childCount() == 0) {
            ++leaves;
            return;
        }
        for (int i = 0; i < item->childCount(); ++i) {
            count(item->child(i));
        }
    };
    for (int i = 0; i < m_index->topLevelItemCount(); ++i) {
        count(m_index->topLevelItem(i));
    }
    return leaves;
}

void ConfigFileEditorPage::applyIndexFilter() {
    if (m_index == nullptr) {
        return;
    }
    const QString needle = m_indexFilter == nullptr ? QString() : m_indexFilter->text().trimmed();
    // 递归下去：**底下有中的，祖先就得留着** —— 不然中的那一条被它自己的分组藏起来了，
    // 用户看到的是"筛选之后什么都没有"。
    std::function<bool(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *item) -> bool {
        bool childVisible = false;
        for (int i = 0; i < item->childCount(); ++i) {
            if (visit(item->child(i))) {
                childVisible = true;
            }
        }
        const bool selfMatch =
            needle.isEmpty() ||
            item->text(0).contains(needle, Qt::CaseInsensitive) ||
            item->text(1).contains(needle, Qt::CaseInsensitive) ||
            item->data(0, entryPathRole()).toString().contains(needle, Qt::CaseInsensitive);
        const bool leaf = item->childCount() == 0;
        const bool visible = leaf ? selfMatch : (childVisible || (needle.isEmpty() ? true
                                                                                  : selfMatch));
        item->setHidden(!visible);
        return visible;
    };
    for (int i = 0; i < m_index->topLevelItemCount(); ++i) {
        visit(m_index->topLevelItem(i));
    }
    if (!needle.isEmpty()) {
        m_index->expandAll();
    }
}

void ConfigFileEditorPage::refreshIndexVisibility() {
    if (m_indexPanel == nullptr) {
        return;
    }
    // 空的时候那颗按钮**点不动**（不是"点了没反应"）：认不出条目就是认不出，"展开一个
    // 空的索引"没有第二种意思。为什么点不动写在它的 tooltip 上。
    const bool empty = m_indexEntries <= 0;
    if (m_indexToggle != nullptr) {
        m_indexToggle->setEnabled(!empty);
        m_indexToggle->setToolTip(empty ? indexEmptyHint() : QString());
    }
    const bool wanted = m_indexToggle != nullptr && m_indexToggle->isChecked() && !empty;
    m_indexPanel->setVisible(wanted);
}

bool ConfigFileEditorPage::indexVisible() const {
    // 问的是 `isHidden()` 不是 `isVisible()`：这一页自己常常整个是藏着的（它是那个堆叠
    // 里被切出来的一格，off 的时候父控件就没有可见性），那时候 `isVisible()` 对**任何**
    // 子控件都回假 —— "这一格收起来了"这件事问的从来不是"它现在看不看得见"。
    return m_indexPanel != nullptr && !m_indexPanel->isHidden();
}

void ConfigFileEditorPage::jumpToEntry(QTreeWidgetItem *item) {
    if (item == nullptr || m_editor == nullptr) {
        return;
    }
    const QVariant beginData = item->data(0, valueBeginRole());
    const QVariant endData = item->data(0, valueEndRole());
    if (!beginData.isValid() || !endData.isValid()) {
        return;  // 分组那一行：它底下没有"某一格值"
    }
    int begin = beginData.toInt();
    int end = endData.toInt();
    // 区间是从**上一次**认出来的正文算的（那个 250 ms 的延时器还没响）。正文动过之后
    // 它就可能落到别处 —— 越界的话宁可不跳，也不去选一段风马牛不相及的字。
    //
    // 判据：那一格第 1 列记着**值原文**（`键 = 值` 里那个值，缩进/引号/大小写都和文件
    // 里逐字一样），而扫描器记区间时用的就是 `text.mid(begin, end - begin)`（见
    // ConfigEntries）。所以"这一段现在还是不是原来那一段"可以直接比字符串。
    // 对不上 = 索引旧了 ⇒ **当场重算一遍**，再按路径把这一条找回来用新区间跳 ——
    // 不是"干脆不跳"：用户点它就是要跳过去，把"请再点一次"推回去才是添麻烦。
    const QString path = item->data(0, entryPathRole()).toString();
    const QString editor = text();
    const auto rangeStillMatches = [&editor](int from, int to, const QString &value) {
        return from >= 0 && to >= from && to <= editor.size() &&
               editor.mid(from, to - from) == value;
    };
    if (!rangeStillMatches(begin, end, item->text(1))) {
        rebuildIndex();  // 索引整棵重建，item 那个指针当场作废
        QTreeWidgetItem *fresh = nullptr;
        QList<QTreeWidgetItem *> stack;
        for (int i = 0; i < m_index->topLevelItemCount(); ++i) {
            stack.append(m_index->topLevelItem(i));
        }
        while (!stack.isEmpty()) {
            QTreeWidgetItem *candidate = stack.takeLast();
            if (candidate->data(0, entryPathRole()).toString() == path) {
                fresh = candidate;
                break;
            }
            for (int i = 0; i < candidate->childCount(); ++i) {
                stack.append(candidate->child(i));
            }
        }
        // 找不回来 = 这一条在这一版正文里已经没了（键被删掉或改名）—— 那就什么都不跳，
        // 这也正是"宁可不动"那条兜底。
        if (fresh == nullptr) {
            return;
        }
        item = fresh;
        begin = item->data(0, valueBeginRole()).toInt();
        end = item->data(0, valueEndRole()).toInt();
    }
    // 兜底还是这两条（重算过之后再对不上就只能是不该跳的那种）。
    if (begin < 0 || end < begin || end > m_editor->document()->characterCount()) {
        return;
    }
    QTextCursor cursor(m_editor->document());
    cursor.setPosition(begin);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    m_editor->setTextCursor(cursor);
    // 滚到眼前（不是"滚到刚好露出来"——那一格常在屏幕最下沿，看着像没动）。
    m_editor->centerCursor();
    // 焦点给编辑框：用户点索引那一格是为了接着改它，不是为了让索引亮着。
    m_editor->setFocus(Qt::OtherFocusReason);
}

void ConfigFileEditorPage::keyPressEvent(QKeyEvent *event) {
    // Esc 得在这一层接住（见 .hpp）。它从编辑框冒上来走到这儿，说明编辑框自己不认这个
    // 键 —— 那它就只该有"退回文件列表"这一个意思。
    if (event->key() == Qt::Key_Escape) {
        emit backRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ConfigFileEditorPage::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    // 焦点只能在这一刻补（见 .hpp 那一 NOTE）：这一块是被堆叠切出来的，露脸的那一下
    // 焦点归了新露面的控件（或者还在刚才那张列表上）。**不补这一下，用户进来打不了
    // 字** —— 而"改这个文件"是这一页唯一的主要功能。
    if (m_editor != nullptr && isEditable()) {
        m_editor->setFocus(Qt::OtherFocusReason);
    }
}

// ---------- 那几行字、那几个名字的唯一出处 ----------

QString ConfigFileEditorPage::pageName() { return QStringLiteral("configFileEditorPage"); }
QString ConfigFileEditorPage::editorName() { return QStringLiteral("configFileEditor"); }
QString ConfigFileEditorPage::pathLabelName() { return QStringLiteral("configFilePath"); }
QString ConfigFileEditorPage::noteLabelName() { return QStringLiteral("configFileNote"); }
QString ConfigFileEditorPage::saveButtonName() { return QStringLiteral("configFileSaveButton"); }
QString ConfigFileEditorPage::openFolderButtonName() {
    return QStringLiteral("configFileOpenFolderButton");
}
QString ConfigFileEditorPage::backButtonName() { return QStringLiteral("configFileBackButton"); }
QString ConfigFileEditorPage::indexPanelName() { return QStringLiteral("configFileIndexPanel"); }
QString ConfigFileEditorPage::indexTreeName() { return QStringLiteral("configFileIndexTree"); }
QString ConfigFileEditorPage::indexFilterName() { return QStringLiteral("configFileIndexFilter"); }
QString ConfigFileEditorPage::indexToggleName() { return QStringLiteral("configFileIndexToggle"); }
QString ConfigFileEditorPage::restoreButtonName() {
    return QStringLiteral("configFileRestoreButton");
}

// 从 20 起：InstanceFilesPage 那几行用的是 8/9/10（行装饰）与 12/13（置顶 / 取消置顶），
// 版本树那一套也在低位上。往后排开，谁也别踩谁。
int ConfigFileEditorPage::valueBeginRole() { return Qt::UserRole + 20; }
int ConfigFileEditorPage::valueEndRole() { return Qt::UserRole + 21; }
int ConfigFileEditorPage::entryPathRole() { return Qt::UserRole + 22; }

QString ConfigFileEditorPage::backText() { return QCoreApplication::translate("ConfigFileEditorPage", "返回"); }
QString ConfigFileEditorPage::saveText() { return QCoreApplication::translate("ConfigFileEditorPage", "保存"); }
QString ConfigFileEditorPage::openFolderText() { return QCoreApplication::translate("ConfigFileEditorPage", "打开文件夹"); }
QString ConfigFileEditorPage::indexToggleText() { return QCoreApplication::translate("ConfigFileEditorPage", "条目"); }
QString ConfigFileEditorPage::indexFilterPlaceholder() { return QCoreApplication::translate("ConfigFileEditorPage", "在条目里找…"); }
QString ConfigFileEditorPage::restoreText() { return QCoreApplication::translate("ConfigFileEditorPage", "还原上一次"); }
QString ConfigFileEditorPage::restoredPrefix() { return QCoreApplication::translate("ConfigFileEditorPage", "已恢复上次保存前的内容（"); }
QString ConfigFileEditorPage::indexEmptyHint() { return QCoreApplication::translate("ConfigFileEditorPage", "文件中未找到可编辑的设置项"); }
QString ConfigFileEditorPage::readOnlyPrefix() { return QCoreApplication::translate("ConfigFileEditorPage", "只读："); }
QString ConfigFileEditorPage::hintText() {
    return QCoreApplication::translate("ConfigFileEditorPage", "选择「保存」后写入文件。保留原文件的换行格式和 BOM。");
}
QString ConfigFileEditorPage::savedPrefix() { return QCoreApplication::translate("ConfigFileEditorPage", "已保存（"); }
