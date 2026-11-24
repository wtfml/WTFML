/**
 * @file UpgradeHistoryDialog.cpp
 * @brief 见头文件：只展示 `UpgradeHistory::Entry`，不改任何文件。
 */

#include "UpgradeHistoryDialog.hpp"
#include <QCoreApplication>

#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "Theme.hpp"

QString UpgradeHistoryDialog::treeName() { return QStringLiteral("upgradeHistoryTree"); }
QString UpgradeHistoryDialog::rollbackName() { return QStringLiteral("upgradeHistoryRollback"); }
QString UpgradeHistoryDialog::detailName() { return QStringLiteral("upgradeHistoryDetail"); }
QString UpgradeHistoryDialog::emptyName() { return QStringLiteral("upgradeHistoryEmpty"); }

UpgradeHistoryDialog::UpgradeHistoryDialog(QWidget *parent) : ChromeDialog(parent, true) {
    setObjectName(QStringLiteral("upgradeHistoryDialog"));
    setWindowTitle(QCoreApplication::translate("UpgradeHistoryDialog", "更新记录"));
    resize(760, 520);
    auto *root = new QVBoxLayout(body());
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(treeName());
    m_tree->setHeaderLabels({QCoreApplication::translate("UpgradeHistoryDialog", "时间"), QCoreApplication::translate("UpgradeHistoryDialog", "版本"),
                             QCoreApplication::translate("UpgradeHistoryDialog", "结果")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    root->addWidget(m_tree, 2);

    m_empty = new QLabel(QCoreApplication::translate("UpgradeHistoryDialog", "此实例暂无升级记录。"), this);
    m_empty->setObjectName(emptyName());
    root->addWidget(m_empty);

    m_detail = new QLabel(this);
    m_detail->setObjectName(detailName());
    m_detail->setWordWrap(true);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_detail, 1);

    m_rollback = new QPushButton(QCoreApplication::translate("UpgradeHistoryDialog", "回滚上次原地升级…"), this);
    m_rollback->setObjectName(rollbackName());
    Theme::markButton(m_rollback, Theme::ButtonKind::Neutral);
    m_rollback->setEnabled(false);
    root->addWidget(m_rollback, 0, Qt::AlignLeft);
    connect(m_rollback, &QPushButton::clicked, this, &UpgradeHistoryDialog::rollbackRequested);

    auto *close = new QPushButton(QCoreApplication::translate("UpgradeHistoryDialog", "关闭"), this);
    Theme::markButton(close, Theme::ButtonKind::Neutral);
    root->addWidget(close, 0, Qt::AlignRight);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
        if (item == nullptr) {
            m_detail->clear();
            return;
        }
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0 || index >= m_entries.size()) {
            m_detail->clear();
            return;
        }
        const UpgradeHistory::Entry &entry = m_entries.at(index);
        QString text = QCoreApplication::translate("UpgradeHistoryDialog", "事务：%1\n").arg(entry.transactionId);
        text += QCoreApplication::translate("UpgradeHistoryDialog", "来源：%1（%2）\n").arg(entry.sourceName.isEmpty()
                                                           ? entry.sourceId
                                                           : entry.sourceName,
                                                       entry.sourceId);
        text += QCoreApplication::translate("UpgradeHistoryDialog", "目标：%1（%2）\n").arg(entry.targetName.isEmpty()
                                                           ? entry.targetId
                                                           : entry.targetName,
                                                       entry.targetId);
        text += QCoreApplication::translate("UpgradeHistoryDialog", "类别：%1\n").arg(entry.categories.isEmpty()
                                                    ? QCoreApplication::translate("UpgradeHistoryDialog", "（原地升级）")
                                                    : entry.categories.join(QCoreApplication::translate("UpgradeHistoryDialog", "、")));
        text += QCoreApplication::translate("UpgradeHistoryDialog", "文件：复制 %1 / 跳过 %2 / 失败 %3\n")
                    .arg(entry.copiedFiles)
                    .arg(entry.skippedFiles)
                    .arg(entry.failedFiles);
        if (entry.snapshotName.isEmpty()) {
            text += QCoreApplication::translate("UpgradeHistoryDialog", "快照：无（副本模式以源实例为回滚材料）\n");
        } else {
            text += QCoreApplication::translate("UpgradeHistoryDialog", "快照：%1%2\n")
                        .arg(entry.snapshotName,
                             entry.rollbackAvailable ? QCoreApplication::translate("UpgradeHistoryDialog", "（可恢复）") : QString());
        }
        if (!entry.problem.isEmpty()) {
            text += QCoreApplication::translate("UpgradeHistoryDialog", "说明：%1").arg(entry.problem);
        }
        m_detail->setText(text.trimmed());
    });

    rebuild();
}

void UpgradeHistoryDialog::setEntries(QList<UpgradeHistory::Entry> entries) {
    m_entries = std::move(entries);
    rebuild();
}

void UpgradeHistoryDialog::rebuild() {
    m_tree->clear();
    const bool empty = m_entries.isEmpty();
    m_empty->setVisible(empty);
    m_tree->setVisible(!empty);
    m_detail->setVisible(!empty);
    for (int i = 0; i < m_entries.size(); ++i) {
        const UpgradeHistory::Entry &entry = m_entries.at(i);
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, entry.finishedAt.isEmpty() ? entry.startedAt : entry.finishedAt);
        const QString from = entry.fromGameVersion.isEmpty() ? QStringLiteral("?")
                                                             : entry.fromGameVersion;
        const QString to = entry.toGameVersion.isEmpty() ? QStringLiteral("?")
                                                         : entry.toGameVersion;
        item->setText(1, QStringLiteral("%1 → %2").arg(from, to));
        QString result = entry.ok ? QCoreApplication::translate("UpgradeHistoryDialog", "成功")
                         : entry.cancelled ? QCoreApplication::translate("UpgradeHistoryDialog", "已取消")
                                           : QCoreApplication::translate("UpgradeHistoryDialog", "失败");
        if (!entry.ok && !entry.problem.isEmpty()) {
            result += QCoreApplication::translate("UpgradeHistoryDialog", "：%1").arg(entry.problem);
        }
        item->setText(2, result);
        item->setData(0, Qt::UserRole, i);
    }
    bool anyRollback = false;
    for (const UpgradeHistory::Entry &entry : m_entries) {
        anyRollback = anyRollback || entry.rollbackAvailable;
    }
    if (m_rollback != nullptr) {
        m_rollback->setEnabled(anyRollback);
    }
    if (!m_entries.isEmpty()) {
        m_tree->setCurrentItem(m_tree->topLevelItem(0));
    }
}


