/**
 * @file GameLogWindow.hpp
 * @brief 「游戏日志」窗：把某一份游戏进程自己的输出（stdout/stderr）实时显示出来。
 * @note 和 GroupDialog / AccountSkinDialog 同一条约定：**ui/ 不依赖业务层**。这里不 include
 * 任何 src/ 下的头，也不认识"会话"是什么 —— 它是"给我一行字我就写上去"的一块控件；
 * 谁来喂、喂哪一份、什么时候销毁，全在 MainWindow 里。
 *
 * 【为什么是一个独立的窗，而不是主页上的一块面板】
 *
 * 界面里**没有日志面板**：日志走 Qt 的日志系统，落在 <数据目录>/logs/wtfml.log。
 * 那对一个启动器是对的 —— 99% 的时间里日志是噪音。
 * 但"游戏起不来、想知道为什么"是真实需求，而那时候用户要的是**那一份游戏自己的**输出，
 * 不是启动器的。所以做成"按需开一个窗"，而不是把噪音请回主页。
 *
 * 【一份游戏一个窗】
 *
 * 多开是这个启动器明确要支持的事（见 LauncherManager 类头的说明），两三个实例的输出
 * 混在一个窗里等于没法看 —— 所以要开的窗不止一个，窗口标题里带实例名。
 *
 * 【关掉这个窗会发生什么】
 *
 * 什么都不发生：这只是个看输出的窗，游戏进程照跑（和「停止」按钮是两回事）。
 * 关掉之后**这一份会话**不再自动重开（见 `GameSessionController` 里那一份"用户关过"的表）。
 *
 * 【按级别染底色 + 顶上一排筛选】
 *
 * 游戏自己打的日志本来就带级别（`[main/INFO]` 那一格）。按严重程度给**整行**染底色
 * （Debug 灰蓝 / Info 不染 / Warn 黄 / Error 红 / Fatal 深红配白字，见 levelBackground），
 * 顶上再摆一排带计数的开关：**开着的那颗就是这一档的颜色**（和正文那一行同一个色，不用读字
 * 就知道哪几档开着），点一下就只隐藏那一档、那颗也跟着回到那档灰。
 *
 * 四件事值得先说清：
 *
 * - **染的是整行，从左沿到右沿**。这是**块**格式（`QTextBlockFormat::setBackground`），
 * 不是逐字的字符底色 —— 字符底色只染"字占的那一截"，一行比视口短的时候后半截就是白的
 * （日志最后那几行、我们自己补的"游戏已退出"都短，看着就像没染上）。块底色铺满整行、
 * 不用往文本里塞空格，复制出去也不脏。INFO 故意**不染**：日志里绝大多数是它，全染上就
 * 成了一张色纸，Warn/Error 反而不显眼了。
 * - **数字数的是"还留在这里的日志记录"**，不是物理文本行。带级别的首行记一次，紧随其后
 * 的崩栈 / 异常正文只是这条记录的续行：照样同色、参与筛选和导出，但不把 ERROR 1 膨胀成
 * ERROR 20。这个窗最多仍只留 maxLogBlocks() 个物理行；最老的首行滚掉时计数才跟着减少。
 * 关掉某几档筛选时数字**不会变小**（记录还在，只是没显示）。
 * - **认不出级别的行跟着上一行走**（崩栈的 `at ...` 续行、我们自己补的那句"游戏已退出"
 * 都属于上一条日志），第一行就认不出来时算一条 Info。五档计数之和因此可能小于物理
 * 行数，差额就是续行和空分隔行。末尾那半行也按这条**先猜**一个（见 insertPendingIntoView），
 * 猜错了等换行到了会按真级别重摆一遍。
 * - **一次摆一整段，不是一行一次**（见 .cpp 里 insertRun）：文档操作是按**次数**收钱的，
 * 一份两万行的日志一行一次要半秒，攒成一段一次只要几十毫秒。
 *
 * 【四个细节】
 *
 * ① 行与行之间有一条细线（见 .cpp 里 LogView；**得是自己画的**：
 * QTextDocument 里没有"块的下边框"这一项）。② logFont() 比系统那个等宽字体大
 * kLogFontPointDelta 号。③ 窗口可最大化最小化（构造函数里那两行 window flags；
 * QDialog 默认只给一个关闭钮）。④ 游戏正常以 0 退出就自动关闭日志界面
 * （autoCloseOnExit()，接线在 MainWindow::onGameFinished()）。
 *
 * 【顶上那一排的右边】
 *
 * 三件事：
 *
 * - **「显示全部」是一颗 `QComboBox`**（行数上限：500 / 1000 / 2000 / 5000 / 10000 /
 * 全部，见 lineLimitChoices()），默认「全部」。
 * - 「清空」在**它旁边**（顶上那一排的最右）；底下那一排只剩「关闭」，右对齐。
 * - 代价说清楚：**五档全关之后没有"一键全勾回来"了**。回退的路是那句占位本身
 * （filteredOutPlaceholderText() 写的就是"上面勾回来就看得见"）。这个取舍是刻意的 ——
 * "五档全关"本来就是个少见状态，勾回五下不算事。
 *
 * 【行数上限是"显示"的上限，不是"留多少"的上限】
 *
 * 两条上限**不是一回事**，别把它们合并：
 * - 模型（m_lines）封在 maxLogBlocks() = 20000 行上，超出丢最老的 —— 那是**内存**的闸。
 * - 视图最多显示 displayLineLimit() 行 —— 那是**可读性**的闸。
 *
 * 选了 500 之后模型里那两万行**一行都没少**：切回 10000 当场又多出九千五百行。这正是这个
 * 控件该有的手感 —— "少看点"是临时的视角，不是"把日志扔了"。所以那条上限走的是**视图的
 * maximumBlockCount**，重建视图就能变。
 *
 * 【导出】
 * 底部工具栏右侧提供导出入口。
 */

#pragma once

#include <QColor>
#include <QDialog>
#include <QFont>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include "RuntimeOutputModel.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QAction;
class QPushButton;
class QToolButton;
class QAbstractButton;
class WindowChrome;
class ElidedLabel;

class GameLogWindow : public QDialog {
    Q_OBJECT

public:
    /**
     * 一条日志的级别。
     *
     * NOTE: **枚举顺序就是严重程度**（Debug 最轻 → Fatal 最重）。顶上那排筛选按钮照这个
     * 顺序摆，内部那几个 QVector（计数 / 是否显示 / 按钮指针）也照这个顺序索引 ——
     * 想加一档的话，除了枚举本身，`levelCount()` 和 levelBackground / levelName /
     * filterButtonName 这几处一起加，别只改一半（验证工程里有"五档两两不同"那几条
     * 盯着文案与颜色，加漏一处会红）。
     *
     * 没有 Trace 这一档：它比 Debug 还轻，和"到底出了什么事"没关系，认出来一律归 Debug。
     */
    enum class LogLevel { Debug, Info, Warn, Error, Fatal };

    // 真正管"哪一档、几行、怎么筛"的是 `RuntimeOutputModel`（服务端控制台
    // 那一页也在用同一份）。这一层保留自己的枚举只是为了不惊动已有的调用点与验证工程 ——
    // 两套枚举**必须逐项对齐**（下面这几个 static_assert 就是那条约定的钉子）。
    static_assert(int(LogLevel::Debug) == int(RuntimeOutputModel::Level::Debug));
    static_assert(int(LogLevel::Info) == int(RuntimeOutputModel::Level::Info));
    static_assert(int(LogLevel::Warn) == int(RuntimeOutputModel::Level::Warn));
    static_assert(int(LogLevel::Error) == int(RuntimeOutputModel::Level::Error));
    static_assert(int(LogLevel::Fatal) == int(RuntimeOutputModel::Level::Fatal));

    /// 有几档（循环/数组的上界）。
    static constexpr int levelCount() { return 5; }
    /// 下标 → 级别。越界回 Info（不 assert：这条路上不值得为一个越界把整个日志窗弄崩）。
    static LogLevel levelAt(int index);
    static int levelIndex(LogLevel level) { return int(level); }

    /**
     * @brief 造一个日志窗（**不显示**，由调用方 show()）。
     * @param instanceName 这一份游戏是哪个实例（只用于标题）。
     * @param parent 父窗口。**传 nullptr**（见下）。留在参数里只是为了那些"挂在别的窗
     * 底下"的用法，产品这条路不走。
     *
     * NOTE: 建出来就带 Qt::WA_DeleteOnClose —— 关掉即销毁。理由是这些窗会**留着一整份
     * 文本**（最多 maxLogBlocks() 行），藏起来不销毁的话，开过十个实例就白占十份
     * 内存。销毁之后由调用方那边把指针摘掉（见 MainWindow 里接 finished 的那一段）。
     *
     * 【为什么**不要**父窗口】
     *
     * 在 Windows 上，"有主的窗"（owned window）**永远压在自己的主窗上面** —— 用户点
     * 启动器也翻不上来，于是日志窗等于把启动器挡死了。那是平台规则，不是可以调的风格，
     * 所以父窗口这件事本身就是病根。
     *
     * 不要父窗口换来三件事：能点到后面去、能自己最小化、任务栏上有自己一格。代价是
     * "跟着启动器一起关掉"要自己接（见 MainWindow 析构里那一段）。
     */
    explicit GameLogWindow(const QString &instanceName, QWidget *parent = nullptr);

    QString instanceName() const { return m_instanceName; }

    // ---------- 打开一个文件（已结束的日志：不追加、不显示运行状态） ----------
    //
    // 查看器和游戏运行时的日志窗是同一个类：染色、级别筛选、搜索、行数上限、导出全都照旧，
    // 只是内容来自盘上的一份文件。**读盘、gzip 解压不在这一层**（ui/ 不碰 src/，也不能在
    // 主线程上做）：调用方在工作线程里读好，再把行递进来。

    /// 切成文件查看器：标题与顶端写文件名，运行徽标、清空、导出错误报告收起，
    /// 「用外部程序打开」露出来；正文先写「正在读取…」。
    void beginFile(const QString &path);
    /// 读好了：@p lines 是全部行，@p cutHeadBytes > 0 表示文件太大、开头那一截没读。
    /// 行数超过 `maxLogBlocks()` 时把模型容量放大到装得下；默认只显示末尾
    /// `fileTailLines()` 行，可用行数上限切到全部（分批上屏，见 .cpp）。
    void setFileLines(const QStringList &lines, qint64 cutHeadBytes = 0);
    /// 读失败：正文写原因。
    void setFileProblem(const QString &problem);
    QString filePath() const { return m_filePath; }
    bool isFileViewer() const { return !m_filePath.isEmpty(); }
    /// 超长日志默认显示的末尾行数。
    static int fileTailLines() { return 5000; }
    /// 「全部」档一批上屏多少行。
    static int fileBatchLines() { return 4000; }
    /// 分批上屏是否还没结束（验证工程等它为假再量）。
    bool fileBatchPending() const { return m_batchPos < m_batchLines.size(); }
    static QString fileWindowTitleText(const QString &fileName);
    static QString fileHintText(qint64 cutHeadBytes, bool tailByDefault);
    /// 右上「…」菜单里「用外部程序打开」那一项。
    QAction *externalOpenAction() const { return m_externalAction; }

    /**
     * @brief 新窗摆哪儿：**优先贴在启动器旁边**（右 → 左 → 居中），不遮住它。
     * @param launcherFrame 启动器主窗的窗口矩形（全局坐标）。
     * @param windowSize 新窗的尺寸。
     * @param availableScreen 那一块屏幕的可用区域（`QScreen::availableGeometry()`，
     * 已经扣掉了任务栏）。
     * @param index 同一次开出来的第几个（同时开着两三份时错开一点，别叠成一摞）。
     * @return 新窗左上角该在的全局坐标（已经钳进那块屏幕里）。
     *
     * NOTE: 纯几何（不读屏幕、不读全局状态），所以验证工程能拿一组假矩形直接量它。
     * 左边右边都放不下（启动器拉满屏那种）时**居中** —— 那种情况躲不掉重叠，
     * 能不重叠的两种情况优先。
     */
    static QPoint placementFor(const QRect &launcherFrame, const QSize &windowSize,
                               const QRect &availableScreen, int index);

    /**
     * @brief 游戏这一趟退出之后，这个窗要不要**自己关掉**。
     * @param exitCode 退出码。
     * @param crashed 是不是异常终止（`QProcess::ExitStatus == CrashExit`）。
     * @return 正常退出**且**退出码为 0 时为真。
     *
     * 游戏正常以 0 退出就自动关闭日志界面。那一趟没有任何可看的东西
     * （真出了事不会给 0），留着一扇只写着一行"已退出"的窗等于替用户留了个垃圾。
     *
     * NOTE: **崩溃（CrashExit）一律不关**，哪怕拿到的码正好是 0：崩溃那一趟正是用户要
     * 逐行看的时候（见 .cpp 里那行「游戏已崩溃退出」）。这也是这个函数收两个参数
     * 而不是一个的原因 —— 只看码的话 `CrashExit + 0`（Windows 上杀进程拿到的码）
     * 会被误判成"正常退出"。
     */
    static bool autoCloseOnExit(int exitCode, bool crashed);

    // ---------- 数据进 ----------

    /**
     * @brief 追加一段输出（游戏自己的换行和缩进都不动它）。
     *
     * 自动做的三件事：① **攒够一整行才认级别**（进程输出是按块来的，一个 chunk 可能断在
     * 行中间 —— 那半行先原样摆在末尾、不染底色，等换行到了再合成一条）；② 认出来之后按
     * 级别染底色（这一档被筛掉的话就不摆出来，但**照样计数**）；③ **本来就在最底下时**
     * 跟到底 —— 用户在往上翻（看刚才那几行报错）时不跟，那种"正在读的东西被新输出顶走"
     * 是这类窗最招人烦的地方。
     */
    void appendText(const QString &text);
    /// 标题旁那颗徽标开窗时是「运行中」；游戏结束（窗口没自动关）：徽标改成退出码，非 0 / 崩溃按危险色。
    void setExited(int exitCode, bool crashed);

    // ---------- 当场读控件 ----------

    /// 现在显示的全文（**被筛掉的不在里面**）。给验证工程用（产品代码不读它）。
    QString logText() const;

    /**
     * @brief 这个窗里**全部**的行（含被筛掉的那几档），一行一句、末尾有换行。
     *
     * 和 `logText()` **不是一回事**：那个是**视图**里那一份（上面那几档开关关掉的行不在
     * 里面）、而且末尾那半行按原样铺着。导出要的是"这一趟到底出了什么"，所以走这一份 ——
     * 用户屏幕上只勾着 ERROR 的时候导出，拿到的仍然必须是整份日志。
     *
     * NOTE: 末尾那半行（还没等到换行的那一截）也算进去，但它本来就没换行，所以**不补换行**
     * —— 补了的话那一份文件和屏幕上看到的那一截对不上。
     */
    QString allLogText() const;
    /**
     * 判据是**模型里记着的级别**（认不出级别的续行跟着上一行走，所以崩栈的 `at ...` 那几行
     * 会跟着它的 ERROR 一起被挑进来）—— 不是再去按字符串匹配一遍。
     */
    QString logTextFrom(LogLevel minLevel) const;

    /// 那一块文本控件（验证工程量几何/只读性用）。
    QPlainTextEdit *getLogView() const;
    /// 标题行「…」菜单里那几项（清空 / 复制全部 / 导出日志 / 导出错误报告）。
    QAction *clearAction() const { return m_clearAction; }
    QAction *copyAction() const { return m_copyAction; }
    QPushButton *getMoreButton() const { return m_moreButton; }
    /// 底下那一排最左那颗（「导出日志…」）。
    QAction *exportLogAction() const { return m_exportLogAction; }
    QAction *exportReportAction() const { return m_exportReportAction; }
    QLabel *getHintLabel() const;
    QToolButton *getFilterButton(LogLevel level) const;
    /// 顶上那一排最右那颗行数下拉框（验证工程拿它切上限、也拿它量"清空在不在旁边"）。
    QComboBox *getLineLimitCombo() const;

    // ---------- 级别与筛选 ----------

    /// 模型里现在留着几行（含被筛掉的）。
    int lineCount() const { return m_model.lineCount(); }
    /// 这一档有几条独立日志记录（续行不重复计数，含被筛掉的记录）。
    int lineCountFor(LogLevel level) const;
    /// 被行数上限（capacity）**挤掉**的行数 —— 导出时要把它们加回"原始共 N 行"里。
    /// 用户按「清空」之后归零（那是主动不要，不是被上限挤掉）。
    int droppedLineCount() const { return m_model.droppedLineCount(); }
    /// 被挤掉的行里、级别不轻于 @p level 的有几行。
    int droppedLineCountFor(LogLevel level) const;
    bool levelVisible(LogLevel level) const;
    /// 改一档的显示与否（会顺手把那颗按钮的勾也对上，然后重画）。给验证工程用。
    void setLevelVisible(LogLevel level, bool visible);

    // ---------- 行数上限（顶上那颗下拉框） ----------

    /// 下拉框里那几档，**顺序就是界面上从上到下的顺序**；`0` 这一档表示"全部"。
    static QVector<int> lineLimitChoices();
    /// 下拉框上那一档写什么。`0`（全部）写「全部」，其余写成数字 + 「条」。
    ///
    /// NOTE: 数字**写全**（1000，不是 1k）—— 直接写数字，不做 k 缩写。
    static QString lineLimitText(int limit);
    /// 开窗时的默认档：全部（进来就看得见全部日志）。
    static int defaultLineLimit() { return 0; }
    /// 那一档在数据里存什么（`QComboBox::itemData`）。校验工程按它认条目。
    static int lineLimitOfIndex(int index);
    /// 现在这个窗最多显示几行（0 = 全部）。
    int displayLineLimit() const { return m_model.displayLimit(); }
    /// 换一档上限（把下拉框也对上，然后重建视图）。给验证工程和那颗下拉框用。
    ///
    /// NOTE: 只影响**视图**，模型一行都不动（见文件头那段）。切到上限更大的那一档时那些行
    /// 会当场回来 —— 前提是它们还在模型里（模型自己封在 maxLogBlocks() 上）。
    void setDisplayLineLimit(int limit);

    /**
     * @brief 认一行的级别；**认不出来时回 fallback**（调用方传"上一行的级别"）。
     *
     * 规则只有一处实现（`RuntimeOutputModel::classifyLine`）；
     * 这里保留同名转发是为了不惊动已有的调用点和验证工程。
     */
    static LogLevel classifyLine(const QString &line, LogLevel fallback);
    static int levelScanHead() { return RuntimeOutputModel::levelScanHead(); }

    // ---------- 搜索（抽出来的共用那一半） ----------

    /// 搜索框里那一格（验证工程拿它打字、也拿它量"有没有这一格"）。
    QLineEdit *getSearchEdit() const { return m_searchEdit; }
    static QString searchEditName() { return QStringLiteral("gameLogSearchEdit"); }
    static QString searchPlaceholderText();
    /// 搜过、但一行都没匹配上时正文区那句占位（和"全被筛掉"是**第三句不同的话**）。
    static QString searchEmptyPlaceholderText();
    static QString searchTooltip();
    /// 现在搜索框里的字（空串 = 没搜）。
    QString searchText() const;

    /// 一条没有换行的输出最多攒多少字符就当成一整条收下（理由见 .cpp）。
    static int maxPendingChars() { return 8192; }

    // ---------- 文案与名字（唯一出处） ----------

    /// 窗口标题。带实例名：同时开着好几份时，靠它认哪个是哪个。
    static QString windowTitleText(const QString &instanceName);
    /// 顶上那行灰字。
    static QString hintText();
    /// 挤掉过行时补在 `hintText()` 后面那一句；没挤掉回空串（两段之间由调用方补空格）。
    static QString droppedHintText(int dropped, int capacity);
    static QString clearButtonText();
    /// 底下那两颗导出键（说明见文件头）。两颗都写成"带省略号"那种
    /// （点下去还会再问一次存哪儿）。
    static QString exportLogButtonText();
    static QString exportReportButtonText();
    /// 两颗键的提示：**把两者的区别说清楚** —— 那是它们唯一容易被搞混的地方。
    static QString exportLogButtonTooltip();
    static QString exportReportButtonTooltip();
    /// 还没有任何输出时文本控件里那句占位。
    static QString emptyPlaceholderText();
    /// 有输出、但五档全被筛掉时那句占位（和上面那句是**两句不同的话**，别合并）。
    static QString filteredOutPlaceholderText();

    /// 这一档在筛选按钮上写什么（级别名 + 计数）。
    static QString filterButtonText(LogLevel level, int count);
    /// 那一排开关上那句提示（顺带说明数字是什么）。
    static QString filterButtonTooltip();
    /// 行数下拉框上那句提示（说清"只是少显示几行，日志没扔"）。
    static QString lineLimitTooltip();

    /// 级别名。**用日志自己那套英文词**（不是"警告/错误"）：它要跟日志里的 `[main/WARN]`
    /// 对得上，用户看的正是那一格。
    static QString levelName(LogLevel level);

    /// 这一档的文字染什么底色。**Info 故意返回无效颜色**（= 不染，用窗口自己的底色）。
    ///
    /// 深浅色两套底：浅色那组是照着"白纸"
    /// 挑的（下面那几个 `k*Bg`），深色预设下浅墨压在上面等于没字。
    /// 所以多了一组深底版（`k*BgDark`），走哪一组由 `Theme::isDarkPalette()` 说了算
    /// （**唯一判据**，和别处"深底上用亮的那一档"是同一个）。Fatal 那个深红两套相同。
    /// 带调色板那一版是**给它自己用的**；不带的那一版问的是应用调色板（验证工程用）。
    static QColor levelBackground(LogLevel level);
    static QColor levelBackground(LogLevel level, const QPalette &palette);
    /// 这一档的文字用什么墨色。只有 Fatal 有（深红底上再写深灰字是看不清的）。
    /// **不带调色板**：别的档要的就是"不设"，让调色板那一支墨（深色下是浅的）自己生效。
    static QColor levelForeground(LogLevel level);

    /**
     * @brief 顶上那颗开关注上这一档时填什么色（**关着**时一律是同一个灰）。
     *
     * 「开着」那一格就是 levelBackground —— **开关的颜色和正文那一行是同一个色**，不用读字
     * 就知道哪几档开着。Info 例外：它的正文底色就是我们自己的窗口底色（`levelBackground`
     * 返回无效色），没有颜色可借，于是一律用那个兜底的开态灰（见 .cpp 里 kSwitchOnRgb）——
     * 硬编一个日志里不存在的颜色反而会让人以为"Info 是蓝的"。**开关的两种状态都由我们自己
     * 画**（理由见 .cpp 里 filterButtonStyle），所以这两个值必须有个确定的出处。
     *
     * 验证工程量的是**按钮上真画出来的像素**，比的就是这里。
     *
     * NOTE: Info 那一档用**正文区自己的底色**（量出来是 `#ffffff`，见 .cpp 里
     * kInfoOnRgb），不用 kSwitchOnRgb 那个兜底灰 —— 那既是最亮的一档，也正好是
     * "Info 这一档的颜色"（它的整行底色就是这个）。
     */
    static QColor filterButtonBackground(LogLevel level, bool on);
    static QColor filterButtonBackground(LogLevel level, bool on, const QPalette &palette);
    /// 合成开关上那几个字的墨色：Fatal 那档是白的，其余**跟着这块地方的墨走**
    /// （浅色下是纯黑；深色下是调色板里的浅墨）。
    static QColor filterButtonForeground(LogLevel level);
    static QColor filterButtonForeground(LogLevel level, const QPalette &palette);

    /**
     * @brief 那排开关**关着**时的底色（不描边：1px 透明边只为保住几何，形状靠底色）。
     *
     * 它们**从这一块地方的底色往它的墨色混一档**（`Theme::neutralFill()`，29% / 34%）。
     * 浅色下是 `#aa`，屏幕上和 `#ababab` 分不出来；深色下自动是深灰。
     */
    static QColor switchOffColor(const QPalette &palette);

    // ---------- 正文那块文本的长相 ----------

    /**
     * @brief 正文用的字体：系统那个等宽字体，**再大一号**。
     *
     * 等宽是必须的（日志里有缩进、有对齐的模组列表，比例字体读起来是散的，见构造函数里
     * 那一段）；大一号是因为日志一屏三十行、
     * 每行几十个字符，小一号的时候找关键字全靠凑近看。
     *
     * NOTE: 唯一出处。构造函数拿它 `setFont`，验证工程拿它跟**系统那个等宽字体**比
     * （不是跟自己比 —— 那个比法是空的，改错了一起变）。
     */
    static QFont logFont();
    /// 大多少号（点）。`1.0` 是"稍微"：再大就一屏放不下几行了。
    static qreal logFontPointDelta() { return 1.0; }

    /**
     * @brief 行与行之间那条细线的颜色。
     *
     * 日志行与行之间要有一条细线分隔。**底色本身分不开相邻的两条 INFO**
     * （它们都不染），所以这条线的主要去处就是那里 —— 它得在白底上看得见（和白差 39*√3 ≈ 67），
     * 也得在 Debug 那个 `#eef2f6` 上看得见（差 45），同时不能深到像一条框线。
     */
    static QColor separatorColor();
    static QColor separatorColor(const QPalette &palette);
    /// 那条线几像素高。1 = 真正的"细线"（2 就开始像表格了）。
    static int separatorHeight() { return 1; }

    /// 最多留多少行。**必须有上限**：一个挂了一百个模组的实例能吐几十万行，
    /// 不封顶的话开一晚上日志就把内存吃干净了。超出之后最老的整段丢掉。
    static int maxLogBlocks() { return RuntimeOutputModel::defaultCapacity(); }

    // ---------- 用户想导出 ----------

signals:
    /// 用户点了「导出日志…」。槽该读 `allLogText()`。
    void exportLogRequested();
    void exportReportRequested();
    /// 文件查看器里点了「用外部程序打开」。
    void openExternalRequested(const QString &path);

public:
    static QString logViewName() { return QStringLiteral("gameLogView"); }
    static QString moreButtonName() { return QStringLiteral("gameLogMoreButton"); }
    static QString hintLabelName() { return QStringLiteral("gameLogHintLabel"); }
    static QString lineLimitComboName() { return QStringLiteral("gameLogLineLimitCombo"); }
    static QString filterButtonName(LogLevel level);

protected:
    /**
     * @brief 主题变了就重新算一遍这一窗的颜色。
     *
     * 这一窗的颜色有两处是**自己画**的（那一排开关的样式表、行间那条线），而这两处都
     * 不跟着调色板自动走 —— 用户在设置页换了深色，这个开着的窗得跟着变，不然就是"深色
     * 主题下一个亮着的日志窗"（正文那块跟着调色板变了，开关和分隔线没变，看着像坏的）。
     *
     * 只在 `PaletteChange` / `StyleChange` 上做事，而且**先比一遍算出来的色**：
     * 这两个事件会因为各种原因连着来几次，每次都重建一遍视图（两万行）是白跑。
     */
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    /// 外框消息（WM_NCHITTEST 等）转给 `WindowChrome`：系统标题栏收进客户区，顶上那条自绘。
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    /// 攒够一整行：认级别、进模型、记账、超上限就丢最老的。回它的级别。
    ///
    /// 记账那一半在 `m_model`（`RuntimeOutputModel::addLine()`，和服务端控制台共用）。
    LogLevel rememberLine(const QString &text);
    /// 视图的块数上限该喂多少 = 现在这一档的行数上限（「全部」时就是 maxLogBlocks()），
    /// **再留一格**给"还没等到换行的那半行"（见构造函数里那段）。喂给 maximumBlockCount()。
    int viewBlockLimit() const;
    /// 按现在的筛选从头重画一遍（切筛选那一下走这里）。
    void rebuildView();
    /**
     * @brief 把**同一档连续的几行**一次摆到末尾（末尾永远留一格空的当光标行）。
     *
     * **成片写**是这个窗快不快的关键：文档操作按**次数**收钱（见 .cpp），所以
     * "一行一次"在长日志上就是"点一下卡半秒"。底色也一次盖一片（跨块选区的
     * `setBlockFormat` 是一次调用）。
     */
    void insertRun(LogLevel level, const QStringList &lines);
    /// 末尾那半行该按哪一档染：**按"上一行那一档"猜**（认不出级别的行就是跟着上一行走的）。
    /// 猜错了不要紧 —— 换行到了会按真级别重摆一遍。
    LogLevel pendingLevelGuess() const;
    /// 把"还没等到换行的那半行"摆在末尾（按猜的那一档染，见上面）。
    void insertPendingIntoView();
    /// 把上面那半行从视图里撤掉（收下它、或者要接着往后写时先撤）。
    void dropPendingFromView();
    /// 那一排按钮上的计数与勾重新对一遍。
    void updateFilterButtons();
    /// 搜索框下面那行灰字：模型被行数上限挤掉过东西时补一句「更早的 N 行已经丢弃」。
    void updateHintLabel();
    /// 五颗开关的样式表按**现在这一份调色板**重算一遍（构造函数和 `changeEvent()` 都走它）。
    void refreshFilterButtonStyles();
    /// 空的时候该说哪句话（"还没有输出"还是"都被筛掉了"）。
    void refreshPlaceholder();

    /// 一批一批把 `m_batchLines` 摆进视图（文件查看器的大日志用，见 .cpp）。
    void pumpBatch();
    /// 新开一轮分批上屏：清视图，之后每轮事件循环摆 `fileBatchLines()` 行。
    void startBatch(QVector<RuntimeOutputModel::VisibleLine> lines);

    QLabel *m_hintLabel = nullptr;
    QLabel *m_stateBadge = nullptr;
    QLabel *m_titleLabel = nullptr;
    ElidedLabel *m_titleInstance = nullptr;
    QAction *m_externalAction = nullptr;
    /// 说明那行灰字的基础句（挤掉行的那句补在它后面）。
    QString m_baseHint;
    QString m_filePath;
    QVector<RuntimeOutputModel::VisibleLine> m_batchLines;
    int m_batchPos = 0;
    /// 每开一轮 +1：排队中的旧批次见号不符就放弃。
    int m_batchGeneration = 0;
    void setStateBadge(const QString &text, bool bad);
    QPlainTextEdit *m_view = nullptr;
    /// 正文区上面那一行搜索框。
    QLineEdit *m_searchEdit = nullptr;
    QPushButton *m_moreButton = nullptr;
    QAction *m_clearAction = nullptr;
    QAction *m_copyAction = nullptr;
    /// 底下那一排那两颗导出键。顺序就是界面上从左到右的顺序。
    QAction *m_exportLogAction = nullptr;
    QAction *m_exportReportAction = nullptr;
    QComboBox *m_lineLimitCombo = nullptr;
    /// 造这个窗时给的那个实例名（见 instanceName()）。只存不用 —— 标题那一下用完就没别处
    /// 需要它了，导出报告那一条得能取回来（那时候会话多半已经从 LauncherManager 里摘掉了）。
    QString m_instanceName;
    // 在窗口打开时确定，切换设置不触发已有海量日志重绘。
    bool m_logColors = true;

    /// 记账那一半：行、计数、档位可见、搜索词、显示上限都在这里，和服务端控制台共用。
    /// **注意显示上限是模型里的 `displayLimit()`**。
    RuntimeOutputModel m_model;
    /// 还没等到换行的那半行（不属于任何一条；模型只收完整行）。
    QString m_pending;

    QVector<QToolButton *> m_filterButtons;
    /// 上一趟算出来的"关态中性灰"（见 `refreshFilterButtonStyles()`）：`changeEvent()` 拿它
    /// 当"真的换了主题没有"的判据 —— `PaletteChange` / `StyleChange` 来得比主题变化勤得多。
    QColor m_lastSwitchOff;
    /// 末尾那一格现在装的是"半行"吗（是的话下一次写正文之前要先把它撤掉）。
    bool m_viewLastBlockIsPending = false;
    /// 自绘标题条（替系统那条白色标题栏）：标题 + 实例名 + 运行徽标 + 三颗窗口按钮；空白处可拖。
    QWidget *m_titleStrip = nullptr;
    QAbstractButton *m_maximizeButton = nullptr;
    WindowChrome *m_chrome = nullptr;
};
