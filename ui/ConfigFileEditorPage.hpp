/**
 * @file ConfigFileEditorPage.hpp
 * @brief 「实例设置 → 文件管理 → 配置文件」点开一个文件之后那一块：行号 + 纯文本编辑框
 * +「返回」+「打开文件夹」+「保存」。
 * @note 这一块是一个**文本编辑器**：**不解析、不重排、不"识别"**。模组的配置文件
 * 都以文本形式存储，直接按 txt 来编辑就够；"它是格式化的、应该能识别"不是这一块
 * 要解决的事 —— 启动器手上只有文件和它的注释（见下一段）。
 *
 * 【为什么不做成"勾选框 / 滑条"那种界面】
 *
 * 因为那是**做不到**的，不是没做。ModMenu 的
 * `com/terraformersmc/modmenu/api/ConfigScreenFactory` 是个 `@FunctionalInterface`，唯一
 * 方法 `Screen create(Screen parent)` —— 它把"给我一个配置界面"这件事**转交给模组自己的
 * Java 代码**，界面上的类型、范围、翻译全是模组用 Cloth Config / YACL 在**代码里**声明的。
 * 那份声明只存在于**游戏进程**里，启动器在外面拿不到。（ModMenu 自己唯一跟文件沾边的功能
 * 是 `modmenu.configsFolder` = 「打开配置文件夹」。）
 *
 * 【左边那一格「条目」：条目 ↔ 值的一一对应】
 *
 * 条目 ↔ 值能一一对应，而且不用外接库 —— 见 `ConfigEntries.hpp` 那一段（注释里
 * 常常就写着说明和 `#Range:`，而类型文件自己就说了）。这一格就是那份清单的落点：
 *
 * - 左边一棵树，**一行一个设置项**（分组 = 段 / 嵌套对象，行上是 `键 = 值`）；
 * - 点一条 → 右边编辑框里**那一格值的字节区间被选中**并滚到眼前（只选、不改）。
 *
 * NOTE: **它不是表单**，别往那个方向改。表单要的是"这一条是什么类型、范围多少、叫什么
 * 名字"（schema），而那份 schema 只活在模组自己的 Java 代码里 —— 启动器手上只有
 * 文件和它的注释。所以这一格干的事是"**帮你找到那一行**"，改还是改文本。
 * NOTE: 这一格是**只读**的：不改正文、不写盘。于是它一个字节的风险都没有 —— 没有它，
 * 用户得用 Ctrl+F 拿注释里那句英文去猜键名。
 *
 * 【为什么它是 QWidget，不是像资源详情页那样一个 QDialog】
 *
 * 因为**开不出第二页**：`OverlayHost::openPage()` 在已经有人占着那一层的时候直接返回
 * nullptr（一层只放一个页面，见 OverlayHost.hpp 那段），而点这一行的那一刻，「实例设置」
 * 自己正占着那一层。所以这一块是**嵌在「配置文件」那一页里面**的 —— 那一页是一个两格子的
 * 堆叠：① 文件列表 ② 这一块。点一个文件 = 换到第二格，点「返回」= 换回第一格。
 *
 * NOTE: 正因为嵌在里面，它**自己带一颗「返回」**（覆盖层那颗返回到的是整个实例设置，不是
 * 这一张列表）。那颗两格之间来回切的事由 InstanceSettingsDialog 办，这一层只发
 * `backRequested()`。
 *
 * 【这一层不碰盘】
 *
 * 和 InstanceSettingsDialog 同一条约定：**读盘、写盘、开文件夹、弹话**都是 MainWindow 的
 * 事（这一层手上没有 OverlayHost，而弹一句话必须有它）。但有**一半**不是 I/O：把编辑框里
 * 的正文合回字节（行尾、BOM）—— 那是纯计算，和 `ConfigFileText` 是同一套东西，所以落在这
 * 一层。分工是：
 *
 * - 这一层：`Loaded`（原字节 + 正文 + 行尾 + BOM）在手，**算出要写哪些字节**；
 * - MainWindow：把那些字节写到那个路径上，回一句结果。
 */
#pragma once

#include <QByteArray>
#include <QString>
#include <QWidget>

#include "ConfigFileText.hpp"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class ConfigFileEditorPage : public QWidget {
    Q_OBJECT

public:
    explicit ConfigFileEditorPage(QWidget *parent = nullptr);

    /// 摆一份文件（点开一条时由 InstanceSettingsDialog 调）。
    ///
    /// @param path 绝对路径（「打开文件夹」要用它算目录，保存要写回它）。
    /// @param loaded 那**一份字节**解出来的东西（见 ConfigFileText::decode）。这一块把它
    /// **留着**（`isDirty()` 要拿它比、保存时要拿它拼行尾），所以进参是值不是引用。
    /// @param stamp 读它那一刻**盘上那一份的大小和修改时间**（见 ConfigFileText::Stamp）。
    /// 保存前拿它和盘上比一眼 —— 对不上说明这中间别人动过（游戏退出时写回、
    /// 或者「打开文件夹」那个窗里改过），那就**不许静静盖掉**。
    /// **不给 = 不知道**，于是这一趟不比对（验证工程合成的那几份走的就是这一档）。
    ///
    /// NOTE: 摆一份新文件**同时会把「这个文件」的那两件事清掉** —— 「它留过备份没有」回到
    /// false、「框里是从备份取回来的」清空（上一条的答案对这一个文件不作数）。所以
    /// 调用方摆完这一份要接着调 `setBackupAvailable()`；不调的话那颗按钮就是不画，
    /// 而这正是"不知道"该有的样子。
    void setFile(const QString &path, const ConfigFileText::Loaded &loaded,
                 const ConfigFileText::Stamp &stamp = ConfigFileText::Stamp());

    /// 现在框里的正文（行尾按 `'\n'`）。**唯一出处是那个编辑框** —— 调用方别自己拼
    /// "用户改了什么"，问它就够了。
    QString text() const;
    /// 和读进来那一份不一样了没有。
    bool isDirty() const;
    /// 这个文件现在给不给改（见 ConfigFileText::Loaded::editable）。只读时「保存」
    /// **不画**（不是画成灰的 —— 灰按钮说的是"还没资格点"，而这儿的问题是"点了也没意义"）。
    bool isEditable() const;
    /// 这一块现在开着的是哪个文件（空串 = 还没 setFile）。
    QString path() const;

    /// 写盘**成功了** —— 底下那行字换成「已保存（时:分:秒）」。由 MainWindow 在
    /// `saveRequested` 之后调回来（它是唯一知道写成了没有的人）。
    ///
    /// NOTE: **没有"写失败了"的那一半**，这是有意的：写不进去（盘满、只读、被占用）是一件
    /// **挡住用户操作**的事 —— 用户以为改好了、盘上其实还是旧的。所以按产品那条硬
    /// 约束（「会挡住用户操作的错误必须弹出来」）它走 MainWindow 的弹窗，不在这一块
    /// 角上写一行小字。这一层也不为它造第二种颜色（仓库里"红"只有日志窗那一档）。
    void markSaved();

    /// 读它那一刻盘上那一份的样子（见 setFile() 的 @param stamp）。
    ConfigFileText::Stamp stamp() const { return m_stamp; }
    /// 存成功之后由 MainWindow 把新的一份记回来 —— 不更新的话，**用户连着存两次**，第二次
    /// 会被自己上一次写下去的那一份判成"盘上被人改过了"。它要的是刚写下去那个 Stat。
    void setStamp(const ConfigFileText::Stamp &stamp) { m_stamp = stamp; }

    // ---------- 世代备份 ----------
    // / 这个文件**有没有留下过备份**（有 ⇒ 「还原上一次」画出来且点得动）。
    // /
    // / NOTE: 由 MainWindow 在打开文件时告诉它（这一层不碰盘 —— 见文件头那段）。而每一次存
    // / 完也会变成 true：那一次存之前刚留了一份。
    bool backupAvailable() const { return m_backupAvailable; }
    void setBackupAvailable(bool available);
    /// 把「上一次」那份内容摆回编辑框里。**只是摆进去**：于是它变脏、底下那行字说清楚这是
    /// 什么时候的那一份，用户按「保存」才真的写回盘上 —— 「还原」不许是一次悄悄的写入。
    void restoreContent(const QString &text, const QString &whenText);

    /// 用户选了"不保存"：把编辑框退回**读进来那一份**，于是 `isDirty()` 变 false。
    ///
    /// NOTE: 这一条和「改过就不许悄悄关掉」是一对：点「返回」时 MainWindow 先看 isDirty()，
    /// 改过就弹一句问，用户说"不保存"才走这儿再切回列表 —— **改过的东西不许悄悄没掉**。
    void discardChanges();

    // ---------- 左边那一格「条目」----------
    // / 左边那一格现在摆着没有（藏着的时候是"这份文件里没认出设置项"，或者用户把它收了）。
    bool indexVisible() const;
    /// 重新认一遍、重摆那棵树。**改完字由那个延时器叫它**（见构造函数里那个 250 ms）。
    /// @return 认出来几条（**和树上摆着的条数不是一回事**：树是按路径分了组的）。
    int rebuildIndex();
    /// 现在树上摆着几条**叶子**（= 几个设置项）。验证工程拿它量"这份文件认出了几条"。
    int indexEntryCount() const;

    // ---------- 名字（验证工程按这些 objectName 找控件）----------
    // / **这一块自己**那个 objectName（`editorName()` 是里面那个编辑框的，别混）。
    static QString pageName();
    static QString editorName();
    static QString pathLabelName();
    static QString noteLabelName();
    static QString saveButtonName();
    static QString openFolderButtonName();
    static QString backButtonName();
    static QString indexPanelName();
    static QString indexTreeName();
    static QString indexFilterName();
    static QString indexToggleName();
    static QString restoreButtonName();

    /// 值那一格在树上的区间记在哪两个角色里（`Qt::UserRole` 起，20 往后 —— 避开
    /// InstanceFilesPage / 版本树那几处已经占掉的 8/9/10 与 12/13）。
    static int valueBeginRole();
    static int valueEndRole();
    /// 这一条在树上的**键路径**（`general.timer` 这种）。重建之后"还选中原来那条"靠它，
    /// 验证工程也拿它认"点的是哪一条"。
    static int entryPathRole();

    static QString backText();
    static QString saveText();
    static QString openFolderText();
    static QString indexToggleText();
    static QString indexFilterPlaceholder();
    /// 「还原上一次」那颗按钮上的字。
    static QString restoreText();
    /// 取回备份之后底下那行字（后面接"那份是什么时候的"）。
    static QString restoredPrefix();
    /// 「条目」那颗按钮在"这份文件里没认出设置项"时为什么点不动（它那行说明）。
    static QString indexEmptyHint();
    /// 只读时那行字的前缀（后面接原因）。验证工程拿它认"这一块现在只读"。
    static QString readOnlyPrefix();
    /// 可编辑、还没存过的时候那行字。
    static QString hintText();
    /// 存成功之后那行字的前缀。
    static QString savedPrefix();

    /// 行号那一格现在有多宽（0 = 没有）。验证工程拿它量"行号真的留出位置了、而且行数变多
    /// 它会变宽"。
    int gutterWidth() const;

    // ---------- 给验证工程用的访问器 ----------
    QPlainTextEdit *editor() const;
    QPushButton *saveButton() const { return m_save; }
    QPushButton *openFolderButton() const { return m_openFolder; }
    QPushButton *backButton() const { return m_back; }
    QLabel *noteLabel() const { return m_note; }
    QLabel *pathLabel() const { return m_pathLabel; }
    QTreeWidget *indexTree() const { return m_index; }
    QLineEdit *indexFilter() const { return m_indexFilter; }
    QPushButton *indexToggle() const { return m_indexToggle; }
    QPushButton *restoreButton() const { return m_restore; }
    QPushButton *historyButton() const { return m_history; }
    /// 点树上那一条（`jumpTo` 的正门）—— 验证工程合成点击时不想真去造一个鼠标事件走
    /// 事件派发的话，用它。**走的还是同一条路**（`itemClicked` 那个连接调的就是它）。
    void jumpToEntry(QTreeWidgetItem *item);

signals:
    /// 查看当前文件的历史备份；读取内容只进入编辑器，不直接覆盖磁盘。
    void historyRequested(const QString &path);
    /// 用户点了「保存」。@param bytes 是**已经合好的整份字节**（行尾、BOM 都还完了，见
    /// `ConfigFileText::encode`）—— 接的人直接写到 @param path 上就行，不要自己再拼一遍。
    void saveRequested(const QString &path, const QByteArray &bytes);
    /// 用户点了「打开文件夹」。
    void openFolderRequested(const QString &path);
    /// 用户点了「还原上一次」—— 请把上一次保存前留下的那一份拿来。**这一层不自己取**：
    /// 读备份是读盘，而它连 AppPaths 都够不着（见文件头"这一层不碰盘"）。
    void restoreRequested(const QString &path);
    /// 用户想回到文件列表（点了「返回」，或者按了 Esc）。**这一层不自己切** —— 切回列表是
    /// InstanceSettingsDialog 那两格的事，而且"改过没有"要不要问一句是 MainWindow 的判断。
    void backRequested();

protected:
    /// Esc = 返回列表。**得在这一层接住**：不然它会一路冒到「实例设置」那个 QDialog 上去，
    /// 那一下关掉的是**整个实例设置页** —— 用户想的是"退回文件列表"，结果整个页面没了。
    void keyPressEvent(QKeyEvent *event) override;
    /// **焦点只能在这一刻补**。这一块是嵌在堆叠里的，切到它那一格时 Qt 会把焦点给新露面
    /// 的控件，而这一块**不拿到焦点就打不了字**（这是它唯一的主要功能）。切回来的时候
    /// 也要补 —— 用户在列表上点了一下，焦点在那张列表上。
    void showEvent(QShowEvent *event) override;

private:
    /// 底下那行字回到"这个文件现在该说什么"（只读的原因 / 那句默认提示）。
    void refreshNote();
    /// 按筛选框里那串字，把那棵树该露的露、该藏的藏（**父分组只要底下有中的就留着**）。
    void applyIndexFilter();
    /// 左边那一格该露还是该收（用户那颗「条目」按下的状态 + 这份文件认没认出东西）。
    void refreshIndexVisibility();
    /// 「还原上一次」那一颗该露不该露。**两个条件缺一不可**：这个文件留过备份、而且它现在
    /// 是可编辑的（只读的文件取回来也写不回去，摆一颗按不动的按钮只是噪音）。
    void refreshRestore();

    QPlainTextEdit *m_editor = nullptr;
    QLabel *m_pathLabel = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_back = nullptr;
    QPushButton *m_openFolder = nullptr;
    QPushButton *m_restore = nullptr;
    QPushButton *m_history = nullptr;
    QPushButton *m_save = nullptr;

    QWidget *m_indexPanel = nullptr;
    QTreeWidget *m_index = nullptr;
    QLineEdit *m_indexFilter = nullptr;
    QPushButton *m_indexToggle = nullptr;
    QTimer *m_indexTimer = nullptr;  ///< 改字之后等手停下来再重认一遍（250 ms）
    /// 这份文件现在认出来几条（0 ⇒ 左边那一格收起来）。
    int m_indexEntries = 0;
    /// 正在摆正文（`setFile()` / `discardChanges()` 里那一句）：这期间那台延时器不该响，
    /// 不然刚摆完就被自己叫起来重认一遍。
    bool m_loading = false;

    QString m_path;
    ConfigFileText::Loaded m_loaded;
    bool m_hasFile = false;
    /// 读进来那一刻盘上那一份的样子（见 setFile() 的 @param stamp）。
    ConfigFileText::Stamp m_stamp;
    /// 这个文件留过备份没有（由 MainWindow 告诉它，见 backupAvailable()）。
    bool m_backupAvailable = false;
    /// 框里这一份**是从备份取回来的**的话，那份备份是什么时候的（空串 = 不是）。
    ///
    /// NOTE: 它就是"现在这一份是取回来的"这一位本身（不另开 bool）：底下那行字必须说清
    /// 这件事 —— 不说的话，用户看到的是一份内容变了的文件，却不知道它是哪来的、
    /// 更不知道**它还没写回盘上**。
    QString m_restoredFrom;
};
