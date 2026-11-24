/**
 * @file TaskCenterPanel.cpp
 * @brief 任务中心那块面板的实现（形状与取舍见头文件）。
 */

#include "TaskCenterPanel.hpp"
#include "Popups.hpp"
#include "TextMarks.hpp"
#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QApplication>
#include <QCoreApplication>
#include <QPointer>
#include <QHash>
#include <QSignalBlocker>

#include <algorithm>

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QMouseEvent>
#include <QFrame>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
// `setProperty()` 收的是 QVariant（那一行"这行是谁"的钥匙）—— 少了它，编译器报的是
// "no matching function for call to setProperty(QString, const QString&)"，
// 看着像"这个函数不存在"，其实是 QVariant 没定义。
#include <QVariant>

#include "Animations.hpp"
#include "ElidedLabel.hpp"
#include "MainWindowUI.hpp"
#include "TaskProgressBar.hpp"
#include "TaskVisuals.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

namespace {
bool taskDragActive = false;
constexpr char taskMime[] = "application/x-wtfml-queued-task";
class TaskDragHandle final : public QLabel {
public:
    TaskDragHandle(const QString &key, const QString &group, QWidget *parent) : QLabel(QStringLiteral("⠿"), parent), m_payload((key + '\n' + group).toUtf8()) {
        setCursor(Qt::OpenHandCursor);
        setToolTip(QCoreApplication::translate("TaskCenterPanel", "拖动调整等待顺序；不会打断运行中的任务。"));
        setObjectName(QStringLiteral("taskDragHandle_%1").arg(key)); setFixedWidth(24);
    }
protected:
    void mousePressEvent(QMouseEvent *event) override { m_start = event->position().toPoint(); event->accept(); }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (!(event->buttons() & Qt::LeftButton) || (event->position().toPoint() - m_start).manhattanLength() < QApplication::startDragDistance()) return;
        // QDrag 属于应用而不是会刷新重建的行；执行期间面板延后重建。
        auto *drag = new QDrag(qApp); auto *mime = new QMimeData;
        mime->setData(taskMime, m_payload); drag->setMimeData(mime);
        taskDragActive = true; drag->exec(Qt::MoveAction); taskDragActive = false; drag->deleteLater();
    }
    void mouseReleaseEvent(QMouseEvent *event) override { event->accept(); }
private:
    QByteArray m_payload; QPoint m_start;
};

bool isActive(TaskCenter::State state) {
    // 判定只有一处（见 TaskCenter::stateIsActive()）。
    return TaskCenter::stateIsActive(state);
}

// ---------- 这一块面板自己的几个尺寸 ----------
//
// NOTE: 这里的留白**另起一套**（不复用 `MainWindowUI.cpp` 里 `BottomBarPanel` 那几个
// `kPanelPadding` / `kPanelRowSpacing`）：那一套是给"一行一个实例"的短行调的
// （行高矮、行挨得近），这一块里是标题 + 小字 + 进度条 + 步骤，行高一档。两个面板
// 长得很不一样，共用一个数只会两边都不合适。
constexpr int kPanelPadding = 14;
/// 两行之间（同一个按钮那一栏里）。
constexpr int kRowSpacing = 9;
/// 两栏之间（「当前工作」和「已完成」中间那一道）：比行距大一倍，分开两栏。
constexpr int kSectionSpacing = 18;
/// 一行里面那几样之间。
constexpr int kRowInnerSpacing = 6;
/// 三列工作区的宽度；窄屏会自动退回单列任务列表。
constexpr int kCompactWidth = 520;
constexpr int kWorkspaceMinWidth = 820;
constexpr int kWorkspaceMaxWidth = 1120;
/// 右栏宽度的**下限 / 上限**。真正用多少按工作区宽度算（见 `kDetailWidthPercent`），
/// 窗口再宽也不停在 260。
constexpr int kDetailWidth = 260;
constexpr int kDetailWidthPercent = 27;
/// 高度上限 = 窗口可用高度的百分之多少（60%）。
constexpr int kHeightPercent = 72;
/// 面板和那颗按钮之间那一道缝（和 `BottomBarPanel` 的 `kPanelGap` 同一个值 ——
/// 两块面板都是从同一颗按钮往上弹的，缝不一样宽看着就是"两块面板没对齐"）。
constexpr int kPanelGap = 6;
/// 标题/小字那一格比面板最多窄这么点（左右边距 + 右边那个百分比/时间的位置）。

/// 小一号、灰一点的字（"正在下载资源对象"、"19:42"、栏名都用它）。
///
/// 参数是 `QWidget *` 而不是 `QLabel *`：面板底下那颗「清除已完成」是 `QPushButton`，
/// 它也要这一身灰字。**两个色角色都要摆** —— QLabel 的字取 `WindowText`，QPushButton
/// 的字取的是 `ButtonText`；只摆前者的话按钮上那四个字会一点没变（不报错、不崩，
/// 看着就是"这句 setPalette 没生效"）。
void applyMutedFont(QWidget *widget, int pointDelta) {
    QFont font = widget->font();
    font = Theme::textFont(font, pointDelta < 0 ? Theme::TextRole::Caption : Theme::TextRole::Body);
    widget->setFont(font);
    const QColor muted = widget->palette().color(QPalette::PlaceholderText);
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, muted);
    palette.setColor(QPalette::ButtonText, muted);
    widget->setPalette(palette);
}

/// 一行的宽度上限：面板最宽那一档减掉左右边距、再给右边那个百分比/时间留一段。
///
/// **水平方向必须是 `Ignored`**。QLabel 的 `minimumSizeHint()` 是整串字的
/// 宽度，`setMaximumWidth()` 只封得住上限、封不住"布局不许把它压得比最小值还窄" ——
/// 于是长文件名会把整行撑宽，行尾那颗「取消」被挤到视口外面（甚至被右侧栏挡住）。
/// Ignored 之后布局只问
/// "这一格分到多宽"，`ElidedLabel` 自己按拿到的宽度画省略号。
ElidedLabel *makeTextLabel(QWidget *parent, const QString &objectName) {
    auto *label = new ElidedLabel(parent);
    label->setObjectName(objectName);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return label;
}

/**
 * @brief 任务中心里的一行（活动的和已完成的共用这一个类，画法按 `Item::active` 分）。
 *
 * 行里每一块的 objectName 都带上**行号**（`MainWindowUI::taskCenterPanelRow*Name()` 给的）
 * —— 验证工程不用数"这一行里有几个 QLabel"这种一改就碎的判据。
 *
 * 整行**可点**：点一下进任务详情页（"点击任务进入
 * 内嵌 `TaskDetailPage`"）。它只在**真挂上了处理器**的时候才装作可点（手型光标 + 悬停底色），
 * 没挂的时候就是一行字（`MainWindow` 那边永远会挂，挂不上的只有验证工程搭的裸面板）。
 */
class TaskCenterRowWidget : public QWidget {
public:
    TaskCenterRowWidget(const TaskCenter::Item &item, int index,
                        const std::function<void()> &onActivate, QWidget *parent,
                        const TaskCenter::RowActionHandler &onRowAction,
                        quint64 handlerRevision,
                        std::function<void(const QString &, bool)> onDrop = {})
        : QWidget(parent), m_onActivate(onActivate), m_onDrop(std::move(onDrop)), m_queueGroup(item.queueGroup),
          m_item(item), m_index(index), m_handlerRevision(handlerRevision),
          m_parentPalette(parent->palette()), m_parentFont(parent->font()) {
        setAcceptDrops(!item.queueGroup.isEmpty() && item.state == TaskCenter::State::Queued && bool(m_onDrop));
        if (m_onActivate) {
            setCursor(Qt::PointingHandCursor);
        }
        setObjectName(MainWindowUI::taskCenterPanelRowName(index));
        // 行是谁：面板里认这一行靠它（行号会随着"清掉一条已完成"整体挪位，钥匙不会）。
        // 这个角色只有一个读者 —— `MainWindow` 那边按 `Item::key` 喂进来；
        // 验证工程也拿它核对"画出来的是不是那几条"。
        setProperty(MainWindowUI::taskCenterPanelRowKeyProperty(), item.key);

        // 列表行 = 列表页那套行的样子（ListRowDelegate）：左边状态图标，中间标题 + 一行小字，
        // 右边百分比 / 时间；正在跑的那几行再挂一根细进度条和"第几步"。
        auto *outerRow = new QHBoxLayout(this);
        outerRow->setContentsMargins(12, 9, 12, 9);
        outerRow->setSpacing(12);
        auto *column = new QVBoxLayout;
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(4);

        // ---------- 第一行：标题 + 右边那个数（排队那几行前面多一组拖动 / 上下移） ----------
        auto *titleRow = new QHBoxLayout();
        titleRow->setContentsMargins(0, 0, 0, 0);
        titleRow->setSpacing(kRowInnerSpacing);
        if (acceptDrops()) {
            outerRow->addWidget(new TaskDragHandle(item.key, item.queueGroup, this), 0, Qt::AlignVCenter);
            for (int delta : {-1, 1}) {
                auto *button = new QPushButton(delta < 0 ? QStringLiteral("↑") : QStringLiteral("↓"), this);
                button->setObjectName(QStringLiteral("taskQueue%1_%2").arg(delta < 0 ? "Up" : "Down", item.key));
                button->setFixedWidth(28); button->setAutoDefault(false);
                button->setToolTip(delta < 0 ? QCoreApplication::translate("TaskCenterPanel", "在等待队列中上移")
                                             : QCoreApplication::translate("TaskCenterPanel", "在等待队列中下移"));
                Theme::markButton(button, Theme::ButtonKind::Ghost);
                connect(button, &QPushButton::clicked, this, [onRowAction, key = item.key, delta] {
                    if (onRowAction) onRowAction(key, delta < 0 ? TaskCenter::RowAction::MoveUp : TaskCenter::RowAction::MoveDown);
                });
                titleRow->addWidget(button);
            }
        }
        const bool active = isActive(item.state);
        const bool failed = item.state == TaskCenter::State::Failed;
        const bool interrupted = item.state == TaskCenter::State::Interrupted;
        // 状态图标：成功 / 失败 / 进行中 / 等待 / 暂停 / 取消各一个（和任务详情页同一套）。
        auto *icon = TaskVisuals::makeGlyphIcon(TaskVisuals::stateGlyph(item.state, palette()), 18, this);
        icon->setObjectName(QStringLiteral("taskCenterPanelRowIcon_%1").arg(index));
        icon->setAccessibleName(MainWindowUI::taskDetailStateText(item.state));
        auto *iconColumn = new QVBoxLayout;
        iconColumn->setContentsMargins(0, 2, 0, 0);
        iconColumn->addWidget(icon);
        iconColumn->addStretch(1);
        outerRow->addLayout(iconColumn);
        outerRow->addLayout(column, 1);

        ElidedLabel *title = makeTextLabel(this, MainWindowUI::taskCenterPanelRowTitleName(index));
        title->setText(item.title);
        {
            QFont font = Theme::textFont(title->font(), Theme::TextRole::Body);
            font.setWeight(QFont::DemiBold);
            title->setFont(font);
        }
        titleRow->addWidget(title, 1, Qt::AlignVCenter);
        if (item.unreadFailure) {
            // 用户还没看过的一次失败：标题后面一颗红点（颜色问 `folderDangerColor()`，和界面上
            // 别处的红是**同一个色** —— 见 MainWindowUI 里 runStateMarkColor() 那段）。
            auto *dot = new QLabel(this);
            dot->setObjectName(MainWindowUI::taskCenterPanelRowDotName(index));
            const int size = MainWindowUI::taskCenterPanelFailureDotSize();
            dot->setFixedSize(size, size);
            dot->setPixmap(VersionIcons::runStateMark(size,
                                                      MainWindowUI::folderDangerColor(palette())));
            titleRow->addWidget(dot, 0, Qt::AlignVCenter);
        }
        // ---------- 行尾那颗「取消」（行内快捷动作）----------
        // 只在**能力位说可以**、而且外面真挂了处理器的时候才长。它是真按钮，点它不会同时
        // 触发整行那一下（父子控件的鼠标事件不会一起送给两边）。
        if (item.capabilities.cancel && onRowAction) {
            auto *cancel = new QPushButton(MainWindowUI::taskCenterPanelRowCancelText(), this);
            cancel->setObjectName(MainWindowUI::taskCenterPanelRowCancelName(index));
            cancel->setCursor(Qt::PointingHandCursor);
            cancel->setFlat(true);
            applyMutedFont(cancel, -1);
            const QString key = item.key;
            // 收下这一下的是这颗按钮自己；故意不当场重建面板：取消要走一趟异步，等账本的
            // `taskChanged` 传来由 `refreshTaskCenterPanel()` 重画（没取消成就本来没新状态可说）。
            QObject::connect(cancel, &QPushButton::clicked, cancel, [onRowAction, key]() {
                if (onRowAction) {
                    onRowAction(key, TaskCenter::RowAction::Cancel);
                }
            });
            titleRow->addWidget(cancel, 0, Qt::AlignVCenter);
        }
        if (active && item.percent >= 0) {
            auto *percent = new QLabel(MainWindowUI::taskCenterPercentText(item.percent), this);
            percent->setObjectName(MainWindowUI::taskCenterPanelRowPercentName(index));
            applyMutedFont(percent, -1);
            titleRow->addWidget(percent, 0, Qt::AlignVCenter);
        } else if (!active && item.finishedAt.isValid()) {
            auto *when = new QLabel(TaskCenter::timeText(item.finishedAt), this);
            when->setObjectName(MainWindowUI::taskCenterPanelRowTimeName(index));
            applyMutedFont(when, -1);
            titleRow->addWidget(when, 0, Qt::AlignVCenter);
        }
        column->addLayout(titleRow);

        // ---------- 第二行：小字（"已下载 12 / 40  文件名"；收了尾的写原因 / 结局） ----------
        if (!item.detail.isEmpty()) {
            ElidedLabel *detail =
                makeTextLabel(this, MainWindowUI::taskCenterPanelRowDetailName(index));
            detail->setText(item.detail);
            applyMutedFont(detail, -1);
            column->addWidget(detail);
        } else if (!active) {
            ElidedLabel *subtitle = makeTextLabel(this, QStringLiteral("taskCenterPanelRowSubtitle_%1").arg(index));
            subtitle->setText((failed || interrupted) && !item.errorSummary.isEmpty()
                                  ? item.errorSummary.section(QLatin1Char('\n'), 0, 0)
                                  : MainWindowUI::taskDetailStateText(item.state));
            applyMutedFont(subtitle, -1);
            if (failed || interrupted) {
                QPalette colors = subtitle->palette();
                colors.setColor(QPalette::WindowText, MainWindowUI::folderDangerColor(palette()));
                subtitle->setPalette(colors);
            }
            column->addWidget(subtitle);
        }

        // ---------- 第三样：进度条（只在**测得出百分比**时才有；测不出就看 detail 那句） ----------
        if (active && item.percent >= 0) {
            auto *bar = new TaskProgressBar(this);
            bar->setObjectName(MainWindowUI::taskCenterPanelRowBarName(index));
            bar->setRange(0, 100);
            bar->setValue(item.percent);
            bar->setFixedHeight(MainWindowUI::taskCenterPanelBarHeight());
            column->addSpacing(2);
            column->addWidget(bar);
        }

        // ---------- 第四样：走到第几步（一句话，不再拿 √ ● ○ 拼一串） ----------
        if (active && !item.steps.isEmpty()) {
            int current = -1;
            for (int i = 0; i < item.steps.size() && current < 0; ++i)
                if (TaskCenter::stepForTask(item.state, item.steps.at(i)).state == TaskCenter::StepState::Running) current = i;
            for (int i = 0; i < item.steps.size() && current < 0; ++i)
                if (TaskCenter::stepForTask(item.state, item.steps.at(i)).state != TaskCenter::StepState::Done) current = i;
            if (current >= 0) {
                auto *step = new QLabel(this);
                step->setObjectName(MainWindowUI::taskCenterPanelRowStepName(index, 0));
                step->setText(QCoreApplication::translate("TaskCenterPanel", "第 %1 / %2 步 · %3")
                                  .arg(current + 1).arg(item.steps.size()).arg(item.steps.at(current).name));
                applyMutedFont(step, -1);
                column->addWidget(step);
            }
        }
    }

    bool matches(const TaskCenter::Item &item, int index, quint64 revision) const {
        // Only row presentation matters here. Detail-only fields are still refreshed
        // from the latest provider snapshot by rebuildDetail(), never from this cache.
        if (m_index != index || m_handlerRevision != revision ||
            m_parentPalette != parentWidget()->palette() || m_parentFont != parentWidget()->font() ||
            m_item.key != item.key || m_item.title != item.title || m_item.detail != item.detail ||
            m_item.state != item.state || m_item.percent != item.percent ||
            m_item.unreadFailure != item.unreadFailure || m_item.finishedAt != item.finishedAt ||
            m_item.queueGroup != item.queueGroup || m_item.capabilities.cancel != item.capabilities.cancel ||
            m_item.steps.size() != item.steps.size()) return false;
        for (qsizetype i = 0; i < item.steps.size(); ++i) {
            const auto &before = m_item.steps.at(i);
            const auto &after = item.steps.at(i);
            if (before.name != after.name || before.state != after.state || before.note != after.note) return false;
        }
        return true;
    }

protected:
    bool acceptsTask(const QMimeData *mime, QObject *source) const {
        if (!acceptDrops() || source != qApp || !mime->hasFormat(taskMime)) return false;
        const auto fields = QString::fromUtf8(mime->data(taskMime)).split('\n');
        return fields.size() == 2 && !fields[0].isEmpty() && fields[1] == m_queueGroup;
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (acceptsTask(event->mimeData(), event->source())) event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        if (!acceptsTask(event->mimeData(), event->source())) return;
        m_dropHover = true; m_dropAfter = event->position().y() >= height() / 2;
        update(); event->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent *event) override { m_dropHover = false; update(); event->accept(); }
    void dropEvent(QDropEvent *event) override {
        if (!acceptsTask(event->mimeData(), event->source())) return;
        m_dropHover = false; update();
        const auto key = QString::fromUtf8(event->mimeData()->data(taskMime)).section('\n', 0, 0);
        if (m_onDrop) m_onDrop(key, event->position().y() >= height() / 2);
        event->acceptProposedAction();
    }
    void enterEvent(QEnterEvent *event) override { QWidget::enterEvent(event); update(); }
    void leaveEvent(QEvent *event) override { QWidget::leaveEvent(event); update(); }
    void paintEvent(QPaintEvent *) override {
        const bool selected = property("taskCenterSelected").toBool();
        if (!selected && !underMouse() && !m_dropHover) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const QColor base = palette().color(QPalette::Base);
        painter.setBrush(selected ? Theme::selectionFill(base) : Theme::rowHoverFill(base));
        painter.drawRoundedRect(rect(), 10, 10);
        if (m_dropHover) {
            painter.setPen(QPen(Theme::focusRingColor(palette()), 2));
            const int y = m_dropAfter ? height() - 1 : 1;
            painter.drawLine(4, y, width() - 4, y);
        }
        if (selected) {
            painter.setBrush(Theme::focusRingColor(palette()));
            painter.drawRoundedRect(QRectF(3, 12, 3, qMax(0, height() - 24)), 1.5, 1.5);
        }
    }
    void mousePressEvent(QMouseEvent *event) override {
        // 左键那一下才开口。**收下这一下**（不往下传）：面板是 Qt::Popup，不收的话它会一路
        // 走到面板的 handleOutsidePress()，把这一下当成"点在面板外面"顺手收回去 ——
        // 症状是"点一行，面板没了，详情页也没开"。
        if (event->button() == Qt::LeftButton && m_onActivate) {
            event->accept();
            m_onActivate();
            return;
        }
        QWidget::mousePressEvent(event);
    }

private:
    std::function<void()> m_onActivate;
    std::function<void(const QString &, bool)> m_onDrop;
    QString m_queueGroup;
    TaskCenter::Item m_item;
    int m_index;
    quint64 m_handlerRevision;
    QPalette m_parentPalette;
    QFont m_parentFont;
    bool m_dropHover = false, m_dropAfter = false;
};

}  // namespace

QString TaskCenter::stepMark(StepState state) {
    // 四个记号是**画出来的一串字符**，不是图标：它们和那行步骤的正文同色同字号，混在
    // 一句里读得下去（换成图标就得跟着调色、还要在两栏里各摆一次）。
    // **挑字有讲究**：`✓` / `✗` 这一档默认界面字体里没有，第一次画出来会触发 277 ms 的
    // 字体回退（见 TextMarks.hpp 头注释），所以成功 / 失败走 `√` / `×`。
    switch (state) {
    case StepState::Pending:
        return QCoreApplication::translate("TaskCenter", "○");
    case StepState::Running:
        return QCoreApplication::translate("TaskCenter", "●");
    case StepState::Done:
        return TextMarks::done();
    case StepState::Failed:
        return TextMarks::failed();
    }
    return QString();
}

bool TaskCenter::stateIsActive(State state) {
    return state == State::Queued || state == State::Running || state == State::WaitingUser
           || state == State::Paused;
}

TaskCenter::Step TaskCenter::stepForTask(State taskState, const Step &step) {
    Step out = step;
    if (out.state != StepState::Running || stateIsActive(taskState)) {
        return out;
    }
    // 收尾了，但这一步还停在"正在跑"。
    switch (taskState) {
    case State::Succeeded:
        out.state = StepState::Done;
        break;
    case State::Cancelled:
        out.state = StepState::Pending;
        if (out.note.isEmpty()) {
            out.note = QCoreApplication::translate("TaskCenter", "已取消");
        }
        break;
    case State::Interrupted:
    case State::Failed:
        out.state = StepState::Failed;
        if (taskState == State::Interrupted && out.note.isEmpty()) {
            out.note = QCoreApplication::translate("TaskCenter", "已中断");
        }
        break;
    case State::Queued:
    case State::Running:
    case State::WaitingUser:
    case State::Paused:
        break;  // 上面已经返回；这里只为把枚举写全
    }
    return out;
}

QString TaskCenter::terminalMark(State state) {
    switch (state) {
    case State::Succeeded:
        return TextMarks::done();
    case State::Failed:
        return TextMarks::failed();
    case State::Cancelled:
        return QCoreApplication::translate("TaskCenter", "—");
    case State::Interrupted:
        return QCoreApplication::translate("TaskCenter", "!");
    case State::Queued:
    case State::Running:
    case State::WaitingUser:
    case State::Paused:
        return QString();
    }
    return QString();
}

QString TaskCenter::timeText(const QDateTime &when) {
    if (!when.isValid()) {
        return QString();
    }
    // 只写"几点几分"：已完成那一栏里最远的东西也就是几十条之前那一次，日期那一截
    // （" 19:42"）会把这半行撑满，而它旁边那个位置本来就是给"什么时候"留的。
    return when.toString(QStringLiteral("HH:mm"));
}

TaskCenterPanel::TaskCenterPanel(QWidget *anchor, const TaskCenter::ItemProvider &provider,
                                 const TaskCenter::ClearFinishedHandler &onClear,
                                 const OutsidePressHandler &onOutsidePress, QWidget *parent,
                                 bool embedded)
    : QWidget(parent, embedded ? Qt::Widget : Qt::Popup | Qt::FramelessWindowHint),
      m_anchor(anchor),
      m_provider(provider),
      m_onClear(onClear),
      m_onOutsidePress(onOutsidePress),
      m_embedded(embedded) {
    setObjectName(MainWindowUI::taskCenterPanelName());
    // 浮层那一档和别的浮层同一张磨砂面（paintEvent 里画），调色板只给行取色。
    // 整页那一档（嵌在覆盖层里）不铺底、不描边：直接站在整窗磨砂上，否则内容区中间
    // 是一整块不透明深色矩形，和轨、顶栏拼成三块。
    setAutoFillBackground(false);
    if (!m_embedded) {
        Popups::preparePopup(this);
    }
    QPalette panelPalette = QApplication::palette();
    panelPalette.setColor(QPalette::Window, Theme::partBackground(Theme::Part::Panel));
    setPalette(panelPalette);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName(QStringLiteral("taskCenterToolbar"));
    auto *toolbar = new QHBoxLayout(m_toolbar);
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(8);
    m_filter = new QComboBox(m_toolbar);
    m_filter->setFixedHeight(36);
    m_filter->setObjectName(QStringLiteral("taskCenterFilter"));
    m_filter->addItems({QCoreApplication::translate("TaskCenterPanel", "全部任务"),
                        QCoreApplication::translate("TaskCenterPanel", "正在进行"),
                        QCoreApplication::translate("TaskCenterPanel", "已结束"),
                        QCoreApplication::translate("TaskCenterPanel", "需要处理")});
    m_filter->setAccessibleName(QCoreApplication::translate("TaskCenterPanel", "筛选任务状态"));
    toolbar->addWidget(m_filter);
    m_search = new QLineEdit(m_toolbar);
    m_search->setObjectName(QStringLiteral("taskCenterSearch"));
    m_search->setPlaceholderText(QCoreApplication::translate("TaskCenterPanel", "搜索任务"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(0);
    m_search->setMaximumWidth(240);
    m_search->setFixedHeight(36);
    toolbar->addWidget(m_search, 1);
    m_clearFinished = new QPushButton(MainWindowUI::taskCenterPanelClearText(), m_toolbar);
    m_clearFinished->setObjectName(MainWindowUI::taskCenterPanelClearButtonName());
    m_clearFinished->setToolTip(QCoreApplication::translate("TaskCenterPanel", "清除全部已结束记录，不影响进行中的任务与下载文件。"));
    m_clearFinished->setVisible(bool(m_onClear));
    m_clearFinished->setFixedHeight(36);
    Theme::markButton(m_clearFinished, Theme::ButtonKind::Neutral);
    toolbar->addWidget(m_clearFinished);
    outer->addWidget(m_toolbar);
    const auto filterChanged = [this] {
        m_selectedKey.clear();
        m_compactDetailOpen = false;
        applyWorkspaceMode();
        requestRefresh();
    };
    connect(m_filter, &QComboBox::currentIndexChanged, this, filterChanged);
    connect(m_search, &QLineEdit::textChanged, this, filterChanged);
    connect(m_clearFinished, &QPushButton::clicked, this, [this] {
        // Keep signal dispatch alive even when the owner immediately refreshes its task snapshot.
        QTimer::singleShot(0, this, [this] { if (m_onClear) m_onClear(); requestRefresh(); });
    });
    auto *workspace = new QHBoxLayout;
    outer->addLayout(workspace, 1);
    workspace->setContentsMargins(kPanelPadding, kPanelPadding, kPanelPadding, kPanelPadding);
    workspace->setSpacing(kSectionSpacing);

    // 中列：按时间和状态排列的任务横行。
    m_scroll = new Theme::GutterScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("taskCenterTaskList"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    // 横向**永远不滚**：中列跟随工作区宽度伸缩，行里的字自己省略号收尾。
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    m_content = new QWidget(m_scroll);
    m_content->setAutoFillBackground(false);
    auto *contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(kRowSpacing);
    contentLayout->addStretch(1);
    m_scroll->setWidget(m_content);
    m_content->installEventFilter(this);
    m_scroll->viewport()->installEventFilter(this);
    // 滚下去看见了才算"看过"（`acknowledgeVisibleFailures()` 按视口相交判）。
    // 没有这一发的话，首屏之外那些失败行会一直不标 —— 用户滚到它跟前也没人再问一次。
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { acknowledgeVisibleFailures(); });

    workspace->addWidget(m_scroll, 1);

    // 右列：选中任务的上下文详情；未选择时显示摘要。
    m_detailPane = new QWidget(this);
    m_detailPane->setObjectName(QStringLiteral("taskCenterDetailPane"));
    m_detailPane->setFixedWidth(kDetailWidth);
    auto *detailLayout = new QVBoxLayout(m_detailPane);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    m_detailScroll = new Theme::GutterScrollArea(m_detailPane);
    m_detailScroll->setFrameShape(QFrame::NoFrame);
    m_detailScroll->setWidgetResizable(true);
    m_detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_detailContent = new QWidget(m_detailScroll);
    m_detailContent->setObjectName(QStringLiteral("taskCenterDetailContent"));
    m_detailContent->setLayout(new QVBoxLayout);
    m_detailScroll->setWidget(m_detailContent);
    detailLayout->addWidget(m_detailScroll, 1);
    m_detailFooter = new QWidget(m_detailPane);
    m_detailFooter->setObjectName(QStringLiteral("taskCenterDetailFooter"));
    m_detailFooter->setMaximumWidth(720);  // 和上面那一栏对齐
    m_detailFooter->setProperty("wtfmlDialogActions", true);  // 按钮一样高（36）
    auto *footerLayout = new QVBoxLayout(m_detailFooter);
    footerLayout->setContentsMargins(0, 0, 4, 6);
    footerLayout->setSpacing(0);
    m_detailFooter->hide();
    detailLayout->addWidget(m_detailFooter);
    workspace->addWidget(m_detailPane);

    rebuild();
}

TaskCenterPanel::~TaskCenterPanel() = default;

void TaskCenterPanel::popUp() {
    rebuild();
    place(/*animate=*/true);
    show();
    raise();
    // 拿焦点只为一件事：Esc 能关（外面点一下 Qt 自己会关，那条不用管）。
    setFocus(Qt::PopupFocusReason);
    m_opened = true;
}

void TaskCenterPanel::showEmbedded() {
    m_opened = true;
    m_compact = width() > 0 && width() < kWorkspaceMinWidth;
    applyWorkspaceMode();
    rebuild();
    show();
}

std::function<void()> TaskCenterPanel::rowActivate(const QString &key) {
    // 每一行各捕一份**自己那把钥匙**：处理器只被告知"点的是哪一项"，不被告知"点的是第几行"
    // —— 行号会随着"清掉一条已完成"整体往上挪，面板开着的时候用户点的那一行早就不是
    // 他看见的那一行了。
    if (!m_embedded && !m_onRowActivated) {
        return {};
    }
    return [this, key]() {
        selectTask(m_selectedKey == key ? QString() : key);
        if (m_embedded && m_compact) {
            m_compactDetailOpen = !m_selectedKey.isEmpty();
            applyWorkspaceMode();
        } else if (!m_selectedKey.isEmpty() && m_compact && m_onRowActivated) {
            m_onRowActivated(key);
        }
    };
}

void TaskCenterPanel::applyWorkspaceMode() {
    if (!m_embedded) {
        return;
    }
    if (!m_compact) {
        m_scroll->show();
        m_detailPane->show();
        m_detailPane->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        updateDetailPaneWidth();
        return;
    }

    // 窄屏不再尝试从已经打开的 OverlayHost 再开一层详情页：任务列表和详情在同一页内切换。
    m_scroll->setVisible(!m_compactDetailOpen);
    m_detailPane->setVisible(m_compactDetailOpen);
    m_detailPane->setMinimumWidth(0);
    m_detailPane->setMaximumWidth(QWIDGETSIZE_MAX);
    m_detailPane->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void TaskCenterPanel::updateDetailPaneWidth() {
    if (m_compact) {
        return;  // 窄屏那一档详情占满整页，宽度归布局管（见 applyWorkspaceMode()）
    }
    // 右栏跟着工作区一起长：窄的时候仍是 kDetailWidth，宽了按比例多分一点，
    // 但**最多一倍**（再宽中列那一排任务就没地方了）。
    const int wanted = qBound(kDetailWidth, width() * kDetailWidthPercent / 100,
                              kDetailWidth * 2);
    if (m_detailPane->minimumWidth() == wanted && m_detailPane->maximumWidth() == wanted) {
        return;
    }
    m_detailPane->setMinimumWidth(wanted);
    m_detailPane->setMaximumWidth(wanted);
}

void TaskCenterPanel::refresh() {
    if (!m_opened) {
        return;  // 还没弹出来过：下一次 popUp() 自己会重建
    }
    if (QGuiApplication::mouseButtons() != Qt::NoButton) {
        deferRefreshUntilRelease();
        return;
    }
    m_deferTicks = 0;
    rebuild();
    place(/*animate=*/false);
    // 重建之后**当场**再判一次"露脸了没有"：`rebuild()` 里那一发是排到下一轮的（布局那时
    // 还没算完），而面板刚打开这一趟的几何在这里已经定下来了 —— 不补这一下，底栏那颗
    // 红点要多等一个事件轮才灭。
    acknowledgeVisibleFailures();
}

void TaskCenterPanel::deferRefreshUntilRelease() {
    if (m_refreshDeferred) {
        return;  // 已经排着了：松手那一趟会把这一期间攒下的所有变化一起画出来
    }
    m_refreshDeferred = true;
    QTimer::singleShot(40, this, [this]() {
        m_refreshDeferred = false;
        if (!m_opened || !isVisible()) {
            return;  // 面板收起来了：下一次 popUp()/showEvent() 自己会刷新
        }
        if (QGuiApplication::mouseButtons() != Qt::NoButton) {
            // 还按着：接着等。但**不能无限等** —— 真按着不放 2 秒以上（或者 Qt 那边
            // 因为焦点切换丢了 release）时，宁可回到"随时重画"，也别让进度停住。
            if (++m_deferTicks >= 50) {
                m_deferTicks = 0;
                // 直接重建：走 `refresh()` 会又一次看到"按着"、再排一次，永远等下去。
                rebuild();
                place(/*animate=*/false);
                return;
            }
            deferRefreshUntilRelease();
            return;
        }
        m_deferTicks = 0;
        refresh();
    });
}

void TaskCenterPanel::requestRefresh() {
    if (m_refreshPending) return;
    m_refreshPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_refreshPending = false;
        if (m_opened && isVisible()) refresh();
    });
}

void TaskCenterPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    requestRefresh();
}

void TaskCenterPanel::place(bool animate) {
    if (m_embedded) {
        const int availableWidth = qMax(0, width());
        m_compact = availableWidth > 0 && availableWidth < kWorkspaceMinWidth;
        applyWorkspaceMode();
        return;
    }
    if (m_anchor == nullptr) {
        return;
    }
    // 高度上限：屏幕**可用**高度（减掉任务栏那一截）的 60%。放在这里而不是构造里，
    // 是因为锚点所在的那块屏幕要等它真摆出来才知道（多显示器时"哪一块屏"由窗口位置定）。
    QScreen *screen = m_anchor->screen();
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect available = screen != nullptr ? screen->availableGeometry() : QRect();
    if (available.isValid()) {
        m_scroll->setMaximumHeight(available.height() * kHeightPercent / 100 - 2 * kPanelPadding);
        m_detailScroll->setMaximumHeight(m_scroll->maximumHeight());
    }

    const int availableWidth = available.isValid() ? available.width() - 16 : kWorkspaceMaxWidth;
    m_compact = availableWidth < kWorkspaceMinWidth;
    applyWorkspaceMode();
    adjustSize();
    const int wantedWidth = m_compact ? qMin(kCompactWidth, availableWidth)
                                      : qBound(kWorkspaceMinWidth, availableWidth,
                                               kWorkspaceMaxWidth);
    resize(wantedWidth, height());
    // 上面那句 resize 之后才是"这一块真正有多宽"；右栏按它重新分一次（见
    // updateDetailPaneWidth()）。
    updateDetailPaneWidth();

    const QPoint anchorTopLeft = m_anchor->mapToGlobal(QPoint(0, 0));
    // 水平方向对齐那颗按钮的**中线**：这块面板通常比按钮宽得多，左对齐容易伸到窗口
    // 右边外面去。居中之后两边出头一样多。
    int x = anchorTopLeft.x() + m_anchor->width() / 2 - width() / 2;
    int y = anchorTopLeft.y() - height() - kPanelGap;
    if (available.isValid()) {
        // 贴边兜底：窗口贴着屏幕上边时（或任务多、面板高）往上放不下，就压进屏幕里
        // —— 顶出去的那一半用户是看不到的，也没法拖（它是 Qt::Popup）。
        const int xMax = available.right() - width() - 4;
        x = xMax >= available.left() + 4 ? qBound(available.left() + 4, x, xMax)
                                         : available.left();
        y = qMax(available.top() + 4, y);
    }
    const QPoint target(x, y);
    if (animate) {
        // 从下面滑上来（和底栏那两块面板同一个手法，见 ui/Animations.hpp）。
        Anim::slideUp(this, target);
    } else {
        move(target);
    }
}

/// 把「这一轮刚做出来、还留在隐藏态」的子控件 show 出来。
///
/// 为什么需要它：整块重建出来的那张脸，**只在面板还没露出来的时候**才会跟着父控件
/// 一起 show。面板**已经露着**的时候重建（`refresh()`：账本变了、行内动作回了话、面板
/// 自己 showEvent 那一发）新做的控件全是留在隐藏态的 —— Qt 的规矩是"挂到一个已经可见的
/// 父控件底下、自己没被 show 过的新控件，只跟父控件**下一次** show 一起露脸"，而父控件
/// 早就是可见的，那一发永远不会来。症状是**最难查的那一种**：行、行尾那颗「取消」、里面
/// 那根条全都真在那儿（`findChild` 找得到、`click()` 点得着、`setFixedHeight()` 量的
/// 高度也对），就是**一个像素都不画**（`isVisible()` 是 false，几何还停在 0,0 —— 隐藏的
/// 控件在布局里算"空"，连位置都不会被摆）。
///
/// NOTE: 只补「**没被显式藏过**」的那些（`WA_WState_ExplicitShowHide` 是 false）。这一块
/// 里现在没有"故意先藏着"的子控件，但这条判据让这个函数**不会**把将来真被 `hide()`
/// 过的东西顺手露出来 —— 那种得靠调用方显式 `setVisible(true)` 恢复。
static void revealBuiltChildren(QWidget *container) {
    if (container == nullptr || !container->isVisible()) {
        return;  // 容器自己还藏着：它 show 的那一发会把这些一起带出来，这里别抢
    }
    const QList<QWidget *> children =
        container->findChildren<QWidget *>(Qt::FindDirectChildrenOnly);
    for (QWidget *child : children) {
        if (!child->isVisible() && !child->testAttribute(Qt::WA_WState_ExplicitShowHide)) {
            child->show();
        }
    }
}

void TaskCenterPanel::rebuild() {
    if (taskDragActive) {
        if (!m_dragRefreshPending) {
            m_dragRefreshPending = true;
            QTimer::singleShot(100, this, [this] { m_dragRefreshPending = false; requestRefresh(); });
        }
        return;
    }
    auto *layout = qobject_cast<QVBoxLayout *>(m_content->layout());
    if (layout == nullptr) {
        return;
    }
    // Keep unchanged rows; a single progress tick must not destroy every completed
    // task's widgets. Index, callbacks and palette/font changes invalidate a row.
    auto *scrollBar = m_scroll->verticalScrollBar();
    const int scrollTop = scrollBar->value();
    const QSignalBlocker blockScroll(scrollBar);
    QHash<QString, TaskCenterRowWidget *> reusable;
    // NOTE: 这里用的是 delete（不是 deleteLater）：下面马上要往同一个布局里摆新行，
    // 留着一批"待删"的老行只会让一帧里看到新旧两套。这个函数**不许**被某一行的
    // 信号处理器直接调到（「清除已完成」那颗按钮走 QTimer 排下一轮，见下面），
    // 所以删到自己脚底下这种可能不存在。
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (auto *row = dynamic_cast<TaskCenterRowWidget *>(item->widget())) {
            const QString key = row->property(MainWindowUI::taskCenterPanelRowKeyProperty()).toString();
            // Providers normally guarantee unique keys. Do not leak duplicate rows.
            delete reusable.take(key);
            reusable.insert(key, row);
        } else {
            delete item->widget();
        }
        delete item;
    }

    if (isVisible() && !m_selectedKey.isEmpty() && (!m_compact || m_compactDetailOpen) && m_onRowAction)
        m_onRowAction(m_selectedKey, TaskCenter::RowAction::Viewed);
    const QList<TaskCenter::Item> items =
        m_provider ? m_provider() : QList<TaskCenter::Item>();
    QList<TaskCenter::Item> active;
    QList<TaskCenter::Item> finished;
    for (const TaskCenter::Item &item : items) {
        const int filter = m_filter->currentIndex();
        const bool attention = item.state == TaskCenter::State::WaitingUser ||
            ((item.state == TaskCenter::State::Failed || item.state == TaskCenter::State::Interrupted) && !item.attentionAcknowledged);
        if ((filter == 1 && !isActive(item.state)) || (filter == 2 && isActive(item.state)) ||
            (filter == 3 && !attention)) continue;
        const QString search = m_search->text().trimmed();
        if (!search.isEmpty() && !(item.title + QLatin1Char(' ') + item.detail + QLatin1Char(' ') + item.errorSummary)
                                    .contains(search, Qt::CaseInsensitive)) continue;
        (isActive(item.state) ? active : finished).append(item);
    }
    m_clearFinished->setEnabled(std::any_of(items.cbegin(), items.cend(), [](const auto &item) { return !isActive(item.state); }));
    // 只把**真在视口里露了脸的**失败行标成"看过了"（`unreadFailure` = 还没看过）。
    // 先把这一趟**建出来**的失败行钥匙收进 `m_unreadFailureKeys`，等布局落定之后
    // （下一轮事件）只标**和视口相交**的那些；滚动条一动再补一遍（滚下去看见了才算看过）。
    // 不做成 provider 里整批标：面板只有屏幕可用高度的 60%，失败行排在下面、用户压根
    // 没滚到，整批标会把底栏红点当场熄灭、失败项从「需要处理」里消失，而且是**落盘**的。
    m_unreadFailureKeys.clear();
    if (m_onRowAction != nullptr && isVisible() && m_scroll->isVisible()) {
        for (const QList<TaskCenter::Item> *rows : {&active, &finished}) {
            for (const TaskCenter::Item &item : *rows) {
                if (item.unreadFailure) {
                    m_unreadFailureKeys.insert(item.key);
                }
            }
        }
        if (!m_unreadFailureKeys.isEmpty()) {
            QTimer::singleShot(0, this, [this]() { acknowledgeVisibleFailures(); });
        }
    }
    const auto selected = [this](const auto &item) { return item.key == m_selectedKey; };
    // Viewing an error acknowledges it. In the attention filter its row may
    // disappear, but keep the detail readable until the user changes selection/filter.
    if (m_filter->currentIndex() != 3 && !m_selectedKey.isEmpty() && !std::any_of(active.cbegin(), active.cend(), selected) &&
        !std::any_of(finished.cbegin(), finished.cend(), selected)) {
        m_selectedKey.clear();
        m_compactDetailOpen = false;
        applyWorkspaceMode();
    }

    // 行的编号**两栏接着数**（不各从 0 开始）：行的名字是按号找的（验证工程靠它量），
    // 两栏各从 0 开始的话「当前工作」第 0 行和「已完成」第 0 行会撞成一个名字。
    int index = 0;
    const auto reuseRow = [&](const TaskCenter::Item &item) -> TaskCenterRowWidget * {
        auto *row = reusable.take(item.key);
        if (row != nullptr && !row->matches(item, index, m_rowHandlerRevision)) {
            delete row;
            row = nullptr;
        }
        return row;
    };
    const auto addSectionTitle = [&](const QString &text) {
        auto *label = new QLabel(text, m_content);
        applyMutedFont(label, -1);
        layout->addWidget(label);
    };

    if (active.isEmpty() && finished.isEmpty()) {
        // 一项都没有：面板里留一句话，不留一大块空白（和底栏那两块面板的空态一个路数）。
        auto *empty = new QLabel(items.isEmpty() ? MainWindowUI::taskCenterPanelEmptyText()
                                                 : QCoreApplication::translate("TaskCenterPanel", "没有符合筛选条件的任务"),
                                 m_content);
        empty->setObjectName(MainWindowUI::taskCenterPanelEmptyLabelName());
        applyMutedFont(empty, 0);
        layout->addWidget(empty);
    }

    if (!active.isEmpty()) {
        addSectionTitle(MainWindowUI::taskCenterPanelActiveSectionText());
        for (const TaskCenter::Item &item : active) {
            auto *row = reuseRow(item);
            if (row == nullptr) row = new TaskCenterRowWidget(
                item, index, rowActivate(item.key), m_content, m_onRowAction, m_rowHandlerRevision,
                [this, target = item.key](const QString &source, bool after) {
                    // 下一轮重取真实快照：拖动期间某项可能已经开跑/取消，不按旧行号改队列。
                    QTimer::singleShot(0, this, [this, source, target, after] {
                        if (!m_provider || !m_onRowAction || source == target) return;
                        const auto current = m_provider(); QString group;
                        for (const auto &row : current) if (row.key == source && row.state == TaskCenter::State::Queued) group = row.queueGroup;
                        if (group.isEmpty()) return;
                        QStringList keys;
                        for (const auto &row : current) if (row.state == TaskCenter::State::Queued && row.queueGroup == group) keys.append(row.key);
                        const int from = keys.indexOf(source), to = keys.indexOf(target);
                        if (from < 0 || to < 0) return;
                        int destination = to + (after ? 1 : 0); if (from < destination) --destination;
                        const auto action = destination < from ? TaskCenter::RowAction::MoveUp : TaskCenter::RowAction::MoveDown;
                        for (int i = 0; i < qAbs(destination - from); ++i) m_onRowAction(source, action);
                        requestRefresh();
                    });
                });
            layout->addWidget(row);
            ++index;
        }
    }
    if (!finished.isEmpty()) {
        if (!active.isEmpty()) {
            layout->addSpacing(kSectionSpacing - kRowSpacing);
        }
        addSectionTitle(MainWindowUI::taskCenterPanelDoneSectionText());
        for (const TaskCenter::Item &item : finished) {
            auto *row = reuseRow(item);
            if (row == nullptr) row = new TaskCenterRowWidget(
                item, index, rowActivate(item.key), m_content, m_onRowAction, m_rowHandlerRevision);
            layout->addWidget(row);
            ++index;
        }
    }
    qDeleteAll(reusable);
    // 内容比上限矮的时候，行堆在上面而不是被摊开（stretch 吃掉下面的空）。
    layout->addStretch(1);
    rebuildDetail(items);
    syncSelection();
    // 面板**已经露着**的时候整块重建，新做出来的那些控件得自己 show 一下
    // （理由全文在 `revealBuiltChildren()` 上面那段）。几个容器都要过一遍：
    // 任务清单、右边详情。
    revealBuiltChildren(m_content);
    revealBuiltChildren(m_detailContent);
    revealBuiltChildren(m_detailFooter);
    // A top-aligned list needs no synchronous layout. Only settle geometry when
    // restoring an actual scroll offset; forcing it on first open delays the page.
    if (scrollTop > 0) layout->activate();
    scrollBar->setValue(scrollTop);
}

void TaskCenterPanel::acknowledgeVisibleFailures() {
    if (m_ackInProgress || m_onRowAction == nullptr || !m_opened || !isVisible() || !m_scroll->isVisible()) {
        return;
    }
    m_ackInProgress = true;
    // 行的几何可能是上一趟布局的（刚 addWidget 完还没摆）：先让它算完再量。
    if (auto *layout = m_content->layout()) {
        layout->activate();
    }
    // 视口那块在 `m_content` 坐标系里：滚动值就是内容已经往上滚掉的像素数。
    const QRect viewport =
        m_scroll->viewport()->rect().translated(0, m_scroll->verticalScrollBar()->value());
    QStringList seen;
    const QList<QWidget *> rows =
        m_content->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *row : rows) {
        const QVariant key = row->property(MainWindowUI::taskCenterPanelRowKeyProperty());
        if (!key.isValid()) continue;
        const QString keyText = key.toString();
        if (!m_unreadFailureKeys.contains(keyText)) continue;
        // `QRect::intersects` 判"挨着也算"（只露一条边不标才怪），这里要的是起码露一部分。
        if (!viewport.intersects(row->geometry())) continue;
        seen.append(keyText);
    }
    for (const QString &key : seen) {
        m_unreadFailureKeys.remove(key);
    }
    m_ackInProgress = false;
    // 动作放在最后发：`onRowAction` → 账本 `taskChanged` → 面板请求重建，重建会把上面
    // 摸过的那些控件全删掉 —— 先收集、后发，循环里就不再碰控件。
    for (const QString &key : seen) {
        if (m_onRowAction) {
            m_onRowAction(key, TaskCenter::RowAction::Seen);
        }
    }
}

void TaskCenterPanel::selectTask(const QString &key) {
    if (key.isEmpty() && m_selectedKey.isEmpty()) {
        return;
    }
    m_selectedKey = key;
    if (!key.isEmpty() && m_onRowAction)
        m_onRowAction(key, TaskCenter::RowAction::Viewed);
    const QList<TaskCenter::Item> items =
        m_provider ? m_provider() : QList<TaskCenter::Item>();
    rebuildDetail(items);
    syncSelection();
    revealBuiltChildren(m_detailContent);
    revealBuiltChildren(m_detailFooter);
}

void TaskCenterPanel::syncSelection() {
    for (QWidget *row : m_content->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        const QVariant key = row->property(MainWindowUI::taskCenterPanelRowKeyProperty());
        if (!key.isValid()) continue;
        row->setProperty("taskCenterSelected", !m_selectedKey.isEmpty() && key.toString() == m_selectedKey);
        row->update();
    }
}

bool TaskCenterPanel::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::MouseButtonPress &&
        (watched == m_content || watched == m_scroll->viewport()) &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
        selectTask(QString());
    return QWidget::eventFilter(watched, event);
}

void TaskCenterPanel::rebuildDetail(const QList<TaskCenter::Item> &items) {
    auto *layout = qobject_cast<QVBoxLayout *>(m_detailContent != nullptr
                                                  ? m_detailContent->layout()
                                                  : nullptr);
    if (layout == nullptr) {
        return;
    }
    while (QLayoutItem *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    auto *footer = qobject_cast<QVBoxLayout *>(m_detailFooter->layout());
    while (QLayoutItem *item = footer->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_detailFooter->hide();
    // 旧的「更多…」按钮已随底栏一起删掉；它的记账也一并换新。
    m_detailActions.reset();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // 和任务详情页（7c）同一套样子：一栏，最宽 720，靠左。
    auto *column = new QWidget(m_detailContent);
    column->setObjectName(QStringLiteral("taskCenterDetailColumn"));
    column->setMaximumWidth(720);
    column->setProperty("wtfmlDialogActions", true);  // 底下那排按钮一样高（36）
    layout->addWidget(column, 1);
    auto *box = new QVBoxLayout(column);
    box->setContentsMargins(0, 2, 4, 6);
    box->setSpacing(6);

    const Theme::Tokens t = Theme::tokens();
    const QColor muted = palette().color(QPalette::PlaceholderText);
    const QColor accent = Theme::textTone(t.accentInk, t.primary);
    const QColor danger = Theme::textTone(t.dangerInk, t.danger);
    const auto sectionTitle = [column](const QString &text) {
        auto *label = new QLabel(text, column);
        label->setFont(Theme::textFont(label->font(), Theme::TextRole::Heading));
        label->setContentsMargins(0, 14, 0, 4);
        return label;
    };

    auto selected = items.constEnd();
    if (!m_selectedKey.isEmpty()) {
        selected = std::find_if(items.constBegin(), items.constEnd(), [this](const auto &item) {
            return item.key == m_selectedKey;
        });
    }
    if (selected == items.constEnd()) {
        m_selectedKey.clear();
        m_detailContent->setProperty("taskKey", QVariant());
        int active = 0;
        int done = 0;
        int attention = 0;
        for (const TaskCenter::Item &item : items) {
            if (isActive(item.state)) {
                ++active;
            } else {
                ++done;
            }
            if (item.state == TaskCenter::State::WaitingUser
                || ((item.state == TaskCenter::State::Failed || item.state == TaskCenter::State::Interrupted)
                    && !item.attentionAcknowledged)) {
                ++attention;
            }
        }
        auto *title = new QLabel(QCoreApplication::translate("TaskCenterPanel", "任务概览"), column);
        Theme::markDisplay(title, Theme::textFont(title->font(), Theme::TextRole::Display));
        box->addWidget(title);
        box->addSpacing(6);
        // 三行「图标 + 名字 + 右侧数」；text() 仍是「需要处理：n」那一句（读屏 / 验证工程认它）。
        const struct { QString word; int count; const char *glyph; QColor color; } lines[] = {
            {QCoreApplication::translate("TaskCenterPanel", "正在进行"), active, "dots", accent},
            {QCoreApplication::translate("TaskCenterPanel", "已完成"), done, "okc", Theme::textTone(t.okInk, t.ok)},
            {QCoreApplication::translate("TaskCenterPanel", "需要处理"), attention, "warn", attention > 0 ? danger : muted},
        };
        for (int i = 0; i < 3; ++i) {
            auto *label = new TaskVisuals::GlyphLabel(
                QCoreApplication::translate("TaskCenterPanel", "%1：%2").arg(lines[i].word).arg(lines[i].count), column);
            label->shown = lines[i].word;
            label->note = QString::number(lines[i].count);
            label->noteColor = palette().color(QPalette::WindowText);
            label->glyph = QString::fromLatin1(lines[i].glyph);
            label->glyphColor = lines[i].color;
            label->glyphSize = 18;
            label->glyphGap = 14;
            label->setMinimumHeight(40);
            label->setContentsMargins(10, 0, 12, 0);
            label->divider = i < 2;
            box->addWidget(label);
        }
        box->addStretch(1);
        return;
    }

    const TaskCenter::Item &item = *selected;
    m_detailContent->setProperty("taskKey", item.key);
    if (m_embedded && m_compact) {
        auto *back = new QPushButton(QCoreApplication::translate("TaskCenterPanel", "← 返回任务列表"), column);
        back->setObjectName(QStringLiteral("taskCenterCompactBack"));
        Theme::markButton(back, Theme::ButtonKind::Ghost);
        connect(back, &QPushButton::clicked, back, [this]() {
            // 这一发**不能同步**做：`rebuildDetail()` 第一件事就是把这一栏（连同 `back`
            // 自己）删掉，而 `back` 正站在这条信号栈里。排到下一轮事件循环再做。
            QTimer::singleShot(0, this, [this]() {
                if (!m_compact) {
                    // 这一轮里面板换了形态/被关掉：「返回列表」这件事已经没有意义。
                    return;
                }
                m_compactDetailOpen = false;
                m_selectedKey.clear();
                syncSelection();
                // 窄屏那一档是"列表 ⇄ 详情"整块换的（见 applyWorkspaceMode()）—— 标志清了
                // 得当场让它摆一次，不能等下面那一趟重建（那一趟是异步的，中间会空一帧）。
                applyWorkspaceMode();
                // 回到列表时顺手刷一次：这一页开着的这段时间里账本可能已经动过。
                requestRefresh();
            });
        });
        box->addWidget(back, 0, Qt::AlignLeft);
        box->addSpacing(6);
    }

    // ---------- 状态那一行：图标 + 状态词 + 时间 ----------
    auto *stateRow = new QHBoxLayout;
    stateRow->setContentsMargins(0, 0, 0, 0);
    stateRow->setSpacing(8);
    const TaskVisuals::Glyph stateGlyph = TaskVisuals::stateGlyph(item.state, palette());
    auto *state = new TaskVisuals::GlyphLabel(MainWindowUI::taskDetailStateText(item.state), column);
    state->setObjectName(MainWindowUI::taskDetailStateName());
    state->shown = state->text();
    state->glyph = stateGlyph.name;
    state->glyphColor = state->shownColor = stateGlyph.color;
    state->setFont(Theme::textFont(state->font(), Theme::TextRole::Caption));
    stateRow->addWidget(state, 0, Qt::AlignVCenter);
    const QString timeLine = MainWindowUI::taskDetailTimeLineText(item.startedAt, item.finishedAt);
    if (!timeLine.isEmpty()) {
        auto *when = new QLabel(timeLine, column);
        when->setObjectName(QStringLiteral("taskCenterDetailTime"));
        applyMutedFont(when, -1);
        stateRow->addWidget(when, 0, Qt::AlignVCenter);
    }
    stateRow->addStretch(1);
    box->addLayout(stateRow);

    // ---------- 标题（手写体）----------
    auto *title = new QLabel(item.title, column);
    title->setObjectName(MainWindowUI::taskDetailTitleName());
    title->setWordWrap(true);
    Theme::markDisplay(title, Theme::textFont(title->font(), Theme::TextRole::Display));
    box->addWidget(title);
    if (!item.detail.isEmpty()) {
        auto *detail = new QLabel(item.detail, column);
        detail->setWordWrap(true);
        applyMutedFont(detail, -1);
        box->addWidget(detail);
    }
    // 「取消」这一颗不露脸时得说清为什么（不能取消的那一类 / 暂时停不得的那一段）。
    if (isActive(item.state) && !item.capabilities.cancel && !item.cancelBlockedReason.isEmpty()) {
        auto *blocked = new TaskVisuals::GlyphLabel(item.cancelBlockedReason, column);
        blocked->setObjectName(QStringLiteral("taskCenterCancelBlockedReason"));
        blocked->setWordWrap(true);
        blocked->glyph = QStringLiteral("info");
        blocked->glyphColor = muted;
        blocked->glyphSize = 14;
        blocked->glyphAt = QPoint(0, 2);
        blocked->setContentsMargins(20, 0, 0, 0);
        applyMutedFont(blocked, -1);
        box->addWidget(blocked);
    }
    // ---------- 进度：细圆角条 + 右边那个数（测不出总量就不画）----------
    if (item.percent >= 0) {
        auto *barRow = new QHBoxLayout;
        barRow->setContentsMargins(0, 8, 0, 0);
        barRow->setSpacing(12);
        auto *bar = new TaskProgressBar(column);
        bar->setObjectName(QStringLiteral("taskCenterDetailProgress"));
        bar->setRange(0, 100);
        bar->setValue(item.percent);
        bar->setFixedHeight(MainWindowUI::taskCenterPanelBarHeight());
        barRow->addWidget(bar, 1, Qt::AlignVCenter);
        auto *percent = new QLabel(MainWindowUI::taskCenterPercentText(item.percent), column);
        applyMutedFont(percent, -1);
        barRow->addWidget(percent, 0, Qt::AlignVCenter);
        box->addLayout(barRow);
    }
    if (!item.bytesText.isEmpty()) {
        auto *bytes = new QLabel(item.bytesText, column);
        applyMutedFont(bytes, -1);
        box->addWidget(bytes);
    }
    // ---------- 出错那一块：浅红圆角底 + 警告图标 ----------
    if (!item.errorSummary.isEmpty()) {
        box->addSpacing(6);
        auto *error = new TaskVisuals::GlyphLabel(item.errorSummary, column);
        error->setObjectName(QStringLiteral("taskCenterDetailError"));
        error->setWordWrap(true);
        error->setTextInteractionFlags(Qt::TextSelectableByMouse);
        error->glyph = QStringLiteral("warn");
        error->glyphAt = QPoint(14, 12);
        error->glyphColor = danger;
        error->fill = t.dangerSoft;
        error->radius = 12;
        error->setContentsMargins(42, 11, 14, 11);
        box->addWidget(error);
    }
    // ---------- 执行流程：图标 + 名字 + 右侧注，行间细线，正在跑那一行浅底 ----------
    if (!item.steps.isEmpty()) {
        auto *heading = sectionTitle(QCoreApplication::translate("TaskCenterPanel", "执行流程"));
        heading->setObjectName(QStringLiteral("taskCenterFlowHeading"));
        box->addWidget(heading);
        auto *steps = new QVBoxLayout;
        steps->setContentsMargins(0, 0, 0, 0);
        steps->setSpacing(0);
        for (int index = 0; index < item.steps.size(); ++index) {
            // 兼容旧任务历史：终态不能再把当时的当前步骤画成“仍在运行”。判定和另外三处
            // 共用 `stepForTask()`。
            const auto step = TaskCenter::stepForTask(item.state, item.steps.at(index));
            auto *row = new QFrame(column);
            row->setObjectName(QStringLiteral("taskCenterFlowStep_%1").arg(index));
            if (step.state == TaskCenter::StepState::Running) {
                const auto paintRow = [row] {
                    row->setStyleSheet(QStringLiteral("QFrame#%1 { background: %2; border-radius: 10px; }")
                                           .arg(row->objectName(), Theme::tokens().hover.name(QColor::HexArgb)));
                };
                paintRow();
                Theme::follow(row, paintRow);
            }
            auto *rowLayout = new QVBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(0);
            auto *name = new TaskVisuals::GlyphLabel(
                step.note.isEmpty() ? step.name : step.name + QStringLiteral(" / ") + step.note, row);
            name->setObjectName(QStringLiteral("taskCenterFlowStepName_%1").arg(index));
            name->shown = step.name;
            name->note = step.note;
            const TaskVisuals::Glyph glyph = TaskVisuals::stepGlyph(step.state, palette());
            name->glyph = glyph.name;
            name->glyphColor = glyph.color;
            name->glyphSize = 18;
            name->glyphGap = 14;
            name->setMinimumHeight(42);
            name->setContentsMargins(10, 0, 12, 0);
            name->divider = index + 1 < item.steps.size();
            QFont font = name->font();
            switch (step.state) {
            case TaskCenter::StepState::Done:
                font.setWeight(QFont::Medium);
                break;
            case TaskCenter::StepState::Failed:
                name->shownColor = name->noteColor = danger;
                font.setWeight(QFont::Medium);
                break;
            case TaskCenter::StepState::Running:
                name->divider = false;
                font.setWeight(QFont::DemiBold);
                break;
            case TaskCenter::StepState::Pending:
                name->shownColor = muted;
                break;
            }
            name->setFont(font);
            if (step.note.length() > 18) name->setToolTip(step.note);
            rowLayout->addWidget(name);
            if (step.state == TaskCenter::StepState::Running && isActive(item.state)) {
                auto *progress = new TaskProgressBar(row);
                progress->setObjectName(QStringLiteral("taskCenterFlowProgress"));
                progress->setRange(0, 0); // 阶段没有独立百分比，不借用整个任务的百分比。
                progress->setFixedHeight(MainWindowUI::taskCenterPanelBarHeight());
                auto *barBox = new QHBoxLayout;
                barBox->setContentsMargins(42, 4, 12, 6);
                barBox->addWidget(progress);
                rowLayout->addLayout(barBox);
            }
            steps->addWidget(row);
        }
        box->addLayout(steps);
    }
    box->addStretch(1);

    // ---------- 底部一排：主钮（实心）+ 最多两颗扁平钮 + 「更多…」----------
    if (!m_onRowAction) {
        return;
    }
    QList<QPushButton *> secondaries;
    QPushButton *primary = nullptr;
    auto *actionsBar = new QWidget(m_detailFooter);
    actionsBar->setObjectName(QStringLiteral("taskCenterDetailActions"));
    auto *actions = new QHBoxLayout(actionsBar);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    const auto makeAction = [&](const QString &text, TaskCenter::RowAction action) {
        auto *button = new QPushButton(text, actionsBar);
        if (action == TaskCenter::RowAction::Cancel) {
            button->setObjectName(MainWindowUI::taskDetailCancelName());
        } else if (action == TaskCenter::RowAction::Retry) {
            button->setObjectName(MainWindowUI::taskDetailRetryName());
        } else {
            button->setObjectName(QStringLiteral("taskCenterDetailAction_%1").arg(int(action)));
        }
        button->setAutoDefault(false);
        const QString key = item.key;
        connect(button, &QPushButton::clicked, button, [this, key, action]() {
            if (m_onRowAction) {
                m_onRowAction(key, action);
            }
        });
        actions->addWidget(button);
        return button;
    };
    // 主钮按"最该做的那件"挑：重试 > 导出报告 > 继续未完成 > 补齐文件；其余按常用程度排。
    const auto offer = [&](bool can, const QString &text, TaskCenter::RowAction action, bool primaryCandidate) {
        if (!can) return;
        QPushButton *button = makeAction(text, action);
        if (primaryCandidate && primary == nullptr) primary = button;
        else secondaries.append(button);
    };
    offer(item.capabilities.retry, QCoreApplication::translate("TaskCenterPanel", "重试"), TaskCenter::RowAction::Retry, true);
    offer(item.capabilities.exportReport, QCoreApplication::translate("TaskCenterPanel", "导出错误报告"), TaskCenter::RowAction::ExportReport, true);
    offer(item.capabilities.recover, QCoreApplication::translate("TaskCenterPanel", "继续未完成操作"), TaskCenter::RowAction::Recover, true);
    offer(item.capabilities.repairImport, QCoreApplication::translate("TaskCenterPanel", "补齐文件"), TaskCenter::RowAction::RepairImport, true);
    offer(item.capabilities.cancel, QCoreApplication::translate("TaskCenterPanel", "取消任务"), TaskCenter::RowAction::Cancel, false);
    offer(item.capabilities.pause, QCoreApplication::translate("TaskCenterPanel", "暂停下载"), TaskCenter::RowAction::Pause, false);
    offer(item.capabilities.openTarget, QCoreApplication::translate("TaskCenterPanel", "打开目标位置"), TaskCenter::RowAction::OpenTarget, false);
    offer(item.capabilities.openLog, QCoreApplication::translate("TaskCenterPanel", "打开日志"), TaskCenter::RowAction::OpenLog, false);
    offer(item.capabilities.openCrashDir, QCoreApplication::translate("TaskCenterPanel", "打开崩溃报告目录"), TaskCenter::RowAction::OpenCrashDir, false);
    if (primary == nullptr && secondaries.isEmpty()) {
        delete actionsBar;
        return;
    }
    m_detailActions = std::make_unique<TaskVisuals::ActionOverflow>(actionsBar, QStringLiteral("taskCenterDetailMore"));
    actions->addWidget(m_detailActions->button());
    actions->addStretch(1);
    // 能摆多宽：窄屏整页那一档 = 这一页的宽，宽屏 = 右栏的宽（都扣掉边距和滚动条那一溜）。
    const int budget = qMin(720, (m_compact ? width() - 2 * kPanelPadding : m_detailPane->minimumWidth()) - 16);
    m_detailActions->arrange(primary, secondaries, 2, budget);
    auto *hairline = new QFrame(m_detailFooter);
    hairline->setObjectName(QStringLiteral("taskCenterDetailHairline"));
    hairline->setFixedHeight(1);
    hairline->setAutoFillBackground(true);
    const auto paintLine = [hairline] {
        QPalette pal = hairline->palette();
        pal.setColor(QPalette::Window, Theme::tokens().divider);
        hairline->setPalette(pal);
    };
    paintLine();
    Theme::follow(hairline, paintLine);
    footer->addSpacing(10);
    footer->addWidget(hairline);
    footer->addSpacing(12);
    footer->addWidget(actionsBar);
    m_detailFooter->show();
}

void TaskCenterPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (!m_embedded) {
        // 单独弹出来那一档（不是产品现在这条主路）：右栏跟着一起伸缩。
        updateDetailPaneWidth();
        return;
    }
    const bool compact = event->size().width() < kWorkspaceMinWidth;
    if (compact != m_compact) {
        m_compact = compact;
        if (!m_compact) {
            m_compactDetailOpen = false;
        }
        applyWorkspaceMode();
        return;
    }
    updateDetailPaneWidth();
}

bool TaskCenterPanel::handleOutsidePress(QMouseEvent *event) {
    if (m_embedded) {
        return false;
    }
    const QPoint local = event->position().toPoint();
    if (rect().contains(local)) {
        return false;  // 落在面板自己身上：照旧往下走
    }
    const QPoint global = event->globalPosition().toPoint();
    const bool claimed = m_onOutsidePress.claimed && m_onOutsidePress.claimed(global);
    if (claimed) {
        // **必须赶在 close() 之前**：Qt 是关窗那一刻读这个属性（理由全文见
        // MainWindowUI.cpp 里 BottomBarPanel::handleOutsidePress() 那两段 NOTE）。
        setAttribute(Qt::WA_NoMouseReplay, true);
    }
    close();
    // 收回去之后才轮到"要不要接着开另一块"：那一下要开的那一块，是**这一块已经收掉之后**
    // 才该出现的（顺序反了的话，新面板会被底下那段"收旧"再关一次）。
    if (claimed && m_onOutsidePress.dismissed) {
        m_onOutsidePress.dismissed(global);
    }
    event->accept();
    return true;
}

void TaskCenterPanel::mousePressEvent(QMouseEvent *event) {
    if (handleOutsidePress(event)) {
        return;
    }
    QWidget::mousePressEvent(event);
}

void TaskCenterPanel::mouseDoubleClickEvent(QMouseEvent *event) {
    // 点得**快**的时候，"外面的一下"到的是双击事件（Windows 上第二发就是它）。
    if (handleOutsidePress(event)) {
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TaskCenterPanel::paintEvent(QPaintEvent *) {
    if (m_embedded) {
        return;
    }
    QPainter painter(this);
    Popups::paintSurface(painter, QRectF(rect()), 14);
}
