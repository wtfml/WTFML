#pragma once
#include "GroupBoxFixture.hpp"
#include <functional>
/**
 * @file main.cpp
 * @brief UI 回归测试入口：使用 offscreen 验证控件交互、布局和主题。
 *
 * 主验证工程（tests/）只链 Qt6::Core/Network、不编译任何 UI 代码，所以
 * "两个下拉框"这次改动的界面部分在这里验。用 offscreen 平台插件跑，不需要真屏幕。
 *
 * 那个环境变量**不许省**。省了跑的就是**真的 windows 平台插件**（会开真窗口），
 * 那几档"只在真窗口下才量得出错值"的断言会跟着变红（`ColorScheme`、三个框的高度…）。
 * 实测：不带它 = 2276 PASS / 7 FAIL；带上它 = 全绿。
 *
 * 这个工程有一份历史偶发 `c0000374`，2026-10-24 已定位到验证环境：
 * * offscreen 插件下 Qt 的 A11y 是**恒开**的；这个工程反复建销
 *   `InstanceSettingsDialog`，A11y 路径会在析构里碰已经释放的 `QString` 数据。
 *   栈落在 `QArrayData::deref` ← `QHash<QString, QString>::~QHash` ←
 *   `InstanceSettingsDialog::~InstanceSettingsDialog()`；
 * * 对照实验（同一条 exe、同一台机器）：`main()` 里 `QAccessible::setActive(true)`
 *   连续 10/10 崩在同一个位置；保持 `false` 连续 50/50 全绿（3119/3119）。
 * * 所以 `main()` 里那一句 `QAccessible::setActive(false)` **就是隔离层**，不是
 *   “靠重链接翻身”。产品跑 windows 插件、A11y 只有读屏软件连着才开，这一段属于
 *   验证环境；它不表示产品的读屏/A11y 已验收。
 * * **另一件事**（2026-10-24 收口）：PageHeap 下那条 `InstanceSettingsDialog::m_fileDirs`
 *   的 use-after-free 不是 A11y，而是夹具里一条**上下文比被捕获变量活得久**的连接
 *   （U10b 那一段，`&dialog` 做上下文、lambda 按引用捕获那一节栈上的 `lastPath`）；
 *   悬空回调把「游戏目录」那份 `QString` 提前放掉，崩点却报在析构里。现在上下文换成
 *   同作用域的 `QObject scopeGuard`。规则：这类连接/定时器的上下文不许活得比捕获的
 *   栈变量久；证据与复验过程见 `docs/dev/TESTING.md`「历史崩溃复核」。
 * * `WTFML_SKIP_JUNCTION=1` 保留给 junction 相关的局部逃生口，但**不是**这条崩溃的
 *   解药；默认仍然要跑那一段。
 */

#include <QApplication>
#include "ArmorTrimChecks.hpp"
#include <QAccessible>
#include <QStyleFactory>
#include <QCheckBox>
#include <QComboBox>
#include <QMenu>
#include <QContextMenuEvent>  // U10L：地址那一格右键（复制完整地址）那两发
#include <QDateTime>  // I4：面板那句"已运行…"的四档话术（纯函数，按毫秒差算）
#include <QDir>  // V：行下面那行路径给的是系统分隔符（QDir::toNativeSeparators）
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QFontMetrics>  // U10k：名字那一格装不下时"画出来是什么"要现算一次
#include <algorithm>
#include <cmath>  // W12：那两档"深一点浅一点"是拿 WCAG 对比度量的（pow 那条 sRGB 曲线）
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QAbstractItemView>  // U10Q：多选那一档（selectionMode() == MultiSelection）
#include <QLabel>
#include <QLineEdit>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QImage>
#include <QKeyEvent>
#include <QEnterEvent>
#include <QShortcut>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QClipboard>  // U40b：旗帜那条「复制指令」按下去真的进了剪贴板
#include <QPointer>
#include <QProcess>      // U10O：造一个**真** junction（mklink 是个 cmd 内建命令，得起进程）
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>   // I4：底栏弹层避让那一条要问"这一块屏的可用区域"（offscreen 是 800x600）
#include <QSet>
#include <QSettings>     // W7/W8：主题落盘那一趟（读写都在一个各自独立的窝里，见那一段）
#include <QSlider>       // #74：应用级滚轮规矩也要盖住滑条（文字不透明度、着色强度…）
#include <QSpinBox>
#include <QTableWidget>  // U40c：附魔那张表的「和上一行同一件」那一列
#include <QStackedWidget>
#include <QStandardItemModel>  // Z13：rowActionTip() 只要一个真 model 的 index（不挂视图）
#include <QStyleHints>   // W2：深浅那一档是平台给的（`colorScheme()`），offscreen 上恒 Unknown
#include <QStyleOptionViewItem>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTemporaryDir>
#include <QToolButton>
#include <QHeaderView>   // U10L：那三栏的伸缩策略（第 0 栏吃剩下、1/2 收着自己那点）
#include <QHelpEvent>
#include <QHoverEvent>   // Z11：条上那两颗按钮的悬停态（悬停那一位默认没人填，得自己发）
#include <QIcon>         // U10L：服务器图标那一格（真塞一枚进去，再看它落没落在那行上）
#include <QFocusEvent>   // U11c：合成一发 FocusIn（`setFocus()` 在 offscreen 底下送不到控件上）
#include <QEvent>        // W6：合成一发「调色板变了」，量 `Theme::follow()` 那一条登记
#include <QJsonDocument>  // U12：「哪几页收起来了」那一格来回比的时候好判
#include <QJsonObject>
#include <QTimer>
#include <QEventLoop>  // ⑬：动效要**真的把事件循环跑起来**才走完那一段（sleep 不算）
#include <QGraphicsEffect>  // ⑭：滚动条露头那一下挂的那层淡入（要读它的 objectName 认人）
#include <QVariantAnimation>  // ⑬：换页那一发的**时间轴**（干等 + 淡入，见那一节）
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QVector3D>
#include <QWheelEvent>

#include <cstdio>

#include "AccountSkinDialog.hpp"
// U10a / U11 那一段：「配置文件」那一页里嵌着的那个编辑器。
// ConfigEntries 是左边那一格「条目」的取数（一条 `键 = 值` 是怎么认出来的）——
// 它是纯函数，这一层正是它该被量的地方。
#include "SettingsPageParts.hpp"
#include "ConfigEntries.hpp"
#include "ConfigFileEditorPage.hpp"
#include "ConfigFileText.hpp"
#include "GameLogWindow.hpp"
#include "GroupDialog.hpp"
#include "GroupListDelegate.hpp"  // N 段：分组设置那张组清单行尾那两颗上下移三角
#include "InlineMessage.hpp"      // O 段：内嵌提示面板（QMessageBox 的替身）
#include "InstanceGroupDelegate.hpp"
#include "InstanceFilesPage.hpp"
#include "InstanceSettingsDialog.hpp"
#include "ResourcePackPage.hpp"
#include "ToolboxPage.hpp"  // U39：信标计算 / 酿造助手那两片
#include "manage/ResourcePackOptions.hpp"
#include "IntegrityReportPage.hpp"
#include "UpdateCenterPage.hpp"  // U33：更新中心（「检查更新」查出来的结论摊成能动手的一页）
#include "JavaMismatchDialog.hpp"
#include "NewServerPage.hpp"  // U34：「新建服务端」那一页（M4）
#include "ServerConsolePage.hpp"  // U36：服务端控制台那一页（M4-3）
// U37：「服务端设置」那一整页（M4-4）—— 常用设置表单 /
// 原始文本编辑 / 世界备份 / 体检。这一页一个字节都不认 `server.properties` 的语法
// （那些全在 `src/server/ServerPropertiesForm*` 那一层，由 `MainWindow` 翻好喂进来）。
#include "ServerSettingsPage.hpp"
// U36：控制台那一页摆的那个状态字是 `ServerModel::statusName()`；那几条 EULA 的话
// （标题 / 正文 / 勾选框 / 链接）的唯一出处是 `ServerRun.hpp` —— 不在这节里另写一份。
#include "server/ServerModel.hpp"
#include "server/ServerRun.hpp"
#include "MainWindowUI.hpp"
// Z11 那两条原生消息的契约断言：拼 MSG 这件事单独一个 TU（chrome_messages.cpp），
// 免得这个一万多行的文件去 include <windows.h>（理由见那个文件的文件头）。
#include "chrome_messages.hpp"
#include "OverlayHost.hpp"  // O 段：内容区覆盖层（设置界面 / 提示都开在这儿）
#include "ResourceDetailPage.hpp"  // Z14：下载页点开一个资源之后那一整页
#include "TaskDetailPage.hpp"      // I5：任务详情页（点任务中心里的一行开出来的那一页）
// 自绘的进度条：三处（底栏 / 面板行 / 详情页）共用的那一条。
// 这一层要量的是"那三处**真的**换成了它"：按 objectName 找得到 `QProgressBar`、但
// `qobject_cast<TaskProgressBar*>` 是空指针，就是漏换了一处（画出来还是原生那个样子）。
#include "TaskProgressBar.hpp"
#include "RowActivation.hpp"
// Z15：「启用 / 禁用」那枚开关（画出来的，所以下面要量像素）和 MOTD 那套配色
// （照游戏的样子画 MOTD 那一格）。
#include "MotdText.hpp"
#include "ToggleSwitch.hpp"
#include "SkinModel.hpp"
#include "SkinViewer.hpp"
// W：主题那一层（背景 / 文字不透明度 / 每一块骨架能不能单独换）。这一层是从各处那几句
// `darker()` 里收出来的，所以量的是"收得对不对、能不能整套换"，不是"某个色好不好看"。
#include "Animations.hpp"  // 动效总开关：这个工程把它关掉
#include "Theme.hpp"
// W11：主题里那两张背景图（主页左栏 / 页面背景）。这一层量的是"没设图时一个像素都不画"
// 和"设了图真的画上去了"—— 两件事都只能用一像素一像素比（见那一段开头的说明）。
#include "ThemeBackground.hpp"
#include "TitleBar.hpp"       // Z11：顶上那条自己画的标题栏
#include "VersionIcons.hpp"
#include "WindowChrome.hpp"   // Z11：`resizeBorder()` —— buttonInset() 要对着它比
#include "WorldSettingsPage.hpp"  // Z16：点存档那支笔之后那一页（世界规则表）
// Z16：规则行上盖的那层官方中文名（表和"认不出来原样回自己"那一条都在这儿量）。
#include "GameRuleNames.hpp"
#include "VersionTreeDelegate.hpp"
#include "DesignDefaults.hpp"
#include "download/ResourceCatalog.hpp"
// 更新检测那三档话术的唯一出处。概览页那颗「检查更新」的 tooltip 里写死的
// 「说不清」必须和它**是同一个字** —— 两边都在同一个 TU 里，这条等号才钉得住。
#include "download/ModDependencies.hpp"
// 「游戏窗口大小」那几个上下限在两侧各写了一遍（ui/ 不许 include src/，见
// MainWindowUI.hpp 里的说明），所以"两边必须一样"这件事只有把两个头放进同一个 TU 才
// 钉得住。这里只调它的**内联静态函数**，不构造 LauncherSettings，所以不需要编那个 .cpp。
#include "settings/LauncherSettings.hpp"



inline int g_pass = 0;
inline int g_fail = 0;

/// child 是不是 ancestor 的后代（含自身）。
inline bool isDescendantOf(QWidget *child, QWidget *ancestor) {
    for (QWidget *w = child; w != nullptr; w = w->parentWidget()) {
        if (w == ancestor) {
            return true;
        }
    }
    return false;
}

/// 「某一项那一整格」是不是被藏起来了。判据落在**这一项自己那一格**上（addOverrideRow
/// 建的 cell：选项行 choiceRow 的父控件），**不是**往上一直走到对话框 ——
/// NOTE: 往上走到对话框那条判据是假的：这一页在 QStackedWidget 里，不是当前那一页时
/// 整页都是 hidden，于是"隔壁那一项也藏着"。而 `hide()` 又不往下传染（父控件藏了，
/// 子控件自己的 isHidden() 仍是假），所以只能问**那一格本身**。
inline bool rowCellHidden(QWidget *dialog, const QString &field) {
    auto *choiceRow = dialog != nullptr
                          ? dialog->findChild<QWidget *>(InstanceSettingsDialog::choiceRowName(field))
                          : nullptr;
    return choiceRow != nullptr && choiceRow->parentWidget() != nullptr &&
           choiceRow->parentWidget()->isHidden();
}

inline void check(bool ok, const QString &what, const QString &detail = QString()) {
    if (ok) {
        ++g_pass;
        std::printf("  PASS  %s\n", qPrintable(what));
    } else {
        ++g_fail;
        std::printf("  FAIL  %s   %s\n", qPrintable(what), qPrintable(detail));
    }
    std::fflush(stdout);
}

/// 防漏判据：一块顶层浮层（`Qt::Popup` / `Qt::ToolTip`）必须无边框 + 半透明。
/// 缺无边框，Windows 不给建分层窗口，圆角外那圈没人画的像素就是直角黑框（第三轮反馈第 4 条）。
/// 返回不合格的理由，空串 = 合格或不归这条管。
/// 白名单只有一条：Qt 自建的弹层（QMenu、QComboBox 的弹层容器、QCompleter 的补全列表）由
/// `Popups::install()` 装的过滤器统一接手，本进程没调过 `Theme::apply()` 时它们保持 Qt 原样，不算。
inline QString popupWindowProblem(const QWidget *widget) {
    const Qt::WindowType type = widget->windowType();
    if (type != Qt::Popup && type != Qt::ToolTip) {
        return QString();
    }
    const bool qtBuilt = qobject_cast<const QMenu *>(widget) != nullptr ||
                         qobject_cast<const QAbstractItemView *>(widget) != nullptr ||
                         qobject_cast<const QComboBox *>(widget->parentWidget()) != nullptr;
    if (qtBuilt && qApp->findChild<QObject *>(QStringLiteral("wtfmlMenuGlass"), Qt::FindDirectChildrenOnly) == nullptr) {
        return QString();
    }
    QStringList missing;
    if (!widget->windowFlags().testFlag(Qt::FramelessWindowHint)) missing << QStringLiteral("无边框");
    if (!widget->testAttribute(Qt::WA_TranslucentBackground)) missing << QStringLiteral("半透明");
    if (missing.isEmpty()) return QString();
    return QStringLiteral("%1(%2) 缺%3").arg(QString::fromLatin1(widget->metaObject()->className()),
                                             widget->objectName(), missing.join(QStringLiteral("、")));
}

/// 防漏守卫：装在 QApplication 上，本进程任何一块顶层浮层上屏（Show）都过一遍
/// `popupWindowProblem()`；进程收尾时 `report()` 记一条断言。按需现建的浮层
/// （WA_DeleteOnClose）在某一时刻遍历 `topLevelWidgets()` 抓不全，所以挂在 Show 上。
class PopupWindowGuard : public QObject {
public:
    explicit PopupWindowGuard(QApplication &app) : QObject(&app) { app.installEventFilter(this); }
    void report() const {
        if (m_shown == 0) return;
        check(m_problems.isEmpty(),
              QStringLiteral("防漏：本进程 %1 次顶层浮层上屏都是无边框 + 半透明").arg(m_shown),
              m_problems.join(QStringLiteral("; ")));
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Show || !watched->isWidgetType()) return false;
        auto *widget = static_cast<QWidget *>(watched);
        const Qt::WindowType type = widget->windowType();
        if (type != Qt::Popup && type != Qt::ToolTip) return false;
        ++m_shown;
        const QString problem = popupWindowProblem(widget);
        if (!problem.isEmpty() && !m_problems.contains(problem)) m_problems << problem;
        return false;
    }

private:
    int m_shown = 0;
    QStringList m_problems;
};

/// 上一个被关掉的提示说了什么（标题 / 正文）。产品规矩是"出错必须弹窗"，
/// 所以"弹没弹、弹的是什么"本身就是要验的东西，不只是为了把测试跑通。
inline QString g_modalTitle;
inline QString g_modalText;

/// 下一条内嵌提示的「票号」：只让**最近一次** `dismissNextInline()` 的两发定时器起作用，
/// 而且**兑现一次就作废**。
///
/// NOTE: 不这么做就会串到**下一条**提示上：300 毫秒那一发是兜底，可它并不会因为 0 毫秒
/// 那一发已经把这条答完而自己撤掉 —— 后面 300 毫秒内再开出来的下一条提示，会被这条
/// 早先排着的兜底当成自己的「用户答了」，而且按的是**它自己**那一号按钮（默认第 0 颗 =「是」）。
/// 实测症状：UiGroupEditorChecks 里「恢复默认分组设置…」那一条点的是第二颗（「否」），
/// 磁盘上却真的换成了默认那五组（`答「否」的时候一个组都没换` 那条变红），
/// 只有在 PageHeap 那种慢一档的环境里才看得到。
inline int &dismissTicket() {
    static int ticket = 0;
    return ticket;
}

/// 让下一条**内嵌提示**自己点掉，并把它说了什么记下来。
///
/// NOTE: 两条 timer 都必要：0 毫秒那条在嵌套事件循环一进去就触发；
/// 300 毫秒那条是兜底（提示是先摆好再进循环的，个别平台上第一条可能跑在
/// 它真正露出来之前）。两条都响时后一条找不到东西，直接返回。
///
/// NOTE: 关它走的是**点第一颗按钮**，不是 `close()`。`QDialog::closeEvent` 只在
/// `isVisible()` 为真时才 `reject()` —— 而在 offscreen 里这一层有可能整个是
/// 藏着的，那样 close() 会变成"什么都没发生"，测试就挂死在嵌套循环里。
/// 点按钮走的是 `clicked → accept() → finished`，看不看得见都作数。
inline void dismissNextInline(OverlayHost *host, int buttonIndex = 0) {
    g_modalTitle.clear();
    g_modalText.clear();
    // 300 毫秒那一发是兜底：它可能在主窗口/覆盖层已经析构之后才醒。捕获裸指针会在
    // `host->findChild()` 上碰已经释放的 QWidget（历史 c0000374 的另一半就在这里）。
    // 用 QPointer + host 作为定时器 context：宿主没了，两发都自动取消。
    const QPointer<OverlayHost> guard(host);
    const int ticket = ++dismissTicket();
    const auto dismiss = [guard, buttonIndex, ticket] {
        // 宿主没了 / 这一号已经作废（答过了，或者又被新的一次 `dismissNextInline()`
        // 顶掉了）⇒ 一个键都不能按：按下去的是**别人**那条提示。
        if (guard.isNull() || ticket != dismissTicket()) {
            return;
        }
        auto *box = guard->findChild<InlineMessageBox *>();
        if (box == nullptr) {
            return;
        }
        // 兑现一次就把这一号作废（下面 300 毫秒那一发跑起来时就不再做第二遍）。
        ++dismissTicket();
        if (auto *title = box->findChild<QLabel *>(InlineMessageBox::titleLabelName())) {
            g_modalTitle = title->text();
        }
        if (auto *text = box->findChild<QLabel *>(InlineMessageBox::textLabelName())) {
            g_modalText = text->text();
        }
        if (auto *button = box->findChild<QPushButton *>(InlineMessageBox::buttonName(buttonIndex))) {
            button->click();
        } else {
            box->close();
        }
    };
    if (!guard.isNull()) {
        QTimer::singleShot(0, guard, dismiss);
        QTimer::singleShot(300, guard, dismiss);
    }
}

/// 仓库根目录：给"扫源码"的断言用。
///
/// 不要拿 `__FILE__` 往上推 —— ccache 的 `CCACHE_BASEDIR` 会把 `__FILE__` 改写成相对路径
/// （跨工作树命中缓存靠的就是这个），推出的根就成了构建目录。CMake 把绝对路径传进来
/// （`WTFML_REPO_ROOT`，见 tests/ui_check/CMakeLists.txt）。
inline QDir repoRoot() {
#ifdef WTFML_REPO_ROOT
    return QDir(QStringLiteral(WTFML_REPO_ROOT));
#else
    return QDir(QFileInfo(QStringLiteral(__FILE__))
                    .absoluteDir()
                    .absoluteFilePath(QStringLiteral("../../..")));
#endif
}

/// 把控件"现在长什么样"抓成一张图 —— 抓出来的是**逻辑像素**，跟控件的坐标同一个尺度。
inline QImage captureWidget(QWidget *widget) {
    QImage image(widget->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget->render(&painter);
    return image;
}

/// 先铺一层**不透明**的底再抓：7a 令牌里按钮 / 输入框的底是半透明的，不垫底的话抓出来是
/// 半透明像素（透明那一格读成 #000000，数"深像素"时整块都算进去）。
inline QImage captureWidgetOn(QWidget *widget, const QColor &under) {
    QImage image(widget->size(), QImage::Format_ARGB32);
    image.fill(under);
    QPainter painter(&image);
    // 不带 DrawWindowBackground：那一档会先拿 backgroundRole 再铺一遍，半透明底就叠成两层（屏幕上没有这一层）。
    widget->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}

/// source-over：半透明的 @p top 叠在不透明的 @p under 上，屏幕上看到的那个色。
inline QColor overOpaque(const QColor &top, const QColor &under) {
    const qreal a = top.alphaF();
    return QColor(qRound(top.red() * a + under.red() * (1 - a)),
                  qRound(top.green() * a + under.green() * (1 - a)),
                  qRound(top.blue() * a + under.blue() * (1 - a)));
}

/// 一块图里**出现次数最多的那个颜色**（按整块数票）。
///
/// 为什么这么量（日志窗那几条底色断言）：一行里既有底色又有字，而字是另一种
/// 颜色 —— 直接取某一个像素太看运气（那一个点可能正好落在字形上、也可能落在行间空白上）。
/// 底色是**铺满那一行**的，所以按整块数票，多数那一票一定是它。
/// 顺带把"行高里到底铺了多少"这种样式细节也一起甩掉了：铺满也好、只铺字形那一圈也好，
/// 底色都是多数。
inline QColor dominantColor(const QImage &image, const QRect &area) {
    const QRect clip = area.intersected(image.rect());
    if (clip.isEmpty()) {
        return QColor();
    }
    QHash<QRgb, int> votes;
    QRgb best = 0;
    int bestCount = 0;
    for (int y = clip.top(); y <= clip.bottom(); ++y) {
        for (int x = clip.left(); x <= clip.right(); ++x) {
            const QRgb rgb = image.pixel(x, y);
            const int count = ++votes[rgb];
            if (count > bestCount) {
                bestCount = count;
                best = rgb;
            }
        }
    }
    return QColor(best);
}

/// 两个颜色差多远（三通道绝对值之和）。判"这两个色看得出来不一样"用。
inline int colorDistance(const QColor &a, const QColor &b) {
    return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue());
}

/// 这个控件装上了滚轮防护吗 —— 判据是**焦点策略**，不是"滚轮事件被吃掉了吗"。
///
/// 为什么要单独验这一条：防护本身的判据是 `!hasFocus()`，而 Qt 对**真滚轮
/// 事件**（spontaneous）是"先按焦点策略给焦点、再派发"（qapplication.cpp:2887 →
/// giveFocusAccordingToFocusPolicy() → :3694/:3732），偏偏 QComboBox 和 QAbstractSpinBox
/// 的默认焦点策略就是 Qt::WheelFocus（qcombobox.cpp:1066 / qabstractspinbox.cpp:1625）——
/// 于是过滤器跑起来时 hasFocus() 已经是 true，防护一次都不生效。
///
/// E2 那两条用的是 QApplication::sendEvent()（合成事件），走的是 qapplication.cpp 里
/// **不给焦点**的那条岔路，所以照不出这个病（探针里 hasFocus() 始终是 false，防护"看起来
/// 生效"）。真滚轮没法在探针里合成，于是这里改验**前提**：焦点策略里不许含 WheelFocus
/// 那一位 —— shouldSetFocus() 的判据就是 `(focusPolicy & policy) == policy`，
/// 不含 WheelFocus 就抢不走焦点。
///
/// 顺带守住"别改坏正常操作"：改完策略 Tab 还得走得到、点还得能聚焦。
/// @return 有问题返回一句人话；没问题返回空串。
inline QString wheelGuardProblem(QWidget *widget) {
    if (widget == nullptr) {
        return QStringLiteral("控件没找到");
    }
    const int policy = static_cast<int>(widget->focusPolicy());
    const int wheel = static_cast<int>(Qt::WheelFocus);
    if ((policy & wheel) == wheel) {
        return QStringLiteral("焦点策略含 WheelFocus 那一位（真滚轮照样抢焦点，防护是哑的）");
    }
    if ((policy & static_cast<int>(Qt::TabFocus)) != static_cast<int>(Qt::TabFocus)) {
        return QStringLiteral("Tab 走不到它了（改策略时误伤）");
    }
    if ((policy & static_cast<int>(Qt::ClickFocus)) != static_cast<int>(Qt::ClickFocus)) {
        return QStringLiteral("点它不再能聚焦（改策略时误伤）");
    }
    return QString();
}

  // namespace



int UiInstallLayoutCheck(QApplication &app);
int UiToolboxPolishCheck(QApplication &app);
int UiToolboxLayoutCheck(QApplication &app);
int UiWindowSizeCheck(QApplication &app);
int UiInputWidthCheck(QApplication &app);
/// 子页面 / 弹框 / 浮层的漏接设计系统巡检：计入的残留不为 0 就失败（守卫）。
int UiSurfaceSweepCheck(QApplication &app);
int UiResourcePacksCheck(QApplication &app);
int UiLifecycleCheck(QApplication &app);
/// 开关程序「雾散开」（ui/WindowMotion）：开场时间线终态、关窗合雾的拦截 / 放行、换页幕布负偏移。
int UiWindowMotionCheck(QApplication &app);
int UiUiRefreshCheck(QApplication &app);
/// 文件页的行由代理画：60 行不建 60 套控件、点开关真改盘上文件名、刷新不跳滚动。
int UiFilesRowsCheck(QApplication &app);
int UiSettingsShotsCheck(QApplication &app);
int UiFoldMotionChecks(QApplication &app);
int UiLoadingStateChecks(QApplication &app);
/// 设计系统默认生效：光秃秃的列表 / 文本框 / 栈拿到默认、退出口、手动接线不重播。
int UiDesignDefaultsChecks(QApplication &app);
int UiReviewShotsCheck(QApplication &app);
// 主页「每日一言」：那一行（进 CTest）与它的离屏样张（不进 CTest）。
int UiDailyQuoteCheck(QApplication &app);
int UiDailyQuoteShots(QApplication &app);
int UiLongInstanceNameChecks(QApplication &app);
int UiLongInstanceNameShots(QApplication &app);
int UiLoadingStateShots(QApplication &app);
int UiDetailChipShots(QApplication &app);
int UiHomeGreetingShots(QApplication &app);
int UiAccountKindShots(QApplication &app);
// 自定义背景图的离屏样张（设置页那一组 + 设了图之后的主页与设置页）。
int UiBackgroundShots(QApplication &app);
int UiThemeCheck(QApplication &app);
int UiPopupCheck(QApplication &app);
int UiNavigationChecks(QMainWindow &window, MainWindowUI &ui, QComboBox * &installed, QPushButton * &startBtn, QStackedWidget * &gameStack, QTreeWidget * &versionTree, QLineEdit * &installNameEdit, QPushButton * &installBtn, QPushButton * &installBackBtn, QTreeWidget * &loaderTree, QListWidget * &folderList, QTreeWidget * &instanceList, QLabel * &instanceFolderLabel, QTabWidget * &tabs);
// 界面语言那一条链路：解析纯函数 + 装翻译后导航轨变英文（见 UiLanguageChecks.cpp）。
int UiLanguageChecks(MainWindowUI &ui, QTabWidget *tabs);
int UiVersionTreeGeometryChecks(QMainWindow &window, QTreeWidget * &versionTree);
int UiInstallFormLayoutChecks(MainWindowUI &ui, QLineEdit * &installNameEdit, QLabel * &installNameError, QPushButton * &installBtn, QTreeWidget * &loaderTree, QWidget * &downloadPage, const QRect &rEdit, const QRect &rInstall, QFrame * &loaderCard, QFrame * &installFooter, const QByteArray &surfaceKey, const std::function<QRect(QWidget *)> &rectInPage);
int UiDownloadCategoriesChecks(MainWindowUI &ui, QStackedWidget * &gameStack, QWidget * &downloadPage, const std::function<QRect(QWidget *)> &rectInPage);
int UiDownloadTypographyChecks(QMainWindow &window, MainWindowUI &ui);
int UiTaskSummaryChecks(MainWindowUI &ui);
int UiBottomBarGeometryChecks(MainWindowUI &ui, QFrame * &bar);
int UiTaskCenterChecks(QMainWindow &window, MainWindowUI &ui, QTabWidget * &tabs);
int UiTaskActionsChecks(QMainWindow &window, MainWindowUI &ui);
int UiBottomBarPopoversChecks(QMainWindow &window, MainWindowUI &ui, QTabWidget * &tabs);
int UiRunStateColorsChecks();
int UiListRowDelegateChecks();
int UiColumnProportionsChecks(QComboBox * &installed, QPushButton * &startBtn, QListWidget * &folderList, QTreeWidget * &instanceList, QTabWidget * &tabs, const std::function<QString(QWidget *)> &surfaceValueOf, const std::function<bool(QWidget *, const QString &)> &inSurface, const std::function<QFrame *(QWidget *, const QString &)> &surfaceOf);
int UiSettingsLayoutChecks(MainWindowUI &ui, QTabWidget * &tabs);
int UiInstanceGroupingChecks(MainWindowUI &ui, QTreeWidget * &instanceList);
int UiGroupEditorChecks();
int UiInstanceOverridesChecks();
int UiContentPagesChecks();
int UiServerPageChecks(InstanceSettingsDialog &dialog);
int UiBatchManagementChecks(const QString &instanceDisplayName, const QString &instanceDirName,
                            const InstanceSettingsDialog::GlobalValues &global);
int UiDownloadNewChecks(InstanceSettingsDialog &dialog, const QString &instanceDisplayName,
                        const QString &instanceDirName,
                        const InstanceSettingsDialog::GlobalValues &global);
int UiEmptyPageChecks(const QString &instanceDisplayName, const QString &instanceDirName,
                      const InstanceSettingsDialog::GlobalValues &global, const QString &root);
int UiQuickPlayChecks(InstanceSettingsDialog &dialog, const QString &instanceDisplayName,
                        const QString &instanceDirName,
                        const InstanceSettingsDialog::GlobalValues &global);
int UiUpgradePageChecks(QApplication &app);
int UiSearchFilterChecks(InstanceSettingsDialog &dialog, const QString &modsDir);
int UiScrollAnchorChecks(const QString &instanceDisplayName, const QString &instanceDirName,
                         const InstanceSettingsDialog::GlobalValues &global);
int UiElidedNameChecks(const QString &instanceDisplayName, const QString &instanceDirName,
                       const InstanceSettingsDialog::GlobalValues &global, const QString &root);
int UiMediumPagesChecks(InstanceSettingsDialog &dialog, const QString &root,
                             const QString &savesDir);
int UiToggleChecks(InstanceSettingsDialog &dialog, const QString &modsDir);
int UiDirectoryButtonsChecks(InstanceSettingsDialog &dialog);
int UiStructureSourcesChecks(const QString &instanceDisplayName, const QString &instanceDirName,
                             const InstanceSettingsDialog::GlobalValues &global);
int UiServerListChecks(InstanceSettingsDialog &dialog);
int UiRowButtonsChecks(InstanceSettingsDialog &dialog, const QString &modsDir);
int UiOverviewButtonsChecks(InstanceSettingsDialog &dialog, const QString &root,
                               const QHash<QString, QString> &dirs);
int UiPageIdentityChecks(InstanceSettingsDialog &dialog);
int UiDetailEntryChecks(const QString &instanceDisplayName, const QString &instanceDirName,
                        const InstanceSettingsDialog::GlobalValues &global);
int UiRowActionsChecks(const QString &instanceDisplayName, const QString &instanceDirName,
                      const InstanceSettingsDialog::GlobalValues &global);
int UiConfigEditorChecks();
int UiSkinMeshChecks();
int UiAccountSkinChecks();
int UiWindowPreferencesChecks(QTabWidget * &tabs);
int UiGameLogActionsChecks(QFrame * &bar);
int UiGameLogPlacementChecks();
int UiLogClassificationChecks();
int UiLogFilteringChecks();
int UiLogColorsChecks();
int UiLogTogglesChecks();
int UiLogSeparatorsChecks();
int UiLogRetentionChecks();
int UiFolderDisplayChecks(MainWindowUI &ui, QListWidget * &folderList, QTabWidget * &tabs);
int UiOverlayGeometryChecks(QMainWindow &window, MainWindowUI &ui, QTabWidget * &tabs, QFrame * &bar);
int UiOverlayNavigationChecks();
int UiLaunchPromptsChecks(MainWindowUI &ui);
int UiTitleBarChecks(QMainWindow &window, MainWindowUI &ui);
int UiInstanceRowActionsChecks();
int UiInstanceRowPaintingChecks();
int UiContentRowsChecks();
int UiWorldEditorChecks();
int UiThemePaletteChecks(QApplication &app, QTabWidget * &tabs, QFrame * &bar);
int UiAnimationsChecks(QApplication &app, QFrame * &bar);
int UiScrollBarsChecks(QApplication &app, QFrame * &bar);
int UiIntegrityReportChecks();
int UiCrashReportChecks();
int UiBackupActionsChecks();
int UiInstanceSnapshotsChecks();
int UiInstanceFiltersChecks(QMainWindow &window, MainWindowUI &ui, QTabWidget * &tabs);
int UiUpdateCenterChecks();
int UiServerModeChecks(QMainWindow &window, MainWindowUI &ui, QTabWidget * &tabs);
int UiServerSettingsChecks();
int UiAccessibilityChecks(QMainWindow &window);
int UiBeaconBrewingChecks();
int UiBannerArmorChecks();

int UiUpgradeReportChecks(QApplication &app);

int UiUpgradeHistoryChecks(QApplication &app);
int UiTopLevelDialogChecks(QApplication &app);

/// 外框在真 HWND 上的行为（专门模块 ui_check_window_chrome，平台插件要 windows）。
int UiWindowChromeCheck(QApplication &app);
// 主页启动卡片（#65）：卡片几态、W 循环接缝与失败铺满转灰（见 UiLaunchCardChecks.cpp）。
int UiLaunchCardChecks(QApplication &app);


/// 智能体页/设置页的 offscreen 检查（专门模块 ui_check_agent）。
int UiAgentCheck();
