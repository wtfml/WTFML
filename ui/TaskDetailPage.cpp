/**
 * @file TaskDetailPage.cpp
 * @brief 任务详情页的实现（形状与取舍见头文件）。
 * @note 7c 起按设计稿 task.html：单栏 720、状态行带图标、步骤行图标 + 右侧注、出错浅红块、
 *       底部操作条。各标签的 text() 仍是「记号 + 文字」（复制摘要 / 读屏 / 验证工程认它），
 *       屏幕上由 GlyphLabel 换成线条图标画。
 */

#include "TaskDetailPage.hpp"

#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include "ElidedLabel.hpp"
#include "MainWindowUI.hpp"
#include "TaskProgressBar.hpp"
#include "TaskVisuals.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

namespace {

constexpr int kColumnWidth = 720;  // 设计稿 .task-detail 单栏宽
constexpr int kStepHeight = 44;    // 设计稿 .step min-height

bool stateIsDanger(TaskCenter::State state) {
    return state == TaskCenter::State::Failed || state == TaskCenter::State::Interrupted;
}

/// 前面画一枚线条图标的标签。`shown` 非空时屏幕上画它（`text()` 留着带记号的原文），
/// 右边还能挂一句 `note`；`shown` 空时按 QLabel 自己画（多行换行那块出错说明用）。
using TaskVisuals::GlyphLabel;

QLabel *makeMutedLabel(QWidget *parent, const QString &objectName, int px = 13) {
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    QFont font = label->font();
    font.setPixelSize(px);
    label->setFont(font);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

QLabel *makeSectionTitle(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setFont(Theme::textFont(label->font(), Theme::TextRole::Heading));
    label->setContentsMargins(0, 14, 0, 4);
    return label;
}

void paintHairline(QFrame *line) {
    QPalette pal = line->palette();
    pal.setColor(QPalette::Window, Theme::tokens().divider);
    line->setPalette(pal);
}

}  // namespace

TaskDetailPage::TaskDetailPage(QWidget *parent) : QDialog(parent) {
    setObjectName(MainWindowUI::taskDetailPageName());
    setWindowTitle(windowTitle());
    // 只影响"单独拿出来用"（验证工程那几处）：嵌进覆盖层的时候，这一页拿到的是内容区的
    // 整幅宽（见 OverlayHost::showPage 里那句 area->layout()->addWidget(dialog)）。
    resize(560, 460);

    // 外层只管"栏靠左、最宽 720"；内容都在 m_box 那一栏里。
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *column = new QWidget(this);
    column->setMaximumWidth(kColumnWidth);
    column->setProperty("wtfmlDialogActions", true);  // 底下那排按钮一样高（36）
    outer->addWidget(column, 1);
    outer->addStretch(0);
    m_box = new QVBoxLayout(column);
    m_box->setContentsMargins(0, 4, 0, 20);
    m_box->setSpacing(6);

    // ---------- 状态那一行：图标 + 状态词 + 时间 ----------
    auto *stateRow = new QHBoxLayout;
    stateRow->setContentsMargins(0, 0, 0, 0);
    stateRow->setSpacing(8);
    m_stateLabel = new GlyphLabel(this);
    m_stateLabel->setObjectName(MainWindowUI::taskDetailStateName());
    {
        QFont font = m_stateLabel->font();
        font.setPixelSize(13);
        m_stateLabel->setFont(font);
    }
    stateRow->addWidget(m_stateLabel, 0, Qt::AlignVCenter);
    m_timeLabel = makeMutedLabel(this, MainWindowUI::taskDetailTimeName());
    stateRow->addWidget(m_timeLabel, 0, Qt::AlignVCenter);
    stateRow->addStretch(1);
    m_box->addLayout(stateRow);

    // ---------- 标题（手写体大标题）----------
    m_titleLabel = new ElidedLabel(this);
    m_titleLabel->setObjectName(MainWindowUI::taskDetailTitleName());
    Theme::markDisplay(m_titleLabel, Theme::textFont(m_titleLabel->font(), Theme::TextRole::Display));
    m_box->addWidget(m_titleLabel);

    // ---------- 一行小字（"已下载 12 / 40  文件名"）----------
    m_detailLabel = makeMutedLabel(this, MainWindowUI::taskDetailDetailName());
    m_detailLabel->setWordWrap(true);
    m_box->addWidget(m_detailLabel);

    // ---------- 进度：条 + 右边那个数 ----------
    // 测不出总量（percent < 0）的时候**整行收起来**，不画一根不确定态的条，也不编一个
    // 百分比出来 —— 和面板那一行同一条规矩。
    auto *barRow = new QHBoxLayout;
    barRow->setContentsMargins(0, 10, 0, 0);
    barRow->setSpacing(14);
    m_bar = new TaskProgressBar(this);
    m_bar->setObjectName(MainWindowUI::taskDetailBarName());
    m_bar->setRange(0, 100);
    m_bar->setFixedHeight(MainWindowUI::taskCenterPanelBarHeight());
    barRow->addWidget(m_bar, 1);
    auto *percent = makeMutedLabel(this, MainWindowUI::taskDetailPercentName());
    barRow->addWidget(percent, 0, Qt::AlignVCenter);
    m_box->addLayout(barRow);

    // ---------- 字节那一句（按字节记账的任务才有）----------
    m_bytesLabel = makeMutedLabel(this, MainWindowUI::taskDetailBytesName());
    m_box->addWidget(m_bytesLabel);

    // ---------- 「到了哪一步」：步骤清单 ----------
    m_box->addWidget(makeSectionTitle(MainWindowUI::taskDetailStepsTitleText(), this));
    m_body = new QWidget(this);
    m_body->setObjectName(MainWindowUI::taskDetailStepsName());
    auto *bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    m_box->addWidget(m_body);

    // ---------- 出错那一块（只在真出过错的时候露）：小标题 + 浅红底说明 ----------
    m_errorTitle = makeSectionTitle(MainWindowUI::taskDetailErrorTitleText(), this);
    m_box->addWidget(m_errorTitle);
    auto *error = new GlyphLabel(this);
    error->glyph = QStringLiteral("warn");
    error->glyphAt = QPoint(16, 14);
    error->radius = 12;
    error->setContentsMargins(46, 13, 18, 13);
    m_errorLabel = error;
    m_errorLabel->setObjectName(MainWindowUI::taskDetailErrorName());
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_box->addWidget(m_errorLabel);

    m_box->addStretch(1);

    // ---------- 底部操作条：一条细线 + 一排按钮，「关闭」推到最右 ----------
    // 露与不露**只读能力位**（见头文件那段）；"关闭"永远在，它是这一页的出路之一
    // （另一条出路是顶上那颗「返回」）。主动作（实心）由 rebuild() 按露出来的那几颗挑。
    auto *hairline = new QFrame(this);
    hairline->setFixedHeight(1);
    hairline->setAutoFillBackground(true);
    paintHairline(hairline);
    Theme::follow(hairline, [hairline] { paintHairline(hairline); });
    m_box->addSpacing(10);
    m_box->addWidget(hairline);
    m_box->addSpacing(14);

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    m_retryButton = makeActionButton(MainWindowUI::taskDetailRetryText(), Action::Retry,
                                     MainWindowUI::taskDetailRetryName());
    m_reportButton = makeActionButton(MainWindowUI::taskDetailReportText(), Action::ExportReport,
                                      MainWindowUI::taskDetailReportName());
    m_recoverButton = makeActionButton(tr("继续未完成操作"), Action::Recover,
                                       QStringLiteral("taskDetailRecover"));
    m_repairButton = makeActionButton(tr("补齐文件"), Action::RepairImport,
                                      QStringLiteral("taskDetailRepairImport"));
    m_pauseButton = makeActionButton(tr("暂停下载"), Action::Pause, QStringLiteral("taskDetailPause"));
    m_cancelButton = makeActionButton(MainWindowUI::taskDetailCancelText(), Action::Cancel,
                                      MainWindowUI::taskDetailCancelName());
    m_crashDirButton = makeActionButton(MainWindowUI::taskDetailCrashDirText(),
                                        Action::OpenCrashDir,
                                        MainWindowUI::taskDetailCrashDirName());
    m_sessionButton = makeActionButton(tr("查看会话"), Action::OpenAgentSession,
                                       QStringLiteral("taskDetailAgentSession"));
    m_copyButton = makeActionButton(MainWindowUI::taskDetailCopyText(), Action::CopySummary,
                                    MainWindowUI::taskDetailCopyName());
    m_logButton = makeActionButton(MainWindowUI::taskDetailLogText(), Action::OpenLog,
                                   MainWindowUI::taskDetailLogName());
    m_targetButton = makeActionButton(MainWindowUI::taskDetailTargetText(), Action::OpenTarget,
                                      MainWindowUI::taskDetailTargetName());
    for (QPushButton *b : {m_retryButton, m_reportButton, m_recoverButton, m_repairButton,
                           m_pauseButton, m_cancelButton, m_crashDirButton, m_sessionButton,
                           m_copyButton, m_logButton, m_targetButton}) {
        actions->addWidget(b);
    }
    // 主钮 + 最多两颗扁平次要钮，其余进「更多…」（rebuild() 按能力位排）。
    m_overflow = std::make_unique<TaskVisuals::ActionOverflow>(this, QStringLiteral("taskDetailMore"));
    actions->addWidget(m_overflow->button());
    actions->addStretch(1);
    auto *close = new QPushButton(MainWindowUI::taskDetailCloseText(), this);
    close->setObjectName(MainWindowUI::taskDetailCloseName());
    Theme::markButton(close, Theme::ButtonKind::Ghost);
    close->setCursor(Qt::PointingHandCursor);
    // 关页 = 走覆盖层那条返回路：`reject()` 发 `QDialog::finished`，`showPage()` 接的就是
    // 这一发（返回箭头也只是转成 `reject()`；`execPage()` 那一档同样是它）。
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    actions->addWidget(close);
    m_box->addLayout(actions);

    setTask(TaskCenter::Item());
}

QPushButton *TaskDetailPage::makeActionButton(const QString &text, Action action,
                                              const QString &objectName) {
    auto *button = new QPushButton(text, this);
    button->setObjectName(objectName);
    Theme::markButton(button, Theme::ButtonKind::Neutral);
    button->setCursor(Qt::PointingHandCursor);
    connect(button, &QPushButton::clicked, this, [this, action] { requestAction(action); });
    return button;
}

void TaskDetailPage::setTask(const TaskCenter::Item &item) {
    m_item = item;
    m_key = item.key;
    rebuild();
}

void TaskDetailPage::rebuild() {
    const TaskCenter::State state = m_item.state;
    const bool active = TaskCenter::stateIsActive(state);
    const Theme::Tokens t = Theme::tokens();
    const QColor muted = palette().color(QPalette::PlaceholderText);
    const QColor accent = Theme::textTone(t.accentInk, t.primary);
    const QColor ok = Theme::textTone(t.okInk, t.ok);
    const QColor danger = Theme::textTone(t.dangerInk, t.danger);

    // ---------- 状态那一行 ----------
    const QString mark = TaskCenter::terminalMark(state);
    const QString word = MainWindowUI::taskDetailStateText(state);
    m_stateLabel->setText(mark.isEmpty() ? word : mark + QLatin1Char(' ') + word);
    {
        auto *label = static_cast<GlyphLabel *>(m_stateLabel);
        label->shown = word;
        if (stateIsDanger(state)) {
            label->glyph = QStringLiteral("xc");
            label->shownColor = label->glyphColor = danger;
        } else if (state == TaskCenter::State::Succeeded) {
            label->glyph = QStringLiteral("okc");
            label->shownColor = label->glyphColor = ok;
        } else if (active) {
            label->glyph = QStringLiteral("dots");
            label->shownColor = label->glyphColor = accent;
        } else {
            label->glyph = QStringLiteral("clock");
            label->shownColor = label->glyphColor = muted;
        }
        label->updateGeometry();
        label->update();
    }
    m_timeLabel->setText(MainWindowUI::taskDetailTimeLineText(m_item.startedAt, m_item.finishedAt));
    m_timeLabel->setVisible(!m_timeLabel->text().isEmpty());

    // ---------- 标题 / 小字 ----------
    m_titleLabel->setText(m_item.title.isEmpty() ? MainWindowUI::taskDetailNoTitleText()
                                                 : m_item.title);
    m_detailLabel->setText(m_item.detail);
    m_detailLabel->setVisible(!m_item.detail.isEmpty());

    // ---------- 进度那两样 ----------
    const bool showPercent = m_item.percent >= 0;
    m_bar->setVisible(showPercent);
    m_bar->setValue(m_item.percent);
    // 数那一格按 objectName 找回来而不是留个指针（加一件新东西时这里不会悄悄跟着错）。
    if (auto *percent = findChild<QLabel *>(MainWindowUI::taskDetailPercentName())) {
        percent->setText(showPercent ? MainWindowUI::taskCenterPercentText(m_item.percent)
                                     : QString());
        percent->setVisible(showPercent);
    }
    m_bytesLabel->setText(m_item.bytesText);
    m_bytesLabel->setVisible(!m_item.bytesText.isEmpty());

    // ---------- 步骤那一栏：图标 + 名字 + 右侧注，行间细线，正在跑那一行浅底 ----------
    auto *body = qobject_cast<QVBoxLayout *>(m_body->layout());
    if (body != nullptr) {
        while (QLayoutItem *child = body->takeAt(0)) {
            delete child->widget();
            delete child;
        }
        for (int i = 0; i < m_item.steps.size(); ++i) {
            const TaskCenter::Step step =
                TaskCenter::stepForTask(m_item.state, m_item.steps.at(i));
            auto *label = new GlyphLabel(m_body);
            label->setObjectName(MainWindowUI::taskDetailStepName(i));
            const QString note = step.note.isEmpty() ? QString()
                                                     : QStringLiteral(" / ") + step.note;
            label->setText(TaskCenter::stepMark(step.state) + QLatin1Char(' ') + step.name + note);
            label->shown = step.name;
            label->note = step.note;
            label->glyphSize = 18;
            label->glyphGap = 14;
            label->setMinimumHeight(kStepHeight);
            label->setContentsMargins(10, 0, 12, 0);
            label->divider = i + 1 < m_item.steps.size();
            QFont font = label->font();
            font.setPixelSize(13);
            switch (step.state) {
            case TaskCenter::StepState::Done:
                label->glyph = QStringLiteral("okc");
                label->glyphColor = ok;
                font.setWeight(QFont::Medium);
                break;
            case TaskCenter::StepState::Failed:
                label->glyph = QStringLiteral("xc");
                label->glyphColor = label->shownColor = label->noteColor = danger;
                font.setWeight(QFont::Medium);
                break;
            case TaskCenter::StepState::Running:
                label->glyph = QStringLiteral("dots");
                label->glyphColor = accent;
                label->fill = t.hover;
                label->radius = 10;
                label->divider = false;
                font.setWeight(QFont::DemiBold);
                break;
            case TaskCenter::StepState::Pending:
                label->glyph = QStringLiteral("clock");
                label->glyphColor = label->shownColor = muted;
                break;
            }
            label->setFont(font);
            body->addWidget(label);
        }
        if (m_item.steps.isEmpty()) {
            // 没有步骤的任务多的是（一次纯文件下载就没有）—— 写一句"没有分步"比留一块空白
            // 强，也让验证工程分得清"这一项本来就没有步骤"和"步骤没画出来"。
            auto *none = makeMutedLabel(m_body, MainWindowUI::taskDetailNoStepsName());
            none->setText(MainWindowUI::taskDetailNoStepsText());
            body->addWidget(none);
        }
    }

    // ---------- 出错那一块 ----------
    const bool hasError = !m_item.errorSummary.isEmpty() && !active;
    m_errorLabel->setVisible(hasError);
    m_errorTitle->setVisible(hasError);
    if (hasError) {
        m_errorLabel->setText(m_item.errorSummary);
        auto *error = static_cast<GlyphLabel *>(m_errorLabel);
        error->fill = t.dangerSoft;
        error->glyphColor = danger;
    }

    // ---------- 按钮那一排 ----------
    m_cancelButton->setVisible(m_item.capabilities.cancel);
    m_retryButton->setVisible(m_item.capabilities.retry);
    m_logButton->setVisible(m_item.capabilities.openLog);
    m_targetButton->setVisible(m_item.capabilities.openTarget);
    m_repairButton->setVisible(m_item.capabilities.repairImport);
    m_recoverButton->setVisible(m_item.capabilities.recover);
    m_pauseButton->setVisible(m_item.capabilities.pause);
    m_reportButton->setVisible(m_item.capabilities.exportReport);
    m_crashDirButton->setVisible(m_item.capabilities.openCrashDir);
    m_sessionButton->setVisible(m_item.capabilities.openAgentSession);
    // 「复制摘要」永远在（它不需要任何能力位 —— 摘要就是这一页上那几行字，任何时候都抄得走）。
    m_copyButton->setVisible(!m_key.isEmpty());
    // 主动作（实心）：重试 > 导出报告 > 继续 > 补齐，能力位给了的第一颗。其余按优先级当扁平
    // 次要钮，最多露两颗，剩下的进「更多…」（按钮还在，只是藏起来、由菜单项去点）。
    QPushButton *primary = nullptr;
    QList<QPushButton *> secondaries;
    for (QPushButton *b : {m_cancelButton, m_pauseButton, m_sessionButton, m_retryButton, m_reportButton,
                           m_recoverButton, m_repairButton, m_crashDirButton, m_logButton, m_targetButton,
                           m_copyButton}) {
        if (b->isHidden()) continue;
        const bool candidate = b == m_retryButton || b == m_reportButton || b == m_recoverButton || b == m_repairButton;
        if (primary == nullptr && candidate) primary = b;
        else secondaries.append(b);
    }
    m_overflow->arrange(primary, secondaries, 2);
}

bool TaskDetailPage::offers(const QPushButton *button) const { return m_overflow->offers(button); }

TaskDetailPage::~TaskDetailPage() = default;
