/**
 * @file WorldSettingsPage.hpp
 * @brief 「实例设置 → 文件管理 → 存档」点行尾那支笔之后那一块：这个世界的信息、改文件的
 * 那几个开关（收在一个折叠框里）、以及**游戏规则表**（一条一行，值就在行上改）。
 * @note 世界根据其存档文件在存档设置（那支笔）里面编辑游戏规则等，
 * 打开之前弹出坏档警告并提供备份按钮。这一块就是"在存档设置里面编辑"。
 *
 * 【这一页的形状】
 *
 * 世界设置点开是一个额外的页面，修改文件的设置放在一个类似版本下载那里、收起来的
 * 框里，默认收起，展开时弹出备份警告；改文件的设置上方是重命名 / 打开文件夹 / 删除
 * 世界（弹窗确认）；下面的世界设置不要求进了世界才显示，而是把所有项都列出来、并允许
 * 修改值。
 *
 * 落到这一页上就是三段，从上到下：
 *
 * ① **操作区**：一排按钮，重命名、打开文件夹、整份存档备份和恢复、打开备份目录以及
 * 删除世界（删除由 MainWindow 弹窗确认）。点笔就直接进这一页。页眉的面包屑
 * 「返回 / 世界设置：<世界名>」在覆盖层上，文字取 `breadcrumbTitle()`。
 * ② **「修改存档文件」**（`fileToggleName()` 那个折叠框）：`level.dat` 里那几格
 * ——**世界名 / 允许作弊 / 极限模式 / 难度**。这几格游戏里改不了（游戏只有"建世界时
 * 选一次"的那一面），只能动文件，所以它们收在一个**默认收起**的框里；点开的那一刻
 * 由 MainWindow 先弹那句备份警告（`fileEditingRequested()`，见下面那个信号）。
 * ③ **「世界设置」**：游戏规则那张表，**一条不少地列出来**（盘上有的用盘上的值、盘上
 * 没有的用游戏默认值，见 `Rule::inFile`），值就在行上改。
 *
 * 【文件结构】
 *
 * 世界所有能改的设置**都在同一个文件里**：`<存档目录>/level.dat`，它是一个 gzip 包着的
 * NBT，解开来是一棵树，根下面那一格 `Data` 里放着这些：
 *
 * * `LevelName`（字符串）—— 世界名。1.13 起游戏列表显示的是**目录名**，这一格只在旧存档
 * 和部分工具里还看得见（`WorldFiles.hpp` 里 `renameWorld()` 那段也说了这件事）；
 * * `allowCommands` / `hardcore`（Byte，0/1）、`Difficulty`（Byte，0~3）—— 上面 ② 里那
 * 三格；
 * * `GameRules` —— **"名字 → 字符串"的一张表**（`keepInventory` = `"true"`、
 * `randomTickSpeed` = `"3"`），**没有类型**：是开关还是数字框，是看那串字推出来的。
 * ③ 那一张表就是它。
 *
 * 所以"改文件的设置"和"世界设置"其实是同一份文件里的东西（不是两处；
 * 真实结构是上面这一段）。分成上下两段是**危险的收在框里、常用的摊在外面**：
 * 世界名 / 极限模式 / 难度改错了会让这个世界变味甚至打不开，而规则改哪条就是哪条。
 *
 * 盘上那份 `GameRules` **不一定有全部条目**（工具做出来的档、从没进过的档、很老的存档
 * 都可能是空的）。所以 ③ 那一列不是"读出来什么摆什么"，而是拿
 * `WorldFiles::knownGameRules()`（启动器认识的规则全集）并出来的 —— 「不要进了
 * 世界才显示」说的就是这件事。
 *
 * 【它和「配置文件」那一页里那个编辑器的关系】
 *
 * 同一个位置、同一个做法：**嵌在文件页里面的第二个格子**（因为 `OverlayHost::openPage()`
 * 在已经有人占着那一层时返回 nullptr，开不出第二个整页覆盖层，见 InstanceSettingsDialog
 * 里 `m_configStack` 那一段）。点笔 = 切到第二格，点「返回」/ 按 Esc = 切回世界列表。
 *
 * 差别在这儿：配置文件那一页给的是一个**纯文本编辑框**（不解析、只帮你找到那一行），
 * 而这一页给的是**表单** —— 因为世界设置这件事的 schema 我们**有**（见上面那段文件结构），
 * 和模组配置那种"schema 只活在模组的 Java 代码里"（见 ConfigFileEditorPage 文件头）不一样。
 *
 * 【这一层不碰盘、也不解 NBT】
 *
 * 和 `InstanceSettingsDialog`、`ConfigFileEditorPage` 同一条约定：读盘、写盘、弹话都是
 * MainWindow 的事。更要紧的一条是**`ui/` 不许 include `src/`** —— 所以这一页既拿不到
 * `WorldFiles::WorldInfo` 也拿不到 `NbtTree::GameRule`，它摆的东西全在下面那两个**只用
 * Qt 类型**的结构里（`Rule` / `WorldView`）。从盘上那份翻译过来、以及把页面上改过的翻译
 * 回去，都是 MainWindow 那一段的事。
 *
 * NOTE: 这个翻译**故意不省**（两边的字段名是一一对应的，看着像在抄写）：省掉它就得让
 * `ui/` 去 include `src/`，那正是这个仓库唯一一条不许越的界线。抄错一处的话
 * 验证工程量的是**页面上那个控件里的值**，会当场红。
 */
#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

class QAbstractButton;
class ElidedLabel;
class QComboBox;
class QEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSpacerItem;
class QTimer;
class QToolButton;
class QPushButton;
class QVBoxLayout;
class ToggleSwitch;
namespace SettingsParts {
class FoldMotion;
}

class WorldSettingsPage : public QWidget {
    Q_OBJECT

public:
    /// 一条规则的值是什么形状 —— **决定行尾那一格画成什么**（开关 / 只能填数字的框 /
    /// 随便填的框）。三档和 `NbtTree::ValueKind` 一一对应（那边在 src/，这一层够不着）。
    enum class Kind {
        Bool,  ///< `"true"` / `"false"` ⇒ 拨动开关
        Int,   ///< 整数 ⇒ 只收 `-?数字` 的输入框
        Text,  ///< 别的 ⇒ 随便填
    };

    /// 一条规则摆在这一页上的样子。
    struct Rule {
        QString name;
        QString value;  ///< 盘上那一串（Bool 那两档是 `"true"` / `"false"`）
        Kind kind = Kind::Text;
        /// 这一条**在存档文件里**（`Data/GameRules` 里有这个键）吗。
        ///
        /// NOTE: 它**不参与** `signatureOf()`（"改过没有"那把尺子）：用户改不了这一位，
        /// 它是"盘上是什么样"的一半，不是页面上的一份值。
        bool inFile = true;
    };

    /**
     * @brief 这一页上摆的所有东西。**只用 Qt 类型**（见文件头）。
     *
     * 每一位都带一个 `hasXxx` —— 它说的是**盘上有没有这一项**（和 `WorldFiles::WorldInfo`
     * 一样，那边读出来的就是它）。★ 它的用处只是说明：**控件不再是灰的**
     * （所有项都列出来、都允许改值），`hasXxx = false` 只让 tooltip 多说
     * 一句「这个存档里没有这一项，现在写的是游戏的默认值；改了才会写进去」。
     */
    struct WorldView {
        QString dirName;              ///< 这一页看的是哪一个存档文件夹
        bool hasLevelName = false;    ///< 世界名（`level.dat` 里的那一个）
        QString levelName;
        bool hasVersionName = false;  ///< 游戏版本（存档建立时那一版）
        QString versionName;
        bool hasFormat = false;       ///< 存档格式号（`DataVersion`）
        QString formatText;
        bool hasLastPlayed = false;   ///< 上次游玩（毫秒时间戳）
        qint64 lastPlayedMs = 0;
        bool hasSize = false;         ///< 这个世界占多大（目录递归加起来；`src/manage/WorldMap`）
        QString sizeText;             ///< 已经翻成"123.4 MB"那种
        bool hasSeed = false;         ///< 种子（1.16+ 在 `WorldGenSettings/seed`，老档在 `RandomSeed`）
        QString seedText;
        bool hasPlayer = false;       ///< `Data/Player/Pos` 在不在（从没进过的世界没有这一格）
        QString playerText;           ///< "x / y / z（维度）"
        bool hasRegions = false;      ///< `region/` 里扫到了至少一个 `.mca`
        /// "3 个区域文件 / 已生成 1234 个区块 / 范围 x … z …"（或者"头部读不全"那句实话）
        QString regionsText;
        /// 只读说明，挂在顶上那行事实的 tooltip 上（"模组维度没算进来""有几个区域文件
        /// 读不出来"这一类）。空串 = 没有要额外说的。
        QString mapNote;
        bool gzipped = true;          ///< 盘上那份是 gzip 包着的（不是的话底下那行小字会说）
        bool hasAllowCommands = false;
        bool allowCommands = false;
        bool hasHardcore = false;
        bool hardcore = false;
        bool hasDifficulty = false;
        int difficulty = 2;           ///< 0 和平 ~ 3 困难
        bool hasRules = false;        ///< `Data/GameRules` **是一张表**（空表也算有；不是表的算没有）
        /// 要摆出来的那一列（**已经并过全集了** —— 见文件头那段文件结构）。顺序就是摆出来
        /// 的顺序（`WorldFiles::allGameRules()` 排好的），这一层**不重排**。
        QList<Rule> rules;
    };

    /**
     * @brief 用户在这页上**真动过**的那几格 —— 相对"打开这一页时摆进去的那一份"。
     *
     * 为什么要有它：这一页上那四格（世界名 / 允许作弊 / 极限模式 / 难度）
     * 现在**一律可动**，盘上没有这一项的时候屏幕上摆的是游戏的默认值。可是"不发明"那条
     * 纪律还在 —— 用户没碰的那几格**一个字节都不许写进存档**。所以"用户到底动了哪几格"
     * 这件事必须有个出处，而它是**这一页自己**知道的（它记着 `setWorld()` 那一刻摆的是
     * 什么），MainWindow 那一头不知道（它手上只有"现在长什么样"）。
     *
     * NOTE: 判据是"和 `setWorld()` 那一刻**逐字**比"，不是"和盘上比" —— 盘上那一份在用户
     * 改的期间可能被别的东西动过（`saveWorldSettings()` 那道闸正是为这件事设的），
     * 拿盘上那份比会把"别人改的"算成"用户改的"。
     * NOTE: `rules` 里**只有值真变过的那几条** —— 打开时盘上没有这一条、屏幕上摆着默认值
     * 而用户一个字没动的那些**不算**（写进去就是"发明"）。所以这一列的判据和上面四格
     * 是同一个：和打开时那一份逐字比。
     */
    struct Delta {
        bool nameChanged = false;
        QString nameValue;
        bool allowCommandsChanged = false;
        bool allowCommandsValue = false;
        bool hardcoreChanged = false;
        bool hardcoreValue = false;
        bool difficultyChanged = false;
        int difficultyValue = 2;
        /// 要写进去的规则：名字 + 值（**只有真变过的那几条**）。
        QList<QPair<QString, QString>> rules;
    };

    explicit WorldSettingsPage(QWidget *parent = nullptr);

    /// 摆一个世界的设置（点那支笔时由 InstanceSettingsDialog 调）。
    void setWorld(const WorldView &view);

    /// 摆成"读不出来"那一档：整页只留那句原因 + 顶上那一排按钮（重命名、打开文件夹、备份、删除）。
    ///
    /// NOTE: 这一档存在的理由是**不要留下一个死胡同** —— 用户看到「这个存档可能坏了」那句
    /// 警告之后，总得有个地方落下去（至少能看见原因、能备份、能打开那个文件夹）。
    void setUnreadable(const QString &dirName, const QString &reason);
    /// 现在是不是"读不出来"那一档。
    bool isUnreadable() const { return m_unreadable; }

    /// 页面上**现在**这一份（用户改过的就是它）。`setWorld()` 之后没动过的话，返回的和
    /// 摆进去的那一份等价。
    WorldView editedView() const;

    /// 用户在这页上真动过的那几格（见上面 `Delta` 那一段的说明）。
    Delta deltaFromOpened() const;

    /// 摆上去之后**用户动过没有**（和 `editedView()` 配套的那一位）。
    ///
    /// 存在的理由只有一个：**覆盖层页眉上那颗返回箭头**（`dialog->reject()`）按不到
    /// MainWindow 那套"重读一遍盘再比"的判据 —— 那一头没有盘、也不该去碰盘。所以这里
    /// 自己记一条基线（`setWorld()` 和 `markSaved()` 各记一次），拿台账对一下就说得出
    /// 话。和 `ConfigFileEditorPage::isDirty()` 是同一件事的两份实现。
    ///
    /// NOTE: 基线**不能**拿 `m_view` 顶替 —— `markSaved()` 不改 `m_view`（存完之后那份快照
    /// 还是刚打开时的样子），于是"存过再关"会被判成"改过还没存"，白问一句。
    bool isDirty() const;

    /// 这个文件**留过备份没有**（有 ⇒ 底下那行小字多一句「已有 level.dat 备份」，见 refreshNote()）。
    ///
    /// NOTE: 由 MainWindow 在打开时告诉它（这一层不碰盘）。和 `ConfigFileEditorPage::
    /// setBackupAvailable()` 同一件事：进这一页**同时清掉**上一条的答案。
    void setBackupAvailable(bool available);

    /// 换个存档目录名（用户在这一页上按了「重命名」并且**真改成了**之后由 MainWindow 调）。
    ///
    /// NOTE: 只动标题上那个名字。`WorldFiles::renameWorld()` 只改目录名、不碰 `level.dat`
    /// （1.13 起游戏列表显示的就是目录名），所以页面上**别的值一个都不用重读**；
    /// 而 `signatureOf()` 里没有 `dirName`，`isDirty()` 因此不受影响 —— 用户在改名
    /// 之前改过的那几格还是"改过还没存"。
    void setDirName(const QString &dirName);

    /// 「修改存档文件」那个折叠框打开 / 收起。**这是唯一改这一位的地方** ——
    /// 用户点那个折叠按钮只会发出 `fileEditingRequested()`（MainWindow 要先去弹那句备份
    /// 警告），答完之后由 MainWindow 拿这个函数把结果摆回来。
    ///
    /// 里面那道 `m_applyingFileEditing` **不能删**：不挡的话，`setFileEditingOpen()` 会顺着
    /// `toggled` 再发一次 `fileEditingRequested()` —— 用户点一下、弹两次警告，第二次
    /// 是在他自己刚答应完之后弹的。挡的只是本页的槽，按钮自己的信号照发，折叠行右侧
    /// 「展开 / 收起」的字才会跟着变。
    void setFileEditingOpen(bool open);
    /// 那个折叠框现在是打开的吗。
    bool fileEditingOpen() const;

    /// 写盘**成功了** —— 底下那行字换成「已保存（时:分:秒）」。由 MainWindow 调回来
    /// （它是唯一知道写成了没有的人）。和「配置文件」那个编辑器同一个做法：**没有"写失败了"
    /// 的那一半**，写不进去是挡住用户操作的事，走 MainWindow 的弹窗。
    void markSaved();

    // ---------- 名字（验证工程按这些 objectName 找控件）----------
    static QString pageName();
    static QString infoLabelName();
    static QString nameEditName();
    static QString allowCommandsName();
    static QString hardcoreName();
    static QString difficultyName();
    static QString ruleFilterName();
    static QString ruleListName();
    static QString ruleNameLabelName();
    /// 名字和值之间那个「未写入」小标记（这一条盘上还没有时才有字，每行都有这一格）。
    static QString ruleDefaultMarkName();
    static QString ruleToggleName();
    static QString ruleEditorName();
    static QString noteLabelName();
    static QString saveButtonName();
    static QString openFolderButtonName();
    /// 「立即备份」：把**整个存档文件夹**打成一个压缩包。
    ///
    /// 保存 `level.dat` 之前另会自动留一代备份（`backupWorldLevelData()`），页面上没有
    /// 只备份 `level.dat` 的按钮，整份备份就是这一颗。
    static QString fullBackupButtonName();
    /// 「恢复备份…」：从备份目录里挑一份还原回来。
    static QString restoreButtonName();
    /// 「打开备份目录」：落点那层文件夹。
    static QString backupFolderButtonName();
    /// 「重命名…」。
    static QString renameButtonName();
    /// 「删除世界…」（确认弹窗在 MainWindow 那一层）。
    static QString deleteButtonName();
    /// 「修改存档文件」那个折叠框的那颗开关。
    static QString fileToggleName();
    /// 折叠框里那一块（世界名 / 允许作弊 / 极限模式 / 难度）。
    static QString fileBodyName();
    /// 那一堆"读不出来"的行（整块藏起来/露出来就是它）。
    static QString bodyName();

    static QString saveText();
    static QString openFolderText();
    static QString fullBackupText();
    static QString restoreBackupText();
    static QString openBackupFolderText();
    static QString renameText();
    static QString deleteText();
    static QString ruleFilterPlaceholder();
    /// 那个折叠框上写的字（默认收起）。
    static QString fileToggleText();
    /// 规则那一列的小标题。
    static QString rulesSectionText();
    /// 筛选之后一条都不剩。
    static QString noMatchText();
    /// 一条规则都没得摆。**产品里走不到这一档**（MainWindow 摆进来的是并过全集的
    /// `WorldFiles::allGameRules()`，几十条），留着是给"只摆了个空 WorldView"那种场面
    /// 一句实话，而不是一片空白（空白看着像"还没读完"）。
    static QString emptyRulesText();
    /// 存档里那份**不是** gzip 包着的（底下那行小字多一句）。
    static QString notGzippedText();
    /// 底下那行小字里"这一列是怎么来的"那两句 —— 就是"把所有的都列出来"的说明书。`@param hasRules` 说的是盘上有没有一张规则表
    /// （`WorldView::hasRules`），没有的时候多一句"下面列的是游戏的默认值"。
    static QString rulesSourceHint(bool hasRules);
    /// 盘上没有这一项的那一格多出来的那句说明（也挂在控件的 tooltip 上）。
    static QString defaultedHintText();

    /// 难度 0~3 的中文名。**和 `WorldFiles::difficultyName()` 逐字一致**（一处是给
    /// 界面看的、一处是给验证工程看的，两边都断言得到）。
    static QString difficultyOptionText(int difficulty);

    // ---------- 给验证工程用的访问器 ----------
    QLineEdit *nameEdit() const { return m_nameEdit; }
    ToggleSwitch *allowCommandsSwitch() const;
    ToggleSwitch *hardcoreSwitch() const;
    QComboBox *difficultyCombo() const { return m_difficulty; }
    QLineEdit *ruleFilter() const { return m_ruleFilter; }
    QListWidget *ruleList() const { return m_ruleList; }
    QLabel *noteLabel() const { return m_note; }
    /// 覆盖层页眉面包屑上写的那句：「世界设置：<目录名>」，读不出来的世界写「存档读取失败：<目录名>」。
    QString breadcrumbTitle() const { return m_titleText; }
    /// 正文顶上那一行事实（版本 / 存档格式 / 上次游玩）。★ 验证工程拿它量"框收着的时候
    /// 排版有没有塌" —— 见 `m_bodyTail` 那段。
    QLabel *infoLabel() const { return m_info; }
    QPushButton *saveButton() const { return m_save; }
    QPushButton *openFolderButton() const { return m_openFolder; }
    QPushButton *fullBackupButton() const { return m_fullBackup; }
    QPushButton *restoreButton() const { return m_restore; }
    QPushButton *backupFolderButton() const { return m_backupFolder; }
    QPushButton *renameButton() const { return m_rename; }
    QPushButton *deleteButton() const { return m_delete; }
    /// 那个折叠框的开关（验证工程靠它量"默认收起"和"点开之前发没发信号"）。
    QAbstractButton *fileToggle() const;
    /// 折叠框里那一块（藏起来/露出来量的是它）。★ 规则表**就在这里面**：量"规则表收没收进
    /// 那个框"用 `ruleList()->parentWidget() == fileBody()`，别去量 `isVisible()`（offscreen
    /// 平台上那一层整个可能是藏着的，见 .cpp 里那条 NOTE）。
    QWidget *fileBody() const { return m_fileBody; }
    /// 整块正文（"读不出来"那一档要把这一整块收掉）。
    QWidget *body() const { return m_body; }
    /// 行尾那个开关（只有 `Kind::Bool` 的行有；别的行是 nullptr）。
    QAbstractButton *ruleToggleAt(int row) const;
    /// 行尾那个输入框（`Kind::Int` / `Kind::Text` 的行才有）。
    QLineEdit *ruleEditorAt(int row) const;
    /// 行上那一格名字里写的是哪一条规则（认条目的唯一出处，和 `pathRole()` 一个做法）。
    static int ruleNameRole();

signals:
    /// 用户点了「保存」。**这一层不带任何值出去** —— MainWindow 用 `editedView()` 取
    /// （它是唯一知道该往哪儿写、写之前要留备份的人）。
    void saveRequested();
    /// 用户点了「立即备份」—— 要的是**整个存档文件夹**那一份压缩包。
    void fullBackupRequested();
    /// 用户点了「恢复备份…」。挑哪一份、以及"还原之前先给现在这一份做安全快照"
    /// 全在 MainWindow 那一层（这一层不碰盘、也没有那份清单）。
    void restoreBackupRequested();
    /// 用户点了「打开备份目录」。
    void openBackupFolderRequested();
    /// 用户点了「打开文件夹」。
    void openFolderRequested();
    /// 用户点了「重命名…」（改目录名那一路全在 MainWindow：它要弹输入框、要碰盘、还要
    /// 把新名字通过 `setDirName()` 摆回来）。
    void renameRequested();
    /// 用户点了「删除世界…」（确认弹窗和真删都在 MainWindow 那一层；这一层一个字节都不碰盘）。
    void deleteRequested();
    /// 用户**点开了**那个折叠框（`wanted = true`）或者收了回去（`false`）。
    ///
    /// 这一发是"用户想改文件了"，**不是"框开了"** —— 收到 true 的那一头（MainWindow）
    /// 得先弹那句备份警告（含「先备份一份」），答完之后再拿 `setFileEditingOpen(true)`
    /// 把框真的打开。展开时要弹那句备份警告。
    void fileEditingRequested(bool wanted);

protected:
    /// **焦点只能在这一刻补**（`setParent()` 会清焦点，见 OverlayHost 那一段）——
    /// 切到这一格时把光标放进第一格能打字的地方（世界名）。
    void showEvent(QShowEvent *event) override;
    /// 挂在 `m_ruleList` 的**视口**上，只认 `QEvent::Resize`：视口一变就让视图**按这个新
    /// 视口把那一列行重摆一遍**（`QListView::doItemsLayout()`）。
    ///
    /// 为什么必须有这一步：视图把"每一行在哪、多宽"记在一份**按当时的视口宽算出来的缓存**里
    /// （`rectForIndex()` 那条路），而 Qt 自己那一趟的次序是"先按**还没有滚动条**的宽度摆
    /// 一遍、再看要不要露滚动条"：这一列的行是在**折叠框还收着的时候**建的（那时列表连
    /// 大小都没有），摆开之后按 635 摆了一遍，等滚动条露出来把视口挤到 621 —— **没有第二趟**
    /// 回来重算那份缓存（两个数差 14，正好是滚动条宽）。屏幕上就是行尾那颗
    /// 开关被视口切掉半个平口，看着像这一页坏了。
    ///
    /// 视口被挤窄的那一下它自己会收到这一发 `Resize` ⇒ 在这儿重摆一遍，缓存跟着视口走。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// 按 `m_view.rules` 重摆一遍那张表（筛选之后也走它）。**重建行**，不是藏行 ——
    /// 行里的内容是 `setItemWidget()` 摆的真控件（和 InstanceFilesPage 一样，藏不掉）。
    ///
    /// 重建会把行里那些真控件（连同用户刚打的字）销毁，所以**重建之前先把它们收回来**
    /// （`harvestEdits()`）。`harvestFromRows = false` 只有 `setWorld()` 一路用 ——
    /// 那一下换的是世界，上一个世界页面上的值不能收进这一份里。见 .cpp 里那段 ★★。
    void rebuildRuleRows(bool harvestFromRows = true);
    /// 把现在摆出来的那些行里的值收回 `m_view.rules`（重建之前那一步，见上）。
    void harvestEdits();
    /// 底下那行小字回到"现在该说什么"。
    void refreshNote();
    /// 收尾那根弹簧的 stretch 跟着折叠框走（收着 = 1、摊开 = 0）。见 `m_bodyTail`。
    void updateBodyTail(bool open);
    /// 行里那一格值现在是什么（按控件类型取）。
    QString valueAt(int row) const;
    /// 这一行过不过搜索框里那串字。
    bool ruleMatches(const Rule &rule) const;
    /// 读不出来那一档该露什么、藏什么。
    void applyUnreadableState();
    /// 那几格"盘上没有这一项"的说明行（世界名 / 允许作弊 / 极限模式 / 难度）回到"现在该说
    /// 什么" —— 盘上没有的挂 `defaultedHintText()`，盘上有的挂它自己那句（见 .cpp）。
    void refreshFileHints();
    /// 「保存」给不给点。**判据只有一条**：现在摆出来的那些输入框有没有被清空的
    /// （开关永远不会空）—— 一个规则的值被清空后存下去，等于把那条规则写成一个空串，
    /// 游戏那边会当成坏值用默认的。用户多半是手滑，不是那个意思。
    void refreshSaveEnabled();
    /// 按条目 sizeHintForRow 同步规则表高度（关掉自身滑条，由外层滚动区统一滚动）。
    void syncRuleListHeight();

    WorldView m_view;
    /// **打开这一页时**摆进去的那一份，之后再不动它 —— `deltaFromOpened()` 就是拿现在这一份
    /// 和它比（`m_view` 不行：`harvestEdits()` 会把它跟着用户改的值一起往前挪）。
    WorldView m_opened;
    bool m_unreadable = false;
    QString m_unreadableReason;
    bool m_backupAvailable = false;
    QString m_savedAt;  ///< 非空 = 刚存成功过（底下那行小字要写它）
    /// `isDirty()` 的基线（`setWorld()` 与 `markSaved()` 各记一次，见那个函数的 NOTE）。
    /// 存的是 `signatureOf(view)` 那一串，不是 `WorldView` 本身 —— `WorldView` 没有
    /// `operator==`，而且这里要的是"等不等"这一个答案。
    QString m_baselineSignature;
    /// 规则名 → 现在摆在第几行（**只记摆出来的那些**，被筛掉的进不来）。`editedView()`
    /// 靠它把"用户改过的那一格"收回去 —— 没进这张表的那些保持原值。
    QHash<QString, int> m_ruleRows;

    QString m_titleText;
    /// `setFileEditingOpen()` 正在摆折叠框：`toggled` 那条槽见到它就不再发 `fileEditingRequested()`。
    bool m_applyingFileEditing = false;
    QScrollArea *m_scroll = nullptr;
    QLabel *m_info = nullptr;
    QWidget *m_body = nullptr;
    /// 正文那一层布局（`updateBodyTail()` 要拿它改 stretch，所以得留个指针）。
    QVBoxLayout *m_bodyLayout = nullptr;
    /// **正文收尾那根弹簧**（正文里最后一个东西）。它存在的理由只有一个：
    ///
    /// 折叠框**收着**的时候，正文里活着的只剩「那一行事实」和「修改存档文件」这颗钮，
    /// 而 `QVBoxLayout` 在没有 stretch 可用时会把多出来的高度**均分**给它们 —— 屏幕上
    /// 就是"那行事实飘在正中间、那颗钮掉到最底下贴着说明"，也就是
    /// "没有展开的时候排版太诡异"那个形状。
    ///
    /// 所以正文尾巴上钉一根弹簧：框收着时它一个人吃掉全部多余高度（上面那两样贴顶），
    /// 框摊开时让给规则表（那一份 stretch 才是 1）。**改这一位的只有
    /// `updateBodyTail()`**，两条路（用户点那颗钮、MainWindow 答完警告摆回来）都要叫它。
    QSpacerItem *m_bodyTail = nullptr;
    /// 那根弹簧在布局里排第几个（`QBoxLayout::setStretch()` 按下标说话）。
    int m_bodyTailIndex = -1;
    /// 「修改存档文件」那个折叠框的开关。和「实例设置 → 启动」那一页的「高级选项」、
    /// 设置页「细分区域与控件颜色」是同一个做法：`Theme::makeFoldRow()` —— 整行一颗可点的
    /// 按钮，右边用文字「展开 / 收起」，不带箭头。
    QPushButton *m_fileToggle = nullptr;
    /// 折叠框里那一块（那四行）。**默认藏起来** —— 开没开是 `m_fileToggle` 那一位。
    QWidget *m_fileBody = nullptr;
    /// 折叠框的开合动效（与设置页「高级」同一份）。
    SettingsParts::FoldMotion *m_fileMotion = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    /// 那四行各自那一句"盘上有没有这一项"的说明（tooltip 的出处）。
    QString m_nameTip;
    QString m_allowCommandsTip;
    QString m_hardcoreTip;
    QString m_difficultyTip;
    ToggleSwitch *m_allowCommands = nullptr;
    ToggleSwitch *m_hardcore = nullptr;
    QComboBox *m_difficulty = nullptr;
    QLineEdit *m_ruleFilter = nullptr;
    /// 改完搜索框**等手停下来**再重摆一遍（见 kRuleFilterDelayMs 那一段 ★）—— 每个按键
    /// 都重摆的话，一行一个控件（Int 那几行还各带一个校验器）的代价是 1.67 ms/次。
    QTimer *m_ruleFilterTimer = nullptr;
    QListWidget *m_ruleList = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_openFolder = nullptr;
    QPushButton *m_rename = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_save = nullptr;
    /// 那三颗（整份存档的备份 / 还原 / 打开备份目录）。和重命名、打开文件夹、删除世界一样是常驻的
    /// —— "读不出来"那一档也照样露着（那正是最需要留一份备份的时候）。
    QPushButton *m_fullBackup = nullptr;
    QPushButton *m_restore = nullptr;
    QPushButton *m_backupFolder = nullptr;
};
