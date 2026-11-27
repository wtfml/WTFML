/**
 * @file Theme.hpp
 * @brief 把「界面上各个地方长什么样」从各自那几句 `darker()` 里收出来，做成一份**能整套
 * 换、也能一处一处换**的数据。
 * @note 各个地方的背景等允许切换，允许各个框架结构进行一个主题的
 * 切换（比如文本透明度等），ui 框架处在能高度个性化定义的状态。
 *
 * 【这一层是什么】
 *
 * 一份 `Spec`（预设 + 覆盖项）描述"界面长什么样"，`apply()` 把它铺到整个应用上。用户能
 * 单独动的东西分三类，**每一类都必须说得出"它在画面上是哪儿"**：
 *
 * | 用户眼里的"地方" | 这一层怎么表达 |
 * |-----------------------------|------------------------------------------------------|
 * | 内容区那一块底 | `Role::Window` |
 * | 输入框 / 列表 / 卡片里那层底 | `Role::Base`（`AlternateBase` 是隔行那一档） |
 * | 选中 / 悬停那一块 | `Role::Highlight`（+ `HighlightedText`） |
 * | 正文 / 次要文字 / 提示文字 | `Role::WindowText` `Text` `PlaceholderText` + `textOpacity` |
 * | 顶上那一条 | `Part::TitleBar`（出厂 = **主题色那一支**，白字白记号）|
 * | 底下那一条 | `Part::BottomBar` |
 * | 压上来的浮层（面板 / 菜单） | `Part::Panel` |
 * | 卡片（整页 / 小提示） | `Part::Card` |
 * | 四周变暗那一层 | `Part::Scrim` |
 *
 * 为什么分成 Role 和 Part 两拨，而不是把十来处背景一律做成 Part：**Part 那一拨今天的
 * 底色全是从窗口底色推出来的**（`darker(105)` / `darker(112)` / `Base` / 一层半透明黑），
 * 调色板里没有它们的位置；而 Role 那一拨本来就有位置（改调色板就是改它们，界面里那些
 * 读 `palette()` 的地方**自动**跟着走，不用挨个去改）。硬把 Role 那拨也做成 Part，就等于
 * 同一件事存两份，早晚对不上。
 *
 * 【默认与兼容】
 *
 * `defaultSpec()` 使用暮色雪山（`dark`），主题色就是它自己那一支 tokens 的 `primary`。
 * 冰川碧湖、落日海崖和暮色雪山由同一套 Tokens 映射到调色板与 QSS。
 * 已存储的系统主题继续跟随系统；旧 light 标识迁移为 emerald，自定义覆盖仍优先。
 *
 * 【换主题那一刻会发生什么】
 *
 * `apply()` 把调色板铺到 `QApplication` 上，Qt 自己会把"调色板变了"发给每一个控件；那些
 * 把自己算出来的颜色**存了一份**的地方（QSS 里嵌死的色、自绘控件的悬停色、块状标记的
 * 位图）靠 `follow()` 跟着重算一遍 —— 它就是"下一次重算"这一个动作的登记处。没登记过的
 * 地方不是错的：每次现建的那些（浮层 / 菜单 / 列表行）下次打开自然就是新的。
 *
 * NOTE: **跟着系统**那一档（`presetId == "system"`）仍然是我们唯一不去钉死
 * `ColorScheme` 的一档：用户在 Windows 设置里切深浅，界面跟着变（`ColorSchemeWatcher`
 * 接住平台的 `colorSchemeChanged` 之后重铺一遍调色板）。调色板本身现在也由我们铺
 * —— 样式表里的颜色是 `palette(...)` 引用，不铺就没地方读那些档位。
 * 明确选了"浅色 / 深色"之后，我们就把那一档钉住 —— 那时候还跟着系统走，等于用户
 * 选了"深色"而界面是浅的。
 */

#pragma once

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QPalette>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QString>
#include <QStringList>
#include <QStyle>

#include <functional>
#include <utility>

class QPainter;
class QAbstractScrollArea;
class QAbstractSpinBox;
class QCheckBox;
class QLayout;
class QPushButton;
class QLabel;
class QStyle;
class QVBoxLayout;
class QWidget;
class QModelIndex;
class QStyleOptionViewItem;

namespace Theme {

// 字阶四档（tokens-r2 §1，逻辑像素，系统缩放交给 Qt）：辅助 13（设计稿 12.5，Qt 取整成 13，
// 和 qRound(12.5) 一致）· 正文 14 · 小节标题 16 半粗 · 页标题 26 手写体 Light。
// Empty 是空状态大字（22 手写体），不进刻度；品牌 30、主页英雄区 50/40 各自定号。
enum class TextRole { Caption, Body, Heading, Display, Empty };
constexpr int textSize(TextRole role) {
    switch (role) {
    case TextRole::Caption: return 13;
    case TextRole::Body: return 14;
    case TextRole::Heading: return 16;
    case TextRole::Display: return 26;
    case TextRole::Empty: return 22;
    }
    return 14;
}
/// 手写体三支（见 assets/fonts/README.md）：文楷 Regular 给主页实例名；文楷 Light 给页标题、
/// 空状态大字和主页大标题；Dancing Script 只给纯西文的品牌与题词。第一次问到时从 qrc 注册，
/// 注册不上返回空串（调用方退回系统字体）。手写体只用在这四处（品牌 / 主页英雄区 / 页标题 /
/// 空状态大字），由 markDisplay() 落到控件上；导航、按钮、胶囊、小节标题、表单、列表、对话框正文
/// 一律系统字（app 字体）。
enum class HandFont { Kai, KaiLight, Script };
/// 主页「每日一言」那一行的文字种类：决定用哪支字体、以及限长（限长在
/// `util/DailyQuote` 里判，这一层只管字体）。名字和那边的 `DailyQuote::Script`
/// 一一对应，但**不能共用那个类型**：ui/ 不许 include src/（分层铁律）。
enum class QuoteScript { Latin, Han, Kana };
/// 三类文字各自的 family 列表（第一档是主字体，后面是回退）。
/// - 拉丁：Dancing Script（品牌手写体）；
/// - 汉字无假名：文楷 Light（和主页大标题同一支）；
/// - 含假名：Windows 自带的日文字体（Yu Mincho → Yu Gothic UI → Meiryo UI）。
///   **文楷没有假名**：实测 assets/fonts 那两支文楷的 cmap 只有 3911 个码点，
///   0x3040–0x30FF（平假名 / 片假名 / 长音符「ー」）一个都没有，连「々」也没有；
///   把文楷排在前面只会让日文句子里汉字用文楷、假名落回系统字，一行两种字形。
QStringList quoteFamilies(QuoteScript script);
/// 「每日一言」的字号也是分档的：正常 16，窄窗 14（见 MainWindowUI 的 applyScale）。
QFont quoteFont(QFont font, QuoteScript script, int pixelSize);
QString handFamily(HandFont which);
/// 把 `font` 换成那一支手写体（注册不上就原样返回）。
QFont handFont(QFont font, HandFont which);
inline QFont textFont(QFont font, TextRole role) {
    font.setPixelSize(textSize(role));
    if (role == TextRole::Heading) font.setWeight(QFont::DemiBold);
    if (role == TextRole::Display || role == TextRole::Empty) {
        font = handFont(font, HandFont::KaiLight);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    }
    return font;
}
/// 放得下整段 @p text 的最大一号（像素，在 [lo, hi] 里挑；只改字号，字族、字距照 @p font）。
/// 到下限还放不下就返回下限，交给省略号。每日一言、主页当前实例名、实例设置页眉共用。
int fitPixelSize(QFont font, const QString &text, int width, int hi, int lo);
/// 手写体控件的唯一入口：设字体并挂 `wtfmlDisplay=true`（ui_check 按它查手写体没有漏到别处）。
void markDisplay(QWidget *widget, const QFont &font);
QString displayPropertyName();

// 色号来自《WTFML_三主题配色规范》，组件共用，用户覆盖仍在 Spec 中保存。
// 带 alpha 的几档（subtle / hover / selected / input / stroke / divider / focus）是叠在磨砂
// 窗口底上的半透明层；page / card 不透明（独立弹出窗口拿它们打底）。
struct Tokens {
    QColor primary, deep, primaryHover, primaryPressed;   // 主按钮底 / 强调字 / 悬停 / 按下
    QColor page, main, card, subtle, hover, selected, input;  // 着色底 / - / 浮层 / 次按钮底 / 悬停 / 选中 / 输入框
    QColor text, secondary, disabled, stroke, divider, focus, thumb, track;
    QColor logWindow, logToolbar, logBase, logHint, logBorder, logDivider;
    QColor onPrimary, accentSoft2, fillPress, fillSoftHover;  // 主按钮字 / 弱强调底 / 中性按下 / 次按钮悬停
    QColor ok, warn, danger, okSoft, warnSoft, dangerSoft;
    QColor dangerSolid, dangerSolidHover, dangerSolidPressed, onDanger;  // 只给对话框里不可逆确认
    QColor glass, glassEdge, scrim;                         // 窗口内浮层面（半透明）/ 1px 亮边 / 遮罩
    QColor heroInk, heroInk2, heroVeil;                     // 主页压在风景上的字；暗角色（不透明 rgb）
    qreal heroScrim, heroTop, railFog;                      // 启动区 / 标题区暗角浓度；亮主题导航轨雾
    QColor groundShade;                                     // 皮肤立绘落地影色
    qreal groundGlint;                                      // 落地影下沿反光（白）浓度
    // 当小字用的强调 / 语义色（tokens-extra §21）：浅色主题是加深版；深色预设把强调字提亮一档
    // （选中 chip / 分段 / 徽章 ≥ 4.5:1）；没设的预设用 textTone() 回落到填充色。调色板 `Link` 取 accentInk。
    QColor accentInk, okInk, warnInk, dangerInk;
};
/// 小字该用的语义色：有加深版用加深版，没有（深色主题）用填充色本身。
inline QColor textTone(const QColor &ink, const QColor &fill) { return ink.isValid() ? ink : fill; }
Tokens tokens(const QString &presetId);
Tokens tokens();

// ---------------------------------------------------------------------------
// 能单独定义的那几块骨架
// ---------------------------------------------------------------------------

/// 界面上能**单独**换底色的那几块骨架（判据见文件头那张表）。
enum class Part {
    TitleBar,   ///< 顶上那一条（52 px）—— 出厂 = 主题色那一支（白字白记号，见文件头那张表）
    BottomBar,  ///< 底下那一条 —— 今天 = 窗口底色 darker(105)，和标题栏**不是**同一档（见上）
    Panel,      ///< 压在内容区上来的浮层：底栏那两块面板、行尾齿轮弹的那两列菜单 —— darker(112)
    Card,       ///< 覆盖层里那张卡片（整页 / 小提示）—— 今天 = `Base`
    Scrim,      ///< 提示框四周变暗那一层 —— 今天 = `rgba(0, 0, 0, 96)`
    /// 四个页面（主页 / 实例 / 下载 / 设置）内容区那一整块底 —— 今天 = 窗口底色。
    /// 内容区那一整块底 —— 今天 = 窗口底色。一块管四页（换一页就得再加一格，
    /// 而那四个页面今天长得一模一样，分成四格只是让用户点四下）。
    PageBackground,
    Count,
};

/// 块数（循环遍历用）。
constexpr int partCount() { return static_cast<int>(Part::Count); }

/// 存盘用的键（"titleBar"）、给人看的名字（"标题栏"）。两者都只有这一处出处。
QString partKey(Part part);
QString partName(Part part);
/// 从存盘的键认回来；认不出返回 false（规格那一侧的兜底）。
bool partFromKey(const QString &key, Part *out);

/// 一张图摆进一块里的摆法。
enum class ImageFit {
    Cover,    ///< 铺满：按比例放大到盖住整块，多出来的裁掉（**默认** —— 不留空白）
    Contain,  ///< 完整放下：按比例缩到整块放得下，四周留出底色
    Stretch,  ///< 拉伸铺满：宽高各自拉满（**不保比例**，照片会变形）
    Tile,     ///< 平铺：原尺寸一张张重复，边缘那几张裁掉
    Count,
};

constexpr int imageFitCount() { return static_cast<int>(ImageFit::Count); }

/// 存盘用的键（"cover"）、给人看的名字（"铺满"）。两者都只有这一处出处。
QString imageFitKey(ImageFit fit);
QString imageFitName(ImageFit fit);
/// 从存盘的键认回来；认不出返回 false（规格那一侧的兜底）。
bool imageFitFromKey(const QString &key, ImageFit *out);

/// 一块上的样子。**没写的那几位 = 不覆盖**：调用方照旧用今天那一句算出来。
struct PartStyle {
    bool hasBackground = false;
    QColor background;
    bool hasForeground = false;
    QColor foreground;
    /// 这一块上文字的不透明度（0~100）；`-1` = 跟全局那一位。
    /// NOTE: `Scrim` 上这一位没有意义（它上面一个字都没有，见 `partForeground()`）。
    int textOpacity = -1;

    bool hasImage = false;
    /// 铺在这一块上的那张图（**绝对路径**）。空的 = 不铺图。
    ///
    /// NOTE: 用户挑的那张**不会**直接用：启动器会把它拷进数据目录
    /// （`<数据目录>/cache/theme/`，见 `src/util/ThemeImage.hpp`）再记这儿 ——
    /// 记用户自己那个路径的话，他自己把图挪走/删掉之后，界面上就是一块突兀的空白，
    /// 而且他多半已经忘了当初挑的是哪一张。
    QString imagePath;
    /// 用户挑的那张**原来叫什么**（挑图那一刻的 `QFileInfo(picked).fileName()`）。
    ///
    /// NOTE: 只有设了图时它才有意义 —— `normalize()` 在"没设图"那一档把它一起清掉。
    /// NOTE: 它可以为空（用户手改注册表、或者以后别的入口直接按路径设图）：那边一律退回
    /// 用 `imagePath` 的文件名，界面上不许因为少一位就写「现在铺的是：」加个空白。
    QString imageName;
    /// 怎么摆（见 `ImageFit`）。
    ImageFit fit = ImageFit::Cover;
    /// 图的不透明度（0~100）。**底下的底色照旧先铺一遍**，所以这一位调低 = 底色透上来
    /// —— 文字要读得清就是靠它（一张照片铺满、字全糊在上面是不能用的）。
    int imageOpacity = 100;
};

// ---------------------------------------------------------------------------
// 能单独定义的调色板角色
// ---------------------------------------------------------------------------

/// 能单独改的调色板角色。**不是把 `QPalette::ColorRole` 整个搬过来**：端出去三十来个
/// 名字（`BrightText`、`LinkVisited`……）只会让"能调什么"变成一份没人看得懂的清单，
/// 而这里每一项都得说得出它在画面上是哪儿（见文件头那张表）。
enum class Role {
    Window,          ///< 内容区那一块底
    Base,            ///< 输入框 / 列表 / 卡片里那层底
    AlternateBase,   ///< 隔行的另一档（表格交替行）
    Text,            ///< `Base` 上的字
    WindowText,      ///< `Window` 上的字（正文）
    Button,          ///< 按钮的底
    ButtonText,      ///< 按钮上的字
    Highlight,       ///< 选中 / 悬停那一块（浮层里的悬停是它加一层透明度）
    HighlightedText, ///< 选中那块上的字
    PlaceholderText, ///< 次要文字（两行式里的第二行、没填的提示）
    Mid,             ///< 分隔线 / 一圈细边
    Count,
};

constexpr int roleCount() { return static_cast<int>(Role::Count); }

QPalette::ColorRole qtRole(Role role);
QString roleKey(Role role);
QString roleName(Role role);
bool roleFromKey(const QString &key, Role *out);

// ---------------------------------------------------------------------------
// 主题色
// ---------------------------------------------------------------------------

/**
 * @brief 主题色那一支的几档。`Spec::accent` 里存的是 `Pure` 那一档，**其余全是算出来的**。
 *
 * 为什么要分档，而不是把默认主题色到处写：同一个色在"顶栏 / 主按钮的大色块"和
 * "白底上一行小字"上不是一回事。纯色要兼顾白字，描边按钮的文字和细边则继续走更深
 * 的两档：既保留层级，也不会让小尺寸文字显得发虚。
 *
 * 这些档**不进 `Role`**（用户不能单独挑其中一个），但会作为样式表的"变量"写进调色板
 * 的几个通用角色（`Accent` / `Light` 等，见 `buildPalette()`）—— 换配色时只要换调色板，
 * 不必重挂整份应用样式表。
 */
enum class AccentShade {
    Pure,     ///< 那个纯色 —— 顶栏的底、当前那一颗导航底下那条线
    Ink,      ///< 白底上写字的那个绿（按钮文本）= 纯色的 60% —— 对白底 9.5:1
    Border,   ///< 按钮那一圈边 = 纯色的 80% —— 对白底 6.7:1（WCAG 对"界面部件"那条线是 3:1）
    Hover,    ///< 按钮悬停时那层浅浅的底（主题色 12% 罩在白上）
    Pressed,  ///< 按钮按下时那层底（主题色 28% 罩在白上）
    Count,
};

constexpr int accentShadeCount() { return static_cast<int>(AccentShade::Count); }

/// 某一档现在的色。**永远有效**（`Spec::accent` 认不出时退回当前预设那一支）。
QColor accentShade(AccentShade shade);
/// 主按钮（实心强调色）悬停 / 按下那一档的底：`markButton(Primary)` 和自画底的圆钮共用这一处。
QColor primaryHoverFill();
QColor primaryPressedFill();
/// 冰川碧湖那一支主题色（挑色框里那行示例色）。出厂主题色不取这里，见 `defaultSpec()`。
/// **全仓唯一出处。**
QColor defaultAccentColor();

/**
 * @brief 往底色里混主题色多少（百分比）—— **行悬停那一层的唯一出处**。
 *
 * 12 不是随手挑的：它和按钮 `:hover` 那一档（`AccentShade::Hover`）**同一个数** ——
 * 界面上"鼠标停在这儿"只该有一种深浅（见 `rowHoverFill()` 那段）。
 * 验证工程读的就是这个常数，别在别处再抄一份。
 */
constexpr int rowHoverAccentPercent() { return 12; }

/**
 * @brief 行/条目**悬停**时铺在它自己那一圈里的底：从**这一行真正的底色**往主题色那头混
 * `rowHoverAccentPercent()`。
 *
 * 悬停在版本/按钮上都要有一个颜色加深的效果。
 *
 * 为什么入参是底色、而不是直接回 `accentShade(AccentShade::Hover)`：按钮那一档是
 * **主题色罩在写上死的白上面**（按钮和输入框在这一套界面里一直当"白纸"处理，不跟系统
 * 深浅 —— 见 `widgetStyleSheet()` 里那段）。行不能照抄：深色预设下 `QPalette::Base` 是
 * `#323232`，罩一层近白会闪一大片。从**这一行自己的底色**出发，两边就都对：
 * **浅色下它和按钮那一档逐字节相等**（底色就是白，混法与 `mixWithWhite(pure, 12)` 完全
 * 同式），深色下也还是"底色加深一点"。
 *
 * 它**不进调色板**（不是 `Role`），理由和上面那几档一样：往调色板里塞东西等于把"跟着
 * 系统深浅走"钉死。
 */
QColor rowHoverFill(const QColor &base);

/**
 * @brief 「中性那一档」：从 @p base 往 @p ink 混 @p percent —— 界面里那些**既不是主题色、
 * 也不是正文**的灰都从它算。
 *
 * 和 `rowHoverFill()` 是**一对**（同一个式子，只是那一头混的是墨色而不是主题色）：
 * 悬停那一层要"底色带一点主题色"，中性面要"底色带一点墨"—— 两条都不能写死。
 * 混法必须和 `mixWithWhite()` **逐字同式**（`(底*(100-p) + 墨*p + 50) / 100`），
 * `percent` 就是"往墨那头走了几成"。
 *
 * 读者：日志窗那排开关的关态底（29）、它那圈边（34）、开态那个兜底灰（9）、行间那条
 * 分隔线（15，从**正文区**的 `Base` 出发）。
 */
QColor neutralFill(const QColor &base, const QColor &ink, int percent = 29);

// ---------------------------------------------------------------------------
// 列表 / 侧栏那一列「选中」的那一块
// ---------------------------------------------------------------------------
//
// 全仓每一处"选中一行"都是**同一档**：由这一行自己的底色混出的低饱和浅主题色底 +
// 主题色那一支里的深墨（与侧栏导航同款）；不画左侧指示条（tokens-r2 §6：选中 = 淡底 + 强调字）。
// 数据行和导航行**长得一样** —— 没有"灰是数据、绿是导航"那条区分。
//
// 底为什么从**这一行自己的底色**出发混，而不是写死一个浅绿：深色预设下
// `QPalette::Base` 是 `#323232`，罩一层近白会闪一大片（理由见 `rowHoverFill()` 那段）。
// 混法和 `rowHoverFill()` **逐字同式**（同一个 `mixToward()`），只差比例 —— 浅色下
// 底就是白，这一档和 `accentShade(AccentShade::Hover)` 只差那两个百分比。
//
// 混的比例比行悬停**大一档**（`rowHoverAccentPercent()` 是 12）：两者同色的话，
// 鼠标一移到当前那一项上"选中"和"指着"就分不出来了。

/// 选中那一行往主题色混多少（百分比，见上面那段）。
constexpr int selectionAccentPercent() { return 20; }

/// 这一行自己的底色，喂给下面这几个取色函数用：**一律走它，别直接读 `color(QPalette::Base)`**。
/// 挂了 `background: transparent` 样式表的列表，Qt 把 `Base` 设成 `NoBrush`（`QRenderRule::configurePalette`
/// 的 embedded 分支），它的颜色是**不透明黑** —— 直接读就把浅色界面当成黑底（#143 左栏白字）。这时按全透明给。
QColor rowBase(const QPalette &palette);

/// 选中那一行的底。入参是**这一行自己的底色**（`rowBase(option.palette)`）；半透明 / 全透明的
/// 先叠到页面底上再算。不传 = 白底（默认那身浅色主题上所有表的底色都是白）。
QColor selectionFill(const QColor &base = QColor(Qt::white));
/// 它上面那行字：主题色那一支里"白底上写字"的那一档（`AccentShade::Ink`，约 7.4:1）。
/// 深底（深色预设）上自动换成那一支里"深底上写字"的那一档 —— 见 .cpp 里那段。
QColor selectionInk(const QColor &base = QColor(Qt::white));
/// 它上面那行**次要**小字（版本树第二行那行小字 / 占位文字 / 行里那些小图标）：
/// 比正文浅一档，但压在这块底上仍然读得清。
QColor selectionSubInk(const QColor &base = QColor(Qt::white));

/**
 * @brief 把"选中那一行画在浅主题色底上"这件事落到**一份能直接取色的调色板**上。
 *
 * @param palette 这一行原本那一份（`option.palette`）。
 * @param selected 这一行此刻是不是选中（`option.state & QStyle::State_Selected`）。
 *
 * 不选中时**原样返回**（一个字节都没动）。选中时返回一份拷贝，里面这几档换掉：
 * `WindowText` / `Text` / `HighlightedText` → `selectionInk(base)`，`PlaceholderText` →
 * `selectionSubInk(base)`，`Highlight` → `selectionFill(base)`（有几个地方按"这一行真正铺
 * 的那一层"重算文字对比度 —— 版本树上的 MOTD —— 读的就是 `Highlight`）。
 * 那几个 `base` 一律取这一份调色板里的 `QPalette::Base`（= 这一行自己的底色）。
 *
 * 为什么要这么一份：**手画正文/图标的代理有三个**（`VersionTreeDelegate` /
 * `InstanceGroupDelegate` / `GroupListDelegate`），它们各自的正文、小字、行尾那几颗按钮
 * 都是从这一份里取色的。底色不再问调色板要（是我们自己混出来那一块），取色那一处就得
 * 跟着换 —— 抄三遍那段"换哪几档"迟早会漏掉一档（漏的那一档在屏幕上就是一行看不出来的字）。
 * 样式那一半（`CE_ItemViewItem` 里印字的那些，比如实例行的正文）在
 * `Theme::createStyle()` 里读的也是这个函数 —— 两条路同一个出处。
 *
 * 用户把"文字透明度"调低过的话（`kInkRoles` 里那几位），换上去的墨**沿用原来那一档的
 * alpha**：选中那一行的字不该比别处更实。
 */
QPalette selectionPalette(const QPalette &palette, bool selected);

// ---------------------------------------------------------------------------
// 按钮
// ---------------------------------------------------------------------------

/**
 * @brief 一颗按钮的"身份"（决定它那圈边和字用哪一档色）。
 *
 * 挂在控件的动态属性上（`wtfmlButtonKind`）；**没挂的按 `Accent` 算** —— 界面上一百来颗
 * 按钮里绝大多数就是普通按钮，犯不着每建一颗都喊一遍。这个属性只影响颜色，不影响尺寸。
 *
 * **四档怎么分**（除 `Primary` 外三档按下面这条分，主动作档用于拉开层级）：
 * 确认性的（如启动、下载、搜索）做成主题色，一般性的（设置、清空搜索）做成黑色，
 * 不可撤销的（如修改存档、删除实例）做成红色。
 * 落成一条能照着判的规矩：
 * - `Accent` —— **干这件事**：启动 / 下载 / 安装 / 搜索 / 登录 / 保存 / 确定 / 应用 /
 * 创建（新建、复制、添加、导入）/ 备份 / 还原。
 * - `Primary` —— 页面上唯一最重要的动作；今天只给主页的「启动游戏」使用实心主题色。
 * - `Neutral` —— **看一眼或者挪个地方**：取消 / 关闭 / 返回 / 打开文件夹 / 刷新 /
 * 清空搜索 / 切换 / 分组 / 更换图标 / 导出 / 浏览 / 那些"快捷方式" / 菜单项 /
 * 查询（检查更新 / 检查并修复 / 测速）。
 *
 * 「查询」列在 `Neutral` 这一档：那一档的说法是"盘上会不会
 * 变"，而"检查一下"这一步**只读不写** —— 按下去出现的是一个结果，东西一个字节都
 * 没动，和「刷新」是同一件事。实例概览里那三颗（检查更新 / 更新中心 / 检查并修复）
 * 显式挂 `Neutral`（见
 * `InstanceSettingsDialog` 里那三处）。真正会写的那些（补全版本文件、创建快照、
 * 复制实例）仍然留在 `Accent`。
 * - `Ghost` —— **低频的辅助动作**：卡片里的一句"复制到剪贴板""查看报告"、行尾的
 * "换个样子"、分组设置底栏的「恢复默认分组」（只改工作副本，真恢复前另有一问）这类。它和 `Neutral` 是同一种判断的两档，分界线是**频率与分量**：
 * `Neutral` 是一颗"看得出是个按钮"的次要按钮（白底 + 一圈边），`Ghost` 只有字
 * （没有底、没有边），挪到边上不抢眼。**判不准就用 `Neutral`** —— Ghost 用滥了
 * 一整页会变成"一堆没有形状的字"。
 * - `Danger` —— **删了就拿不回来**：删除（实例 / 文件 / 账号 / 分组）/
 * 还原上一次 / 清空日志 / 写进存档（「世界设置…」那一页的保存）。
 *
 * NOTE: 判不准的时候选 `Neutral`。把一颗普通按钮染成绿的，代价是"绿不再意味着该按"；
 * 少染一颗只是那颗按钮朴素一点。
 */
enum class ButtonKind {
    Accent,   ///< 确认性（**默认**，不挂属性）：现行样式表里与 Neutral 画法相同，底取调色板 button，字为正文色
    Primary,  ///< 页面唯一主动作：主题色实底 + 白字（例如主页的「启动游戏」）
    Neutral,  ///< 一般性：浅中性色底 + 深色文字，默认不画边
    Ghost,    ///< 低频辅助：没有底、没有边，只有字（边留成透明的 1px，**几何不变**）
    Danger,   ///< 不可撤销：淡红底 + 红字，默认不画边
    DangerSolid,  ///< 对话框里不可逆的最终确认：实心暗哑红 + 浅字（其余危险动作一律用 Danger）
};

/// 属性名 + 属性值（`markButton()` 挂的就是这一对）；`QSS` 里那几条选择器也照着它写。
QString buttonKindKey(ButtonKind kind);
/// 给一颗按钮钉一个身份（**只改颜色**）。下一趟按钮样式重算时生效。
void markButton(QWidget *button, ButtonKind kind);

/// 「这一颗是页面上的刷新按钮」的标记。F5 靠它找当前页该点哪一颗
/// （见 `MainWindow::refreshTopmostPage()`）；没挂这一条的按钮 F5 一律不碰。
/// 只管认人，不种样式 —— 长什么样仍由 `markButton()` 那一档决定。
void markRefreshButton(QWidget *button);
/// 上面那条标记的属性名（测试按它认人，别自己抄字符串）。
QString refreshActionPropertyName();
/// 「一般性」那一档的黑（次要按钮的字与边、未勾选的勾选框）。**唯一出处。**
QColor neutralInkColor();

/**
 * @brief 「灰掉」那一档的三个色（`QPushButton:disabled` 的底 / 字·边 / 边）。**唯一出处。**
 *
 * Disabled 状态单独设计：降低对比度但仍保留轮廓和文字可读性，
 * 不能让正常 Secondary 和 Disabled 看起来是同一档。所以：
 * - **轮廓还在**（`disabledBorderColor()` 那 1px）—— 完全抹掉边的话，灰掉的一格在
 * 白底上就只剩几个灰字，"这儿本来有一颗按钮"这件事看不出来了；
 * - **字还读得清**（`disabledInkColor()` 对底 ≥ 3:1，见 ui_check 那条现算的判据）——
 * 再浅一档就纯是装饰了；
 * - 和 `Neutral`（白底 + 纯黑字 + 纯黑边）**差着一整档**，一眼分得开。
 */
QColor disabledInkColor();
QColor disabledBorderColor();
QColor disabledFillColor();

/**
 * @brief 键盘焦点环那一圈色（可见焦点环）。
 *
 * 深底上给浅主题色、浅底上给深主题色（`AccentShade::Hover` / `AccentShade::Ink`），
 * 判据就是 `isDarkPalette()` —— 和别处"深色下用亮的那一档"是同一个判断，别另立一条。
 *
 * 为什么**不算进** `AccentShade` 那几档：那几档是"同一个色在画面上深浅不同"，这一圈色
 * 是**跟着底翻的**（浅底用深的那档、深底用浅的那档），语义不是"第几档深浅"。
 *
 * 画它的地方只有一处：`Theme::createStyle()` 那层 `WtfmlProxyStyle` 里截
 * `PE_FrameFocusRect`（见 .cpp 里那段 —— 那是全应用唯一一个"每个控件都会走"的点）。
 */
QColor focusRingColor(const QPalette &palette);

/**
 * @brief 条目视图此刻带着**键盘焦点**（Tab / 方向键到达，不是鼠标点进来的）。
 *
 * 判据和按钮、输入框的焦点环是同一条：`ControlFocusVisibility` 按"最后一次输入是不是键盘"
 * 给视图挂 `wtfmlKeyboardFocus`。**别按 `QFocusEvent::reason()` 判。**
 */
bool itemViewKeyboardFocus(const QWidget *view);

/// 这一行是不是「键盘当前行」：视图带着键盘焦点，且 `index` 就是它的当前行。
/// 行尾按钮的显示判据 = 鼠标悬停，**或者**这一条（键盘走到哪一行，那一行就露出按钮）。
bool isKeyboardCurrentRow(const QStyleOptionViewItem &option, const QModelIndex &index);

/**
 * @brief 自绘进度条（`ui/TaskProgressBar`）那一根**轨道**的底色。**唯一出处。**
 *
 * 从中性那一支算（`neutralFill()`）：底色往墨色那头混一档，于是浅底给一支浅灰、深底给一支
 * 深灰 —— 和日志窗那排开关是同一个混法。**不写死一支灰**：底栏、任务面板、详情页三处的底
 * 各不一样，写死一支在另一种底上不是看不见就是一块疙瘩。
 */
QColor progressTrackColor(const QPalette &palette);

/**
 * @brief 自绘进度条**往前走的那一截**的色（`ui/TaskProgressBar` 的**唯一出处**）。
 *
 * 就是主题色本身：浅底下用它本色（`AccentShade::Pure`）、深底下用亮一档
 * （`AccentShade::Hover`）—— 判据和焦点环同一条（`isDarkPalette()`），别另立一条。
 */
QColor progressFillColor(const QPalette &palette);

/// 全应用那一份控件样式表（按钮 / 输入框 / 勾选框；`apply()` 铺的就是它）。
/// 要它是因为"这些东西长什么样"也得能量 —— 验证工程逐条读它算出来的色。
QString widgetStyleSheet();

// ---------------------------------------------------------------------------
// 「面」与「文字角色」那两个属性
// ---------------------------------------------------------------------------

/**
 * @brief 页面骨架那几个"面"与几档文字，各自由一个**动态属性**认领。
 *
 * 页面骨架是"侧栏 + 一个正文表面"，正文内部靠排版、间距和少量小节卡片建立层级；
 * 形状直接落在属性上：
 * `footer->setProperty("wtfmlSurface", "footer")`。这一组函数就是那两个属性名和那几个值
 * 的**唯一出处**：`widgetStyleSheet()` 里那几条选择器按它们写，产品代码按它们挂，
 * 验证工程也按它们找控件（`findChild<QFrame*>()` 那一族按 objectName 找，而面是**没有**
 * objectName 的 —— 一页里同样的面有好几块，名字只能是属性）。
 *
 * 两件事分开记：
 * · `wtfmlSurface` —— **一块面的底**（侧栏 / 正文 / 小节卡片 / 页面底栏）；
 * · `wtfmlTextRole` —— **一行字是什么身份**（页面标题 / 小节标题 / 次要说明）。
 *
 * 色**不在这儿**：面与字的颜色一律由 `widgetStyleSheet()` 按当前调色板现算
 * （见那几条 QSS 那句 `mixToward()` 的注释）—— 这里给的只是"叫什么"，不是"长什么样"。
 */
QString surfacePropertyName();
QString sidebarSurfaceValue();
QString workspaceSurfaceValue();
QString sectionSurfaceValue();
QString footerSurfaceValue();
QString textRolePropertyName();
QString pageTitleTextRoleValue();
QString sectionTitleTextRoleValue();
QString secondaryTextRoleValue();

// ---------------------------------------------------------------------------
// 尺寸令牌
// ---------------------------------------------------------------------------
//
// 把页面间距、卡片圆角、侧栏宽度、页头高度、按钮高度收进
// `Theme`/布局规格层，**不统一成一个数字，统一成有限的 3–4 档节奏**。
// 于是全仓的"间距"只留三档：
//
// 页面外边距 20 ／ 区块间距 16 ／ 控件间距 8
//
// 圆角只留四档：控件 6 ／ 按钮 8 ／ 主表面 12 ／ 小节 8。
//
// 间距那三档的**取数处仍然是 `MainWindowUI`**（`pageMargin()` / `kColumnSpacing` /
// `kFilterRowSpacing`）—— 页面骨架整个搭在那儿，那是"布局规格层"；这里只收两位
// **样式表自己也要用**的数（动作高度两档 + 圆角），不然同一个数会在 QSS 字符串里再写
// 一遍（写了就会漂，而漂了只在屏幕上差几像素，谁都看不出来）。
//
// 动作高度这一档只管 `QPushButton`。输入框 / 下拉框 / 数字框**仍然是 24**（那是
// 另一条线，见 `widgetStyleSheet()` 里那段）—— 动作按钮
// 32、输入控件 24，这是两件事。
//
// 两处**故意不跟令牌**的（写在这儿免得以后被"顺手统一"掉）：
// * 主页那颗「启动游戏」是 76（`kStartButtonMinHeight`）—— 它是这一页唯一的主角；
// * 下载页的「安装」是 40（`MainWindowUI::installButtonMinHeight()`）—— 它是浮在内容区
// 底部的主操作，字号和别处按钮一样是正文 14。

/// 普通动作按钮（确定 / 取消 / 应用 / 全部那些）的最小高度。
constexpr int actionHeight() { return 36; }
/// "页面上唯一最重要的动作"那一档（`ButtonKind::Primary`），比普通动作略高一档。
constexpr int primaryActionHeight() { return 36; }
/// 主按钮（`ButtonKind::Primary`，强调色实心）左右内边距与最小总宽。**唯一出处**：
/// 样式表里 `padding` 取前者、`min-width` 按总宽 88 扣掉内边距与 1px 边框后写入，
/// 所以「计算」「启动实例」这种两三个字的主按钮不再窄成一条。图标档按钮不吃这一条。
constexpr int primaryButtonPadX() { return 20; }
constexpr int primaryButtonMinWidth() { return 88; }

/// 整页的左右边距（第 3 档，#136）—— 覆盖层与主标签页（管理 / 下载 / 设置 / 智能体 / 工具箱）
/// 的**唯一出处**，换页时内容不跳。右边这一个是「滑条外缘 → 窗口右缘」；不滚动的控件
/// 要和滚动内容右缘对齐，再让出 `scrollBarGutterInset()`。
constexpr int pageInsetLeft() { return 16; }
constexpr int pageInsetRight() { return 12; }
/// 一级页标题行上沿离内容区上边多远。
constexpr int pageTitleInsetTop() { return 22; }

/// 这一位是**图标按钮**（齿轮 / 底栏面板上那几颗 / 详情页顶上的 chip）：它按自己钉死的
/// 边长来，不跟 `actionHeight()` 那一档。
QString iconButtonSizeValue();
/// 给一颗按钮钉成"图标按钮"（**只改尺寸那一档**，不动颜色）。和 `markButton()` 一样是
/// 挂一个动态属性，下一趟按钮样式重算时生效。
void markIconButton(QWidget *button);

// ---------------------------------------------------------------------------
// chip（可勾选的小按钮）
// ---------------------------------------------------------------------------

/**
 * @brief 一排 chip 的高度（px）—— **唯一出处**。
 *
 * 24 而不是 `actionHeight()`（32）：chip 是**筛选条件**，不是"按下去会发生一件事"的主
 * 行动按钮。和它并排的搜索框带 `wtfmlChipHeight`，压到同高，两者并排摆着才是"一行控件"。
 *
 * NOTE: 它同时是**资源详情页那排游戏版本 chip** 的高度，两处必须是同一个数。
 */
constexpr int chipHeight() { return 30; }

/// 输入框 / 下拉框的默认外高（设计稿令牌「控件高 常规」36）。下拉箭头那格跟着整高走。
constexpr int inputHeight() { return 36; }

/**
 * @brief chip 那颗按钮长什么样（QSS 字符串）。
 *
 * 一圈细边 + 透明底；鼠标悬停时淡淡一层 Highlight；**勾上的那颗换成 Highlight 实底 +
 * HighlightedText 的字**。颜色全从 @p palette 里取（不写死灰）—— 换主题、换深色，这一排
 * 跟着走。
 *
 * 为什么要单独一套 QSS：全局那一份按钮样式（`widgetStyleSheet()`）里**没有 `:checked`
 * 这一条** —— 那四档 kind 说的是"这颗按钮是干什么的"，而"哪一颗此刻按着"是筛选这一类
 * 控件自己的事。缺了它的症状很难自己发现：勾选状态**逻辑上是对的**（`isChecked()` 为真），
 * 只有屏幕上那颗按着的和没按的长得一模一样。
 * 调用方必须 `setCheckable(true)`：这套样式只描述"勾上什么样"，不负责让按钮能勾。
 */
QString chipStyleSheet(const QPalette &palette);

/**
 * @brief 把一颗按钮摆成 chip（高度 + 圆角外观 + 换主题重挂），**勾选状态自己摆**。
 *
 * 三件事一起做，因为少任何一件都会留下一个"看着像 chip、其实不是"的按钮：
 * ① `setFixedHeight(chipHeight())`；
 * ② `markIconButton()` —— 报一句"我按自己钉的那一档高度来"，否则全局那条
 * `min-height` 会把它顶到 32（口径见 `markIconButton()` 那段）；
 * ③ 挂 chip 样式，并接一条 `Theme::follow()`：QSS 里嵌的是**色名**，主题一换得重算一遍
 * （常驻的那些 chip 不会有人替它们重建）。
 */
void markChipButton(QPushButton *button);

/// 徽标（设计稿 .badge）：22 高胶囊、12 px 字；中性 fill-soft + ink-2，其余是各自的淡底 + 本色。
enum class BadgeTone { Neutral, Accent, Ok, Warn, Danger };
void markBadge(QLabel *label, BadgeTone tone);

/// 分段（设计稿 .seg）：容器 fill-hover 底、圆角 12、内边距 3；段是可勾选的 QPushButton，
/// 30 高、圆角 9、霞鹜文楷 14；勾上 = accent-soft 底 + accent 字。样式在 `widgetStyleSheet()`
/// 里按属性挂（`wtfmlSegmented` / `wtfmlSegment`），这里只打属性、定高、互斥。
void markSegmented(QWidget *container, const QList<QPushButton *> &segments);
/// 开关（设计稿 .switch）：40×22 胶囊，勾上 = 强调色底 + 右侧圆钮。控件仍是那个 QCheckBox
/// （objectName、toggled、text 都不变，text 只当无障碍名，不画）；行名由设置行左侧那一格写。
void markSwitch(QCheckBox *box);

// ---------------------------------------------------------------------------
// 色块（一颗按钮，面上就是它代表的那块色）
// ---------------------------------------------------------------------------
//
// 为什么是按钮而不是画上去的色块：它得能点、能拿到键盘焦点、能走 Tab。一颗 `QPushButton`
// 三样都是白给的，而"自己画一块"这三样就得各写一遍（焦点环那一圈还得自己接上
// `PE_FrameFocusRect`，见 `focusRingColor()`）。
// 面上那个色是**我们自己挂的样式表**（应用级那份只对"素"按钮生效，控件自己挂的赢），
// 所以它**不跟着调色板走**：换主题时块里那个色照旧是用户选的那个 —— 那正是它要表达的。

/**
 * @brief 色块那一圈边：**跟着这块色自己的明暗翻** —— 深色给白边、浅色给黑边。
 *
 * 判据取的是 `color.lightness() < 128`（和 `isDarkPalette()` 同一个式子）。没有这一圈，
 * 一块近黑的主题色搁在浅色页面上就是"一个没有形状的洞"。
 */
QColor swatchBorder(const QColor &color);

/**
 * @brief 把 @p button 摆成一块**色块**（面上就是 @p color）。
 *
 * 三件事：`markIconButton()`（报一句"我按自己钉死的边长来"，否则全局那条 `min-height`
 * 会把它顶到 32）、挂自己的样式表、把鼠标形状换成手形。
 *
 * @param button **调用方自己 `setFixedSize()`** —— 色块多大是各处的排版说了算
 * （设置页那一行 64×24、取色卡片里那张色板各 48×24），这一层不该再有一份尺寸表。
 * NOTE: 悬停 / 按下**不换底**（换底色 = 换掉了它要表达的那件事），只在边框上给一点反应。
 */
void markColorSwatch(QPushButton *button, const QColor &color);


// ---------------------------------------------------------------------------
// 两个控件工厂（外观都在应用级样式表里，这里只搭结构）
// ---------------------------------------------------------------------------

/**
 * @brief 折叠行：**整行就是一个按钮**，右边用文字「展开 / 收起」表示开合。
 *
 * 不带 ▸ / ▾ 箭头（8 号反馈：这种小箭头不要再出现），也不留系统 QToolButton 那圈方框。
 * 结构：`checkable QPushButton` + 一个子布局（左标题、右状态字），两颗 `QLabel` 都
 * `WA_TransparentForMouseEvents` —— 点在行上任何位置都是按那颗按钮。
 * 按钮自己的 `text()` 留空（画的是子控件），`isChecked()` = 展开着。
 */
QPushButton *makeFoldRow(QWidget *parent, const QString &title);

/**
 * @brief 数字框外套一层「− 值 +」：小整数（0–5 那种）用它，大范围仍用滑块 + 小框。
 *
 * 值仍由调用方那颗 `QAbstractSpinBox` 持有 —— 范围、suffix、`valueChanged`、持久化全不动，
 * 这里只是把它夹在两个 `Qt::NoFocus` 的 −/+ 按钮中间（点一下 = `stepDown()` / `stepUp()`）。
 * 返回的是外面那一层容器，调用方摆它、往里 `addWidget`。
 *
 * @param spinWidth 大于 0 时给数字框定宽（各页那一列要对齐）。
 */
QWidget *makeNumberStepper(QWidget *parent, QAbstractSpinBox *spin, int spinWidth = 0);

/// 输入类控件（`QLineEdit` / `QComboBox` / `QAbstractSpinBox` / 勾选框那个框）的圆角。
constexpr int controlRadius() { return 10; }
/// `QPushButton` 的圆角（比输入类大一档：它比输入框矮胖，同一个圆角看着更方）。
constexpr int buttonRadius() { return 10; }
/// 主表面（`wtfmlSurface="workspace"` / `"sidebar"`）的圆角。
constexpr int surfaceRadius() { return 0; }
/// 小节卡片与主表面统一使用直角。
constexpr int sectionRadius() { return 0; }

// ---------------------------------------------------------------------------
// 滚动条
// ---------------------------------------------------------------------------

/**
 * @brief 滚动条有多粗（逻辑像素）—— **唯一出处**。
 *
 * 滚动条是嵌在页面边上的条，不是直接一个白色条。
 *
 * 12 是**比 Fusion 出厂值（`PM_ScrollBarExtent`）细一档**挑的：这一套界面里的滚动条
 * 一直在页面的最右边那条边线上，宽了看着像另开了一栏。细到 12 之后，真正看得见的东西
 * 只有中间那 6 px 的把手（见 `scrollBarHandleMargin()`），剩下的都是"条嵌在页面边上"
 * 那点留白。
 *
 * 这一位有三处读者：`widgetStyleSheet()` 里那两段 QSS（竖的给 `width`、横的给 `height`）、
 * 以及验证工程（量真控件的宽度 / 量把手的左右两边是不是各留了这么宽）。
 */
constexpr int scrollBarThickness() { return 6; }

/// 把手离滚动条自己那条边线的距离（左右各一份，横的那一档是上下各一份）。
/// 于是**看得见的把手宽 = `scrollBarThickness()` - 2 * 这一位**。
constexpr int scrollBarHandleMargin() { return 1; }

/// 把手两头的圆角：**就是可见宽度的一半**，因此始终是两头圆的胶囊。
/// 写成一算就出来的式子，免得两个数将来各自漂走（漂了只会"看着不那么圆"，不会报错）。
constexpr int scrollBarHandleRadius() { return (scrollBarThickness() - 2 * scrollBarHandleMargin()) / 2; }

/// 把手最短画多长。Fusion 自己也有一条（`PM_ScrollBarSliderMin`）；这一条是我们钉的，
/// 内容长到上千行时把手不至于短成一个小点。
constexpr int scrollBarMinHandleLength() { return 28; }

/// 表单/列表内容右缘与滚动条外框左缘之间的布局间隙（第 3 档：10 px；加 1 px 把手边距 = 11 px）。
constexpr int contentToScrollBarGap() { return 10; }

/// 视口页里不滚动控件（页眉右侧按钮、搜索框、工具条、底部操作行等）需要让出的右侧总宽度：
/// 内容间距 + 滚动条占位（10 + 6 = 16 px），使其右缘与表单页内容右缘、页眉右按钮对齐。
constexpr int scrollBarGutterInset() { return contentToScrollBarGap() + scrollBarThickness(); }

/// 把不滚动子布局的右边距设为 `scrollBarGutterInset()`（保留原有左/上/下边距）。
void alignToScrollGutter(QLayout *layout);

/// 往视口页的竖排布局里加入一个不滚动控件，右边留出 `scrollBarGutterInset()`（不改控件父对象）。
void addScrollGutterAligned(QVBoxLayout *parent, QWidget *widget,
                            Qt::Alignment alignment = Qt::Alignment());

/// 往视口页的竖排布局里加入一个不滚动子布局，并把它的右边距对齐到 `scrollBarGutterInset()`。
void addScrollGutterAligned(QVBoxLayout *parent, QLayout *child);

/// 给视口页的主滚动控件（列表/树/多行文本框）统一挂上滚动条槽位与内容右缘留白：
/// 1) 竖滑条常驻并用 `wtfmlIdle` 在无需滚动时隐去把手，保证切换页面或内容增减时滑条列不跳动；
/// 2) 挂 `wtfmlScrollContentGap = contentToScrollBarGap()`，让 `DesignDefaults::RowDelegate`
///    与表头在最右列留出 10 px（距可见把手 11 px）；
/// 3) 对 `QPlainTextEdit` / `QTextEdit` 同步将可见输入面收在内容右缘以内、滑条留在最右列。
void applyViewportScrollGutter(QAbstractScrollArea *view);

/**
 * @brief 右边常留一条"死区"的滚动区：宽度永远是 `scrollBarThickness()` 那一格。
 *
 * 上下拖拽窗口调整大小的时候文字不能因为边上滚动条的出现和消失
 * 重排列，在那里留一个死区不要让文本进去。
 *
 * 普通 `QScrollArea` 的竖条是 `AsNeeded` 的：内容一高，条子就**从视口里**拿走
 * `scrollBarThickness()` 那么宽，里面那些换行的说明文字当场重排一遍；窗口再高一点、
 * 条子收回去，文字又跳回去。这里把条子改成**常驻**（`ScrollBarAlwaysOn`）：视口宽度
 * 从头到尾是同一个值，条子要么在那一格里画把手、要么那一格什么都不画 —— 就是那条
 * "死区"，文字不许进去。
 *
 * 只给"里面是换行文字"的那几层页面用（设置页每一栏、覆盖层整页、任务中心详情…）。
 * 条目视图（列表 / 树 / 表格）不要换：它们自己那一圈边线和选中块的几何都在别处钉着
 * （见 [[wtfml-scrollbar-look]]），多出来的这条留白只会让行里的列宽少一截。
 */
class GutterScrollArea : public QScrollArea {
public:
    explicit GutterScrollArea(QWidget *parent = nullptr) : QScrollArea(parent) {
        // 条子**常驻**（`AlwaysOn`），不是 `AsNeeded`：`QAbstractScrollArea` 只在条子
        // "露着"的时候才从视口里拿走那一格 —— 常驻 ⇒ 视口宽度从头到尾是同一个值，
        // 里面换行的文字永远不重排。装得下的时候那一格什么都不画（见 `syncIdle()`），
        // 就是那条"死区"。
        // 试过 `setViewportMargins()`：那个接口是给表头/页脚留位的，条子照样再从视口里
        // 拿一格 ⇒ 条子一露，文字反而**一次少两格**（量出来 230 → 220）。
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
        QScrollBar *bar = verticalScrollBar();
        connect(bar, &QScrollBar::rangeChanged, this, [this] { syncIdle(); });
        syncIdle();
    }

private:
    /// 装得下（`minimum == maximum`）时把条子标成"空的"：QSS 那条 `[wtfmlIdle="true"]`
    /// 把把手画成透明 ⇒ 常驻的那一格看上去是一条留白，而不是一根永远拖不动的把手。
    void syncIdle() {
        QScrollBar *bar = verticalScrollBar();
        const bool idle = bar->maximum() <= bar->minimum();
        if (bar->property("wtfmlIdle").isValid() && bar->property("wtfmlIdle").toBool() == idle) {
            return;
        }
        bar->setProperty("wtfmlIdle", idle);
        // 动态属性是 QSS 选择器的一部分 ⇒ 改完**必须重新 polish** 一次，Qt 不会自己来看。
        bar->style()->unpolish(bar);
        bar->style()->polish(bar);
        bar->update();
    }
};

/**
 * @brief 把手平时的色。**带 alpha 的一支中性灰**，跟着深浅各取一边。
 *
 * 为什么不像按钮那样直接写死一支（比如 `#c0c0c0`）：滚动条底下那层是**页面自己的底**，
 * 它跟着主题深浅走（浅色 `#fcfcfc` / 深色 `#2b2b2b`）—— 写死一支灰，在另一种底上不是
 * 看不见就是一块黑疙瘩。所以底色深就给一支**浅**灰、底色浅就给一支**深**灰，
 * 两边都是"看得见但不抢眼"。
 *
 * 为什么带 alpha 而不是给一个实色：这一套界面里"把手底下那层"没有统一的一个色
 * （有的是页面的底、有的是白卡片），alpha 让它在任何一层上都还是"这层加深一点"，
 * 这正是 `rowHoverFill()` 那条规矩的同一种做法。
 *
 * 它**不进调色板**（不是 `Role`）—— 和悬停/选中那两档同一个理由：往调色板里塞东西
 * 等于把"跟着系统深浅走"钉死（见 `apply()` 里那段）。
 */
QColor scrollHandleColor();
/// 鼠标停在把手上（比平时实一档）。
QColor scrollHandleHoverColor();
/// 正拖着把手（最实的一档）。
QColor scrollHandlePressedColor();

/**
 * @brief 应用级样式（`QApplication::setStyle()` 用的那一份）：Fusion + 下拉框那个 `∨` 箭头。
 *
 * **产品与三个验证工程都从这儿拿**，谁也别再自己写 `QStyleFactory::create("Fusion")` ——
 * 「样式基线是同一个东西」由这一个函数统一，四处各写一行相同代码的写法迟早会漂。
 * 返回的对象**归调用方**（`QApplication::setStyle()` 会接过去）。
 */
QStyle *createStyle();

// ---------------------------------------------------------------------------
// 一份主题
// ---------------------------------------------------------------------------

/**
 * @brief 界面的样子。
 *
 * 语义色（危险 / 警告 / 正常 / 报错）**默认是空的** —— 空的意思不是"没有颜色"，而是
 * "按当前这一档深浅现推"（见 `dangerColor()`），于是深色主题下那处红自动变成浅红。
 * 只有用户在预设之外点名要一个色时才写进来。
 */
struct Spec {
    /// 预设 id（`presetIds()` 里那几个之一）。**故意没有 "custom"**：覆盖项是叠在预设
    /// 上面的，换了覆盖项预设还是那一个（"深色 + 我改的那几项"读起来比"自定义"准）。
    /// NOTE: 默认构造这里是 `system`（没说过用哪一档 = 跟着系统）；出厂那一份看
    /// `defaultSpec()`（暮色雪山）。
    QString presetId = QStringLiteral("system");
    /// 全体文字的不透明度（0~100）。100 = 一点也不透（今天的样子）。
    int textOpacity = 100;
    /// **主题色**（那一支强调色）。出厂 = 当前预设的 `tokens(presetId).primary`
    /// （新安装是暮色雪山的 #B7B3EE），永远有效（认不出退回同一支）。
    ///
    /// NOTE: 它**不是** `Theme/Accents/*` 底下那四个（那四个是"语义色"：危险 / 警告 /
    /// 正常 / 报错）。单数那个是"这一套界面是什么颜色"，复数那几个是"红黄绿各是
    /// 哪一个红黄绿"—— 注册表里单复数是两个前缀，别看串了。
    QColor accent;
    /// 覆盖的调色板角色：`int(Role)` → 色。
    QHash<int, QColor> roles;
    /// 覆盖的骨架：`int(Part)` → 那几项。
    QHash<int, PartStyle> parts;
    /// 语义色的覆盖项（无效 = 按深浅现推）。
    QColor danger;
    QColor warning;
    QColor ok;
    QColor errorText;
};

/// 整个应用当前那一份（**不是每个窗口一份**：主题是应用程序级的，见 .cpp 里那段 NOTE）。
const Spec &current();
/// 只换内存里这一份（不落盘、也不铺开 —— 要生效得再 `apply()`）。参数越界会被收拾干净。
void setSpec(const Spec &spec);

/// "出厂"那一份：暮色雪山、一项覆盖都没有（主题色是暮色雪山那一支 primary）。
Spec defaultSpec();

/// 四个预设，按设置页「配色」那一格的下拉顺序：
/// `dark`（暮色雪山，出厂默认）/ `emerald`（冰川碧湖）/ `ocean`（落日海崖）/ `system`（跟着系统）。
QStringList presetIds();
QString presetName(const QString &id);
bool isPresetId(const QString &id);
/// 某一个预设的"出厂"那一份（覆盖项是空的）。认不出的 id 给 `defaultSpec()`。
Spec presetSpec(const QString &id);

/// 把 `spec` 收拾成一份合法的（认不出的 Role/Part 丢掉、不透明度钳到 0~100、色不合法
/// 的丢掉）。**盘上读回来的那一份必须过这一道**：注册表是能手改的。
Spec normalize(const Spec &spec);

// ---------------------------------------------------------------------------
// 铺开
// ---------------------------------------------------------------------------

/// 这一份调色板算不算深色。判据取的是**窗口底色的明度**（不是标志位）—— 用户把内容区
/// 调成黑的、预设却写着浅色的时候，跟着标志位走会让"深底上的深红"这种话成真。
/// NOTE: 这个判据**只有这一处**（`MainWindowUI::paletteIsDark()` 转调这儿）。
bool isDarkPalette(const QPalette &palette);
/// 当前这一份算不算深色（拿应用调色板问上面那个函数）。
bool isDark();

/// 从 `base` 出发，叠上 `spec` 的角色覆盖与文字不透明度。
/// NOTE: 「跟着系统」那一档要先问平台现在是不是深色（`platformColorSchemeDark()`），
/// 所以严格说它会读一下 `qApp` 的 `styleHints()`；其余预设是纯函数。
QPalette buildPalette(const Spec &spec, const QPalette &base);

/**
 * @brief 把当前那一份铺到整个应用上。
 *
 * 三件事，按这个顺序：定深浅那一档 → 算调色板 → 铺开（并让该重算的重新算一遍）。
 *
 * NOTE: 「跟着系统」也会铺一份我们自己算的调色板；平台深浅变了由 `ColorSchemeWatcher`
 * 接住重铺（见 .cpp 里那段）。
 */
void apply(QApplication &app);

/// 把"该重算了"这一下敲给每一个控件（`QEvent::PaletteChange`）。
/// `apply()` 自己在"调色板没动、只有哪一块变了"那条路上会调它。
/// NOTE: 只该由 `apply()` 调 —— 从别处敲这一发等于凭空调一次重算。
void notifyWidgets();

// ---------------------------------------------------------------------------
// 每一块 / 每一个语义色：现在的颜色
// ---------------------------------------------------------------------------

/// 这一块现在的底色。**永远有效**：没覆盖过就返回今天那一份算出来的（见文件头那张表）。
QColor partBackground(Part part);

/// 这一块**自己**设过底色没有（没设过时 `partBackground()` 给的是"今天那一份"算出来的）。
bool partHasBackground(Part part);
/// 这一块现在的字色：没覆盖过返回 `fallback`（调用方自己那句）。
///
/// NOTE: 这一块**自己**那一位不透明度（`PartStyle::textOpacity`）在这儿生效；全局那一位
/// 落在调色板的文字角色上（`buildPalette()` 干的），而 `fallback` 多半就是从调色板
/// 取来的 —— 在这儿再乘一次就是乘了两遍。所以这里**只**用那一块自己的值。
QColor partForeground(Part part, const QColor &fallback);

/// 这一块的**出厂**字色是不是白的那一支（`partForeground()` 在没人覆盖过时给的就是它）。
///
/// 要它是给"次要文字"用的：标题栏的底现在是主题色那一支绿，而它上面"不是当前那一颗"
/// 的导航写的是**白墨压淡**（`strong` 加个 alpha），不是调色板里那个次要灰 —— 白底绿条
/// 上再用一个灰，那两个字就糊在一起了。调用方（`TitleBar::applyColors()`）靠这一位知道
/// "该走压淡，还是该走调色板那个次要色"。**只有标题栏是 true。**
bool partLightInk(Part part);

/// 这一块上铺的那张图：路径 + 怎么摆 + 多透。没设过时 `path` 是空的（`isSet()` 为假）。
///
/// NOTE: 这一层**不碰磁盘**（路径存不存在、解不解得开都不在这儿判）—— `ThemeBackground`
/// 画的时候读不到了就只铺底色，不报错也不弹窗。理由是那一步在**每一次重绘**上：
/// 一张存在 U 盘里、拔了盘的主题图不该让整个界面画不出来。
struct PartImage {
    QString path;
    /// 用户挑的那张**原来叫什么**（见 `PartStyle::imageName`）。可能是空串 —— 界面那一侧
    /// 拿它显示之前先看空不空，空的话退回落盘那个文件的名字。
    QString name;
    ImageFit fit = ImageFit::Cover;
    int opacity = 100;
    bool isSet() const { return !path.isEmpty(); }
};
PartImage partImage(Part part);

/// 按盘上那张图解一份出来，**同一份文件只解一次**。
///
/// 判据是 `路径|文件大小|修改时间`：同一个路径换了另一张图时后两位会变。同一张背景图
/// 可能被好几块 `ThemeBackground` 和整窗底（`Glass::Backdrop`）一起用，各解一次就是
/// 同一份解码付好几遍（主题切换那一下最明显）。缓存按张数封顶，超了就丢最久没用的那张。
///
/// 读不出来（删了、挪走了、根本不是图片）返回空图，不报错 —— 调用方按"只有底色"处理。
QImage cachedImage(const QString &path);

/// 提示框四周那一层。**就是 `partBackground(Part::Scrim)`** —— 单独留一个名字是因为
/// `OverlayHost::scrimColor()` 用这个名字。
QColor scrimColor();

/// 语义色。**参数的调色板决定没覆盖时是哪一档**（深底上用亮的、浅底上用深的），
/// 覆盖过就一律用覆盖的那一个（覆盖优先）。
QColor dangerColor(const QPalette &palette);
QColor warningColor(const QPalette &palette);
QColor okColor(const QPalette &palette);
/// 表单报错那处红（`installNameErrorColor()`）。和 `dangerColor()` **不是同一个色**：
/// 浅底那一档 #c0392b 和 #c02828 差一点点，各有各的对比度来历，别顺手合并。
QColor errorTextColor(const QPalette &palette);

// ---------------------------------------------------------------------------
// 落盘
// ---------------------------------------------------------------------------

/// 存到 `QSettings`（组织名/应用名在 `src/main.cpp` 里设的那一份，和别的设置同一个窝）。
void save(const Spec &spec);
/// 从 `QSettings` 读回来（读不出来 / 认不出的项一律退回出厂那一档，不报错也不猜）。
Spec load();

// ---------------------------------------------------------------------------
// 导入 / 导出（提供导入/导出主题）
// ---------------------------------------------------------------------------
//
// 一份主题进得去、出得来。**换成 JSON 只是为了"用户能手看、能手改"**，字段和
// `QSettings` 里那套**一一对应**（`preset` / `textOpacity` / `accent` / `roles` /
// `parts` / `semantic`），所以看盘上那一份和看导出来的那一份是同一件事。
//
// 里头的**图片路径是导出这台机器上的那一条**（主题里存的就是路径）。换台机器导入之后
// 那几张图找不到 —— 这是诚实的：图**不在**这一份文件里（真要带上就得把几兆的位图塞进
// 一个 JSON）。`MainWindow` 导入之后会自己数一遍找不到的，并说一句（见那边）。

/// `Spec` → JSON（颜色一律 `#aarrggbb`，和 `QSettings` 里那套同一个写法）。
QJsonObject specToJson(const Spec &spec);
/// JSON → `Spec`。认不出的项一律按出厂那一档处理（走 `normalize()`）。
/// 失败（不是一份主题文件）时返回 false 并写 `*error`（参数可给 nullptr）。
bool specFromJson(const QJsonObject &json, Spec *out, QString *error);

// ---------------------------------------------------------------------------
// 跟着调色板重算
// ---------------------------------------------------------------------------

/// `follow()` 的实现体（模板里只有这一句转发，好让模板不依赖 `QWidget` 的完整定义）。
void followImpl(QWidget *owner, std::function<void()> fn);

/**
 * @brief 把"主题一变就重算一遍"登记在 @p owner 身上。
 *
 * QSS 里嵌死的颜色（`.arg(color.name())` 那种）和自绘控件存下来的那几份色**不会**自己
 * 跟着调色板走，得有人重算。这一发就是那个登记处：`fn` 在 owner 收到"调色板变了"时跑
 * 一遍，owner 没了这一条自己掉（parent 就是 owner）。
 *
 * NOTE: 【防自咬】`fn` 里多半就是 `setStyleSheet()` / `setPalette()`，而这两样各自会再发
 * 一次 `PaletteChange` —— 不拦的话就是"重算 → 又收到 → 再重算"。所以重算期间来的
 * 事件一律忽略（见 .cpp 里那个 `m_running`）。
 *
 * NOTE: 浮层 / 菜单 / 列表里那些**每次现建**的控件不用登记：它们下一次打开就是新的。
 * 要登记的是**常驻**的那几处（底栏那几颗按钮、头像、日志窗那五颗筛选）。
 */
template <typename F>
void follow(QWidget *owner, F &&fn) {
    if (owner == nullptr) {
        return;
    }
    followImpl(owner, std::function<void()>(std::forward<F>(fn)));
}

}  // namespace Theme
