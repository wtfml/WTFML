/**
 * @file InlineMessage.hpp
 * @brief `QMessageBox` 的替身：提示开在**主窗口内容区中间**（四周变暗），不再另开一个窗。
 *
 * 【为什么要有这一层】
 * "说一句""问一句"是界面里最碎、最常出现的一类，统一走这里，保证**不出现独立窗口**。
 * 这里给出一组**和 QMessageBox 同名同义**的封装：
 *
 * QMessageBox::warning(this, t, x); → InlineMessage::warning(m_ui->overlay(), t, x);
 *
 * 语义一个字不变（同步返回、按钮含义、默认按钮），调用点可以单独编过、单独验。
 *
 * 【尺寸只有一套】
 * 面板宽度 = `OverlayHost::promptContentWidth()`（唯一出处）。所有提示一样宽，
 * 只有高矮随字数走 —— 不允许出现尺寸不一样的弹窗。
 *
 * NOTE: **不画图标**。QMessageBox 那个感叹号/问号这里没有替代品 —— 标题已经把那句话
 * 的性质说清楚了（「下载失败」「删除账号」），加一个自己画的图标反而是新开销。
 * 要加的话加在这里，别在调用点各画各的。
 */

#pragma once

#include <QColor>
#include <QDialog>
#include <QString>
#include <QStringList>

class QCheckBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class OverlayHost;

/// 这一句话是哪一类。只影响**标题的颜色**（见 .cpp 里那段 NOTE）。
enum class MessageKind {
    Information,
    Warning,
    Critical,
    Question,
};

/**
 * @brief 内容区中间那一块提示面板。
 *
 * 它是 `QDialog`（而不是普通 `QWidget`）：这样 Esc = 拒绝、回车 = 默认按钮、`finished()`
 * 这些"一问一答"该有的东西全是白拿的，而且能直接塞进 `OverlayHost::execPrompt()`。
 * **它自己从来不是窗口** —— 覆盖层会先把 `Qt::Dialog` 摘掉再摆上去。
 */
class InlineMessageBox : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief 构造。
     * @param kind 哪一类（只影响标题颜色）。
     * @param title 标题（粗体那一行）。
     * @param text 正文。**可以多段**（里面有 \n\n 就是分段），宽度是固定的，
     * 所以换行由它自己撑开 —— 调用点不用管排版。
     * @param buttons 按钮文字，从左到右。空的话补一颗「确定」。
     * @param defaultIndex 默认按钮的下标（回车打的那一颗）。**-1 = 没有默认按钮** ——
     * "别让一次误按回车就把事办了"那种场合用它（比如「停止游戏」）。
     * @param destructiveIndex 危险动作那一颗（停止游戏 / 删除）使用危险色，同排按钮等宽。**-1 = 没有**。
     * @param parent 父控件（覆盖层会重新认领它，这里给不给都不影响它是子控件）。
     */
    InlineMessageBox(MessageKind kind, const QString &title, const QString &text,
                     const QStringList &buttons, int defaultIndex, int destructiveIndex,
                     QWidget *parent = nullptr);

    /// 被点中的按钮下标。**关掉 / 按 Esc / 点外面都停在 -1**（= 什么都没选）。
    int clickedIndex() const { return m_clicked; }

    /// 这一句是哪一类。**给验证工程用的**：验证想知道"这是出错还是一句报信"，读的就是
    /// 这一格 —— 只占一行的公开访问器，比让测试去猜标题的颜色结实。
    MessageKind kind() const { return m_kind; }

    /**
     * @brief 在**正文和按钮行之间**插一行输入框，返回它。
     *
     * 这是 `QInputDialog::getText` 那一档的替身（「重命名实例」「新建分组」这些问一句名字的）。
     * 之所以不是另一个类：卡片、宽度、按钮行、Esc/回车的规矩这里全都已经有了，唯一多出来的
     * 就是这一格 —— 再写一个 `InlineInputBox` 等于把那一套抄第二遍。
     *
     * NOTE: 别在 `InlineMessageBox` 的 ctor 上加参数来做这件事：这一格**只有**"问名字"那一档
     * 要，而 ctor 的六个参数已经够长了。多一步 `setLineEdit()`，调用点一眼看得出它多
     * 要了什么。
     */
    QLineEdit *setLineEdit(const QString &initial);

    /// 那一行现在写的是什么。没插过输入框就是空串。
    QString lineEditText() const;

    /**
     * @brief 在正文和按钮行之间放一张**列表**，返回它。
     *
     * 这是 `QInputDialog::getItem` 那一档的替身（「装到哪个实例里」「用哪一个」这种
     * "从现成的几项里挑一个"）。和 `setLineEdit()` 同一个理由长在这个类上：卡片、宽度、
     * 按钮行、Esc/回车的规矩这里全都有了，再写一个 `InlineListBox` 等于把那一套抄第二遍。
     *
     * @param items 每一行显示的字，从左到右就是从上到下。
     * @param initialRow 一开始选中第几行（越界 / −1 = 不选）。
     *
     * 高度**不跟条数长**：最多露 `kListVisibleRows` 行，再多就在列表自己内部滚 —— 一页
     * 提示的高度不该由"用户有几个实例"决定（选到一个只有两行的框和选到一个铺满屏幕的框，
     * 是两种东西）。
     * NOTE: 和 `setLineEdit()` 一样是**可选的一步**，不进 ctor；也**只能插一张**（再调一次
     * 就是换内容 + 换选中项，不插第二张）。
     */
    QListWidget *setList(const QStringList &items, int initialRow);

    /// 列表里现在选中第几行。**没插过列表、或者一行都没选 = −1**。
    int listRow() const;

    /// 插进来的那张列表（没插过就是 nullptr）。给验证工程用。
    QListWidget *listWidget() const { return m_list; }

    /**
     * @brief 在正文下面、按钮行上面补一行**小字**（比正文小一号、颜色浅一档）。
     * @param text 那句话。**空串 = 不补**（视图里不留一行空的）。
     *
     * 「可以在设置里关掉」这一类**附注**：它必须看得见，但不该和正文抢注意力 ——
     * 正文是"现在要出什么事"，这一行是"以后不想看见它去哪儿关"。
     * 字号和颜色都取自本控件的调色板（`QPalette::PlaceholderText`，全仓"比正文浅一档"的
     * 唯一去处），所以换主题时它跟着走，不用在调用点各写一遍颜色。
     *
     * NOTE: 和 `setLineEdit()` 一样是**可选的一步**，不进 ctor：ctor 那六个参数已经够长了，
     * 而这一行只有"带附注"的那两处要。
     */
    void setNote(const QString &text);

    /// 那一行小字现在写的是什么。没设过就是空串。
    QString noteText() const;

    /**
     * @brief 在正文（和附注）下面、按钮行上面补一个**勾选框**，返回它。
     *
     * 这是"我确认过了"那一档（服务端首次启动的 EULA 闸用它）。之所以长在这个
     * 类上而不是另写一个框：卡片、宽度、按钮行、Esc/回车的规矩这里全都有了 —— 同
     * `setLineEdit()` / `setList()` 那两条的理由。
     *
     * @param text 勾选框右边那一句。照抄调用方给的原话（EULA 那一句的出处是
     * `ServerRun::eulaCheckBoxText()`）。
     * @param acceptButtonIndex **勾了才能按的那一颗按钮**（-1 = 不绑）。
     * 这是这一格真正的用途：EULA 那道闸上，「同意并启动」在用户没勾之前必须是灰的
     * —— 允许他先按那一颗再回头补勾，就等于"那一勾没拦任何东西"，而它恰恰是这句
     * 承诺（`eula=true` 只由用户那一下摁出来）唯一的技术落点。越界 / -1 = 不绑。
     * @return 那个勾选框。**一开始是没勾上的**（我们不替任何人预勾）。
     */
    QCheckBox *setCheckBox(const QString &text, int acceptButtonIndex = -1);

    /// 那个勾选框勾上了没有。**没插过 = false**（没得勾，也就不算"确认过了"）。
    bool checkBoxChecked() const;

    /**
     * @brief 在正文和按钮行之间插一块**调用方自己搭的东西**，返回它。
     *
     * 给"这一格上面那几种形状都装不下"的那一档用（今天只有取色卡片 `InlineMessage::pickColor()`
     * —— 它要在提示里摆一张色板）。上面那几条 `setLineEdit()` / `setList()` / `setCheckBox()`
     * 说的都是"某一件事"，所以它们各自知道自己长什么样；这一条不描述内容，只提供一个位置。
     *
     * 宽度**钉成 `OverlayHost::promptContentWidth()`**（和那几条同一条规矩）：卡片的高矮
     * 才算得准，而且"所有提示一样宽"这件事不会因为谁塞了一块宽东西就破掉 —— 塞宽了是
     * **被卡片裁掉**，不是把卡片撑开。选择器可显式指定 preferredWidth，窄窗口仍按可用宽度收缩。
     * NOTE: 只能插一块（再调一次就是换内容）。和 `setLineEdit()` 一样**不进 ctor**。
     * NOTE: 摆上去之后，这一块里**第一个 `QLineEdit`** 会拿到焦点（和 `setLineEdit()`
     * 那一条同一个理由、同一个时机，见 `showEvent()`）。
     * NOTE: 这一块里挂了属性 `wtfmlPromptActionsLead` 的那一格会被挪到按钮行左边（预设弹窗的汇总）。
     */
    QWidget *setContent(QWidget *content, int preferredWidth = 0);

    /// 插进来的那一块（没插过就是 nullptr）。给验证工程用。
    QWidget *content() const { return m_content; }

    /// 插进来的那个勾选框（没插过就是 nullptr）。给验证工程用。
    QCheckBox *checkBox() const { return m_checkBox; }

    /**
     * @brief 在正文下面补一行**可点的链接**，返回那个标签。
     *
     * 正文下面这一行可点的链接让"同意"两个字有出处：让人把它打在屏幕上之前，
     * 得能真看见他同意的是什么。
     *
     * @param text 显示的字（**不是**网址本身：一个光秃秃的 https://… 摆在提示里没人会读）。
     * @param url 点了之后去哪。
     *
     * 它自己**不开浏览器**：点了只发 `linkActivated(url)`，谁去开由调用方决定 —— 和
     * `ResourceDetailPage` 那条一模一样（那一层要能写日志、开不了还要弹一句）。所以这里
     * `setOpenExternalLinks(false)` 是**故意的**，别改成 true：改了之后这一路会绕过日志和
     * 失败提示，而那两样正是"点了没反应"这类故障唯一的线索。
     * NOTE: 和 `setLineEdit()` 一样是**可选的一步**，不进 ctor；也**只能插一行**（再调一次
     * 就是换字 + 换落点）。
     */
    QLabel *setLink(const QString &text, const QString &url);

    /// 换掉标题左边图标块里的图标（底色仍按 kind）。弹框说的是某样东西时用它，
    /// 和 JavaMismatchDialog 的 java 图标同一个做法；空串 = 退回按 kind 取。
    void setHeaderIcon(const QString &icon);

    /// 那一行链接现在指向哪儿。没插过就是空串。
    QString linkUrl() const;

    /// 插进来的那个链接标签（没插过就是 nullptr）。给验证工程用。
    QLabel *linkLabel() const { return m_linkLabel; }

    // ---------- objectName（验证工程靠它们认控件） ----------
    static QString titleLabelName() { return QStringLiteral("inlineMessageTitle"); }
    /// 设计稿对话框头（tokens-extra §对话框）：32px 语义色图标块 + 手写体 22 标题。
    /// @p icon 空 = 按 @p kind 取（信息/提问 info、警告 warn、出错 xc）。JavaMismatchDialog 也用它。
    static QHBoxLayout *makeHeader(QWidget *owner, QLabel *title, MessageKind kind,
                                   const QString &icon = QString());
    /// 插进来的那一行输入框。**只有"问名字"那一档有。**
    static QString lineEditName() { return QStringLiteral("inlineMessageEdit"); }
    /// 插进来的那张列表。**只有走过 `setList()` 的才有。**
    static QString listName() { return QStringLiteral("inlineMessageList"); }
    /**
     * @brief `setList()` 那张列表**最多露几行**（再多就在列表内部滚）。
     *
     * 这是一个**规格数**，验证工程读它，别在测试里写死 6 —— 改了这里，"多出来的那几行
     * 不再把卡片撑高"那条断言要跟着一起改（它量的就是这个数）。
     */
    static int listVisibleRows();
    /// 正文下面那一行小字。**只有走过 `setNote()` 的才有。**
    static QString noteLabelName() { return QStringLiteral("inlineMessageNote"); }
    /// 插进来的那个勾选框。**只有走过 `setCheckBox()` 的才有。**
    static QString checkBoxName() { return QStringLiteral("inlineMessageCheck"); }
    /// 插进来的那一行可点链接。**只有走过 `setLink()` 的才有。**
    static QString linkName() { return QStringLiteral("inlineMessageLink"); }
    static QString textScrollName() { return QStringLiteral("inlineMessageTextScroll"); }
    static QString textLabelName() { return QStringLiteral("inlineMessageText"); }
    static QString buttonRowName() { return QStringLiteral("inlineMessageButtons"); }
    /// 第 index 颗按钮的名字。**位置是能变的**（按钮表由调用方给），所以认按钮要按下标，
    /// 不要按文字 —— 文字是显示内容，会被改。
    static QString buttonName(int index) {
        return QStringLiteral("inlineMessageButton%1").arg(index);
    }

    /// 第 index 颗按钮。**没那一颗就是 nullptr**（`setCheckBox()` 要把某一颗绑上去）。
    QPushButton *buttonAt(int index) const;

signals:
    /**
     * @brief 用户点了那一行链接（`setLink()` 插的那一行）。
     * @param url `setLink()` 当时给的那一串。
     *
     * 这一层**不开浏览器**（见 `setLink()` 那段），所以这条信号是唯一的出口；没人接就
     * 等于用户点了没反应 —— 调用方一定要接。
     */
    void linkActivated(const QString &url);

protected:
    /**
     * @brief 摆上去之后把光标放回输入那一格（只有 `setLineEdit()` 插过的那一档有事做）。
     */
    void showEvent(QShowEvent *event) override;

private:
    /**
     * @brief 按**屏幕上看到的顺序**（先上后下、同一行先左后右）把这一窗里的 Tab 顺序重排一遍。
     *
     * 为什么非要有这一下：焦点链默认跟的是**创建顺序**，而这个类里「输入框 / 列表 /
     * 小字那一行 / 那个勾 / 那行链接 / 调用方自己那块」全是 `setXxx()` 插进来的
     * —— 它们创建得比**按钮行还晚**（ctor 里那一排按钮建完之后才轮到它们）。照默认走，
     * Tab 从输入框出发会**往回跳**到下面那排按钮上，再按一下又跳回输入框：眼睛看着的顺序
     * 和键盘走的顺序正好拧着。每一处"插了一行"之后都调它一次，位置变了就重排。
     */
    void relinkTabOrder();

    int m_clicked = -1;
    MessageKind m_kind = MessageKind::Information;
    QLabel *m_titleLabel = nullptr;
    QHBoxLayout *m_header = nullptr;
    QLabel *m_textLabel = nullptr;
    QLineEdit *m_lineEdit = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_noteLabel = nullptr;
    QCheckBox *m_checkBox = nullptr;
    QLabel *m_linkLabel = nullptr;
    QWidget *m_content = nullptr;
    QString m_linkUrl;
};

namespace InlineMessage {

/// 报一句就走（`QMessageBox::information` 的替身）。
void information(OverlayHost *host, const QString &title, const QString &text);
/// 同上，warning 那一档。
void warning(OverlayHost *host, const QString &title, const QString &text);
/// 同上，critical 那一档。
void critical(OverlayHost *host, const QString &title, const QString &text);

/**
 * @brief 问一句（`QMessageBox::question(..., Yes | No, default)` 的替身）。
 * @param destructive "是"那一颗是不是**不可撤销**的（删除那一类）。**默认 false**。
 * 它真的会染色：`true` ⇒ 那颗按红（`ButtonKind::Danger`）、另一颗按
 * 黑（`Neutral`）。
 *
 * **标错比不标更糟**：标成 `true` 而其实能撤销，用户学会的是"红不代表什么"；
 * 真不可撤销的没标，红就白设了。判据只有一条 —— **点下去之后盘上少了一样
 * 用户自己找不回来的东西**。所以"从列表里移除文件夹""删除账号"里只有后者是红的
 * （前者盘上那些文件一个都不动）。
 * @return 用户点了"是"那一颗。
 *
 * NOTE: 关掉 / 按 Esc / 点外面一律按**"否"**算 —— 和 QMessageBox 一样（Esc 走的是
 * "escape button"）。调用点用 `!= QMessageBox::Yes` 判断时方向要一致。
 */
bool question(OverlayHost *host, const QString &title, const QString &text, const QString &yesText,
              const QString &noText, bool defaultYes, bool destructive = false);

/**
 * @brief 问一句名字（`QInputDialog::getText` 的替身）。
 * @param title 标题。
 * @param label 正文（"这个名字是干什么用的"那句话；可以带 `\n`）。
 * @param initial 输框里预填的内容。
 * @param ok 用户在不在"确定"那一颗上。**取消 / Esc / 点外面都是 false**（和 QInputDialog 一样）。
 * @return 输框里现在的内容。**`ok` 为 false 时也有值** —— 和 `QInputDialog::getText` 一样，
 * 调用点先看 `ok` 就行了。
 *
 * NOTE: 这一档和 `information()` 一样是**同步**的（内部一个嵌套事件循环），所以调用点
 * `const QString x = getText(...)` 那一行的形状一个字没变。
 */
QString getText(OverlayHost *host, const QString &title, const QString &label,
                const QString &initial, bool *ok);

/**
 * @brief 从现成的几项里挑一个（`QInputDialog::getItem` 的替身）。
 * @param items 每一行的字。
 * @param initialRow 一开始选中第几行（越界 = 不选）。
 * @param rowOut 用户选中的行号；**取消 / Esc / 点外面 / 没有覆盖层都是 −1**。
 * @return 用户点了「确定」并且**真的选中了一行**。`rowOut` 只在返回 true 时才有意义。
 *
 * NOTE: 和 `getText()` 一样是**同步**的（内部一个嵌套事件循环），所以调用点的形状是
 * `if (InlineMessage::pickFromList(..., &row)) { … }`。
 * NOTE: `items` 为空时**直接返回 false**、什么都不弹 —— 一个"一行都没有"的列表框没有意义，
 * 而它长得和"有东西可挑"一模一样，用户只会以为界面卡了。调用方那一路要先自己判空
 * （比如"一个实例都没有，先去装一个"）。
 */
bool pickFromList(OverlayHost *host, const QString &title, const QString &text,
                  const QStringList &items, int initialRow, int *rowOut);

/**
 * @brief 挑一个颜色（`QColorDialog` 的替身）。
 *
 * 颜色选择也不该另开一个系统对话框，而"挑一个颜色"正是那种一句话就能问完
 * 的小事 —— 所以它开成**内容区中间那一块卡片**（和别的提示一模一样宽），不是系统那个
 * 颜色对话框。卡片里是一张**色板**（4 行 × 8 块，点一下就填进右边那一格）+ 一行十六进制
 * 输入框（能手打、能手改别人给的色号）。
 *
 * @param title 标题（「主题色」「危险色」这些 —— 由调用方说清在挑哪一处）。
 * @param text 一行说明（"这一处管的是画面上哪儿"）。
 * @param initial 一开始填着的色。
 * @return 用户挑的那个色。**取消 / Esc / 点外面都给一个无效色**（`isValid()` 为假）——
 * 调用点那一行的形状是 `const QColor picked = InlineMessage::pickColor(...);
 * if (!picked.isValid()) return;`。
 *
 * NOTE: 和 `getText()` 一样是**同步**的（内部一个嵌套事件循环）。
 * NOTE: 「确定」那一颗在输入框里不是一个能认出来的色之前**是灰的** —— 认不出的色号点了也
 * 没有意义，而"点了确定、颜色没变"是那种最难解释的失败。色板点一下就一定是合法的。
 */
QColor pickColor(OverlayHost *host, const QString &title, const QString &text,
                 const QColor &initial);

/// 取色卡片里那一行十六进制输入框的 objectName（`pickColor()` 建的那一块）。给验证工程用。
QString colorEditName();
/// 取色卡片里色板第 @p index 块的 objectName（0 ~ `colorSwatchCount()-1`）。
QString colorSwatchName(int index);
/// 取色卡片里那个「现在选的是哪个色」的预览块。
QString colorPreviewName();
/// 色板一共几块（4 行 × 8 列）。
int colorSwatchCount();

/**
 * @brief 按钮表自己定的那种问话。
 * @param kind 哪一类（只影响标题颜色）。
 * @param buttons 按钮文字，从左到右。**空的话补一颗「确定」**。
 * @param defaultIndex 默认按钮下标，-1 = 没有（危险动作那两颗一律给 -1：误按回车不该把事办了）。
 * @param destructiveIndex 危险动作那一颗，-1 = 没有。**它会按红**。
 * 判据只有一条：**点下去之后盘上少了一样用户自己找不回来的东西**；其余按位置分档
 * （默认那一颗 = 主题色，剩下的 = 黑），见 `InlineMessageBox` 的 ctor。
 * @param dismissOnOutsideClick 点框外面算不算"关掉它"。**默认 false**。
 * @param note 正文下面那行**小字**（`InlineMessageBox::setNote()`）。**空串 = 不补**。
 * 放在最后是因为它和上面那几格不是一类：上面几格决定"这个框是什么"，这一格只是
 * 附注一下 —— 两处用到它的调用点（启动前那两条确认）写的也都是"提醒本身 + 去哪儿关"。
 * @return 被点中的下标；关掉 / Esc / 点外面 = **-1**。
 */
int ask(OverlayHost *host, MessageKind kind, const QString &title, const QString &text,
        const QStringList &buttons, int defaultIndex = -1, int destructiveIndex = -1,
        bool dismissOnOutsideClick = false, const QString &note = QString());

}  // namespace InlineMessage

