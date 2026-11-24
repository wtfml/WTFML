/**
 * @file ActivityPopover.cpp
 * @brief 任务中心浮动面板（见 .hpp）。
 */
#include "ActivityPopover.hpp"
#include <QCoreApplication>

#include "Theme.hpp"
#include "VersionIcons.hpp"

#include <QApplication>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

using TaskCenter::Item;
using TaskCenter::State;

constexpr int kPadding = 14;
constexpr int kGapToAnchor = 8;

bool isCurrent(State s) { return s == State::Running || s == State::WaitingUser || s == State::Paused; }
bool isFinished(State s) {
    return s == State::Succeeded || s == State::Failed || s == State::Cancelled || s == State::Interrupted;
}

QLabel *textLabel(const QString &text, QWidget *parent, Theme::TextRole role, const QColor &ink,
                  bool bold = false) {
    auto *label = new QLabel(text, parent);
    QFont f = Theme::textFont(label->font(), role);
    f.setBold(bold);
    label->setFont(f);
    QPalette p = label->palette();
    p.setColor(QPalette::WindowText, ink);
    label->setPalette(p);
    label->setTextFormat(Qt::PlainText);
    return label;
}

/// 一行标题太长时省略号收尾（面板宽度定死，不能被长名字撑开）。
QLabel *elidedTitle(const QString &text, QWidget *parent, int width) {
    const Theme::Tokens t = Theme::tokens();
    auto *label = textLabel(QString(), parent, Theme::TextRole::Body, t.text, true);
    label->setText(QFontMetrics(label->font()).elidedText(text, Qt::ElideRight, width));
    if (label->text() != text) label->setToolTip(text);
    return label;
}

QLabel *iconLabel(const QString &name, const QColor &color, QWidget *parent, int size = 18) {
    auto *label = new QLabel(parent);
    label->setFixedSize(size, size);
    label->setPixmap(VersionIcons::line(name, size, color));
    return label;
}

/// 行尾那几颗扁平小按钮（无底，悬停才淡淡一层，见全局 Ghost 样式）。
QPushButton *iconButton(const QString &icon, const QString &tip, const QString &name, QWidget *parent) {
    auto *button = new QPushButton(parent);
    button->setObjectName(name);
    Theme::markButton(button, Theme::ButtonKind::Ghost);
    Theme::markIconButton(button);
    button->setIcon(VersionIcons::line(icon, 16, Theme::tokens().secondary));
    button->setFixedSize(26, 26);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QPushButton *textButton(const QString &text, const QString &name, QWidget *parent) {
    auto *button = new QPushButton(text, parent);
    button->setObjectName(name);
    Theme::markButton(button, Theme::ButtonKind::Ghost);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

/// 4px 细进度条（设计稿 .bar）；percent < 0 画一段不确定态。
class ThinBar : public QWidget {
public:
    ThinBar(int percent, QWidget *parent) : QWidget(parent), m_percent(percent) { setFixedHeight(4); }
protected:
    void paintEvent(QPaintEvent *) override {
        const Theme::Tokens t = Theme::tokens();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(t.track);
        p.drawRoundedRect(rect(), 2, 2);
        p.setBrush(t.primary);
        if (m_percent >= 0) {
            p.drawRoundedRect(QRectF(0, 0, width() * qBound(0, m_percent, 100) / 100.0, height()), 2, 2);
        } else {
            p.drawRoundedRect(QRectF(width() * 0.3, 0, width() * 0.25, height()), 2, 2);
        }
    }
private:
    int m_percent;
};

/// 整行可点（进详情）的那一层。
class ClickRow : public QWidget {
public:
    ClickRow(std::function<void()> onClick, QWidget *parent) : QWidget(parent), m_onClick(std::move(onClick)) {
        if (m_onClick) setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }
protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (m_onClick && event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            const auto fn = m_onClick;  // 回调里可能把整块面板删掉：先拷一份
            fn();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }
    void paintEvent(QPaintEvent *) override {
        if (!m_onClick || !underMouse()) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::tokens().hover);
        p.drawRoundedRect(rect(), 8, 8);
    }
private:
    std::function<void()> m_onClick;
};

}  // namespace

ActivityPopover::ActivityPopover(QWidget *anchor, Handlers handlers)
    : GlassPopover(anchor), m_handlers(std::move(handlers)) {
    setObjectName(objectNameValue());
    setFixedWidth(kWidth);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(kPadding, 12, kPadding - 4, kPadding);
    outer->setSpacing(8);

    // ---------- 标题行：手写体标题 +「全部任务」（筛选 / 搜索 / 历史 / 详情都在整页里） ----------
    auto *header = new QHBoxLayout;
    header->setContentsMargins(2, 0, 0, 0);
    auto *title = new QLabel(QCoreApplication::translate("ActivityPopover", "任务中心"), this);
    title->setFont(Theme::textFont(title->font(), Theme::TextRole::Heading));
    header->addWidget(title);
    header->addStretch(1);
    auto *openAll = textButton(QCoreApplication::translate("ActivityPopover", "全部任务"), openAllButtonName(), this);
    openAll->setToolTip(QCoreApplication::translate("ActivityPopover", "整页查看：筛选、搜索、历史与详情"));
    connect(openAll, &QPushButton::clicked, this, [this] {
        const auto fn = m_handlers.openAll;
        close();
        if (fn) fn();
    });
    header->addWidget(openAll);
    outer->addLayout(header);

    auto *scroll = new Theme::GutterScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    m_body = new QWidget(scroll);
    m_body->setAutoFillBackground(false);
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(0, 0, 4, 0);
    m_bodyLayout->setSpacing(6);
    scroll->setWidget(m_body);
    outer->addWidget(scroll, 1);

    Theme::follow(this, [this] { rebuild(); });
    rebuild();
}

ActivityPopover::~ActivityPopover() = default;

void ActivityPopover::refresh() {
    rebuild();
}

QWidget *ActivityPopover::sectionHeader(const QString &title, QWidget *trailing) {
    auto *row = new QWidget(m_body);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(2, 8, 0, 0);
    layout->addWidget(textLabel(title, row, Theme::TextRole::Caption, Theme::tokens().secondary));
    layout->addStretch(1);
    if (trailing != nullptr) {
        trailing->setParent(row);
        layout->addWidget(trailing);
    }
    return row;
}

void ActivityPopover::rebuild() {
    if (m_bodyLayout == nullptr) return;
    while (QLayoutItem *item = m_bodyLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const Theme::Tokens t = Theme::tokens();
    const QList<Item> items = m_handlers.items ? m_handlers.items() : QList<Item>();
    QList<Item> current, queued, finished;
    for (const Item &item : items) {
        if (isCurrent(item.state)) current.append(item);
        else if (item.state == State::Queued) queued.append(item);
        else if (isFinished(item.state)) finished.append(item);
    }
    std::stable_sort(queued.begin(), queued.end(),
                     [](const Item &a, const Item &b) { return a.queueOrder < b.queueOrder; });
    std::stable_sort(finished.begin(), finished.end(),
                     [](const Item &a, const Item &b) { return a.finishedAt > b.finishedAt; });
    const int textWidth = kWidth - 2 * kPadding - 110;
    const auto activate = [this](const QString &key) -> std::function<void()> {
        if (!m_handlers.rowActivated) return {};
        return [this, key] {
            const auto fn = m_handlers.rowActivated;
            close();
            fn(key);
        };
    };
    const auto act = [this](const QString &key, TaskCenter::RowAction action) {
        return [this, key, action] {
            if (m_handlers.rowAction) m_handlers.rowAction(key, action);
            refresh();
        };
    };

    if (items.isEmpty()) {
        auto *empty = textLabel(QCoreApplication::translate("ActivityPopover", "暂无进行中的任务"), m_body, Theme::TextRole::Body, t.secondary);
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumHeight(64);
        m_bodyLayout->addWidget(empty);
    }

    // ---------- 当前：标题 + 百分比、一行小字、细进度条、步骤 ----------
    for (const Item &item : current) {
        auto *row = new ClickRow(activate(item.key), m_body);
        row->setObjectName(partName(QStringLiteral("Row"), item.key));
        auto *col = new QVBoxLayout(row);
        col->setContentsMargins(4, 6, 4, 6);
        col->setSpacing(4);
        auto *top = new QHBoxLayout;
        top->addWidget(elidedTitle(item.title, row, textWidth), 1);
        if (item.percent >= 0) {
            top->addWidget(textLabel(QStringLiteral("%1%").arg(item.percent), row, Theme::TextRole::Body,
                                     t.primary, true));
        }
        col->addLayout(top);
        if (!item.detail.isEmpty()) {
            auto *detail = textLabel(item.detail, row, Theme::TextRole::Caption, t.secondary);
            detail->setText(QFontMetrics(detail->font()).elidedText(item.detail, Qt::ElideRight, kWidth - 2 * kPadding - 12));
            col->addWidget(detail);
        }
        col->addWidget(new ThinBar(item.percent, row));
        for (const TaskCenter::Step &raw : item.steps) {
            const TaskCenter::Step step = TaskCenter::stepForTask(item.state, raw);
            auto *line = new QHBoxLayout;
            line->setSpacing(10);
            line->setContentsMargins(0, 2, 0, 0);
            QString icon = QStringLiteral("clock");
            QColor ink = t.secondary;
            if (step.state == TaskCenter::StepState::Done) { icon = QStringLiteral("okc"); ink = t.ok; }
            else if (step.state == TaskCenter::StepState::Running) { icon = QStringLiteral("runc"); ink = t.primary; }
            else if (step.state == TaskCenter::StepState::Failed) { icon = QStringLiteral("xc"); ink = t.danger; }
            line->addWidget(iconLabel(icon, ink, row, 16));
            line->addWidget(textLabel(step.name, row, Theme::TextRole::Caption,
                                      step.state == TaskCenter::StepState::Running ? t.text : t.secondary,
                                      step.state == TaskCenter::StepState::Running), 1);
            if (!step.note.isEmpty())
                line->addWidget(textLabel(step.note, row, Theme::TextRole::Caption, t.secondary));
            col->addLayout(line);
        }
        m_bodyLayout->addWidget(row);
    }

    // ---------- 等待中：上移 / 下移 / 移除（拖动排序在整页里） ----------
    if (!queued.isEmpty()) {
        m_bodyLayout->addWidget(sectionHeader(QCoreApplication::translate("ActivityPopover", "等待中"), nullptr));
        for (int i = 0; i < queued.size(); ++i) {
            const Item &item = queued.at(i);
            auto *row = new ClickRow(activate(item.key), m_body);
            row->setObjectName(partName(QStringLiteral("Row"), item.key));
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(4, 4, 0, 4);
            layout->setSpacing(10);
            auto *col = new QVBoxLayout;
            col->setSpacing(0);
            col->addWidget(elidedTitle(item.title, row, textWidth - 40));
            col->addWidget(textLabel(QCoreApplication::translate("ActivityPopover", "第 %1 个").arg(i + 1), row, Theme::TextRole::Caption, t.secondary));
            layout->addLayout(col, 1);
            const bool movable = !item.queueGroup.isEmpty();
            if (movable) {
                auto *upButton = iconButton(QStringLiteral("up"), QCoreApplication::translate("ActivityPopover", "上移"), partName(QStringLiteral("MoveUp"), item.key), row);
                upButton->setEnabled(i > 0);
                connect(upButton, &QPushButton::clicked, this, act(item.key, TaskCenter::RowAction::MoveUp));
                layout->addWidget(upButton);
                auto *downButton = iconButton(QStringLiteral("down"), QCoreApplication::translate("ActivityPopover", "下移"), partName(QStringLiteral("MoveDown"), item.key), row);
                downButton->setEnabled(i + 1 < queued.size());
                connect(downButton, &QPushButton::clicked, this, act(item.key, TaskCenter::RowAction::MoveDown));
                layout->addWidget(downButton);
            }
            if (item.capabilities.cancel) {
                auto *remove = iconButton(QStringLiteral("close"), QCoreApplication::translate("ActivityPopover", "移出队列"), partName(QStringLiteral("Remove"), item.key), row);
                connect(remove, &QPushButton::clicked, this, act(item.key, TaskCenter::RowAction::Cancel));
                layout->addWidget(remove);
            }
            m_bodyLayout->addWidget(row);
        }
    }

    // ---------- 已结束：最近几条，失败的能重试；「清除」只清记录 ----------
    QStringList unreadShown;
    if (!finished.isEmpty()) {
        QPushButton *clear = nullptr;
        if (m_handlers.clearFinished) {
            clear = textButton(QCoreApplication::translate("ActivityPopover", "清除"), clearButtonName(), m_body);
            clear->setToolTip(QCoreApplication::translate("ActivityPopover", "清除已结束记录，不影响下载好的文件"));
            connect(clear, &QPushButton::clicked, this, [this] {
                if (m_handlers.clearFinished) m_handlers.clearFinished();
                refresh();
            });
        }
        m_bodyLayout->addWidget(sectionHeader(QCoreApplication::translate("ActivityPopover", "已结束"), clear));
        for (int i = 0; i < finished.size() && i < kFinishedLimit; ++i) {
            const Item &item = finished.at(i);
            if (item.unreadFailure) unreadShown.append(item.key);
            auto *row = new ClickRow(activate(item.key), m_body);
            row->setObjectName(partName(QStringLiteral("Row"), item.key));
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(4, 4, 0, 4);
            layout->setSpacing(10);
            const bool ok = item.state == State::Succeeded;
            layout->addWidget(iconLabel(ok ? QStringLiteral("okc") : QStringLiteral("xc"), ok ? t.ok : t.danger, row));
            auto *col = new QVBoxLayout;
            col->setSpacing(0);
            col->addWidget(elidedTitle(item.title, row, textWidth));
            QString note = item.errorSummary.isEmpty() ? TaskCenter::timeText(item.finishedAt) : item.errorSummary;
            auto *noteLabel = textLabel(note, row, Theme::TextRole::Caption, t.secondary);
            noteLabel->setText(QFontMetrics(noteLabel->font()).elidedText(note, Qt::ElideRight, textWidth));
            if (noteLabel->text() != note) noteLabel->setToolTip(note);
            col->addWidget(noteLabel);
            layout->addLayout(col, 1);
            if (item.capabilities.retry) {
                auto *retry = textButton(QCoreApplication::translate("ActivityPopover", "重试"), partName(QStringLiteral("Retry"), item.key), row);
                connect(retry, &QPushButton::clicked, this, act(item.key, TaskCenter::RowAction::Retry));
                layout->addWidget(retry);
            }
            m_bodyLayout->addWidget(row);
        }
    }
    m_bodyLayout->addStretch(1);
    // 面板开着时新加进布局的行要等下一拍才 show，那之前 sizeHint 把它们当隐藏算成 0：这里直接 show。
    for (QWidget *row : m_body->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) row->show();
    place();
    // 摆出来的失败就算看过了（底栏红点跟着灭）；放到下一拍发：Seen → 账本变 → 这里重建。
    if (!unreadShown.isEmpty() && m_handlers.rowAction) {
        QTimer::singleShot(0, this, [this, unreadShown] {
            for (const QString &key : unreadShown) m_handlers.rowAction(key, TaskCenter::RowAction::Seen);
        });
    }
}

bool ActivityPopover::openAllIn(QWidget *root) {
    auto *pop = root ? root->findChild<QFrame *>(objectNameValue()) : nullptr;
    auto *all = pop ? pop->findChild<QPushButton *>(openAllButtonName()) : nullptr;
    if (all == nullptr) return false;
    all->click();
    return true;
}

void ActivityPopover::place() {
    QWidget *host = parentWidget();
    if (host == nullptr || m_anchor == nullptr) return;
    m_body->adjustSize();
    // 上下边距 + 标题行 + 间距，照布局实算（估小了就会冒出滚动条）。
    const QMargins m = layout()->contentsMargins();
    const int chrome = m.top() + m.bottom() + layout()->spacing() + layout()->itemAt(0)->sizeHint().height();
    const QPoint anchorTop = m_anchor->mapTo(host, QPoint(0, 0));
    const int bottom = anchorTop.y() - kGapToAnchor;
    const int maxHeight = qMax(160, bottom - 24);
    const int height = qMin(maxHeight, m_body->sizeHint().height() + chrome);
    const int x = qBound(8, anchorTop.x(), qMax(8, host->width() - kWidth - 8));
    setGeometry(x, bottom - height, kWidth, height);
    raise();
}
