/**
 * @file UpgradeReportDialog.cpp
 * @brief 见头文件：只摆控件、只转发；分组与筛选一律走 `operations/UpgradeReport`。
 */

#include "UpgradeReportDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <utility>

#include "Theme.hpp"

QString UpgradeReportDialog::searchName() { return QStringLiteral("upgradeReportSearch"); }
QString UpgradeReportDialog::bucketFilterName() { return QStringLiteral("upgradeReportBucket"); }
QString UpgradeReportDialog::showCompatibleName() {
    return QStringLiteral("upgradeReportShowCompatible");
}
QString UpgradeReportDialog::treeName() { return QStringLiteral("upgradeReportTree"); }
QString UpgradeReportDialog::summaryName() { return QStringLiteral("upgradeReportSummary"); }

UpgradeReportDialog::UpgradeReportDialog(QWidget *parent) : ChromeDialog(parent, true) {
    setObjectName(QStringLiteral("upgradeReportDialog"));
    setWindowTitle(tr("兼容性报告"));
    resize(720, 520);
    auto *root = new QVBoxLayout(body());
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    auto *tools = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setObjectName(searchName());
    m_search->setPlaceholderText(tr("搜索模组名或依据"));
    m_search->setClearButtonEnabled(true);
    m_bucket = new QComboBox(this);
    m_bucket->setObjectName(bucketFilterName());
    m_bucket->addItem(tr("全部"), -1);
    for (const UpgradeReport::Bucket bucket : UpgradeReport::buckets()) {
        m_bucket->addItem(UpgradeReport::bucketTitle(bucket), int(bucket));
    }
    m_showCompatible = new QCheckBox(tr("显示正常兼容项"), this);
    m_showCompatible->setObjectName(showCompatibleName());
    m_showCompatible->setChecked(false);
    tools->addWidget(m_search, 1);
    tools->addWidget(m_bucket);
    tools->addWidget(m_showCompatible);
    root->addLayout(tools);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(treeName());
    m_tree->setHeaderLabels({tr("项目"), tr("依据")});
    m_tree->setRootIsDecorated(true);
    m_tree->setFrameShape(QFrame::NoFrame);  // 不描框：列表那块淡底自己就是边界
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    root->addWidget(m_tree, 1);

    m_summary = new QLabel(this);
    m_summary->setObjectName(summaryName());
    m_summary->setWordWrap(true);
    root->addWidget(m_summary);

    auto *close = new QPushButton(tr("关闭"), this);
    Theme::markButton(close, Theme::ButtonKind::Neutral);
    root->addWidget(close, 0, Qt::AlignRight);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &) { refresh(); });
    connect(m_bucket, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { refresh(); });
    connect(m_showCompatible, &QCheckBox::toggled, this, [this](bool) { refresh(); });

    refresh();
}

void UpgradeReportDialog::setRows(QList<UpgradeReport::Row> rows) {
    m_rows = std::move(rows);
    refresh();
}

void UpgradeReportDialog::refresh() {
    const int only = m_bucket->currentData().toInt();
    const bool hasOnly = only >= 0;
    const UpgradeReport::Bucket bucket = hasOnly ? UpgradeReport::Bucket(only)
                                                  : UpgradeReport::Bucket::Compatible;
    const QList<UpgradeReport::Row> visible =
        UpgradeReport::filtered(m_rows, m_search->text(), hasOnly, bucket,
                                m_showCompatible->isChecked());
    const QHash<UpgradeReport::Bucket, int> counts = UpgradeReport::counts(m_rows);

    m_tree->clear();
    for (const UpgradeReport::Bucket group : UpgradeReport::buckets()) {
        if (hasOnly && group != bucket) {
            continue;
        }
        if (!m_showCompatible->isChecked() && group == UpgradeReport::Bucket::Compatible) {
            continue;
        }
        auto *header = new QTreeWidgetItem(m_tree);
        header->setText(0, QStringLiteral("%1 (%2)")
                              .arg(UpgradeReport::bucketTitle(group))
                              .arg(counts.value(group)));
        QFont font = header->font(0);
        font.setBold(true);
        header->setFont(0, font);
        header->setFirstColumnSpanned(true);
        header->setExpanded(!UpgradeReport::foldedByDefault(group));
        for (const UpgradeReport::Row &row : visible) {
            if (row.bucket != group) {
                continue;
            }
            auto *item = new QTreeWidgetItem(header);
            item->setText(0, row.name);
            item->setText(1, row.detail);
        }
    }

    const int unresolved = UpgradeReport::unresolvedRequired(m_rows);
    if (unresolved > 0) {
        m_summary->setText(tr("尚有 %1 项必要内容需处理。").arg(unresolved));
    } else {
        m_summary->setText(tr("必要内容均已明确处理。"));
    }
}

