#include "Support.hpp"

#include "UpgradeReportDialog.hpp"
#include "operations/UpgradeReport.hpp"

// UG7：兼容性报告弹窗 —— 分组、默认折叠、搜索、必要项计数（判断在 UpgradeReport）。
int UiUpgradeReportChecks(QApplication &app) {
    Q_UNUSED(app);
    using UpgradeReport::Bucket;
    using UpgradeReport::Row;

    UpgradeReportDialog dialog;
    QList<Row> rows;
    rows.append({QStringLiteral("Fabric API"), QStringLiteral("与目标发行版一致"),
                 Bucket::Compatible, true});
    rows.append({QStringLiteral("Sodium"), QStringLiteral("目标版本没有发行版"),
                 Bucket::Incompatible, true});
    rows.append({QStringLiteral("Iris"), QStringLiteral("查询超时"), Bucket::Failed, false});
    rows.append({QStringLiteral("Mystery"), QStringLiteral("认不出"), Bucket::Unknown, false});
    rows.append({QStringLiteral("Cloth Config"), QStringLiteral("缺必要前置"),
                 Bucket::MissingDependency, true});
    dialog.setRows(rows);

    auto *tree = dialog.findChild<QTreeWidget *>(UpgradeReportDialog::treeName());
    check(tree != nullptr, "UG7 报告列表在");
    check(tree != nullptr && tree->topLevelItemCount() == 4,
          "UG7 默认不画「正常兼容」那一组（折起来）",
          tree != nullptr ? QString::number(tree->topLevelItemCount()) : QString());
    auto *summary = dialog.findChild<QLabel *>(UpgradeReportDialog::summaryName());
    check(summary != nullptr && summary->text().contains(QStringLiteral("尚有 2 项")),
          "UG7 摘要说出必要项里还有几项没处理",
          summary != nullptr ? summary->text() : QString());

    auto *show = dialog.findChild<QCheckBox *>(UpgradeReportDialog::showCompatibleName());
    check(show != nullptr, "UG7 「显示正常兼容项」那个勾选框在");
    if (show != nullptr) {
        show->setChecked(true);
    }
    check(tree != nullptr && tree->topLevelItemCount() == 5, "UG7 勾上之后五组都在");
    auto *first = tree != nullptr ? tree->topLevelItem(0) : nullptr;
    check(first != nullptr && first->text(0).contains(QStringLiteral("正常兼容 (1)")),
          "UG7 第一组是正常兼容，标题带条数",
          first != nullptr ? first->text(0) : QString());

    auto *search = dialog.findChild<QLineEdit *>(UpgradeReportDialog::searchName());
    check(search != nullptr, "UG7 搜索框在");
    if (search != nullptr) {
        search->setText(QStringLiteral("sod"));
    }
    int rowsShown = 0;
    if (tree != nullptr) {
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            rowsShown += tree->topLevelItem(i)->childCount();
        }
    }
    check(rowsShown == 1, "UG7 搜索之后只剩 Sodium 那一条", QString::number(rowsShown));

    // ---- UG7b 自绘窄标题条（ChromeDialog）：系统标题栏收掉之后，标题 / 关闭得自己管 ----
    auto *strip = dialog.findChild<QWidget *>(ChromeDialog::titleStripName());
    auto *title = dialog.findChild<QLabel *>(ChromeDialog::titleLabelName());
    check(strip != nullptr && strip->height() == ChromeDialog::stripHeight() && strip->y() == 0,
          "UG7b 标题条贴在最上面、是窄的那一档",
          strip != nullptr ? QStringLiteral("y=%1 h=%2").arg(strip->y()).arg(strip->height()) : QString());
    check(title != nullptr && title->text() == dialog.windowTitle(), "UG7b 条上的字就是 windowTitle()",
          title != nullptr ? title->text() : QString());
    dialog.setWindowTitle(QStringLiteral("兼容性报告 · 测试"));
    check(title != nullptr && title->text() == QStringLiteral("兼容性报告 · 测试"), "UG7b 改了 windowTitle 条上跟着改");
    check(tree != nullptr && dialog.body()->isAncestorOf(tree), "UG7b 内容排在 body() 里（标题条下面）");
    auto *closeButton = dialog.findChild<QAbstractButton *>(ChromeDialog::closeButtonName());
    check(closeButton != nullptr, "UG7b 条上有关闭钮");
    if (closeButton != nullptr) {
        dialog.show();
        closeButton->click();
        check(!dialog.isVisible() && dialog.result() == QDialog::Rejected, "UG7b 点 × = reject()（和 Esc 一个意思）");
    }

    return g_fail == 0 ? 0 : 1;
}
