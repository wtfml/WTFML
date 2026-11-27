#include "Support.hpp"

#include "UpgradeHistoryDialog.hpp"
#include "operations/UpgradeHistory.hpp"

// UG8：更新记录弹窗 —— 空态、行文本、选中后的细节。
int UiUpgradeHistoryChecks(QApplication &app) {
    Q_UNUSED(app);
    UpgradeHistoryDialog dialog;

    auto *empty = dialog.findChild<QLabel *>(UpgradeHistoryDialog::emptyName());
    auto *tree = dialog.findChild<QTreeWidget *>(UpgradeHistoryDialog::treeName());
    check(empty != nullptr && !empty->isHidden() && tree != nullptr && tree->isHidden(),
          "UG8 没有记录时只摆一句「还没有升级记录」");

    UpgradeHistory::Entry entry;
    entry.transactionId = QStringLiteral("t-9");
    entry.sourceId = QStringLiteral("src");
    entry.sourceName = QStringLiteral("原实例");
    entry.targetId = QStringLiteral("dst");
    entry.targetName = QStringLiteral("新实例");
    entry.fromGameVersion = QStringLiteral("1.20.1");
    entry.toGameVersion = QStringLiteral("1.21.4");
    entry.finishedAt = QStringLiteral("2026-11-05T08:01:00Z");
    entry.ok = true;
    entry.categories = {QStringLiteral("saves"), QStringLiteral("config")};
    entry.copiedFiles = 12;
    entry.skippedFiles = 3;
    dialog.setEntries({entry});

    check(tree != nullptr && tree->topLevelItemCount() == 1, "UG8 有记录时列表里一行");
    auto *item = tree != nullptr ? tree->topLevelItem(0) : nullptr;
    check(item != nullptr && item->text(1) == QStringLiteral("1.20.1 → 1.21.4"),
          "UG8 版本那一列是「旧 → 新」", item != nullptr ? item->text(1) : QString());
    check(item != nullptr && item->text(2) == QStringLiteral("成功"),
          "UG8 结果那一列说成功", item != nullptr ? item->text(2) : QString());
    check(empty != nullptr && empty->isHidden(), "UG8 有记录时那句空态收起来");

    auto *detail = dialog.findChild<QLabel *>(UpgradeHistoryDialog::detailName());
    check(detail != nullptr && detail->text().contains(QStringLiteral("快照：无")),
          "UG8 没有快照的记录明说「没有」（不冒充可恢复）",
          detail != nullptr ? detail->text() : QString());
    check(detail != nullptr && detail->text().contains(QStringLiteral("t-9"))
              && detail->text().contains(QStringLiteral("源实例"))
              && detail->text().contains(QStringLiteral("复制 12")),
          "UG8 选中之后下面显示事务号、来源与复制计数",
          detail != nullptr ? detail->text() : QString());

    // UG9：只有存在"可恢复快照"的记录时，回滚按钮才亮。
    auto *rollback = dialog.findChild<QPushButton *>(UpgradeHistoryDialog::rollbackName());
    check(rollback != nullptr && !rollback->isEnabled(),
          "UG9 没有可恢复快照时「回滚上次原地升级…」不可点");
    UpgradeHistory::Entry inPlace = entry;
    inPlace.transactionId = QStringLiteral("t-inplace");
    inPlace.snapshotName = QStringLiteral("instance-20261105.zip");
    inPlace.rollbackAvailable = true;
    dialog.setEntries({inPlace, entry});
    check(rollback != nullptr && rollback->isEnabled(),
          "UG9 有可恢复快照时按钮亮起来");
    bool asked = false;
    QObject::connect(&dialog, &UpgradeHistoryDialog::rollbackRequested, [&asked] { asked = true; });
    if (rollback != nullptr) {
        rollback->click();
    }
    check(asked, "UG9 点下去发出回滚请求");

    return g_fail == 0 ? 0 : 1;
}



