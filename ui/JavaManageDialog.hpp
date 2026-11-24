/**
 * @file JavaManageDialog.hpp
 * @brief 「Java 管理」整页：主版本一行一个，行尾一颗「下载 / 重新下载」。
 * @note 和 JavaMismatchDialog / GroupDialog / InstanceSettingsDialog 同一条约定：
 * **这一层不认识 Java**。哪些版本装了、下载怎么下、下到哪儿，全在
 * MainWindow 那边（那边才碰得到 GlobalJavaSettings，而它是 src/ 下的东西）。
 * 这一页只做两件事：把外面喂进来的"哪一行有路径"画出来，把用户按的是哪一行
 * **发出去**。
 *
 * 【为什么是整页而不是一个对话框】
 *
 * 支持的主版本列表是固定的，但每一行都要同时摆下一条可能很长的路径和一颗按钮 ——
 * 挤不进设置页那一行里。整页还有一个好处：返回箭头、标题、滚动区都是 OverlayHost 白送的
 * （见 OverlayHost.hpp 那段"每个页面长得一样"）。
 *
 * 【按钮文字是状态，不是身份】
 *
 * 同一颗按钮在"装了"和"没装"两种状态下写着「重新下载」和「下载」。**别拿文字去
 * 认是哪一颗** —— 认 objectName（rowButtonName(major)），理由和 MainWindow 里
 * 底栏那两颗按钮一样：文字跟着状态变，身份不跟着变。
 *
 * 【这一页也是"选哪个 Java"那把钥匙】
 *
 * 这一页只管装与修是不够的：设置页那颗按钮之外，还得有**全局手动指定 Java 的
 * 唯一入口**，否则盘上已经是手动档的配置会变成一个回不去的死档。所以顶上多一条
 * 「自动（按版本挑选）」，装了的那几行行尾多一颗「选用」—— 点谁就钉死谁。
 * **每一行都在同一列上**，认的是 useButtonName(major) / autoUseButtonName()。
 *
 * 这几条都只认"扫描认得出来的 Java"。自己装在别处、扫描不进去的那一份（另一个盘上的
 * JDK、GraalVM、别的启动器带的运行时）走底下那颗「手动指定 java.exe…」：这一页仍然
 * 不认识路径，按钮只发 manualPathRequested()，选文件、认主版本、写设置全归外面。
 */
#pragma once

#include <QDialog>
#include <QList>
#include <QMap>
#include <QString>

class QLabel;
class QPushButton;

class JavaManageDialog : public QDialog {
    Q_OBJECT

public:
    explicit JavaManageDialog(QWidget *parent = nullptr);

    // ------------------------------------------------------------------
    // 这一页管什么（纯数据，验证工程直接钉这张表）
    // ------------------------------------------------------------------

    /// 统一的管理/下载列表，从新到旧；批量下载也读取它，避免界面和执行器漏档。
    /// 包括 Java 11；Java 16 仍走启动时既有的组件下载路径，
    /// 因 Adoptium latest 接口不再提供该档，管理页暂不提供它的批量下载。
    static QList<int> managedMajors();

    /// 一行左边那段字：「Java 25：」。间距和新旧无关 —— 同一个页面上两种写法
    /// （`Java25：` 和 `Java 21：`）看起来像是打错了。
    static QString rowLabelText(int major);

    /// 一行右边那段字：装了就照原样写路径，没装就写「缺失」。
    /// 空串是**唯一**的"没装"表示法（外面扫不到东西时填的就是空串）。
    static QString pathText(const QString &javaPath);

    /// 一整行的全文（左边那段 + 右边那段）。验证工程按它逐字核对。
    static QString rowText(int major, const QString &javaPath);

    /// 行尾那颗按钮的文字。装了 = 「重新下载」，没装 = 「下载」。
    ///
    /// 「重新下载」而不是「已安装」是有意的：这一页是个**修**东西的地方 ——
    /// 路径在那儿但打不开（杀软删了文件、目录搬了家）正是用户来这儿的原因，
    /// 摆一颗点不动的「已安装」等于告诉他"没事，回去吧"。
    static QString rowButtonText(bool installed);

    /// 底下那两颗按钮的文字。抽出来是为了让断言按字面核对这两句。
    static QString downloadAllText();
    static QString detectText();

    /// 底下那颗「手动指定 java.exe…」。选哪份、认不认得出主版本、写不写设置，
    /// 全是外面的事 —— 这一页只把"用户要自己挑一份"这句话发出去。
    static QString manualText();

    // ---------- 「选哪个 Java」那两颗（见文件头那一段） ----------

    /// 顶上那一条「自动（按版本挑选）」。
    static QString autoRowText();
    /// 行尾那颗「选用」（自动那一条上也有同样一颗）。同一句话，因为它干的是同一件事。
    static QString useButtonText();
    /// 当前正在用的那一条尾巴上挂的记号。
    ///
    /// 写在文字里而不是另画一个勾：这一页的每一行宽度差得远（路径可能长到折行），
    /// 单独一列记号得跟着每一行的宽度各自对齐，看起来像三件不相干的东西。
    static QString usedMarkText();

    /// 页眉上的标题。
    QString titleText() const;

    /// 底下那行说明。写清"下到哪儿去了" —— 不然用户会以为这一页在动他系统里那份 Java。
    static QString hintText();

    // ------------------------------------------------------------------
    // 外面喂进来的状态
    // ------------------------------------------------------------------

    /// 摆一行的状态。@param javaPath 空串 = 没装（显示「缺失」、按钮写「下载」）。
    ///
    /// NOTE: 不在 managedMajors() 中的主版本直接忽略，不按扫描结果任意加行。
    void setRuntime(int major, const QString &javaPath);

    /// 现在是哪一档在用。@param autoSelect 真 = 自动那一档；假 = 钉死了 javaPath 那条。
    ///
    /// 每开一次页灌一遍。手动档下**按路径**认行，
    /// 不按主版本：用户钉的可能是一份这一页压根不管的 Java（其他主版本、或者一个
    /// 已经打不开的路径），那种情况下各行都不带记号 —— 这是实话，总比随便指一条
    /// 说"就是它"强。
    void setSelection(bool autoSelect, const QString &javaPath);

    /// 正在后台扫描或下载。各行按钮和底部动作全部禁用。
    ///
    /// NOTE: 后台任务没结束时不能再开第二份；不禁掉的话，连点三下就会排三遍同一版本。
    void setBusy(bool busy);
    bool isBusy() const { return m_busy; }

    /// 这一行现在算"装了"吗。
    bool isInstalled(int major) const;

    /// 现在有没有缺的（决定「下载所有缺少的 Java」亮不亮）。
    bool hasMissing() const;

    // ---------- 控件的 objectName（验证工程靠这些名字认控件，别改回按文字去认） ----------
    static QString rowLabelName(int major) { return QStringLiteral("javaRowLabel-%1").arg(major); }
    static QString rowButtonName(int major) { return QStringLiteral("javaRowButton-%1").arg(major); }
    static QString useButtonName(int major) { return QStringLiteral("javaUseButton-%1").arg(major); }
    static QString autoLabelName() { return QStringLiteral("javaAutoRowLabel"); }
    static QString autoUseButtonName() { return QStringLiteral("javaAutoUseButton"); }
    static QString downloadAllButtonName() { return QStringLiteral("javaDownloadAllButton"); }
    static QString detectButtonName() { return QStringLiteral("javaDetectButton"); }
    static QString manualButtonName() { return QStringLiteral("javaManualButton"); }
    static QString hintLabelName() { return QStringLiteral("javaManageHintLabel"); }

signals:
    /// 用户点了某一行那颗按钮。**只报主版本号**，别的什么都不说 —— 下载怎么做、
    /// 下到哪儿、下完界面怎么更新，全是接这个信号那边的事。
    void downloadRequested(int major);

    /// 用户点了「下载所有缺少的 Java」。**单独一个信号**，不拿 downloadRequested(-1)
    /// 当暗号：那种编码方式正是底栏那两颗按钮的注释里明确否掉的（"编号是第几颗，
    /// 不是按钮上画着什么"）—— 主版本号那一格里 -1 不是"全部"，是"没有这个版本"。
    void downloadAllMissingRequested();

    /// 用户点了「检测已有 Java」。外面要重新扫一遍盘，然后把结果 setRuntime 回来。
    void detectRequested();

    /// 用户点了「手动指定 java.exe…」。外面去问路径、认主版本，成了再 setSelection()。
    /// **不报路径**：这一页压根不认识路径（见文件头最后一段）。
    void manualPathRequested();

    /// 用户点了「自动（按版本挑选）」那条上的「选用」。
    void useAutoRequested();

    /// 用户点了某一行上的「选用」：把全局钉死成那一份。**只报主版本号**，
    /// 路径由接的人自己去 m_localJavaVersions 里取（这一页不替它保管路径）。
    void useRuntimeRequested(int major);

private:
    struct Row {
        QLabel *label = nullptr;
        /// 「选用」。没装的行上它是**藏起来**的（不是画灰）—— 见 setRuntime()。
        QPushButton *use = nullptr;
        /// 「下载 / 重新下载」。
        QPushButton *button = nullptr;
        bool installed = false;
        /// 这一行现在指着哪条路径（空 = 没装）。标"当前正在用"那一格时按它认。
        QString path;
    };

    /// 按当前 m_rows 重算"底下那颗按钮亮不亮"。
    void refreshDownloadAllState();
    /// 按 m_selection 重画那两条"（当前）"记号。
    void refreshUsedMarks();

    QMap<int, Row> m_rows;
    QLabel *m_autoLabel = nullptr;
    QPushButton *m_autoUse = nullptr;
    QPushButton *m_downloadAll = nullptr;
    QPushButton *m_detect = nullptr;
    QPushButton *m_manual = nullptr;
    bool m_busy = false;
    /// 现在用的是哪一档。
    bool m_autoSelect = true;
    QString m_selectedPath;
};
