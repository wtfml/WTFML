#include "Support.hpp"

#include "InstanceUpgradePage.hpp"
#include "operations/UpgradePageState.hpp"
#include "operations/UpgradeSelection.hpp"

// UG：迁移式升级页的骨架（只画状态、只转发动作）。
//
// 判断在 `operations/UpgradePageState` / `UpgradeSelection` 里已经由 `verify_instances`
// 的 US 段钉过；这里只验"页面把这些状态画对了没有、把用户动作原样发出去没有"。
int UiUpgradePageChecks(QApplication &app) {
    Q_UNUSED(app);
    using InstanceUpgradePage = ::InstanceUpgradePage;

    InstanceUpgradePage page;
    page.setVersionOptions({QStringLiteral("1.21.4"), QStringLiteral("1.20.1")});
    page.setLoaderOptions({{QString(), QStringLiteral("原版")},
                           {QStringLiteral("fabric"), QStringLiteral("Fabric")}},
                          {QStringLiteral("0.15.0"), QStringLiteral("0.16.0")});

    int toggles = 0;
    QString lastKey;
    QObject::connect(&page, &InstanceUpgradePage::categoryToggled,
                     [&toggles, &lastKey](const QString &key, bool) {
                         ++toggles;
                         lastKey = key;
                     });

    UpgradePageState::State state;
    state.sourceId = QStringLiteral("源实例");
    state.sourceGameVersion = QStringLiteral("1.20.1");
    state.sourceLoaderKey = QStringLiteral("fabric");
    state.sourceLoaderVersion = QStringLiteral("0.15.0");
    page.setState(state);

    const QList<UpgradeSelection::Category> categories = UpgradeSelection::categories();
    int found = 0;
    QStringList checkedKeys;
    for (const UpgradeSelection::Category &category : categories) {
        auto *box = page.findChild<QAbstractButton *>(InstanceUpgradePage::categoryCheckName(category.key));
        if (box == nullptr) {
            continue;
        }
        ++found;
        if (box->isChecked()) {
            checkedKeys.append(category.key);
        }
    }
    check(found == 9, "UG1 九类都摆出来了", QString::number(found));
    check(!checkedKeys.contains(QStringLiteral("screenshots"))
              && !checkedKeys.contains(QStringLiteral("schematics")),
          "UG1 截图 / 原理图默认不勾", checkedKeys.join(QLatin1Char(',')));

    auto *blocker = page.findChild<QLabel *>(InstanceUpgradePage::blockerLabelName());
    check(blocker != nullptr && !blocker->text().isEmpty(),
          "UG2 名字还空着时底部说得出原因（不是一颗灰按钮没有解释）",
          blocker != nullptr ? blocker->text() : QString());
    auto *preview = page.findChild<QPushButton *>(InstanceUpgradePage::previewButtonName());
    check(preview != nullptr && !preview->isEnabled(), "UG2 有阻断时「预览并确认」不可点");

    state.target.gameVersion = QStringLiteral("1.21.4");
    state.target.instanceName = QStringLiteral("新实例");
    state.selection.state.set(QStringLiteral("saves"), true);
    UpgradePageState::markReportChecked(state);
    page.setState(state);
    check(preview != nullptr && preview->isEnabled(),
          "UG3 目标 + 名字 + 选择 + 报告都齐了之后可以点确认",
          blocker != nullptr ? blocker->text() : QString());

    auto *summary = page.findChild<QLabel *>(InstanceUpgradePage::summaryLabelName());
    check(summary != nullptr && summary->text().contains(QStringLiteral("已选 1 类")),
          "UG3 底部摘要显示已选类数", summary != nullptr ? summary->text() : QString());

    auto *modBox = page.findChild<QAbstractButton *>(
        InstanceUpgradePage::categoryCheckName(QStringLiteral("mods")));
    check(modBox != nullptr, "UG4 找得到 Mod 文件那一格");
    if (modBox != nullptr) {
        modBox->setChecked(true);
    }
    check(toggles == 1 && lastKey == QStringLiteral("mods"),
          "UG4 勾选把类别键原样发出去（页面自己不改状态）",
          QStringLiteral("%1 次 / %2").arg(toggles).arg(lastKey));

    page.setAmounts(12, 4 * 1024 * 1024);
    check(summary != nullptr && summary->text().contains(QStringLiteral("12 个文件"))
              && summary->text().contains(QStringLiteral("4.0 MB")),
          "UG5 扫描完的合计（文件数 + 体积）进摘要", summary != nullptr ? summary->text() : QString());

    // UG6：每一类的文件数/字节数、以及"没有"这一档。
    page.setCategoryAmounts({{QStringLiteral("saves"), {3, 2048}},
                             {QStringLiteral("mods"), {0, 0}}});
    auto *savesNote = page.findChild<QLabel *>(
        InstanceUpgradePage::categoryNoteName(QStringLiteral("saves")));
    check(savesNote != nullptr && savesNote->text().contains(QStringLiteral("3 个"))
              && savesNote->text().contains(QStringLiteral("2.0 KB")),
          "UG6 存档那一行显示「3 个 · 2.0 KB」", savesNote != nullptr ? savesNote->text() : QString());
    auto *modsNote = page.findChild<QLabel *>(
        InstanceUpgradePage::categoryNoteName(QStringLiteral("mods")));
    check(modsNote != nullptr && modsNote->text().contains(QStringLiteral("无")),
          "UG6 空的那一类明说「无」", modsNote != nullptr ? modsNote->text() : QString());
    check(summary != nullptr && summary->text().contains(QStringLiteral("3 个文件")),
          "UG6 摘要按选中的类别合计（只选了存档 ⇒ 3 个文件）",
          summary != nullptr ? summary->text() : QString());
    check(blocker != nullptr, "UG6 状态没被这一轮数字打乱");

    // UG10：改目标之后，页面必须回到"需要重新检查"（不能拿旧报告确认）。
    UpgradePageState::State stale = state;
    page.setState(stale);
    check(preview != nullptr && preview->isEnabled(), "UG10 先摆一份可确认的状态");
    stale.target.gameVersion = QStringLiteral("1.21.5");
    page.setState(stale);
    check(preview != nullptr && !preview->isEnabled(),
          "★★ UG10 目标一改，确认按钮立刻不可点");
    check(blocker != nullptr && blocker->text().contains(QStringLiteral("重新检查")),
          "UG10 而且说得出要重新检查", blocker != nullptr ? blocker->text() : QString());

    // UG11：某一类里有链接时，那一行明说"不会跟随"（复制链路遇到链接会拒绝）。
    page.setCategoryLinks({{QStringLiteral("saves"), 2}});
    check(savesNote != nullptr
              && savesNote->text().contains(QStringLiteral("含 2 个链接"))
              && savesNote->text().contains(QStringLiteral("不会跟随")),
          "UG11 有链接的那一类在行里说明", savesNote != nullptr ? savesNote->text() : QString());
    page.setCategoryLinks({{QStringLiteral("saves"), 0}});
    check(savesNote != nullptr && !savesNote->text().contains(QStringLiteral("链接")),
          "UG11 清零之后那段说明消失", savesNote != nullptr ? savesNote->text() : QString());

    return g_fail == 0 ? 0 : 1;
}




