/**
 * @file UpdateCenterPage.hpp
 * @brief 更新中心：把「检查更新」查出来的那一份结论摊成**能动手的一页**。
 *
 * 【它和那个弹出提示的区别】
 *
 * 一个弹出提示只能给一段**文本**（`InlineMessage::information`）：能看、看不了
 * 第二眼（关掉就没了）、更不能动手。这一页上每一行都带着自己的动作：
 *
 * * 勾上（多选）→ 底下那颗「更新选中的 N 项」一次做完；
 * * 一行的「更新」→ 就这一条（"逐项"那一档）；
 * * 「忽略这一版」/「固定这一版」→ 以后别再提示我（判据在 `src/manage/UpdatePolicy`）；
 * * 底下那颗「撤销上一次更新」→ 退回这一批动手之前（回滚点，见 `src/manage/UpdateRollback`）。
 *
 * 一页上**不出现第二个窗口**：它是 `OverlayHost::showPage()` 盖在内容区
 * 上的整页，顶上那颗 `← 返回` 和底下那颗「关闭」都是 `reject()`。
 *
 * 【它认的是自己那套 Report，不是 `ModDependencies::UpdateVerdict`】
 *
 * `ui/` **不 include `src/`**（见 `ui/MainWindowUI.hpp` 与 `MainWindow.cpp` 里那几段）。
 * 所以这一页拿到的是"画得出来的字符串 + 几档状态"：**唯一翻译点**在 `MainWindow`
 * （`updateCenterReportForPage()`）—— 和 `integrityReportForPage()` / `taskCenterItem()`
 * 是同一个路数。这一页自己**一个网络都不发、一个文件都不碰**。
 *
 * 【"这一行现在算不算可更新"不在这里判】
 *
 * 忽略 / 固定那两档的判据（`Suppress`）在 `src/manage/UpdatePolicy` 里，由翻译点算好之后
 * 当作 `Row::suppress` 递进来。这一页只负责"画成哪一句话、露哪几颗按钮"—— 在这里再判一遍
 * 的话，"名单里剔掉谁"和"这一行写什么"迟早会对不上（用户看到"已忽略"却还算在可更新的计数
 * 里，就是那种错）。
 */

#pragma once

#include <QDialog>
#include <QSet>
#include <QString>
#include <QStringList>

class QLabel;
class QPushButton;
class QVBoxLayout;
class QWidget;

class UpdateCenterPage : public QDialog {
    Q_OBJECT

public:
    /// 这一条在检测里是哪一档。**和 `ModDependencies::UpdateState` 一一对应**
    /// （那一层在 src/ 里，这一层只认这三个名字）。
    enum class State {
        Outdated,  ///< 有新版本
        UpToDate,  ///< 已是最新
        Unknown,   ///< 说不清（反查没认出来 / 项目没有版本列表）
    };

    /// 这一条**现在**为什么不提示更新。**和 `UpdatePolicy::Suppress` 一一对应**。
    enum class Suppress {
        None,     ///< 没有记录
        Ignored,  ///< 忽略这一版
        Pinned,   ///< 固定这一版
    };

    /// 列表里的一行（全是**显示用**的字符串 + 判据算好的几档状态）。
    struct Row {
        QString path;   ///< 盘上那一条的绝对路径（**回到 MainWindow 认人就用它**）
        QString name;   ///< 名字（资源名，取不到时是文件名）
        QString fileName;  ///< 盘上那个文件的名字（小字那行）
        /// 「现在这一版」那一句（版本号；不知道时是空串）。
        QString currentText;
        /// 「换成这一版」那一句（`Outdated` 那几条才有）。
        QString targetText;
        State state = State::Unknown;
        Suppress suppress = Suppress::None;
        /// 要换上的那一版的 id（记「忽略这一版」用）。空 = 拼不出来。
        QString targetVersionId;
        /// 盘上这一份的内容 sha1（记「固定这一版」用）。空 = 算不出来。
        QString currentSha1;
        /// `Unknown` 那一档的原因（别的档是空串）。
        QString note;

        /// 这一行能不能勾。**勾得上的，就是底下那颗「更新选中的 N 项」会去更新的那些**：
        /// `Outdated`（有新版本）、要换上的那一版拼得出来、而且**没有**被"别再提示我"
        /// 按住（已忽略 / 已固定的那几条勾了也没有下一件事可做 —— 用户刚说完"别动它"，
        /// 紧接着让批量更新把它一起换掉，是这一页最不该发生的事）。
        ///
        /// 那几条自己的「更新」那颗 chip **照旧能用**：那是一句"我现在就要这一条"的
        /// 明确指名，和"批量勾选"不是一回事。
        bool selectable() const {
            return state == State::Outdated && suppress == Suppress::None
                   && !targetVersionId.isEmpty();
        }
        /// 它现在算不算"要提示的更新"（被忽略/固定的那几条**不算** —— 名字上那个 ↑ 和
        /// 实例页「可更新」那一档筛的就是这一档）。
        bool offered() const { return state == State::Outdated && suppress == Suppress::None; }
    };

    /// 一整份（`MainWindow::updateCenterReportForPage()` 是唯一翻译点）。
    struct Report {
        QString instanceName;  ///< 哪个实例
        /// 一句话结论（`查了 41 个文件，3 个有新版本。`）—— 由翻译点拼好（那句话的唯一
        /// 出处在 `ModDependencies::updateSummaryText()`）。
        QString summary;
        /// 反查那一趟自己的毛病（几个文件读不动 / 整趟没跑完）。可以空。
        QString note;
        /// 非空 = 这一页**没有结论**可摆（没查过、或者那个实例一条能查的都没有）。
        /// 这时候列表整个是空的，用户要看到的就是这句话 + 下一步怎么办。
        QString error;
        QList<Row> rows;
        /// 有回滚点时它里面有几个文件（0 = 没有回滚点，「撤销上一次更新」那颗收起来）。
        int undoFileCount = 0;
        /// 「撤销上一次更新（N 个文件）」那句话（翻译点拼好，理由是 N 的说法只有一处）。
        QString undoText;

        /// 真·要提示的更新有几条（剔掉已忽略 / 已固定的）。**这是"这一页上还有几件事要办"
        /// 那个数**（底下那颗按钮的能不能点看的是 `checkedPaths()`，不是它）。
        int offeredCount() const {
            int n = 0;
            for (const Row &row : rows) {
                if (row.offered()) {
                    ++n;
                }
            }
            return n;
        }
        /// 已经"别再提示我"的那几条（两档合起来）。
        int suppressedCount() const {
            int n = 0;
            for (const Row &row : rows) {
                if (row.state == State::Outdated && row.suppress != Suppress::None) {
                    ++n;
                }
            }
            return n;
        }
    };

    explicit UpdateCenterPage(QWidget *parent = nullptr);

    /// 这一页的标题（`OverlayHost::showPage` 顶上那一条写的字）。
    static QString windowTitle() { return tr("更新中心"); }

    /// 摆一份（**每次开页都重新喂一遍**：同一页对象会被复用，见 MainWindow 那边）。
    /// 用户勾过的那些**留着**（这一页被重喂多半是因为他刚点了忽略/固定，勾选掉了的话
    /// 他得重新勾一遍 —— 而"我勾了三条，中间忽略了一条"是很正常的一串动作）。
    void setReport(const Report &report);

    /// 现在勾中的那几条（按列表顺序）——验证工程和 MainWindow 都读它。
    QStringList checkedPaths() const;

signals:
    /// 用户点了「更新」（某一行）或「更新选中的 N 项」。@p paths 就是要更新的那几条。
    void updateRequested(const QStringList &paths);
    /// 用户点了某一行上的「忽略这一版」/「不再忽略」。@p ignored 是**点完之后**该是什么样。
    void ignoreToggled(const QString &path, bool ignored);
    /// 同上，那一颗「固定这一版」/「不再固定」。
    void pinToggled(const QString &path, bool pinned);
    /// 用户点了「撤销上一次更新」。
    void undoRequested();

private:
    /// 按 `m_report` 把整页重画一遍。
    void rebuild();
    /// 勾选变了之后：底下那颗按钮的字和能不能点。
    void refreshSelectionUi();

    Report m_report;
    /// 勾中的那几条（**按路径记**，不是按下标：重画之后行会重排、会少掉几条）。
    QSet<QString> m_checked;

    QVBoxLayout *m_box = nullptr;
    /// 滚动区里那块"装两组"的容器（`rebuild()` 每次把它下面除了末尾弹簧的东西整块拆掉重画）。
    QWidget *m_bodyParent = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QLabel *m_noteLabel = nullptr;
    /// 每一行的勾选框按路径记着 —— `refreshSelectionUi()` 只改按钮，不重画行。
    QList<QPair<QString, class QCheckBox *>> m_checks;
    QPushButton *m_updateSelectedButton = nullptr;
    QPushButton *m_undoButton = nullptr;
};
