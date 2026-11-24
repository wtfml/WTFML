/**
 * @file UpdateCenterPage.cpp
 * @brief 更新中心那一页的实现（形状与取舍见头文件）。
 */

#include "UpdateCenterPage.hpp"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "MainWindowUI.hpp"
#include "Theme.hpp"

namespace {

QLabel *makeMutedLabel(QWidget *parent, const QString &objectName) {
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    QPalette palette = label->palette();
    QColor muted = palette.color(QPalette::WindowText);
    muted.setAlpha(150);
    palette.setColor(QPalette::WindowText, muted);
    label->setPalette(palette);
    return label;
}

QFrame *makeSeparator(QWidget *parent) {
    auto *line = new QFrame(parent);
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedHeight(8);
    return line;
}

/// 行尾那三颗 chip 共用的一套（高度、样式、"跟着主题重刷"三件事都在 Theme 那一处 ——
/// 和资源详情页、实例页那一排筛选 chip 是**同一份**）。
QPushButton *makeRowChip(QWidget *parent, const QString &text, const QString &objectName,
                         const QString &tip) {
    auto *button = new QPushButton(text, parent);
    button->setObjectName(objectName);
    button->setToolTip(tip);
    Theme::markChipButton(button);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

}  // namespace

UpdateCenterPage::UpdateCenterPage(QWidget *parent) : QDialog(parent) {
    setObjectName(MainWindowUI::updateCenterPageName());
    setWindowTitle(windowTitle());
    // 只影响"单独拿出来用"（验证工程那几处）：嵌进覆盖层时这一页拿的是内容区的整幅宽。
    resize(760, 560);

    m_box = new QVBoxLayout(this);
    m_box->setContentsMargins(12, 12, 12, 12);
    m_box->setSpacing(10);

    // ---------- 总述那一句（大一号、加粗）----------
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setObjectName(MainWindowUI::updateCenterSummaryName());
    m_summaryLabel->setWordWrap(true);
    {
        QFont font = m_summaryLabel->font();
        font.setBold(true);
        font = Theme::textFont(font, Theme::TextRole::Heading);
        m_summaryLabel->setFont(font);
    }
    m_box->addWidget(m_summaryLabel);

    // ---------- 「没有结论」那一块（没查过 / 这个实例没什么可查的）----------
    m_errorLabel = new QLabel(this);
    m_errorLabel->setObjectName(MainWindowUI::updateCenterErrorName());
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_box->addWidget(m_errorLabel);

    // ---------- 反查那一趟自己的毛病（几个文件读不动）----------
    // 和上面那一块分开：一个是"这一趟根本没有结论"，一个是"结论在，但它自己有个缺口"。
    m_noteLabel = makeMutedLabel(this, MainWindowUI::updateCenterNoteName());
    m_noteLabel->setWordWrap(true);
    m_box->addWidget(m_noteLabel);

    m_box->addWidget(makeSeparator(this));

    // ---------- 两组的内容都长在滚动区里（每次 setReport() 整块重建）----------
    auto *scroll = new Theme::GutterScrollArea(this);
    scroll->setObjectName(MainWindowUI::updateCenterPageName() + QStringLiteral("Scroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_bodyParent = new QWidget(scroll);
    m_bodyParent->setObjectName(MainWindowUI::updateCenterPageName() + QStringLiteral("Body"));
    auto *bodyLayout = new QVBoxLayout(m_bodyParent);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(12);
    bodyLayout->addStretch(1);
    scroll->setWidget(m_bodyParent);
    m_box->addWidget(scroll, 1);

    m_box->addWidget(makeSeparator(this));

    // ---------- 底下那一排按钮 ----------
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    auto *close = new QPushButton(MainWindowUI::updateCenterCloseText(), this);
    close->setObjectName(MainWindowUI::updateCenterCloseName());
    Theme::markButton(close, Theme::ButtonKind::Neutral);
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    actions->addWidget(close);
    actions->addStretch(1);

    // 「撤销上一次更新」：**只在真有一份回滚点的时候露**（没有的时候给一颗点了什么都不做的
    // 按钮，比不给还糟 —— 和完整性报告页那颗「补齐…」是同一条）。它不动这一页：真正去还原
    // 的是 MainWindow（这一层碰不到盘）。
    m_undoButton = new QPushButton(this);
    m_undoButton->setObjectName(MainWindowUI::updateCenterUndoName());
    m_undoButton->setToolTip(MainWindowUI::updateCenterUndoTipText());
    Theme::markButton(m_undoButton, Theme::ButtonKind::Neutral);
    m_undoButton->setCursor(Qt::PointingHandCursor);
    connect(m_undoButton, &QPushButton::clicked, this, [this] { emit undoRequested(); });
    actions->addWidget(m_undoButton);

    // 「更新选中的 N 项」：这一页唯一的主动作（Primary = 主题色实底 + 白字）。
    m_updateSelectedButton = new QPushButton(this);
    m_updateSelectedButton->setObjectName(MainWindowUI::updateCenterUpdateSelectedName());
    Theme::markButton(m_updateSelectedButton, Theme::ButtonKind::Primary);
    m_updateSelectedButton->setCursor(Qt::PointingHandCursor);
    connect(m_updateSelectedButton, &QPushButton::clicked, this, [this] {
        const QStringList paths = checkedPaths();
        if (!paths.isEmpty()) {
            emit updateRequested(paths);
        }
    });
    actions->addWidget(m_updateSelectedButton);
    m_box->addLayout(actions);

    setReport(Report());
}

QStringList UpdateCenterPage::checkedPaths() const {
    // **按列表顺序**（不是 `m_checked` 那张哈希表的顺序）：更新那一批下完之后任务里那几步、
    // 日志里那几行都照这个顺序念，同一个实例连点两次的顺序要一样。
    QStringList paths;
    for (const Row &row : m_report.rows) {
        if (row.selectable() && m_checked.contains(row.path)) {
            paths.append(row.path);
        }
    }
    return paths;
}

void UpdateCenterPage::setReport(const Report &report) {
    m_report = report;
    // 勾过的那些**留着**：重喂多半是因为用户刚点了忽略/固定，"我勾了三条、中间忽略了一条"
    // 是很正常的一串动作，勾选掉了的话他得从头再勾一遍。★ 留下来的那些还要**再筛一遍**
    // ——重喂之后那一条可能已经不是"能勾的那一档"了（刚被忽略掉的那条就是）。
    QSet<QString> keep;
    for (const Row &row : m_report.rows) {
        if (row.selectable() && m_checked.contains(row.path)) {
            keep.insert(row.path);
        }
    }
    m_checked = keep;
    rebuild();
}

void UpdateCenterPage::refreshSelectionUi() {
    const int count = checkedPaths().size();
    m_updateSelectedButton->setText(MainWindowUI::updateCenterUpdateSelectedText(count));
    // 一条都没勾就点不动（它能干的事就是"把勾中的那几条换掉"，没勾中的时候那颗按钮没有
    // 任何含义）。★ 这正是"按钮不靠状态猜"那条规矩：能不能点，就是"有没有勾中"。
    m_updateSelectedButton->setEnabled(count > 0);
}

void UpdateCenterPage::rebuild() {
    m_summaryLabel->setText(tr("「%1」%2").arg(m_report.instanceName, m_report.summary));

    // ---------- 「没有结论」那一块 ----------
    const bool hasError = !m_report.error.isEmpty();
    m_errorLabel->setVisible(hasError);
    if (hasError) {
        QPalette palette = m_errorLabel->palette();
        m_errorLabel->setText(MainWindowUI::updateCenterEmptyTitleText() + QStringLiteral("\n")
                              + m_report.error + QStringLiteral("\n\n")
                              + MainWindowUI::updateCenterEmptyHintText());
        palette.setColor(QPalette::WindowText, MainWindowUI::folderDangerColor(palette));
        m_errorLabel->setPalette(palette);
    }

    // ---------- 反查那一趟的话 ----------
    m_noteLabel->setText(m_report.note);
    m_noteLabel->setVisible(!m_report.note.isEmpty());

    // ---------- 两组 ----------
    auto *body = qobject_cast<QVBoxLayout *>(m_bodyParent->layout());
    if (body != nullptr) {
        // 除了末尾那条弹簧，其余整块拆掉重画。
        while (body->count() > 1) {
            QLayoutItem *child = body->takeAt(0);
            if (QWidget *w = child->widget()) {
                // **先把这一格摘出这一页、再排删除**（不是光一句 `deleteLater()`）。两件事
                // 一起办，两件都是真的：
                // · 认人：组头和行上的 objectName 是**按组号/行号现拼的**
                // （`updateCenterGroupName()` / `updateCenterRowName()`）⇒ 新旧两批
                // **同名**，而 `deleteLater()` 要等下一趟事件循环才真的删。旧那一批在
                // 新的一批画出来之后**还挂在这一页的孩子树里** ⇒ 谁 `findChild` 谁就
                // 可能先撞上旧的那一批：名单两行全对，组头却读成「0 条」（那是构造里
                // `setReport(Report())` 那一趟留下的，它一个行都没有 —— 所以只有行能
                // 读到新的那一批）。
                // · 视觉：旧的那一批这时候几何还没变、也还可见 ⇒ 重画那一瞬间新旧叠在
                // 一处（`InstanceFilesPage` 那个重建里为这个专门写过一句「先藏再排删除」）。
                // `setParent(nullptr)` 把这两件一并办了：控件**立刻**从这一页的孩子树里
                // 消失，而 Qt 换爹的时候本来就会把它**自动藏起来**（换到 nullptr = 变成
                // 顶层窗口，一样是藏着的）。
                // 这里**不能**学 `IntegrityReportPage` / `TaskDetailPage` 那样当场
                // `delete`：这一趟重建可能是被**这一页自己的按钮**点出来的（忽略 / 固定
                // 那两颗 → `setReport()`），当场删就是把正在发信号的那颗按钮删在信号里。
                w->setParent(nullptr);
                w->deleteLater();
            }
            delete child;
        }
        m_checks.clear();

        QList<Row> outdated;
        QList<Row> rest;
        for (const Row &row : m_report.rows) {
            if (row.state == State::Outdated) {
                outdated.append(row);
            } else {
                rest.append(row);
            }
        }

        int rowIndex = 0;
        int groupIndex = 0;
        const auto addGroup = [&](const QString &title, const QList<Row> &rows,
                                  bool chips) {
            auto *titleLabel = new QLabel(m_bodyParent);
            titleLabel->setObjectName(MainWindowUI::updateCenterGroupName(groupIndex));
            {
                QFont font = titleLabel->font();
                font.setBold(true);
                titleLabel->setFont(font);
            }
            titleLabel->setText(title);
            body->addWidget(titleLabel);

            auto *groupBody = new QWidget(m_bodyParent);
            groupBody->setObjectName(MainWindowUI::updateCenterGroupBodyName(groupIndex));
            auto *rowsLayout = new QVBoxLayout(groupBody);
            rowsLayout->setContentsMargins(12, 0, 0, 0);  // 缩进，看得出是这一组里的
            rowsLayout->setSpacing(8);
            for (const Row &row : rows) {
                const int index = rowIndex++;
                auto *line = new QWidget(groupBody);
                line->setObjectName(MainWindowUI::updateCenterRowName(index));
                auto *lineLayout = new QHBoxLayout(line);
                lineLayout->setContentsMargins(0, 0, 0, 0);
                lineLayout->setSpacing(8);

                // 勾选框：**只有"能更新的那几条"有**（已是最新 / 说不清 / 已忽略的那些勾了
                // 也没有下一件事可做）。不能勾的那几行**占着同一格宽度**（隐藏但保留尺寸）——
                // 不保留的话两组左边参差不齐。
                auto *check = new QCheckBox(line);
                check->setObjectName(MainWindowUI::updateCenterRowCheckName(index));
                QSizePolicy checkPolicy = check->sizePolicy();
                checkPolicy.setRetainSizeWhenHidden(true);
                check->setSizePolicy(checkPolicy);
                const bool selectable = row.selectable();
                check->setVisible(selectable);
                check->setEnabled(selectable);
                check->setChecked(selectable && m_checked.contains(row.path));
                if (selectable) {
                    const QString path = row.path;
                    connect(check, &QCheckBox::toggled, this, [this, path](bool on) {
                        if (on) {
                            m_checked.insert(path);
                        } else {
                            m_checked.remove(path);
                        }
                        refreshSelectionUi();
                    });
                }
                lineLayout->addWidget(check);
                m_checks.append({row.path, check});

                // 名字 + 文件名（两行）
                auto *textColumn = new QVBoxLayout;
                textColumn->setContentsMargins(0, 0, 0, 0);
                textColumn->setSpacing(2);
                auto *nameLabel = new QLabel(line);
                nameLabel->setObjectName(MainWindowUI::updateCenterRowNameLabelName(index));
                nameLabel->setText(MainWindowUI::updateCenterRowLabelText(row.name, row.fileName));
                textColumn->addWidget(nameLabel);
                auto *fileLabel = makeMutedLabel(line,
                                                 MainWindowUI::updateCenterRowFileNameLabelName(index));
                // 名字取到了才另起一行写文件名；取不到时上面那行写的**就是**文件名。
                fileLabel->setText(row.name.isEmpty() ? QString() : row.fileName);
                fileLabel->setVisible(!row.name.isEmpty() && !row.fileName.isEmpty());
                textColumn->addWidget(fileLabel);
                lineLayout->addLayout(textColumn, 1);

                // 状态那一格
                auto *stateLabel = new QLabel(line);
                stateLabel->setObjectName(MainWindowUI::updateCenterRowStateName(index));
                // 「可更新」那一档写的是**要换成的那一版**；「已是最新」那一档写的是**现在
                // 这一版**（两档各看各的那一份版本号，不是同一个字段）。
                stateLabel->setText(MainWindowUI::updateCenterRowStateText(
                    int(row.state), int(row.suppress),
                    row.state == State::Outdated ? row.targetText : row.currentText));
                // 「说不清」那一档的缘由放在鼠标提示里（那一格只有四个字，说不下）。
                stateLabel->setToolTip(row.note);
                // 「可更新」那一档是**要用户看的**（他点这一下就是来看它的），用正常墨色；
                // 另外两档只是说明，压暗。**不给那一档上色** —— 上色是"这一条有事"的信号，
                // 可用在那三颗 chip 上了，再给状态文字上一个色，一页上会有两种"红"在抢眼球。
                if (row.state != State::Outdated) {
                    QPalette palette = stateLabel->palette();
                    QColor muted = palette.color(QPalette::WindowText);
                    muted.setAlpha(150);
                    palette.setColor(QPalette::WindowText, muted);
                    stateLabel->setPalette(palette);
                }
                lineLayout->addWidget(stateLabel);

                if (chips) {
                    // ①「更新」：就这一条（"逐项"那一档）。
                    auto *one = makeRowChip(line, MainWindowUI::updateCenterRowUpdateText(),
                                            MainWindowUI::updateCenterRowUpdateName(index),
                                            MainWindowUI::updateCenterRowUpdateTipText());
                    const QString path = row.path;
                    connect(one, &QPushButton::clicked, this,
                            [this, path] { emit updateRequested(QStringList{path}); });
                    lineLayout->addWidget(one);

                    // ②「忽略这一版」/「不再忽略」
                    const bool ignored = row.suppress == Suppress::Ignored;
                    auto *ignore = makeRowChip(
                        line,
                        ignored ? MainWindowUI::updateCenterRowIgnoreUndoText()
                                : MainWindowUI::updateCenterRowIgnoreText(),
                        MainWindowUI::updateCenterRowIgnoreName(index),
                        MainWindowUI::updateCenterRowIgnoreTipText());
                    connect(ignore, &QPushButton::clicked, this, [this, path, ignored] {
                        emit ignoreToggled(path, !ignored);
                    });
                    lineLayout->addWidget(ignore);

                    // ③「固定这一版」/「不再固定」
                    const bool pinned = row.suppress == Suppress::Pinned;
                    auto *pin = makeRowChip(line,
                                            pinned ? MainWindowUI::updateCenterRowPinUndoText()
                                                   : MainWindowUI::updateCenterRowPinText(),
                                            MainWindowUI::updateCenterRowPinName(index),
                                            MainWindowUI::updateCenterRowPinTipText());
                    connect(pin, &QPushButton::clicked, this, [this, path, pinned] {
                        emit pinToggled(path, !pinned);
                    });
                    lineLayout->addWidget(pin);
                }

                rowsLayout->addWidget(line);
            }
            body->addWidget(groupBody);
            ++groupIndex;
        };

        addGroup(MainWindowUI::updateCenterOutdatedGroupTitleText(outdated.size()), outdated,
                 /*chips=*/true);
        addGroup(MainWindowUI::updateCenterRestGroupTitleText(rest.size()), rest,
                 /*chips=*/false);
    }

    // ---------- 底下那两颗 ----------
    m_undoButton->setText(MainWindowUI::updateCenterUndoText(m_report.undoFileCount));
    m_undoButton->setVisible(m_report.undoFileCount > 0);
    refreshSelectionUi();
}
