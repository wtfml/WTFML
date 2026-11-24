#pragma once
// Private page scaffold and shared form primitives, not a public UI contract.
#include "MainWindowUI.hpp"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>

namespace MainWindowPageLayout {
QLabel *makeHomeTitle(const QString &text, QWidget *parent);
QLabel *makeHomeHint(const QString &text, QWidget *parent);
/// 每个选项卡页面共用的边距：贴着选项卡内容区留一圈呼吸空间。
/// **值本身在 MainWindowUI::pageMargin()** —— 内容区那个覆盖层（OverlayHost）也是一整个
/// "页面"，它留白得和别的页面一样，所以那一边读的是同一个出处。
inline const int kPageMargin = MainWindowUI::pageMargin();

/// 左右两栏之间的间距（= 令牌里的"区块间距"那一档）。
inline constexpr int kColumnSpacing = 16;

/// 宽屏下“选择/导航”侧栏按比例无限长大是不允许的。默认 900 px 窗口里各栏还没碰到这些上限，
/// 所以 7:3 / 3:7 / 2:8 的比例完全保留；只有窗口明显拉宽后，多出来的空间才全部交给
/// 真正承载内容的那一栏。
inline constexpr int kNarrowSideMaxWidth = 260;

inline constexpr int kRichSideMaxWidth = 420;

// ---------- 各处手调的尺寸 ----------
// 集中放这里是为了"改外观只需看这一处"：这些数值是照着当前样式（Fusion）的控件度量
// 调出来的，换背景色/字体/控件高度时就是这一组要重估。散在各自的构建代码里每次都得
// 全文件搜一遍。
//
// NOTE: 设置页那个数字框的宽度（kMemorySpinWidth）不在这里 —— 它只服务
// addInlineControl() 一个调用点，留在它旁边更好找。

/// 底栏整条的高度。给固定值：栏里三格的内容高度都是自己定的，跟着窗口高变没有意义。
inline constexpr int kBottomBarHeight = 62;

/// 进度文字那格（"已下载 1234 / 3580 xxx.jar"）的宽度下限，免得**文件名一换整条就左右跳**。
/// 它管的是底栏**中间那一格自己**的宽度，不是左右两格的 —— 左右那两格用的是
/// MainWindowUI::bottomBarRunStateColumnWidth() / bottomBarAccountColumnWidth()。
inline constexpr int kStatusLabelMinWidth = 96;
/// 设置页「标签 | 控件」那一行左边标签那一列的宽。
inline constexpr int kSettingsLabelWidth = 148;

/// 左下角头像的边长（正方形）。**值本身不在这里** —— 它和"主窗口拿什么尺寸去裁头像"
/// 必须是同一个数，所以唯一的出处是 MainWindowUI::avatarSize()（见头文件）。
inline const int kAvatarSize = MainWindowUI::avatarSize();

/// 「启动游戏」按钮的高度。两行文字仍有足够呼吸空间，但不占掉启动卡近一半高度。
inline constexpr int kStartButtonMinHeight = 76;

/// 主窗口默认大小（首次启动、设置页「恢复默认」）：1024 宽刚好不触发导航轨的窄窗强制折叠。
inline constexpr int kMainWindowDefaultWidth = 1024;
inline constexpr int kMainWindowDefaultHeight = 620;
/// 主窗口最小大小：工具箱目录页和各工具页、主页在这个大小下整页放得下、不出纵向滚动条，
/// 设置四个页签没有横向溢出。高度看工具箱「旗帜」页：585 起整页放得下（584 差 1px），再留 5px 给 125% / 150% 缩放取整和字体回退。
inline constexpr int kMainWindowMinWidth = 900;
inline constexpr int kMainWindowMinHeight = 590;

/// 主页立绘画布：宽 = 右栏宽（唯一出处 `MainWindowUI::homeRightColumnWidth()`），高按 SkinViewer
/// 的人物比例算出来约 322 —— 人高约 270，和设计稿 home.html 那个立绘一样大。窄窗封顶到七成二。
inline constexpr int kHomeSkinViewerWidth = MainWindowUI::homeRightColumnWidth();
inline constexpr int kHomeSkinViewerHeight = 322;
/// 主页两栏的左右页边距（相等），和左栏里再往里缩的那一截：合起来是设计稿标题 / 启动区离页左 44。
inline constexpr int kHomeHeroMargin = 36;
inline constexpr int kHomeHeroInset = 8;
/// 右上账号胶囊的宽（设计稿按内容约 180；名字长了在里面省略）。
inline constexpr int kHomeAccountPillWidth = 184;
inline constexpr int kHomeAccountPillHeight = 44;
/// 实例带卡片的宽 / 高（设计稿 .card：196 宽，封面 48 + 上下 8）。
/// 196 是**下限**、320 是上限：卡片宽度跟着实例名走（名字整句放得下就宽一点，放不下
/// 才在 320 那档省略），一排里不给它分"剩下的宽度"。
inline constexpr int kHomeTileWidth = 196;
inline constexpr int kHomeTileMaxWidth = 320;
inline constexpr int kHomeTileHeight = 64;
/// 主页启动区卡片底下给「启动游戏」柔光留的余量（光晕向下偏 10、模糊 30）。
inline constexpr int kHomeLaunchGlowRoom = 24;

/// 主页左栏两张卡之间的竖直留白（「开始游戏」和「上次游玩」）。
///
/// 不走 `layout->setSpacing()`：那只管"相邻两项之间"，而这一栏里还有两段 `addStretch`，
/// 它们和卡片之间也会按 spacing 拉开 —— 那两段留白是这一栏"居中"的算法本身，动不得。
/// 主页快速区是一组连续操作，不应像两张互不相干的大卡片那样隔开。
/// 外层布局本身已有 spacing，不额外塞空白，让「收藏 / 上次游玩」视觉上连成一组。
inline constexpr int kHomeCardGap = 0;
/// 主页大标题的字号（设计稿 home.html：细楷 50，窄窗 40）。
inline constexpr int kHomeHeroTitlePixelSize = 50;
/// 大标题下那行「每日一言」的字号区间：按句长在区间里取放得下整句的最大一号，
/// 到下限还放不下就收省略号。宽窗 18–24，窄窗（<1024）16–20。行高按上限预留，换句不跳。
inline constexpr int kHomeQuoteMinPixelSize = 18;
inline constexpr int kHomeQuoteMaxPixelSize = 24;
inline constexpr int kHomeQuoteNarrowMinPixelSize = 16;
inline constexpr int kHomeQuoteNarrowMaxPixelSize = 20;
/// 主页「当前实例」名的字号区间（文楷）：长名先收字号，到下限还放不下才收省略号；行高按上限预留。
/// 下限不低于页标题的 26，再小就和眉题、元信息那几行分不出主次。
inline constexpr int kHomeCurrentNameMinPixelSize = 28;
inline constexpr int kHomeCurrentNameMaxPixelSize = 40;

/// 「额外 JVM 参数」多行输入框的高度，约三四行的样子；再高会把下面那几项挤出可视区。
inline constexpr int kJvmArgsEditHeight = 72;

/// 几个按钮的宽度下限：文案长短差得多（「安装」/「应用」这种），
/// 不给下限就会各自缩成一小块、排在一起长短不齐。
inline constexpr int kDownloadButtonMinWidth = 96;

/// 「账号与外观…」那个入口按钮的最小宽度。文案固定，但给一个下限，免得以后换文案时
/// 按钮缩成一小块。**主页那个和设置页那个共用这一个值** —— 它们说的是同一件事。
inline constexpr int kAccountSkinButtonMinWidth = 140;

inline constexpr int kApplyJvmArgsButtonMinWidth = 72;

/// 下载页每一页顶上那一行筛子里，控件之间（以及那一行里两行之间）的间距
/// = 令牌里的"控件间距"那一档（见 `ui/Theme.hpp` 的尺寸令牌那一节）。
/// 比 kColumnSpacing（16）小一档：这一行里的东西是一组，挨着摆才像一组。
inline constexpr int kFilterRowSpacing = 8;

/// 侧栏那一列的宽度。**唯一出处**（`MainWindowUI::sidebarWidth()`）—— 规格是
/// 208~220 px，取中间那个整数。
///
/// NOTE: 导航那一列不按份数分宽：它本来就不是"和正文抢宽度"的东西，按比例分的话
/// 窗口一拉宽它也跟着长，多出来的全成了空白（这正是"左侧导航 / 右侧正文
/// 缺少主次关系"那条）。主页**不**走这一档 —— 它左边是"选版本 + 启动"，那是正文，
/// 仍然是 7:3 的份数。
inline const int kSidebarWidth = MainWindowUI::sidebarWidth();

/// 正文面（`wtfmlSurface="workspace"`）里那一圈内边距 = 令牌里的"区块间距"那一档。
///
/// 与页面外边距保持接近但略小：面内紧凑，面与窗口之间留出更明确的呼吸空间。
inline constexpr int kSurfacePadding = 18;

/// 页头两行之间的间距。
inline constexpr int kHeaderSpacing = 3;

/// 设置页里数字框的宽度（够写「65536 MB」再加一对上下箭头）。
inline constexpr int kMemorySpinWidth = 130;

/// 内存那根条的最短宽度。太短就拖不准：32 GB 的机器上那张档位表有五十多格，
/// 260 px 摊下来一格才 5 px 上下（够点得着），再窄就要靠键盘挪档了。
inline constexpr int kMemorySliderMinWidth = 260;

/// 「内存分配」那一行最右边那颗下拉框的宽度：装得下「手动分配」四个字 + 一对箭头。
inline constexpr int kMemoryModeComboWidth = 116;

/// 「游戏窗口」那一行两个数字框的宽度：装得下「7680」（上限）加一对箭头就够了 ——
/// 它没有后缀，比 kMemorySpinWidth 窄一些才不像两个空盒子。
inline constexpr int kWindowSizeSpinWidth = 90;

/// 「游戏窗口」那一行中间那个「×」。
inline constexpr int kWindowSizeRowSpacing = 8;

/// 一栏：控件本身 + 它自己的竖排布局 + 这一栏那个"面"，右栏还多一个页头。
struct Column {
    QWidget *widget = nullptr;
    QVBoxLayout *layout = nullptr;
    /// 这一栏那个"面"（侧栏是 `wtfmlSurface="sidebar"`，正文是 `"workspace"`）。
    QFrame *surface = nullptr;
    /// 页头（**只有正文那一栏有**，侧栏是 nullptr）。里面是标题 + 说明 + 右上角操作。
    QWidget *header = nullptr;
    /// 页头里那两行字。
    QLabel *title = nullptr;
    QLabel *subtitle = nullptr;
    /// 页头右上角那一条（页面级操作塞这儿）。
    QHBoxLayout *actions = nullptr;
};

/// 一个页面的左右两栏（= 统一页面骨架 `PageScaffold`）。
struct TwoColumns {
    Column left;
    Column right;
};

/// 设置页里一个小节：**一个标题 + 一张内容卡片**。
///
/// 小节用"标题 + 内容卡片"，不用传统
/// `QGroupBox` 的缺口式标题。一节 = 一张 `wtfmlSurface="section"` 的卡片
/// （浅一档的底 + 10 px 圆角，**不画边**），标题和这一节所有的项**都在卡片里面**。
///
/// 为什么不用「一行粗体标题横跨两列、下面接着几行设置项」那种排法：标题横跨整幅、
/// 项也横跨整幅，两者之间只有"挨着"这一层关系 —— 一项到底是哪一节的，得靠"往上找
/// 最近那个粗体行"去猜；一节一张卡则是**装在里面**的关系，一眼就是。顺带卡片自己还会把内容收进一圈内边距里
/// （18/17/18/18），"说明文字一行跨得太长"那件事也跟着好一档（另有 820 的宽度上限，
/// 见 `settingsFormMaxWidth()`）。
struct SettingsSection {
    QString title;
    /// 这一节的卡片（`wtfmlSurface="section"`）。**它就是项们的父窗**。
    QFrame *card = nullptr;
    /// 卡片里那条竖排布局。项按顺序往里加（见 `addSettingItem()`）。
    QVBoxLayout *layout = nullptr;
};

/// `makeHintedControl()` 的"控件 + 它那层竖排布局"一起拿：设置页里 29 处都是这两样
/// 一起用，分开写就要在每一处再 qobject_cast 一次。
struct HintedControlWithLayout {
    QWidget *widget = nullptr;
    QVBoxLayout *layout = nullptr;
};

/// 一行"无外边距的横排"：设置页里那些「条 + 数字框 + 下拉框」和主题色块行共用。
struct InlineRow {
    QWidget *widget = nullptr;
    QHBoxLayout *layout = nullptr;
};

void stabilizeDownloadTree(QTreeWidget *tree);

void markSurface(QWidget *widget, const QString &value);

void markTextRole(QWidget *label, const QString &value);

/// 搜索框左边那枚放大镜（跟着主题重新着色）。
void addSearchIcon(QLineEdit *edit);

void styleSidebarList(QListWidget *list);

TwoColumns buildTwoColumnPage(QWidget *page, const QString &tab);

void fillPageHeader(Column &column, const QString &title, const QString &subtitle);

SettingsSection addSettingsSection(QGridLayout *grid, int &row, const QString &title,
                                   QWidget *parent);

void addSettingItem(const SettingsSection &section, const QString &labelText, QWidget *control,
                    QWidget *parent);

QWidget *makeHintedControl(QWidget *parent);

HintedControlWithLayout makeHintedControlWithLayout(QWidget *parent);

InlineRow makeInlineRow(QWidget *parent);

QLabel *addDynamicHint(QWidget *box, const QString &text, QWidget *parent);

QLabel *addDynamicErrorHint(QWidget *box, QWidget *parent);

void addHint(QWidget *box, const QString &text, QWidget *parent);

// ---------- 页面上的两种"小节"和那层滚动正文 ----------
//
// 同一栏里两种页形反复出现，抽成三样，别在各页各抄一份：
// · `makeSectionCard()`：浅色面 + 一圈内边距 + 一条粗标题 —— 下载页那张安装页
//   （「模组加载器」）和服务端表单（「服务端」）用的是它；
// · `makePageSection()`：**不套卡片**，只有一条粗标题 + 下面那片内容 —— 实例设置页
//   （「显示哪些页」）和预设详情（「预设内容」…）用的是它；
// · `makePageScroll()`：无边框 + 可伸缩 + 横向不滚的正文层（底下留出浮着的主按钮）。

/// 一张小节卡片。@p layoutOut 拿回来的是卡片里的竖布局（标题已经在里面）。
QFrame *makeSectionCard(const QString &title, QWidget *parent,
                        QVBoxLayout **layoutOut = nullptr);

/// 一页上的小节（无卡片）。@p layoutOut 同上。
QWidget *makePageSection(const QString &title, QWidget *parent,
                         QVBoxLayout **layoutOut = nullptr);

/// 一条设置行（设计稿 instance-overview）：左边标题（+ 至多一句说明，次要色小字）、右边控件，行底一条极淡分隔。
/// 长解释留在控件的悬停提示里。@p below = 控件放到标题下面、占满整行（一排按钮那种）。
/// 实例设置概览和分组设置共用这一份。
QWidget *makeSettingRow(QWidget *parent, QWidget *title, const QString &desc, QWidget *control, bool below = false);
/// 设置行左边那个标题（正文字号、加粗）。
QLabel *makeSettingRowTitle(const QString &text, QWidget *parent);

/// 正文那层滚动区。@p bottomMargin 是给浮着的主按钮留的高度（不要浮按钮就传 0）。
QScrollArea *makePageScroll(QWidget *parent, int bottomMargin,
                            QVBoxLayout **bodyOut = nullptr);

void syncDownloadHeaderVisibility(QWidget *header, QStackedWidget *downloadStack,
                                  QStackedWidget *gameStack, QStackedWidget *serverStack,
                                  QWidget *categoryChips = nullptr);
}
