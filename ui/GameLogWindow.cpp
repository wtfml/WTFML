/**
 * @file GameLogWindow.cpp
 * @brief 实现每次游戏会话的日志窗口。
 * @note 顶部筛选/行数，中间日志正文，底部关闭；正文占用剩余空间。
 */

#include "GameLogWindow.hpp"

// WheelGuard 住在 MainWindowUI.hpp 里（和 AccountSkinDialog.cpp / InstanceSettingsDialog.cpp
// 一样，直接引那一份，不为一个三十行的小类再开一个头文件）。
#include "MainWindowUI.hpp"
#include "Glass.hpp"
#include "TitleBar.hpp"
#include "WindowChrome.hpp"
#include "ElidedLabel.hpp"
#include "Theme.hpp"   // 按钮身份、`neutralFill()`（那几档中性灰）、`isDarkPalette()`
#include "VersionIcons.hpp"

#include <QApplication>
#include <QAction>
#include <QGuiApplication>
#include <QClipboard>
#include <QMenu>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSettings>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

/// 「到底了」的容差（像素/行）。滚到最底时 value() 未必精确等于 maximum()（一行的
/// 高度可能除不尽），留一点点余量才不会"贴着底却判成没贴着"。
constexpr int kFollowTailSlack = 4;

/// 级别那几档的底色。**都在这里，一处**（验证工程按它们量像素）。
/// 挑色的两条：四个染色的档两两看得出来（`0xeef2f6` 和窗口底色白只差 23，那是有意的 ——
/// Debug 是"最不值得看"的一档），以及深红那档配上白字还读得清。
constexpr int kWarnBg[] = {0xff, 0xf0, 0xbf};
constexpr int kErrorBg[] = {0xf9, 0xd9, 0xde};
constexpr int kFatalBg[] = {0xed, 0xb5, 0xbd};

/// 深色底那一套（深浅色入口）。
///
/// 上面那一组是照着"白纸"挑的：深色预设下正文区的 `Base` 是深底、墨是浅的，同一批浅色
/// 方块铺上去就是"浅墨压浅底" —— 一个字都读不出来。所以同一族色相各压一版：
/// 挑色两条 —— ① 四个档两两看得出（和浅色那一组同一个要求）；② 明度都压在 0x40 上下，
/// 浅墨（深色预设里那支 `#e0e0e0` 上下）压上去有 8:1 以上，Debug/Warn/Error 三档之间
/// 也分得开（三通道距离都在 20 以上，验证工程那条阈值是 20）。
/// **Fatal 两套相同**：`#d32f2f` 本来就是"深底"，白字压上去 5.3:1，两种主题下都对。
constexpr int kWarnBgDark[] = {0x3a, 0x32, 0x18};
constexpr int kErrorBgDark[] = {0x3a, 0x1f, 0x1f};
constexpr int kFatalBgDark[] = {0xd3, 0x2f, 0x2f};

/// 顶上那排开关"没开"和"开着但这一档没有颜色"（只有 Info 是后者）那两种灰。
///
/// **开关的两种状态都得由我们自己画**（理由见 filterButtonStyle），一旦交给样式，
/// 就必须自己给出这两种灰。它们**算出来的**（`Theme::neutralFill()`，见
/// `GameLogWindow::switchOffColor()`）—— 浅色下是 `#aa` / `#d9`，深色下自动是深灰。
/// **关态那一个百分比（29）只有一个出处**（那边那个函数）。
/// `kSwitchOnRgb` 那个兜底灰现在只剩这一处（下面那个 9%），
/// 真用到的档一个都不会走到它（只有 Info 没有底色，而它另有去处，见下）。
constexpr int kSwitchOnPercent = 9;
/// Info 那颗"开着"时候的颜色（要再亮一点）。

/// 行与行之间那条细线的颜色（日志行与行之间要有一条细线分隔）。
///
/// 挑这个灰的理由是"两头都得看得见"：白底上差 39*√3 ≈ 67，Debug 那个 `#eef2f6` 上差 45
/// （验证工程里"五档两两看得出来"的阈值是 20）。**不能比这更深** —— 再深一点整块正文就
/// 变成表格了，而这里要的只是"两条 INFO 之间有条界线"（相邻两档的底色本来就分得开，
/// 分不开的恰恰是相邻的两条 INFO：它们都不染底）。
/// 它也是算出来的（`separatorColor()`，从**正文区**的 `Base` 往它的墨混 15%）
/// —— 浅色下是 `#d9d9d9`。

/// 中性档的百分比（关态底 / 那个兜底的开态灰）—— **和上一段说的同一件事**。
constexpr int kSwitchOffPercent = 29;

/// 三通道字面量 → 色（`k*Bg` 那一堆都是这么用的）。
QColor fromRgb(const int rgb[3]) {
    return QColor(rgb[0], rgb[1], rgb[2]);
}

/// 这一档该用哪一套底（浅色那组 / 深色那组）。`dark` 由 `Theme::isDarkPalette()` 给。
QColor levelTint(const int light[3], const int dark[3], bool isDark) {
    return fromRgb(isDark ? dark : light);
}

/// 正文比系统那个等宽字体大几号（点）。"稍微"大一点：再大就一屏放不下几行。
constexpr qreal kLogFontPointDelta = 1.0;

/// 顶上那排开关的样式。
///
/// 【为什么连"没开"那一种也要自己画】
///
/// 只写 `QToolButton:checked { background-color: ... }` 的话，原生的"按下去"那层立体感
/// 会照样盖在我们的底色上，于是底色被冲淡（`#fff4cc` 量成 `#e8e1c7`、Fatal 那个
/// `#d32f2f` 被冲成 `#d75a5a`）——
/// 而"按钮的颜色 == 那一行日志的颜色"正是这件事的全部意义，冲淡了就不成立了。
/// 所以两种状态都写全（背景 + 边 + 圆角 + 内边距），由我们自己画。**两边的边和内边距必须
/// 一模一样**：差一点就是"点一下按钮自己变大变小"、整排跟着跳（验证工程里有一条盯着它）。
QString filterButtonStyle(const QColor &onBg, const QColor &onInk, const QColor &offBg,
                          const QColor &offInk) {
    // 内边距取 `1px 5px`：两边加起来正好把按钮撑到 **19 px**，
    // 这一排不因为换了个画法就长高一点。

    // 【悬停那一档为什么必须自己写一条】
    //
    // 一旦给控件挂了样式表，Qt 就把这颗按钮整只交给样式表去画 —— 原生 Fusion 那层
    // "鼠标停上来会亮一点"的立体感**一并被顶掉**，而且不写 `:hover` 也不会漏回原生
    // （上面那条 `QToolButton { ... }` 已经把这只控件接管了）。所以不显式补一条，
    // 划过时就是**一点变化都没有**（这五颗不补就会"划过毫无反应"）。
    // 只收**没勾着**的那一颗：勾着的那档划过去是"选中"的样子，不该跟着变（`:!checked`）。
    // `:enabled` 兜住以后万一有被 disable 的一颗（和 `QPushButton:hover:enabled` 同款）。
    //
    // 【这一颗为什么不走 `Theme::rowHoverFill()`，而是把自己往下压】
    //
    // 用户那句「把所有悬停在版本/按钮上都加一个颜色加深的效果」在别处（那些行、白底按钮）
    // 落成的都是 `Theme::rowHoverFill()` —— 底色往主题色混 12%，底色是白时 R 和 B 一起被
    // 压低，看上去正是"加深一层"。这颗按钮的底色是**中灰** `#ababab`，同一个混法算出来是
    // `#a1b3a4`：三通道距离只有 14（比仓库自己"两两看得出来"的阈值 20 还小），而且 G 反而
    // **升高**了 —— 又淡又发绿，恰恰是"加深"的反面。
    // 所以这一档把**自己的底色**往下压同一档：`darker(100 + rowHoverAccentPercent())`
    // 即 `darker(112)` = `#999999`（距离 31，和别处白底那一档的 27 是同一个量级）。
    // **"一档是多少"这个数仍然只有 Theme 一个出处**（`rowHoverAccentPercent()`）——
    // 变的只是"从哪一档出发、往哪边压"，那是底色自己决定的，别处也一样。
    const QColor hoverBg = offBg.darker(100 + Theme::rowHoverAccentPercent());

    return QStringLiteral("QToolButton { background-color: %1; color: %2; border: 1px solid transparent;"
                          " border-radius: 12px; padding: 3px 11px; }"
                          "QToolButton:hover:enabled:!checked { background-color: %3; }"
                          "QToolButton:checked { background-color: %4; color: %5; }")
        .arg(offBg.name(), offInk.name(), hoverBg.name(), onBg.name(), onInk.name());
}

/// 这一档的**块**格式：底色（铺满整行）在里面。
///
/// 【为什么底色挂在"块"上，而不是逐字的字符格式上】
///
/// 逐字的底色只染**字占的那一截**：一行比视口短（日志最后那几行、我们自己补的
/// "游戏已退出，退出码：-1" 都是短的）时，后半截是白的 —— 看着就像"最后一行没染上"。
/// 块格式是**整行**铺满（Qt 画块底色时按行的宽度填），而且不用往文本里塞空格，
/// 复制出去还是干净的一行。
///
/// NOTE: 没有底色那一档（Info）返回的就是一个**空的**块格式 —— `setBlockFormat` 是
/// "换掉"不是"合并"，所以空的那份正好把上一行继承下来的底色清掉（不显式清的话，
/// 记在 Fatal 后面的一行会拖一条红底）。
QTextBlockFormat blockFormatFor(GameLogWindow::LogLevel level, const QPalette &palette, bool colors) {
    QTextBlockFormat format;
    const QColor background = colors ? GameLogWindow::levelBackground(level, palette) : QColor();
    if (background.isValid()) {
        format.setBackground(background);
    }
    return format;
}

/// 这一档的**墨色**（只有 Fatal 有：深红底上再写深灰字读不清）。底色不在这里（见上）。
/// 不带调色板：别的档要的就是"这一档不设墨"，让正文区自己那一支（深色主题下是浅的）生效。
QColor logInk(GameLogWindow::LogLevel level, const QPalette &palette, bool colors = true) {
    if (!Theme::isDarkPalette(palette)) return QColor("#202020");
    if (!colors) return QColor("#D5DEE3");
    static const QColor darkInk[] = {QColor("#94A3AF"), QColor("#79D4A5"), QColor("#FFD166"),
                                    QColor("#FFB8BD"), QColor("#FF858B")};
    const int index = int(level);
    return darkInk[index >= 0 && index < GameLogWindow::levelCount() ? index : 1];
}

QTextCharFormat charFormatFor(GameLogWindow::LogLevel level, const QPalette &palette, bool colors) {
    QTextCharFormat format;
    const QColor ink = logInk(level, palette, colors);
    if (ink.isValid()) {
        format.setForeground(ink);
    }
    return format;
}

/// 这一窗的级别枚举 → 共用模型那一套（两套枚举的顺序钉在 .hpp 的 static_assert 上）。
RuntimeOutputModel::Level modelLevel(GameLogWindow::LogLevel level) {
    return static_cast<RuntimeOutputModel::Level>(int(level));
}

/**
 * @brief 正文那块文本控件：**每一行底下补一条细线**，其余照 QPlainTextEdit 原样。
 *
 * 【为什么要自己画】
 *
 * 「行与行之间一条线」在 QTextDocument 里**没有对应的写法**：块格式能设底色、能设行距，
 * 但没有"块的下边框"这一项（`QTextFrameFormat` 那圈边框是**整个文档框**的，不是每一行的）；
 * 想在文档里造一条线，只能往每一行后面塞一个真正的分隔块 —— 那会把"一行 = 一格"这件事
 * 毁掉（复制出来多一堆横线，行号、计数、光标位置全跟着偏）。
 *
 * 【为什么必须是子类】
 *
 * 剩下的路只有自己画，而画这件事要拿到"这一格在视口里的哪一条"：`firstVisibleBlock()` /
 * `blockBoundingGeometry()` / `contentOffset()` **都是 protected**，外面拿不到。所以是子类。
 *
 * 【画在哪儿】
 *
 * 遍历视口里看得见的那几格（三十来格，白送），在每一格的下沿画 `separatorHeight()` 像素。
 * 画在**基类之后**（盖在上面）—— 选中一段文本时那条高亮上会横着过几条线，那正是想要的
 * 样子：一眼看出选了几行。**最后一格（光标行）底下不画**：那是整份文本的末尾，
 * 不是"两行之间"。
 */
class LogView : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;

protected:
    void paintEvent(QPaintEvent *event) override {
        QPlainTextEdit::paintEvent(event);

        QPainter painter(viewport());
        // 这条线画在**正文区**上，所以从这一块自己的底和墨算（深色下自动是深灰）。
        painter.setPen(GameLogWindow::separatorColor(palette()));
        const int thickness = GameLogWindow::separatorHeight();
        const int right = qMax(0, viewport()->width() - 1);

        QTextBlock block = firstVisibleBlock();
        while (block.isValid()) {
            const int top =
                qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
            if (top > event->rect().bottom()) {
                break;  // 这一格已经在重画区下面了，再往下只会更远
            }
            const int bottom = top + qRound(blockBoundingRect(block).height());
            if (block.next().isValid() && bottom > event->rect().top()) {
                for (int i = 0; i < thickness; ++i) {
                    const int y = bottom - 1 - i;
                    painter.drawLine(0, y, right, y);
                }
            }
            block = block.next();
        }
    }
};

}  // namespace

GameLogWindow::GameLogWindow(const QString &instanceName, QWidget *parent)
    : QDialog(parent), m_instanceName(instanceName) {
    m_logColors = QSettings().value(QStringLiteral("Look/LogColors"), true).toBool();
    const auto logTokens = Theme::tokens();
    QPalette logPalette = palette();
    logPalette.setColor(QPalette::Window, logTokens.logWindow);
    logPalette.setColor(QPalette::Button, logTokens.logToolbar);
    logPalette.setColor(QPalette::Base, logTokens.logBase);
    logPalette.setColor(QPalette::Text, Theme::isDark() ? QColor("#D5DEE3") : QColor("#202020"));
    logPalette.setColor(QPalette::PlaceholderText, logTokens.logHint);
    setPalette(logPalette);
    setWindowTitle(windowTitleText(instanceName));
    // 允许最大化/最小化（这个窗要能最大化、最小化）。QDialog 默认
    // 只给一个关闭钮：一个"想摊开看"的窗，拉到整屏看长崩栈是常态（一行几十个字符，
    // 820 宽要左右拖）。**必须在 show() 之前设** —— show 之后改 flags 会把窗藏起来。
    setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);
    // 关掉即销毁（理由见 .hpp）。父窗口还挂着的那一份所有权随之解除 —— Qt 会自己处理。
    setAttribute(Qt::WA_DeleteOnClose);
    // 大小给得比别的对话框宽松：日志是宽行文本，窄了就得左右拖。比只有正文那版高一点点，
    // 顶上多了一排开关（那一排不从正文里抢，正文那一行拿的是 stretch 1）。
    resize(820, 520);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---------- 标题条（替系统标题栏，和主窗口同一套：WindowChrome 收标题栏 + 自绘条）----------
    // 原来窗里那行「手写体标题 + 运行徽标」并进这一条：同一个标题不出两遍。
    // 条上的字和徽标不接鼠标（WindowChrome 认空白处 = 拖窗，`childAt()` 跳过它们），
    // 于是除了三颗窗口按钮，整条都能拖、双击能最大化。
    m_titleStrip = new QWidget(this);
    m_titleStrip->setObjectName(QStringLiteral("gameLogTitleStrip"));
    m_titleStrip->setFixedHeight(TitleBar::barHeight());
    {
        auto *strip = new QHBoxLayout(m_titleStrip);
        strip->setContentsMargins(20, 0, 0, 0);
        strip->setSpacing(10);
        m_titleLabel = new QLabel(tr("游戏日志"), m_titleStrip);
        m_titleLabel->setObjectName(QStringLiteral("gameLogTitle"));
        m_titleLabel->setFont(Theme::textFont(m_titleLabel->font(), Theme::TextRole::Heading));
        m_titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        strip->addWidget(m_titleLabel, 0, Qt::AlignVCenter);
        // 实例名（文件查看器里是文件名）；没有名字时藏起来，`beginFile()` 再露出来。
        m_titleInstance = new ElidedLabel(m_titleStrip);
        m_titleInstance->setText(instanceName);
        m_titleInstance->setMinimumWidth(1);  // 长名收省略号，不把窗口的最小宽撑开（全名在系统窗口标题里）
        m_titleInstance->setObjectName(QStringLiteral("gameLogTitleInstance"));
        m_titleInstance->setForegroundRole(QPalette::PlaceholderText);
        m_titleInstance->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_titleInstance->setVisible(!instanceName.isEmpty());
        strip->addWidget(m_titleInstance, 0, Qt::AlignVCenter);
        m_stateBadge = new QLabel(m_titleStrip);
        m_stateBadge->setObjectName(QStringLiteral("gameLogStateBadge"));
        m_stateBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
        strip->addWidget(m_stateBadge, 0, Qt::AlignVCenter);
        strip->addStretch(1);
        // 清空 / 复制 / 两种导出是同一类小动作：收进标题条右边那颗「…」（在三颗窗口按钮左边；
        // 窗口本身有关闭钮，关掉走 close()，WA_DeleteOnClose 与 finished() 照旧）。
        m_moreButton = new QPushButton(m_titleStrip);
        m_moreButton->setObjectName(moreButtonName());
        m_moreButton->setToolTip(tr("更多操作"));
        m_moreButton->setIcon(VersionIcons::line(QStringLiteral("dots"), 16, Theme::tokens().secondary));
        m_moreButton->setCursor(Qt::PointingHandCursor);
        Theme::markButton(m_moreButton, Theme::ButtonKind::Ghost);
        auto *menu = new QMenu(m_moreButton);
        m_copyAction = menu->addAction(tr("复制全部"));
        m_exportLogAction = menu->addAction(exportLogButtonText());
        m_exportLogAction->setToolTip(exportLogButtonTooltip());
        m_exportReportAction = menu->addAction(exportReportButtonText());
        m_exportReportAction->setToolTip(exportReportButtonTooltip());
        m_externalAction = menu->addAction(tr("用外部程序打开"));
        m_externalAction->setVisible(false);  // 只有文件查看器用得上，见 beginFile()
        menu->setToolTipsVisible(true);
        menu->addSeparator();
        m_clearAction = menu->addAction(clearButtonText());
        m_clearAction->setToolTip(tr("只清这个窗口里看到的内容，日志文件不受影响。"));
        connect(m_moreButton, &QPushButton::clicked, this, [this, menu]() {
            menu->popup(m_moreButton->mapToGlobal(QPoint(m_moreButton->width() - menu->sizeHint().width(),
                                                         m_moreButton->height() + 4)));
        });
        strip->addWidget(m_moreButton, 0, Qt::AlignVCenter);
        strip->addSpacing(4);
        auto *minimize = new TitleBarButton(TitleBarButton::Glyph::Minimize, QColor(), QColor(), m_titleStrip);
        minimize->setObjectName(QStringLiteral("gameLogMinimize"));
        minimize->setToolTip(tr("最小化"));
        auto *maximize = new TitleBarButton(TitleBarButton::Glyph::Maximize, QColor(), QColor(), m_titleStrip);
        maximize->setObjectName(QStringLiteral("gameLogMaximize"));
        maximize->setToolTip(tr("最大化"));
        auto *close = new TitleBarButton(TitleBarButton::Glyph::Close, QColor(), QColor(), m_titleStrip);
        close->setObjectName(QStringLiteral("gameLogClose"));
        close->setToolTip(tr("关闭"));
        for (QAbstractButton *button : {static_cast<QAbstractButton *>(minimize),
                                        static_cast<QAbstractButton *>(maximize),
                                        static_cast<QAbstractButton *>(close)})
            strip->addWidget(button, 0, Qt::AlignTop);
        m_maximizeButton = maximize;
        connect(minimize, &QAbstractButton::clicked, this, &QWidget::showMinimized);
        connect(maximize, &QAbstractButton::clicked, this, [this] {
            isMaximized() ? showNormal() : showMaximized();
        });
        connect(close, &QAbstractButton::clicked, this, &QWidget::close);
        TitleBar::styleCaptionButtons(minimize, maximize, close);
        Theme::follow(m_titleStrip, [minimize, maximize, close] {
            TitleBar::styleCaptionButtons(minimize, maximize, close);
        });
    }
    root->addWidget(m_titleStrip);
    m_chrome = new WindowChrome(this, m_titleStrip, this);
    setStateBadge(MainWindowUI::runStateRunningText(), false);

    auto *layout = new QVBoxLayout;
    layout->setContentsMargins(28, 6, 28, 16);
    layout->setSpacing(10);
    root->addLayout(layout, 1);

    m_filterButtons = QVector<QToolButton *>(levelCount(), nullptr);
    // 模型那几张表（计数 / 可见）在 `RuntimeOutputModel` 的构造函数里已经摆好了；这一窗
    // 只需要把容量对上 `maxLogBlocks()`（两者现在是同一个出处）。

    // ---------- 顶上那排：五档开关 + 「显示全部」 ----------
    // 摆在**最顶上**（说明文字之上）：它是"我现在想看哪几档"，先决定这个再看内容。
    auto *filters = new QHBoxLayout;
    filters->setSpacing(6);
    for (int i = 0; i < levelCount(); ++i) {
        const LogLevel level = levelAt(i);
        auto *button = new QToolButton(this);
        button->setObjectName(filterButtonName(level));
        button->setText(filterButtonText(level, 0));
        // 勾上 = 显示这一档。开局五档全勾着（用户第一次打开该看到的是全部）。
        button->setCheckable(true);
        button->setChecked(true);
        // 只写字，不画图标、不摆箭头 —— 一排小开关，占了宽度就是挤正文。
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        // 数字是什么、点一下会怎样，写在这里而不是写进那行说明：
        // 说明那一行是给"这个窗是干嘛的"用的，计数这件事只在鼠标停在按钮上时才需要。
        button->setToolTip(filterButtonTooltip());
        // 勾上的那颗**染成这一档自己的底色** —— 和正文里那一行**同一个色**：这样"哪几档
        // 开着、哪几档是什么色"不用读字，一眼就看出来（开关和"这一档的颜色"合成了一件事）。
        // 关掉之后回到那档灰，也就是这颗按钮不显眼时的样子。
        //
        // 五颗都走同一份样式（**含 INFO 那颗**）：两种状态都由我们自己画，"没开"那个灰也就
        // 由同一份样式给 —— 单独留一颗走原生的话，它那圈立体感会和旁边四颗不一样。
        // INFO 那颗的"开"色是**正文区自己的底色**（它没有颜色可借，见 filterButtonBackground）。
        // 样式**不在这里算**：`refreshFilterButtonStyles()` 一处算五颗（主题一变还得重来
        // 一遍，见 changeEvent()）—— 两份的话迟早有一份忘了跟着调色板走，而忘了的表现
        // 正是"深色主题下这几颗还是浅色的"。
        connect(button, &QToolButton::toggled, this, [this, level](bool on) {
            m_model.setLevelVisible(modelLevel(level), on);
            rebuildView();
        });
        m_filterButtons[i] = button;
        filters->addWidget(button);
    }
    // 五颗的样式一次算出来（调色板这一刻已经定了：控件是从应用那儿继承来的）。
    refreshFilterButtonStyles();
    filters->addStretch(1);

    // 右边这两件：**先摆下拉框，再摆清空**（"把清除放到这个的旁边"）—— 于是清空在最右、
    // 下拉框紧贴着它左边那一格。
    //
    // 这一颗是 从「显示全部」按钮换过来的：那一颗的位置给行数上限，清空从底下
    // 那一排挪上来。换掉的代价（五档全关之后没有一键全勾回来）写在 .hpp 里。
    // ---------- 搜索那一行 ----------
    //
    // 和档位筛选**同时**生效（`RuntimeOutputModel::matches()` 里是交集）：用户既可能
    // "只看 ERROR"，也可能"在全部里找某一串"。搜索只影响**视图**，模型留着全部（导出的
    // 那两份仍然走 `allLogText()` / `logTextFrom()`，不会被搜索框吃掉）。
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName(searchEditName());
    m_searchEdit->setPlaceholderText(searchPlaceholderText());
    m_searchEdit->setToolTip(searchTooltip());
    m_searchEdit->setClearButtonEnabled(true);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_model.setSearchText(text);
        rebuildView();
    });
    m_searchEdit->setMinimumWidth(240);
    m_searchEdit->setMaximumWidth(300);

    m_lineLimitCombo = new QComboBox(this);
    m_lineLimitCombo->setObjectName(lineLimitComboName());
    for (int limit : lineLimitChoices()) {
        m_lineLimitCombo->addItem(lineLimitText(limit), limit);
    }
    m_lineLimitCombo->setToolTip(lineLimitTooltip());
    // 和主页那两个下拉框同一套尺寸规矩（见 MainWindowUI.cpp 里 installedVersionCombo 那段）：
    // 按内容算宽度，灌数据/换档时不会忽然跳一下。这里条目最长的是「10000 条」，
    // 12 是那边用的数，这里也够。
    m_lineLimitCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_lineLimitCombo->setMinimumContentsLength(12);
    // 滚轮防护（见 MainWindowUI.hpp 里的 WheelGuard）：**必须装**，而且理由在这个窗里比
    // 别处更要紧 —— 这是一扇"滚着看日志"的窗，鼠标从那条下拉框上划过的机会多得是，
    // 不装的话往上翻日志时会被静默换成"只显示 500 条"，而内容一少看起来像日志丢了。
    WheelGuard::install(m_lineLimitCombo);
    // 摆到**它的**那一档（默认「全部」）。倒序找第一个数据等于默认值的：产品里「全部」
    // 排在最后，但这个循环不依赖顺序。
    for (int i = 0; i < m_lineLimitCombo->count(); ++i) {
        if (m_lineLimitCombo->itemData(i).toInt() == m_model.displayLimit()) {
            m_lineLimitCombo->setCurrentIndex(i);
            break;
        }
    }
    connect(m_lineLimitCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        // 换一档 = 换视图那一层的闸，然后重画（模型一行不动，见 .hpp 里那段）。
        setDisplayLineLimit(lineLimitOfIndex(index));
    });
    filters->addWidget(m_searchEdit);
    filters->addWidget(m_lineLimitCombo);


    layout->addLayout(filters);

    m_baseHint = hintText();
    m_hintLabel = new QLabel(m_baseHint, this);
    m_hintLabel->setObjectName(hintLabelName());
    m_hintLabel->setWordWrap(true);
    {
        // 和设置页里那些说明文字同一套写法：小一号、灰色。
        QFont hintFont = m_hintLabel->font();
        hintFont = Theme::textFont(hintFont, Theme::TextRole::Caption);
        m_hintLabel->setFont(hintFont);
        // 灰的那一支取**这一窗自己的**（窗的调色板把 `PlaceholderText` 摆成了 `logHint`）。
        // ★ 两个角色**一起写**：`QLabel` 不吃 `PlaceholderText`，要把它抄进 `WindowText`；
        // 而只写 `WindowText` 的话，这份局部调色板里没标的 `PlaceholderText` 装上之后会
        // 退回应用那一支 —— 两个角色从此各说各的（量出来 `#64776d` 与 `#607c70`）。
        QPalette hintPalette = m_hintLabel->palette();
        const QColor hintInk = logPalette.color(QPalette::PlaceholderText);
        hintPalette.setColor(QPalette::PlaceholderText, hintInk);
        hintPalette.setColor(QPalette::WindowText, hintInk);
        m_hintLabel->setPalette(hintPalette);
    }
    // 说明那一行挂在标题下面（第 1 格），筛选那一排在它下面。
    layout->insertWidget(1, m_hintLabel);

    m_view = new LogView(this);
    // 视口有自己的调色板；只设置外层对话框会在 QSS 抛光后留下输入框底色。
    m_view->setPalette(logPalette);
    m_view->viewport()->setPalette(logPalette);
    // 正文半透明、不描边：底下那层磨砂（paintEvent）从字缝里透上来，和主窗同一张底。
    {
        const QColor base = logTokens.logBase;
        m_view->setStyleSheet(
            QStringLiteral("QPlainTextEdit { background-color: rgba(%1, %2, %3, %4); color: %5;"
                           " border: none; border-radius: 10px; }")
                .arg(base.red()).arg(base.green()).arg(base.blue())
                .arg(Theme::isDark() ? 115 : 140)  // QSS 的 alpha 是 0–255 整数，写小数会被读成 0
                .arg(logPalette.color(QPalette::Text).name()));
    }
    m_view->setObjectName(logViewName());
    // 只读：这是"看输出"的窗，不是输入框。只读之后光标不闪、也打不进字，但它**仍然可以
    // 选中复制**（那正是用户拿报错去搜的时候要干的）。
    m_view->setReadOnly(true);
    // 行数封顶（理由见 .hpp）。用 setMaximumBlockCount 而不是自己数行：它就是这个用途，
    // 而且丢弃是最老的那些，不需要重排整块文本。
    //
    // NOTE: **比模型多一格**。多出来的那一格是"还没等到换行的那半行"（它不在模型里，
    // 见 insertPendingIntoView）。两个上限必须对齐，不然跑满两万行之后模型里最早
    // 那一行会被视图悄悄丢掉、而模型还留着 —— 切一次筛选它就又冒出来了。
    m_view->setMaximumBlockCount(viewBlockLimit());
    m_view->setPlaceholderText(emptyPlaceholderText());
    // 等宽字体：日志里有缩进、有对齐的表格（崩栈、模组列表），比例字体读起来是散的。
    // 字号比系统那个大一号（稍微大一点），见 logFont()。
    m_view->setFont(logFont());
    // 长行不折行：MC 的日志里有很长的路径和 JSON，折了反而更难认；横向滚动条由它自己出。
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_view, 1);

    connect(m_exportLogAction, &QAction::triggered, this, &GameLogWindow::exportLogRequested);
    connect(m_exportReportAction, &QAction::triggered, this, &GameLogWindow::exportReportRequested);
    connect(m_externalAction, &QAction::triggered, this,
            [this] { emit openExternalRequested(m_filePath); });
    connect(m_copyAction, &QAction::triggered, this,
            [this]() { QGuiApplication::clipboard()->setText(allLogText()); });

    // 「清空」清的是**这个窗看到的东西**：模型、计数、那半行一起清（视图只是模型的投影，
    // 只清视图的话按钮上会留着「ERROR 12」而底下一行都没有 —— 看着像坏的）。游戏进程一个
    // 都不动：这仍然只是个视图，和"关掉这个窗不会停游戏"是同一件事。
    // **筛选的勾不动**：那是"我现在想看哪几档"的偏好，不是日志内容。
    connect(m_clearAction, &QAction::triggered, this, [this]() {
        m_model.clear();
        m_pending.clear();
        m_view->clear();
        m_viewLastBlockIsPending = false;
        updateFilterButtons();
        refreshPlaceholder();
    });
}

// ---------------------------------------------------------------------------
// 数据进来
// ---------------------------------------------------------------------------

void GameLogWindow::appendText(const QString &text) {
    if (text.isEmpty()) {
        return;
    }
    QScrollBar *bar = m_view->verticalScrollBar();
    // 先判"现在在不在底下"，再插字 —— 插完 scrollbar 的 maximum 就变了，那时候再判
    // 一定是"不在底下"。
    const bool followTail = bar->value() >= bar->maximum() - kFollowTailSlack;

    // 末尾那半行先撤掉：接下来要么它被补全（撤了再按级别摆一遍），要么后面还有别的行
    // 要摆 —— 两种情况都不能把那半行留在中间。
    m_pending += text;
    dropPendingFromView();

    // 这一块里收完的行按档攒成段，最后（或换档时）一次写出去 —— 理由见 insertRun：
    // 游戏一次性吐两千行时，一分块一次和一行一次差着一个数量级。
    QStringList run;
    LogLevel runLevel = LogLevel::Info;
    const auto flushRun = [this, &run, &runLevel]() {
        if (!run.isEmpty()) {
            insertRun(runLevel, run);
            run.clear();
        }
    };

    int cut = m_pending.indexOf(QLatin1Char('\n'));
    while (cut >= 0) {
        QString line = m_pending.left(cut);
        m_pending.remove(0, cut + 1);
        // CRLF：游戏在 Windows 上多半是 \r\n，那个 \r 是行尾符不是内容（留着的话
        // 复制出来每行末尾都躲一个回车，粘到别处就多一行）。
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        const LogLevel level = rememberLine(line);
        // 这一档被筛掉的话不摆出来 —— 但**照样进了模型、照样计数**（数字不会因为
        // 用户关掉某一档就变小，见 .hpp 里那段）。筛掉的这几行不进段，但**也不必**
        // 把段切断：它们不摆出来，段里剩下的行在视图里本来就是挨着的。
        if (m_model.matches(modelLevel(level), line)) {
            if (!run.isEmpty() && level != runLevel) {
                flushRun();
            }
            runLevel = level;
            run.append(line);
        }
        cut = m_pending.indexOf(QLatin1Char('\n'));
    }

    // 攒到一定量还没等到换行就当成一整条收下。**这条闸是必需的**：用 \r 刷进度条的模组
    // 会一直不给换行，不封顶的话 m_pending 就随着输出无限长，一个"看日志的窗"能把内存吃光
    // （模型那个 maxLogBlocks() 管不到它 —— 它还没成一行）。
    if (m_pending.size() >= maxPendingChars()) {
        const QString flushed = m_pending;
        m_pending.clear();
        const LogLevel level = rememberLine(flushed);
        if (m_model.matches(modelLevel(level), flushed)) {
            if (!run.isEmpty() && level != runLevel) {
                flushRun();
            }
            runLevel = level;
            run.append(flushed);
        }
    }
    flushRun();
    if (!m_pending.isEmpty()) {
        insertPendingIntoView();
    }
    refreshPlaceholder();
    if (followTail) {
        bar->setValue(bar->maximum());
    }
}

GameLogWindow::LogLevel GameLogWindow::rememberLine(const QString &text) {
    // 记账那一半整个在共用模型里：认级别、算计数、超上限丢最老的，都是
    // `RuntimeOutputModel::addLine()`。这里只负责把新级别回给调用方（摆视图时要按档分段）。
    const LogLevel level = static_cast<LogLevel>(int(m_model.addLine(text)));
    updateFilterButtons();
    return level;
}

void GameLogWindow::insertRun(LogLevel level, const QStringList &lines) {
    if (lines.isEmpty()) {
        return;
    }
    // 【为什么是"一段一次"，不是"一行一次"】
    QString text = lines.join(QLatin1Char('\n'));
    text += QLatin1Char('\n');  // 末尾这一下把"光标行"那一格也造出来（见下）

    QTextCursor cursor(m_view->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    const int endPos = cursor.position();

    // 这一段的起点**从"插完的位置"往回数**（`endPos - text.size()`），
    // 不能用插入**之前**记下的那个整数。
    //
    // NOTE: 往回数的长度就是 `text.size()`（`insertText` 插的是纯文本，一个字符不多不少），
    // 所以不裁的时候这一句和插入前那个整数**是同一个值**，正常路上零区别。
    const int startPos = endPos - text.size();
    if (startPos >= 0 && endPos > startPos) {
        // 底色和墨色各一次盖过这一整段。选区从这段的第一个字到**最后一个换行之前** ——
        // 末尾那一格空的（光标行）不在选区里，它得留着不带底色。
        //
        // NOTE: 外面那个 if 是"这一整段**全被裁掉了**"的兜底（一段比整个行数上限还长时
        // 才会走到）：那种情况下起点是个负数，不去碰句子里的位置（`setPosition` 拿到
        // 越界值会把光标弄到未定义处，之后两行 setFormat 打在不知道什么地方上）。
        QTextCursor span(m_view->document());
        span.setPosition(startPos);
        span.setPosition(endPos - 1, QTextCursor::KeepAnchor);
        span.setBlockFormat(blockFormatFor(level, m_view->palette(), m_logColors));
        span.setCharFormat(charFormatFor(level, m_view->palette(), m_logColors));
    }

    // 收尾那一格（光标行）**显式清掉格式**：它是上面那个换行分出来的新块，会把这一档的
    // 底色和墨色继承下去 —— 不清的话"还没写的空行"底下拖着一条彩色的尾巴（Fatal 那一档
    // 还会把白字也带下去，之后写的字都看不见）。
    QTextCursor tail(m_view->document());
    tail.movePosition(QTextCursor::End);
    tail.setBlockFormat(QTextBlockFormat());
    tail.setCharFormat(QTextCharFormat());
}

GameLogWindow::LogLevel GameLogWindow::pendingLevelGuess() const {
    // 跟"上一行那一档"猜 —— 认不出级别的行本来就跟上一行走（见 classifyLine），半行是
    // 一条还没写完的行，多数时候它写完就是上一档的续行。真级别已经写在头里了（进程
    // 按块给输出，多半整行一起到）时，这个函数认出来的就是真级别。
    return classifyLine(m_pending, static_cast<LogLevel>(int(m_model.lastLevel())));
}

void GameLogWindow::insertPendingIntoView() {
    if (m_pending.isEmpty()) {
        return;
    }
    QTextCursor cursor(m_view->document());
    cursor.movePosition(QTextCursor::End);
    // 半行**按猜的那一档染**。
    //
    // 原因是"日志框的最后一行"站着的**就是**这半行：进程输出按块到，末尾那一条往往还没
    // 等到换行（游戏崩了的那一行尤其如此，它可能永远等不到）。不染的话，用户盯着看的
    // 那一行正好永远是白的。
    //
    // 猜错的代价很小：换行到了会按真级别重摆一遍（rememberLine + insertRun），颜色当场
    // 改对。且"猜"不是乱猜 —— 就是 classifyLine 在认不出级别时本来会用的那个 fallback。
    const LogLevel guess = pendingLevelGuess();
    cursor.setBlockFormat(blockFormatFor(guess, m_view->palette(), m_logColors));
    cursor.setCharFormat(charFormatFor(guess, m_view->palette(), m_logColors));
    cursor.insertText(m_pending);
    m_viewLastBlockIsPending = true;
}

void GameLogWindow::dropPendingFromView() {
    if (!m_viewLastBlockIsPending) {
        return;
    }
    m_viewLastBlockIsPending = false;
    // 只撤**那一格里的字**，不撤那一格本身 —— 末尾永远留一格空的当光标行，
    // 这样"摆一行"永远是同一个动作（写进末尾那一格，再起一格新的）。
    QTextCursor cursor(m_view->document());
    cursor.movePosition(QTextCursor::End);
    cursor.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    // 那一格上的格式跟着清掉：猜的那份底色不能赖在空行上（它又要变回"光标行"了）。
    cursor.setBlockFormat(QTextBlockFormat());
    cursor.setCharFormat(QTextCharFormat());
}

// ---------------------------------------------------------------------------
// 重画与筛选
// ---------------------------------------------------------------------------

void GameLogWindow::rebuildView() {
    QScrollBar *bar = m_view->verticalScrollBar();
    // 重画之前的卷动位置：本来在底下就继续跟到底；翻着看中间的话，尽量留在原处
    // （内容变了位置本来就没法完全对上，钳到新范围里就行）。
    const bool followTail = bar->value() >= bar->maximum() - kFollowTailSlack;
    const int kept = bar->value();

    m_view->clear();
    m_viewLastBlockIsPending = false;
    ++m_batchGeneration;  // 上一轮还没摆完的分批作废
    m_batchLines.clear();
    m_batchPos = 0;
    QVector<RuntimeOutputModel::VisibleLine> visible = m_model.visibleLines();
    if (isFileViewer() && visible.size() > fileBatchLines()) {
        // 几万行的文件一次摆完要卡住界面：分批，每批之间让出事件循环。
        // 实时日志窗不走这条（那条路上行数是一点点涨起来的，不会一次摆很多）。
        startBatch(std::move(visible));
        return;
    }
    // 连续同一档的行攒成一段，一段一次写出去（理由见 insertRun）—— 重画一次的代价
    // 从"行数"降到"档数变化次数"，这正是"点一下筛选要等半秒"那条的解法。
    QStringList run;
    LogLevel runLevel = LogLevel::Info;
    for (const RuntimeOutputModel::VisibleLine &line : visible) {
        const LogLevel level = static_cast<LogLevel>(int(line.level));
        if (!run.isEmpty() && level != runLevel) {
            insertRun(runLevel, run);
            run.clear();
        }
        runLevel = level;
        run.append(line.text);
    }
    insertRun(runLevel, run);  // run 空的话它自己会早退
    insertPendingIntoView();
    refreshPlaceholder();
    if (followTail) {
        bar->setValue(bar->maximum());
    } else {
        bar->setValue(qMin(kept, bar->maximum()));
    }
}

void GameLogWindow::startBatch(QVector<RuntimeOutputModel::VisibleLine> lines) {
    m_batchLines = std::move(lines);
    m_batchPos = 0;
    pumpBatch();
}

void GameLogWindow::pumpBatch() {
    if (m_batchPos >= m_batchLines.size()) return;
    QScrollBar *bar = m_view->verticalScrollBar();
    const bool followTail = bar->value() >= bar->maximum() - kFollowTailSlack;
    const int end = qMin(int(m_batchLines.size()), m_batchPos + fileBatchLines());
    QStringList run;
    LogLevel runLevel = LogLevel::Info;
    for (int i = m_batchPos; i < end; ++i) {
        const RuntimeOutputModel::VisibleLine &line = m_batchLines.at(i);
        const LogLevel level = static_cast<LogLevel>(int(line.level));
        if (!run.isEmpty() && level != runLevel) {
            insertRun(runLevel, run);
            run.clear();
        }
        runLevel = level;
        run.append(line.text);
    }
    insertRun(runLevel, run);
    m_batchPos = end;
    if (followTail) bar->setValue(bar->maximum());
    if (m_batchPos < m_batchLines.size()) {
        const int generation = m_batchGeneration;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation == m_batchGeneration) pumpBatch();
        });
    } else {
        m_batchLines.clear();
        m_batchPos = 0;
        refreshPlaceholder();
    }
}

void GameLogWindow::beginFile(const QString &path) {
    m_filePath = path;
    const QString name = QFileInfo(path).fileName();
    setWindowTitle(fileWindowTitleText(name));
    m_titleLabel->setText(tr("查看日志"));
    m_titleInstance->setText(name);
    m_titleInstance->setToolTip(QDir::toNativeSeparators(path));
    m_titleInstance->setVisible(true);
    m_stateBadge->setVisible(false);
    m_externalAction->setVisible(true);
    m_exportReportAction->setVisible(false);  // 报告是按游戏会话出的，文件没有会话
    m_clearAction->setVisible(false);
    m_baseHint = tr("正在读取…");
    updateHintLabel();
    m_view->setPlaceholderText(tr("正在读取…"));
}

void GameLogWindow::setFileLines(const QStringList &lines, qint64 cutHeadBytes) {
    m_model.clear();
    m_pending.clear();
    m_model.setCapacity(qMax(RuntimeOutputModel::defaultCapacity(), int(lines.size())));
    for (const QString &line : lines) m_model.addLine(line);
    m_baseHint = fileHintText(cutHeadBytes, int(lines.size()) > fileTailLines());
    m_view->setPlaceholderText(emptyPlaceholderText());
    updateFilterButtons();
    // 超长日志默认只显示末尾；模型留着全部，切到「全部」时当场回来。
    const int limit = lines.size() > fileTailLines() ? fileTailLines() : 0;
    const bool unchanged = limit == m_model.displayLimit();
    setDisplayLineLimit(limit);
    if (unchanged) {
        m_view->setMaximumBlockCount(viewBlockLimit());
        rebuildView();
    }
}

void GameLogWindow::setFileProblem(const QString &problem) {
    m_model.clear();
    m_pending.clear();
    m_view->clear();
    m_baseHint = tr("无法显示这份文件。");
    updateFilterButtons();
    m_view->setPlaceholderText(problem);
}

int GameLogWindow::viewBlockLimit() const {
    // 「全部」那一档喂的就是模型那个上限。两条上限对齐是不变量
    // （见构造函数里那段）：不对齐的话，模型里最早那一行会被视图悄悄丢掉、切一次筛选它
    // 又冒出来。
    const int shown = m_model.displayLimit();
    const int limit = shown <= 0 ? m_model.capacity() : qMin(shown, m_model.capacity());
    return limit + 1;  // 多出来那一格留给"还没等到换行的那半行"
}

void GameLogWindow::setDisplayLineLimit(int limit) {
    if (limit == m_model.displayLimit()) {
        return;
    }
    m_model.setDisplayLimit(limit);
    if (m_lineLimitCombo != nullptr) {
        // 灌值不要把处理函数再绕回来（那只会在重建里再重建一次）。
        const QSignalBlocker blocker(m_lineLimitCombo);
        for (int i = 0; i < m_lineLimitCombo->count(); ++i) {
            if (m_lineLimitCombo->itemData(i).toInt() == limit) {
                m_lineLimitCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    // 顺序要紧：**先**改上限、再重建。上限调大时 Qt 不会自己补回什么（得靠重建从模型里
    // 再摆一遍），调小时它自己就砍掉开头那一截 —— 两种都由这一次重建兜住。
    m_view->setMaximumBlockCount(viewBlockLimit());
    rebuildView();
}

void GameLogWindow::setLevelVisible(LogLevel level, bool visible) {
    const int index = levelIndex(level);
    if (m_model.levelVisible(modelLevel(level)) == visible) {
        return;
    }
    m_model.setLevelVisible(modelLevel(level), visible);
    if (QToolButton *button = m_filterButtons.at(index)) {
        const QSignalBlocker blocker(button);
        button->setChecked(visible);
    }
    rebuildView();
}

bool GameLogWindow::levelVisible(LogLevel level) const {
    return m_model.levelVisible(modelLevel(level));
}

int GameLogWindow::lineCountFor(LogLevel level) const {
    return m_model.countFor(modelLevel(level));
}

int GameLogWindow::droppedLineCountFor(LogLevel level) const {
    return m_model.droppedLineCountFrom(modelLevel(level));
}

void GameLogWindow::updateFilterButtons() {
    for (int i = 0; i < levelCount(); ++i) {
        if (QToolButton *button = m_filterButtons.at(i)) {
            button->setText(filterButtonText(levelAt(i), m_model.countFor(modelLevel(levelAt(i)))));
        }
    }
    updateHintLabel();
}

void GameLogWindow::updateHintLabel() {
    if (m_hintLabel == nullptr) {
        return;  // 构造期还没摆到这一格（updateFilterButtons 只在收行/清空时来）
    }
    QString text = m_baseHint;
    const QString dropped = droppedHintText(m_model.droppedLineCount(), m_model.capacity());
    if (!dropped.isEmpty()) {
        text += QLatin1Char(' ') + dropped;
    }
    // 每收一行都会走到这里：只在真的变了的时候 setText，不然行数一直在涨的那些
    // 实例会把整行字反复重排（`QLabel::setText` 会触发布局）。
    if (m_hintLabel->text() != text) {
        m_hintLabel->setText(text);
    }
}

void GameLogWindow::refreshFilterButtonStyles() {
    // 这一窗的颜色**不跟着调色板自己走**（开关和分隔线都是我们自己画的），所以每换一次
    // 主题都得重算一遍 —— **一处算五颗**，别再在别处写第二份（见构造函数里那条 NOTE）。
    const QPalette &palette = this->palette();
    for (int i = 0; i < levelCount(); ++i) {
        QToolButton *button = m_filterButtons.at(i);
        if (button == nullptr) {
            continue;
        }
        const LogLevel level = levelAt(i);
        button->setStyleSheet(
            filterButtonStyle(filterButtonBackground(level, /*on=*/true, palette),
                              filterButtonForeground(level, palette),
                              switchOffColor(palette),
                              // 关着那一颗的字用的是**中性墨**，不是"这一档的墨"：
                              // Fatal 那一档开着时是白字，而关着时它那颗底是中灰，
                              // 白字压上去就成了"看不见的按钮"。
                              palette.color(QPalette::WindowText)));
    }
    // 记下这一趟用的是哪一档中性灰：`changeEvent()` 拿它当"真的换了主题没有"的判据
    // （这两个事件来得比主题变化勤得多）。
    m_lastSwitchOff = switchOffColor(palette);
}

void GameLogWindow::setExited(int exitCode, bool crashed) {
    setStateBadge(crashed ? tr("游戏已崩溃退出，退出码：%1").arg(exitCode)
                          : tr("游戏已退出，退出码：%1").arg(exitCode),
                  crashed || exitCode != 0);
}

void GameLogWindow::setStateBadge(const QString &text, bool bad) {
    m_stateBadge->setText(text);
    Theme::markBadge(m_stateBadge, bad ? Theme::BadgeTone::Danger
                                   : text == MainWindowUI::runStateRunningText() ? Theme::BadgeTone::Ok
                                                                                 : Theme::BadgeTone::Neutral);
}

void GameLogWindow::paintEvent(QPaintEvent *event) {
    // 独立顶层窗：借主窗那一张磨砂风景铺满（没有主窗时退回调色板的窗口色）。
    if (auto *backdrop = Glass::Backdrop::active()) {
        QPainter painter(this);
        backdrop->paintScene(painter, rect());
        return;
    }
    // WindowChrome 在真 Windows 上开了半透明（为了系统背板）：没有磨砂底时自己铺窗口色，别透成空的。
    QPainter(this).fillRect(rect(), palette().color(QPalette::Window));
    QDialog::paintEvent(event);
}

bool GameLogWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    if (m_chrome != nullptr && m_chrome->handleNativeEvent(message, result)) return true;
    return QDialog::nativeEvent(eventType, message, result);
}

void GameLogWindow::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        if (auto *button = qobject_cast<TitleBarButton *>(m_maximizeButton)) {
            button->setRestoreGlyph(isMaximized());
            button->setToolTip(isMaximized() ? tr("还原") : tr("最大化"));
        }
        return;
    }
    if (event->type() != QEvent::PaletteChange && event->type() != QEvent::StyleChange) {
        return;
    }
    // 来源：构造函数里那一发 `setWindowFlags()`（要加最大化/最小化两颗钮，见那一段的
    // NOTE）会走到 `QWidgetPrivate::setParent()` → `inheritStyle()`。而**应用级样式表
    // 在这一趟跑之前被换过**（换主题 / 启动时铺首那一份）时，这一发就在新控件**刚建出来
    // 还没摆完**的时候送一个 StyleChange 进来 —— 那一刻 `m_filterButtons` 还是空向量、
    // `m_view` 还是 nullptr，`refreshFilterButtonStyles()` 里那句 `.at(i)` 读到界外、
    // `rebuildView()` 里那句 `m_view->verticalScrollBar()` 直接空指针。
    //
    // 为什么平时看不见：**应用级样式表这一趟没换过**的时候 `inheritStyle()` 算出"继承的
    // 样式和手上这个是同一个"，Qt 压根不发这一个事件（浅色那一趟一直是这么过来的 ——
    // 所以这一条只在"主题是运行时换的 / 换了之后再开这个窗"这一条路上才露头）。
    //
    // 这一趟**没有任何东西要重算**：一个字都还没往模型里装，构造收尾自己会调一次
    // `refreshFilterButtonStyles()`。所以直接早退（判据取 `m_view` —— 它在构造里是
    // **最后一个**建出来的，它在了就意味着上面那几件都在了）。
    if (m_view == nullptr) {
        return;
    }
    // 先比一遍算出来的色：这两个事件会因为各种原因连着来（挂样式表、换调色板、
    // 父窗重抛光……），每次都重建一遍视图（最多两万行）是白跑。
    const QColor off = switchOffColor(palette());
    if (off == m_lastSwitchOff) {
        return;
    }
    refreshFilterButtonStyles();  // 它顺手把 `m_lastSwitchOff` 记成新值
    // 正文那几档的底色是**块格式**（烘在文档里的），换主题得整块重建才换得掉；
    // 行间那条线是按正文区自己的调色板现问的，重建时跟着一起重画。
    rebuildView();
}

void GameLogWindow::refreshPlaceholder() {
    // 三句话**各说各的下一步**：
    // · 一行都没有 → 「还没有输出」（下一步是等游戏出话）；
    // · 有行、但档位筛得一行不剩 → 「都被筛掉了」（下一步是勾回来）；
    // · 有行、档位没筛干净但搜索没匹配上 → 「没有搜到」（下一步是清掉搜索词）。
    // 做成一句的话，用户看到空窗只会以为日志窗坏了。
    //
    // 判据是"**现在一行都没有显示**"，不是"模型是不是空的"：还有行、只是全被筛掉的
    // 时候，那句占位该说的是"这几档都被筛掉了"。反过来只要有行看得见，正文就占着地方，
    // 占位那句话根本不显形 —— 这时给"还没有输出"（那是"这一份游戏还没说过话"的意思，
    // 而它显然说过了）。
    const int matching = m_model.matchingLineCount();
    QString want = emptyPlaceholderText();
    if (matching == 0 && !m_model.lines().isEmpty()) {
        want = m_model.searchText().isEmpty() ? filteredOutPlaceholderText()
                                              : searchEmptyPlaceholderText();
    }
    if (m_view->placeholderText() != want) {
        m_view->setPlaceholderText(want);
    }
}

// ---------------------------------------------------------------------------
// 认级别
// ---------------------------------------------------------------------------

GameLogWindow::LogLevel GameLogWindow::classifyLine(const QString &line, LogLevel fallback) {
    // 规则只有一处实现（共用模型那一份，服务端控制台也走它）。这里保留同名转发。
    return static_cast<LogLevel>(
        int(RuntimeOutputModel::classifyLine(line, modelLevel(fallback))));
}

GameLogWindow::LogLevel GameLogWindow::levelAt(int index) {
    if (index < 0 || index >= levelCount()) {
        return LogLevel::Info;
    }
    return static_cast<LogLevel>(index);
}

QString GameLogWindow::levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return QStringLiteral("DEBUG");
        case LogLevel::Info:
            return QStringLiteral("INFO");
        case LogLevel::Warn:
            return QStringLiteral("WARN");
        case LogLevel::Error:
            return QStringLiteral("ERROR");
        case LogLevel::Fatal:
            return QStringLiteral("FATAL");
    }
    return QStringLiteral("INFO");
}

QColor GameLogWindow::levelBackground(LogLevel level) {
    // 不带调色板那一版问的是**应用调色板**（验证工程和几个自由函数用的就是它）。
    return levelBackground(level, QApplication::palette());
}

QColor GameLogWindow::levelBackground(LogLevel level, const QPalette &palette) {
    // 走哪一套底由 `Theme::isDarkPalette()` 说了算 —— **唯一判据**，和别处"深底上用亮的
    // 那一档"是同一个（别看"主题叫什么"，那会把判据散成两处）。
    const bool dark = Theme::isDarkPalette(palette);
    if (dark) return QColor("#171E23");
    switch (level) {
        case LogLevel::Debug:
            return QColor();
        case LogLevel::Info:
            // 不染（用窗口自己的底色）。日志里绝大多数是 INFO，全染上就成了一张色纸，
            // Warn/Error 反而不显眼了；"没染色的那些"本身就是最自然的一档。
            return QColor();
        case LogLevel::Warn:
            return levelTint(kWarnBg, kWarnBgDark, dark);
        case LogLevel::Error:
            return levelTint(kErrorBg, kErrorBgDark, dark);
        case LogLevel::Fatal:
            return levelTint(kFatalBg, kFatalBgDark, dark);
    }
    return QColor();
}

QColor GameLogWindow::levelForeground(LogLevel level) {
    return logInk(level, QApplication::palette());
}

QColor GameLogWindow::filterButtonBackground(LogLevel level, bool on) {
    return filterButtonBackground(level, on, QApplication::palette());
}

QColor GameLogWindow::filterButtonBackground(LogLevel level, bool on, const QPalette &palette) {
    if (!on) {
        return switchOffColor(palette);
    }
    // 开着那一格：有底色的档就用**正文那个色**；Info 没有（它返回无效色），用**正文区
    // 自己的底色**（浅色下是白、深色下是那个深底）—— 那一档"没有颜色"这件事本身就是它。
    // 最底下那个兜底灰只留给"哪天再多一档没有底色的"用（今天一档都走不到）。
    const QColor tint = levelBackground(level, palette);
    if (tint.isValid()) {
        return tint;
    }
    if (level == LogLevel::Info) {
        return palette.color(QPalette::Base);
    }
    return Theme::neutralFill(palette.color(QPalette::Window),
                              palette.color(QPalette::WindowText), kSwitchOnPercent);
}

QColor GameLogWindow::filterButtonForeground(LogLevel level) {
    return filterButtonForeground(level, QApplication::palette());
}

QColor GameLogWindow::filterButtonForeground(LogLevel level, const QPalette &palette) {
    return logInk(level, palette);
}

QColor GameLogWindow::switchOffColor(const QPalette &palette) {
    return Theme::neutralFill(palette.color(QPalette::Window),
                              palette.color(QPalette::WindowText), kSwitchOffPercent);
}

// ---------------------------------------------------------------------------
// 读现场
// ---------------------------------------------------------------------------

QString GameLogWindow::logText() const { return m_view->toPlainText(); }

QString GameLogWindow::allLogText() const {
    // 一行一句（正文那一半整个由共用模型给）。末尾那半行（m_pending）**原样接上去、
    // 不补换行** —— 补了的话这一份文件就和屏幕上看到的那一截对不上了。
    return m_model.allText() + m_pending;
}

QString GameLogWindow::logTextFrom(LogLevel minLevel) const {
    // 比的是**模型的级别**（认不出级别的续行跟着上一行走，见 .hpp），不再按字符串匹配一遍：
    // 匹配第二遍就会出现"屏幕上是红的、导出里没有"这种两边对不上的事。
    return m_model.textFrom(modelLevel(minLevel));
}

QPlainTextEdit *GameLogWindow::getLogView() const { return m_view; }



QLabel *GameLogWindow::getHintLabel() const { return m_hintLabel; }

QComboBox *GameLogWindow::getLineLimitCombo() const { return m_lineLimitCombo; }

QToolButton *GameLogWindow::getFilterButton(LogLevel level) const {
    return m_filterButtons.at(levelIndex(level));
}

// ---------------------------------------------------------------------------
// 摆哪儿
// ---------------------------------------------------------------------------

namespace {
/// 贴着启动器留的那条缝（像素）：不留的话两扇窗的阴影/边框会叠在一起。
constexpr int kPlacementGap = 12;
/// 同时开出来的第 N 扇窗往下往右错开这么多（不错开的话后开的完全盖住先开的）。
constexpr int kPlacementCascade = 28;
}  // namespace

QPoint GameLogWindow::placementFor(const QRect &launcherFrame, const QSize &windowSize,
                                   const QRect &availableScreen, int index) {
    // 顺序是**右 → 左 → 居中**：日志是宽行文本，起点离启动器越远越好，但「不遮住启动器」
    // 优先于「放在右边」。左右都放不下（启动器拉满屏/屏幕很窄）那种情况躲不开重叠，
    // 那时至少别把它甩到屏幕外面去。
    //
    // NOTE: 挑边看的是**不带错开**的那个位置，错开的那点距离（28）不该决定整扇窗去哪一边
    // —— 错开**不能**算进"放得下吗"：否则第二扇窗会因为右边少了 28 像素就整个翻到左边，
    // 正好压在左边那扇上。错开只影响挑完边之后往哪偏。
    const int cascade = qMax(0, index) * kPlacementCascade;
    const int plainRight = launcherFrame.right() + 1 + kPlacementGap;
    const int plainLeft = launcherFrame.left() - kPlacementGap - windowSize.width();
    const int screenRight = availableScreen.right() + 1;
    const bool fitsRight = plainRight + windowSize.width() <= screenRight;
    const bool fitsLeft = plainLeft >= availableScreen.left();

    int x = 0;
    if (fitsRight) {
        // 右边界那一下钳：screenRight - width ≥ plainRight（就是上面 fitsRight），所以钳完
        // **永远不会**跑到 plainRight 左边去，也就永远不会压住启动器。
        x = qMin(plainRight + cascade, screenRight - windowSize.width());
    } else if (fitsLeft) {
        x = qMax(plainLeft - cascade, availableScreen.left());  // 同理：钳完仍 ≤ plainLeft
    } else {
        // 左右都挤不下：居中（比贴边看着像故意的）。
        x = launcherFrame.center().x() - windowSize.width() / 2;
    }

    // 竖直方向：和启动器的上沿对齐（两扇窗的标题栏在一行上，扫过去就是一行），错开的那几扇
    // 往下走一点。横竖都错开之后，后开的窗不会**完全**盖住先开的（先开的那扇至少露出上面
    // 一条标题栏，还点得到）—— 屏幕上真摆不下两三扇的时候，这就是能做到的全部。
    const int y = qBound(availableScreen.top(), launcherFrame.top() + cascade,
                         qMax(availableScreen.top(),
                              availableScreen.bottom() + 1 - windowSize.height()));
    return QPoint(qBound(availableScreen.left(), x,
                         qMax(availableScreen.left(), screenRight - windowSize.width())),
                  y);
}

// ---------------------------------------------------------------------------
// 退出去之后
// ---------------------------------------------------------------------------

bool GameLogWindow::autoCloseOnExit(int exitCode, bool crashed) {
    return !crashed && exitCode == 0;
}

// ---------------------------------------------------------------------------
// 正文那一块的长相
// ---------------------------------------------------------------------------

QFont GameLogWindow::logFont() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    // 点大小和像素大小二选一（字体表给的可能是任一种）：**两头都照顾**，不然
    // `pointSizeF()` 回 -1 的那台机器上这个"大一号"会静默失效（界面看着和没说一样）。
    if (font.pointSizeF() > 0) {
        font.setPointSizeF(font.pointSizeF() + kLogFontPointDelta);
    } else if (font.pixelSize() > 0) {
        // 1 pt = 1/72 英寸、1 px 在 96 dpi 上是 1/96 英寸 ⇒ 1 pt ≈ 1.33 px。
        font.setPixelSize(font.pixelSize() + qMax(1, qRound(kLogFontPointDelta * 96.0 / 72.0)));
    }
    return font;
}

QColor GameLogWindow::separatorColor() {
    return separatorColor(QApplication::palette());
}

QColor GameLogWindow::separatorColor(const QPalette &palette) {
    // 底色和墨都取 `Base` / `Text`（**不是** Window 那一对）：这条线画在正文区上 ——
    // 那一块用的就是 Base 的底和 Text 的墨。浅色下是 `#d9d9d9`，深色下就是压在深底上
    // 的一条深灰。
    return Theme::isDarkPalette(palette) ? Theme::tokens(QStringLiteral("dark")).logDivider
                                         : Theme::tokens().logDivider;
}

// ---------------------------------------------------------------------------
// 文案与名字
// ---------------------------------------------------------------------------

QString GameLogWindow::windowTitleText(const QString &instanceName) {
    // 实例名是空的（会话快照没拿到）时不留一个光秃秃的破折号。
    if (instanceName.isEmpty()) {
        return QCoreApplication::translate("GameLogWindow", "游戏日志");
    }
    return QCoreApplication::translate("GameLogWindow", "游戏日志 — %1").arg(instanceName);
}

QString GameLogWindow::fileWindowTitleText(const QString &fileName) {
    return QCoreApplication::translate("GameLogWindow", "%1 — 日志查看").arg(fileName);
}

QString GameLogWindow::fileHintText(qint64 cutHeadBytes, bool tailByDefault) {
    QString text = QCoreApplication::translate("GameLogWindow", "已保存的日志，不再更新。");
    if (cutHeadBytes > 0) {
        text += QCoreApplication::translate("GameLogWindow", "文件过大，只读取了末尾部分。");
    }
    if (tailByDefault) {
        text += QCoreApplication::translate("GameLogWindow", "当前显示末尾 %1 行，可在右上角调整。")
                    .arg(fileTailLines());
    }
    return text;
}

QString GameLogWindow::hintText() {
    return QCoreApplication::translate("GameLogWindow", "此窗口显示本次运行的游戏日志。关闭窗口后，游戏会继续运行。");
}

QString GameLogWindow::droppedHintText(int dropped, int capacity) {
    if (dropped <= 0) {
        return QString();
    }
    return QCoreApplication::translate("GameLogWindow", "窗口最多保留 %1 行，更早的 %2 行已经丢弃。")
        .arg(capacity)
        .arg(dropped);
}

QString GameLogWindow::clearButtonText() { return QCoreApplication::translate("GameLogWindow", "清空"); }


QString GameLogWindow::exportLogButtonText() { return QCoreApplication::translate("GameLogWindow", "导出日志…"); }

QString GameLogWindow::exportReportButtonText() { return QCoreApplication::translate("GameLogWindow", "导出错误报告…"); }

QString GameLogWindow::exportLogButtonTooltip() {
    // 这句话（和下面那句）唯一的用途就是把两颗键**分开**：它们只差"要不要给别人的那一份
    // 环境信息"，而这一点从键上的字看不出来。所以两句都写成"你会拿到什么"。
    return QCoreApplication::translate("GameLogWindow", "把全部游戏输出存成文本文件（含被筛掉的几档，最多 %1 行）。")
        .arg(maxLogBlocks());
}

QString GameLogWindow::exportReportButtonTooltip() {
    return QCoreApplication::translate("GameLogWindow", "存一份可以直接发给别人的报告：启动器版本、系统、这个实例、退出码，" "加上错误那几行和完整日志。");
}

QString GameLogWindow::emptyPlaceholderText() { return QCoreApplication::translate("GameLogWindow", "等待游戏输出…"); }

QString GameLogWindow::filteredOutPlaceholderText() {
    return QCoreApplication::translate("GameLogWindow", "当前筛选无日志。勾选要查看的日志级别。");
}

QString GameLogWindow::searchPlaceholderText() {
    return QCoreApplication::translate("GameLogWindow", "搜索日志（不区分大小写，与级别筛选同时生效）");
}

QString GameLogWindow::searchEmptyPlaceholderText() {
    return QCoreApplication::translate("GameLogWindow", "没有匹配的日志。换个关键词，或清空搜索框。");
}

QString GameLogWindow::searchTooltip() {
    return QCoreApplication::translate("GameLogWindow", "只看包含这一串的行；导出仍是整份日志。");
}

QString GameLogWindow::searchText() const {
    return m_searchEdit != nullptr ? m_searchEdit->text() : m_model.searchText();
}

QString GameLogWindow::filterButtonText(LogLevel level, int count) {
    return QStringLiteral("%1 %2").arg(levelName(level)).arg(qMax(0, count));
}

QString GameLogWindow::filterButtonTooltip() {
    return QCoreApplication::translate("GameLogWindow", "点一下就不显示这一档。数字是日志记录数（崩栈续行不重复计算）。" "窗口最多保留 %1 行文本。")
        .arg(maxLogBlocks());
}

QVector<int> GameLogWindow::lineLimitChoices() {
    return RuntimeOutputModel::lineLimitChoices();
}

QString GameLogWindow::lineLimitText(int limit) {
    return RuntimeOutputModel::lineLimitText(limit);
}

int GameLogWindow::lineLimitOfIndex(int index) {
    return RuntimeOutputModel::lineLimitOfIndex(index);
}

QString GameLogWindow::lineLimitTooltip() {
    return QCoreApplication::translate("GameLogWindow", "只影响显示，日志一行都不会少（最多留 %1 行）。")
        .arg(maxLogBlocks());
}

QString GameLogWindow::filterButtonName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return QStringLiteral("gameLogFilterDebug");
        case LogLevel::Info:
            return QStringLiteral("gameLogFilterInfo");
        case LogLevel::Warn:
            return QStringLiteral("gameLogFilterWarn");
        case LogLevel::Error:
            return QStringLiteral("gameLogFilterError");
        case LogLevel::Fatal:
            return QStringLiteral("gameLogFilterFatal");
    }
    return QStringLiteral("gameLogFilterInfo");
}
