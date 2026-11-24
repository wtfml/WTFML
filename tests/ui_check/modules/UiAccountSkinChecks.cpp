#include "Support.hpp"
int UiAccountSkinChecks() {
    {
        AccountSkinDialog dialog;
        dialog.show();
        QApplication::processEvents();

        // 这个窗里所有控件都靠 objectName 找回来 —— 不去认文字（文字会随数据变：
        // 账号名后面会挂「（当前）」）。
        const auto child = [&dialog](const QString &name) {
            return dialog.findChild<QWidget *>(name);
        };
        QListWidget *accountList =
            qobject_cast<QListWidget *>(child(AccountSkinDialog::accountListName()));
        QListWidget *capeList =
            qobject_cast<QListWidget *>(child(AccountSkinDialog::capeListName()));
        QPushButton *loginBtn =
            qobject_cast<QPushButton *>(child(AccountSkinDialog::addAccountButtonName()));
        QPushButton *switchBtn =
            qobject_cast<QPushButton *>(child(AccountSkinDialog::switchButtonName()));
        QPushButton *removeBtn =
            qobject_cast<QPushButton *>(child(AccountSkinDialog::removeButtonName()));
        QPushButton *refreshBtn =
            qobject_cast<QPushButton *>(child(AccountSkinDialog::refreshButtonName()));
        QPushButton *uploadBtn =
            qobject_cast<QPushButton *>(child(AccountSkinDialog::uploadButtonName()));
        QPushButton *applyCapeBtn =
            qobject_cast<QPushButton *>(child(AccountSkinDialog::applyCapeButtonName()));
        QPushButton *closeBtn =
            qobject_cast<QPushButton *>(child(AccountSkinDialog::closeButtonName()));
        QLabel *statusLabel =
            qobject_cast<QLabel *>(child(AccountSkinDialog::statusLabelName()));
        QComboBox *variantCombo =
            qobject_cast<QComboBox *>(child(AccountSkinDialog::variantComboName()));
        QLineEdit *offlineEdit =
            qobject_cast<QLineEdit *>(child(AccountSkinDialog::offlineNameEditName()));
        QPushButton *addOfflineBtn =
            qobject_cast<QPushButton *>(child(AccountSkinDialog::addOfflineButtonName()));
        QPushButton *pickOfflineBtn = qobject_cast<QPushButton *>(
            child(AccountSkinDialog::offlineSkinButtonName()));
        QPushButton *resetOfflineBtn = qobject_cast<QPushButton *>(
            child(AccountSkinDialog::offlineSkinResetButtonName()));
        SkinViewer *viewer = qobject_cast<SkinViewer *>(child(AccountSkinDialog::viewerName()));

        const bool allThere = accountList != nullptr && capeList != nullptr &&
                              loginBtn != nullptr && switchBtn != nullptr &&
                              removeBtn != nullptr && refreshBtn != nullptr &&
                              uploadBtn != nullptr && applyCapeBtn != nullptr &&
                              closeBtn != nullptr &&
                              statusLabel != nullptr && variantCombo != nullptr &&
                              viewer != nullptr && offlineEdit != nullptr &&
                              addOfflineBtn != nullptr && pickOfflineBtn != nullptr &&
                              resetOfflineBtn != nullptr;
        check(allThere, "十七块控件都按 objectName 找得到");
        // NOTE: 下面整段都挂在这个 if 里 —— 少一块控件时直接解引用会段错误，
        // 那样后半套断言的结果就全看不到了。
        if (allThere) {
            // ---- X1 文案与初始状态 ----
            check(dialog.windowTitle() == AccountSkinDialog::windowTitle(),
                  "窗口标题是产品那句", dialog.windowTitle());
            check(loginBtn->text() == AccountSkinDialog::addAccountButtonText() &&
                      switchBtn->text() == AccountSkinDialog::switchButtonText() &&
                      removeBtn->toolTip() == AccountSkinDialog::removeButtonText() &&
                      removeBtn->text().isEmpty() && !removeBtn->icon().isNull() &&
                      refreshBtn->text() == AccountSkinDialog::refreshButtonText(),
                  "左边四个按钮的文字都是产品文案");
            check(offlineEdit->placeholderText() == AccountSkinDialog::offlineNamePlaceholder() &&
                      offlineEdit->text().isEmpty(),
                  "离线名字那个框一开始是空的，占位文字说明这里填什么",
                  offlineEdit->placeholderText());
            check(addOfflineBtn->toolTip() == AccountSkinDialog::addOfflineButtonText()
                      && !addOfflineBtn->isVisibleTo(&dialog) && !offlineEdit->isVisibleTo(&dialog),
                  "默认不展开离线那一行，按钮说明仍是产品文案",
                  addOfflineBtn->toolTip());
            {
                check(child(QStringLiteral("accountKindSegment")) == nullptr,
                      "账号列表上方没有「正版 / 离线」滑块");
                auto *menu = dialog.findChild<QMenu *>(AccountSkinDialog::addAccountMenuName());
                auto *microsoft = dialog.findChild<QAction *>(AccountSkinDialog::addMicrosoftActionName());
                auto *offlineAction = dialog.findChild<QAction *>(AccountSkinDialog::addOfflineActionName());
                check(menu != nullptr && microsoft != nullptr && offlineAction != nullptr &&
                          microsoft->text() == AccountSkinDialog::microsoftAccountText() &&
                          offlineAction->text() == AccountSkinDialog::offlineAccountText() &&
                          microsoft->isEnabled() && menu->actions().size() == 2,
                      "「添加账号」菜单两项：微软账号 / 离线账号，微软账号默认可选");
                if (microsoft != nullptr && offlineAction != nullptr) {
                    int logins = 0;
                    const auto link = QObject::connect(&dialog, &AccountSkinDialog::loginRequested, &dialog,
                                                       [&logins] { ++logins; });
                    microsoft->trigger();
                    check(logins == 1 && !offlineEdit->isVisibleTo(&dialog),
                          "选「微软账号」：直接发 loginRequested，不展开名字框");
                    offlineAction->trigger();
                    check(dialog.offlineAddRowShown() && offlineEdit->isVisibleTo(&dialog) &&
                              addOfflineBtn->isVisibleTo(&dialog),
                          "选「离线账号」：就地展开名字框 +「添加」");
                    const QString unavailable = QStringLiteral("这个版本暂时不能登录微软账号。");
                    dialog.setOnlineLoginUnavailable(unavailable);
                    microsoft->trigger();
                    check(!microsoft->isEnabled() && microsoft->toolTip() == unavailable && logins == 1,
                          "微软登录用不了：「微软账号」置灰、悬停是那句说明，点不出登录");
                    dialog.setOnlineLoginUnavailable(QString());
                    check(microsoft->isEnabled(), "说明清空后「微软账号」回来");
                    QObject::disconnect(link);
                    dialog.clearOfflineNameDraft();
                }
            }
            check(statusLabel->text() == AccountSkinDialog::notLoggedInNote(),
                  "一进来状态行说的是「还没登录」那句", statusLabel->text());
            // 这个控件任何一档状态都不该是空白的（和主页右栏那个小人同一条约定）。
            check(viewer->skin() == SkinModel::defaultSteveSkin() && viewer->cape().isNull(),
                  "预览一进来就是默认外观，不是一片空白");
            check(variantCombo->count() == 2 &&
                      variantCombo->itemText(0) == AccountSkinDialog::variantClassicText() &&
                      variantCombo->itemText(1) == AccountSkinDialog::variantSlimText(),
                  "模型下拉框两项：经典 / 纤细");
            check(variantCombo->itemData(0).toString() ==
                          AccountSkinDialog::variantClassicValue() &&
                      variantCombo->itemData(1).toString() ==
                          AccountSkinDialog::variantSlimValue(),
                  "两项带的值是**小写**的 classic / slim（传给官方就是这两个）",
                  variantCombo->itemData(0).toString() + " / " +
                      variantCombo->itemData(1).toString());
            check(dialog.selectedVariant() == AccountSkinDialog::variantClassicValue(),
                  "默认是经典模型");
            check(!switchBtn->isEnabled() && !removeBtn->isEnabled() &&
                      !applyCapeBtn->isEnabled(),
                  "还没喂数据时，三个「对所选做事」的按钮都灰着");

            // ---- X1b 离线账号的「换一张…」/「恢复默认外观」 ----
            // 两组按钮**互斥**：正版账号走「上传皮肤」（那是 Mojang 上传），离线账号走本地
            // 挑图 —— 两个一起露会让用户以为离线也能上传。
            check(pickOfflineBtn->text() == AccountSkinDialog::offlineSkinButtonText() &&
                      resetOfflineBtn->text() ==
                          AccountSkinDialog::offlineSkinResetButtonText(),
                  "离线那两颗的文字是产品文案",
                  pickOfflineBtn->text() + " / " + resetOfflineBtn->text());
            check(pickOfflineBtn->toolTip() ==
                          AccountSkinDialog::offlineSkinButtonToolTip() &&
                      resetOfflineBtn->toolTip() ==
                          AccountSkinDialog::offlineSkinResetButtonToolTip(),
                  "两颗的 tooltip 都在（说清只影响启动器里的预览）");
            check(!uploadBtn->isHidden() && pickOfflineBtn->isHidden() &&
                      resetOfflineBtn->isHidden(),
                  "默认（正版 / 还没喂状态）：只露「上传皮肤」");
            dialog.setOfflineSkinState(true, /*hasCustomSkin=*/false);
            QApplication::processEvents();
            check(uploadBtn->isHidden() && !pickOfflineBtn->isHidden() &&
                      !resetOfflineBtn->isHidden(),
                  "离线账号：「上传皮肤」收起来，露出「换一张…」「恢复默认外观」");
            check(pickOfflineBtn->isEnabled() && !resetOfflineBtn->isEnabled(),
                  "还没手动挑过时，「恢复默认外观」灰着（没东西可恢复）");
            dialog.setOfflineSkinState(true, /*hasCustomSkin=*/true);
            QApplication::processEvents();
            check(resetOfflineBtn->isEnabled(),
                  "手动挑过之后，「恢复默认外观」可点");
            dialog.setOfflineSkinState(false, /*hasCustomSkin=*/false);
            QApplication::processEvents();
            check(!uploadBtn->isHidden() && pickOfflineBtn->isHidden() &&
                      resetOfflineBtn->isHidden(),
                  "切回正版账号：又只剩「上传皮肤」（两组不共存）");

            // ---- X2 一个账号都没有：列表整块收起，页签下面直接接入口；「未登录」只在页头说一次 ----
            dialog.setAccounts({});
            QApplication::processEvents();
            check(accountList->count() == 0 && !accountList->isVisibleTo(&dialog),
                  "空列表不占位（不摆占位行、不留一行高的空白）",
                  QString::number(accountList->count()));
            check(!statusLabel->text().isEmpty() && statusLabel->text() == AccountSkinDialog::notLoggedInNote(),
                  "「未登录」由页头副题说", statusLabel->text());
            check(dialog.selectedUuid().isEmpty(), "所以 selectedUuid() 是空串");
            check(!switchBtn->isEnabled() && !removeBtn->isEnabled(),
                  "没有账号可选的时候那两个按钮照样灰着");

            // ---- X3 有账号：选中「当前」那一条，名字后面挂标记 ----
            QList<AccountSkinDialog::AccountRow> rows;
            rows.append(AccountSkinDialog::AccountRow{QStringLiteral("uuid-a"),
                                                      QStringLiteral("Alice"),
                                                      QStringLiteral("2026-09-22T10:00:00Z"),
                                                      true});
            rows.append(AccountSkinDialog::AccountRow{QStringLiteral("uuid-b"),
                                                      QStringLiteral("Bob"), QString(), false});
            rows.append(AccountSkinDialog::AccountRow{QStringLiteral("uuid-c"),
                                                      QStringLiteral("Carol"), QString(), false});
            dialog.setAccounts(rows);
            QApplication::processEvents();
            check(accountList->count() == 3, "三条账号都摆出来了",
                  QString::number(accountList->count()));
            check(accountList->currentRow() == 0, "默认选中「当前」那一条（第 0 条）",
                  QString::number(accountList->currentRow()));
            check(accountList->item(0)->text() ==
                      QStringLiteral("Alice") + AccountSkinDialog::currentMark(),
                  "当前账号那一行挂着「（当前）」", accountList->item(0)->text());
            check(accountList->item(1)->text() == QStringLiteral("Bob"),
                  "不是当前的那些**不挂**标记", accountList->item(1)->text());
            check(dialog.selectedUuid() == QStringLiteral("uuid-a"),
                  "selectedUuid() 给的是选中那一条的 uuid", dialog.selectedUuid());
            check(switchBtn->isEnabled() && removeBtn->isEnabled(),
                  "有选中项了，那两个按钮跟着可点");

            // savedAt 落在 tooltip 上（列表里写不下"什么时候存的"）。
            check(!accountList->item(0)->toolTip().isEmpty() &&
                      accountList->item(0)->toolTip().contains(QStringLiteral("2026")),
                  "带时间的账号有 tooltip，里面有那个日期",
                  accountList->item(0)->toolTip());
            check(accountList->item(1)->toolTip().isEmpty(),
                  "没有时间的账号不摆一个空 tooltip");

            // ---- X4 没有「当前」账号时选第一条 ----
            // 凭据被清掉了（比如删了当前账号）但列表还在，这时不能一行都不选 ——
            // 一行都不选的话三个按钮全是灰的，看着像坏了。
            {
                QList<AccountSkinDialog::AccountRow> noneCurrent;
                noneCurrent.append(AccountSkinDialog::AccountRow{QStringLiteral("uuid-b"),
                                                                 QStringLiteral("Bob"),
                                                                 QString(), false});
                noneCurrent.append(AccountSkinDialog::AccountRow{QStringLiteral("uuid-c"),
                                                                 QStringLiteral("Carol"),
                                                                 QString(), false});
                dialog.setAccounts(noneCurrent);
                QApplication::processEvents();
                check(accountList->currentRow() == 0,
                      "一个「当前」都没有时选中第一条",
                      QString::number(accountList->currentRow()));
            }

            // ---- X5 按钮发出来的 uuid 是**选中的那个**，不是第 0 条 ----
            // 这里选第 3 条就是钉这一件事：拿 currentRow() 当"第几条"用会退化成永远动
            // 第一条 —— 而那个症状（"我明明选的是 Carol，切过去变成 Alice 了"）很难查。
            {
                int switchCount = 0;
                int removeCount = 0;
                QString switchedUuid;
                QString removedUuid;
                QObject::connect(&dialog, &AccountSkinDialog::switchRequested, &dialog,
                                 [&](const QString &uuid) {
                                     ++switchCount;
                                     switchedUuid = uuid;
                                 });
                QObject::connect(&dialog, &AccountSkinDialog::removeRequested, &dialog,
                                 [&](const QString &uuid) {
                                     ++removeCount;
                                     removedUuid = uuid;
                                 });

                dialog.setAccounts(rows);
                QApplication::processEvents();
                accountList->setCurrentRow(2);
                QApplication::processEvents();
                check(dialog.selectedUuid() == QStringLiteral("uuid-c"),
                      "选到第 3 条（Carol）", dialog.selectedUuid());

                switchBtn->click();
                check(switchCount == 1 && switchedUuid == QStringLiteral("uuid-c"),
                      "点「切换到所选」发的就是 Carol 的 uuid", switchedUuid);
                removeBtn->click();
                check(removeCount == 1 && removedUuid == QStringLiteral("uuid-c"),
                      "点「删除所选」发的也是 Carol 的 uuid", removedUuid);
            }

            // ---- X6 披风列表 ----
            {
                int selectCount = 0;
                int hideCount = 0;
                QString selectedCape;
                QObject::connect(&dialog, &AccountSkinDialog::capeSelectRequested, &dialog,
                                 [&](const QString &id) {
                                     ++selectCount;
                                     selectedCape = id;
                                 });
                QObject::connect(&dialog, &AccountSkinDialog::hideCapeRequested, &dialog,
                                 [&] { ++hideCount; });

                QList<AccountSkinDialog::CapeRow> capes;
                capes.append(AccountSkinDialog::CapeRow{QStringLiteral("cape-1"),
                                                        QStringLiteral("Migrator"), true,
                                                        QStringLiteral("https://textures.example/cape-1")});
                capes.append(AccountSkinDialog::CapeRow{QStringLiteral("cape-2"),
                                                        QStringLiteral("MineCon 2016"), false,
                                                        QStringLiteral("https://textures.example/cape-2")});
                dialog.setCapes(capes);
                QApplication::processEvents();
                check(qAbs(viewer->yaw() - 25.0) < 1.0,
                      "打开页面/刷新披风列表时不自动把人物转到背面", QString::number(viewer->yaw()));
                {
                    // 披风贴图到了：只补进地址对得上的那一格（另一格仍画占位色条）。
                    QImage texture(64, 32, QImage::Format_ARGB32);
                    texture.fill(Qt::red);
                    dialog.setCapeTexture(QStringLiteral("https://textures.example/cape-2"), texture);
                    dialog.setCapeTexture(QStringLiteral("https://textures.example/other"), texture);
                    check(capeList->item(2)->data(Qt::UserRole + 3).value<QImage>().size() == QSize(64, 32) &&
                              capeList->item(1)->data(Qt::UserRole + 3).isNull(),
                          "★ 披风贴图按地址认领：对得上的那一格换成贴图，对不上的不动");
                    check(viewer->cape().isNull(),
                          "没选中的非当前披风贴图到了，不要把人物身上的披风换掉");
                    dialog.setCapeTexture(QStringLiteral("https://textures.example/cape-1"), texture);
                    check(viewer->cape().size() == QSize(64, 32) &&
                              viewer->cape().pixelColor(0, 0) == Qt::red,
                          "当前披风的贴图到了才贴到人物身上");
                }
                check(capeList->count() == 3, "「无披风」+ 两件披风都摆出来了",
                      QString::number(capeList->count()));
                check(capeList->item(1)->text().isEmpty() &&
                          capeList->item(1)->data(Qt::AccessibleTextRole).toString() == QStringLiteral("Migrator"),
                      "披风格不显示披风名，只留无障碍名字", capeList->item(1)->text());
                check(capeList->item(2)->text().isEmpty(),
                      "另一件也不显示名字", capeList->item(2)->text());
                check(dialog.selectedCapeId().isEmpty() && !applyCapeBtn->isEnabled(),
                      "还没选披风时「应用」灰着");

                capeList->setCurrentRow(2);
                QApplication::processEvents();
                check(dialog.selectedCapeId() == QStringLiteral("cape-2"),
                      "选中第 3 行（cape-2）的 id 认得出来", dialog.selectedCapeId());
                check(applyCapeBtn->isEnabled(), "选中之后那个按钮可点");

                applyCapeBtn->click();
                check(selectCount == 1 && selectedCape == QStringLiteral("cape-2"),
                      "点「应用」发的是选中那件的 id", selectedCape);
                check(viewer->cape().size() == QSize(64, 32) &&
                          viewer->cape().pixelColor(0, 0) == Qt::red,
                      "点「应用」后人物预览当场披上选中那件");
                check(qAbs(viewer->yaw() - 205.0) < 1.0,
                      "点「应用」后转到背面 3/4", QString::number(viewer->yaw()));
                dialog.setCapes(capes);  // 模拟接口刷新回来：列表重建、选中被清掉
                QApplication::processEvents();
                check(viewer->cape().size() == QSize(64, 32) &&
                          viewer->cape().pixelColor(0, 0) == Qt::red,
                      "刷新列表后预览仍是刚才点「应用」的那件，不退回旧当前披风");
                {
                    QImage stale(64, 32, QImage::Format_ARGB32);
                    stale.fill(Qt::blue);
                    dialog.setLooks(QImage(), stale, false);
                    check(viewer->cape().pixelColor(0, 0) == Qt::red,
                          "换皮肤/资料刷新带回旧披风图时，不会把刚选的那件盖掉");
                }

                // 列表第一项「无披风」：选中后点「应用」= 取消当前披风。
                capeList->setCurrentRow(0);
                QApplication::processEvents();
                check(capeList->item(0)->text().isEmpty() && capeList->item(0)->data(Qt::UserRole + 4).toBool(),
                      "列表第一项是「无披风」（只留斜杠格，不显示文字）");
                check(applyCapeBtn->isEnabled(), "选中「无披风」后「应用」可点");
                applyCapeBtn->click();
                check(hideCount == 1, "应用「无披风」发的是取消披风信号");
                check(viewer->cape().isNull(), "应用「无披风」后人物预览当场把披风摘掉");
                check(qAbs(viewer->yaw() - 25.0) < 1.0,
                      "应用「无披风」后回到正面视角", QString::number(viewer->yaw()));

                dialog.setCapes({});
                QApplication::processEvents();
                check(capeList->count() == 1 && capeList->item(0)->data(Qt::UserRole + 4).toBool() &&
                          !applyCapeBtn->isEnabled(),
                      "一件披风都没有时列表仍摆「无披风」，未选中时按钮灰着");
            }

            // ---- X7 setLooks：同一份贴图进预览，下拉框跟着当前模型走 ----
            {
                QImage testSkin(64, 64, QImage::Format_ARGB32_Premultiplied);
                testSkin.fill(Qt::transparent);
                {
                    QPainter painter(&testSkin);
                    painter.fillRect(0, 0, 64, 64, QColor(20, 200, 60));
                }
                QImage testCape(64, 32, QImage::Format_ARGB32_Premultiplied);
                testCape.fill(Qt::transparent);
                {
                    QPainter painter(&testCape);
                    painter.fillRect(0, 0, 64, 32, QColor(200, 30, 30));
                }

                QList<AccountSkinDialog::CapeRow> lookCapes;
                lookCapes.append(AccountSkinDialog::CapeRow{QStringLiteral("look-cape"),
                                                            QStringLiteral("Look"), true,
                                                            QStringLiteral("look-url")});
                dialog.setCapes(lookCapes);
                dialog.setCapeTexture(QStringLiteral("look-url"), testCape);
                dialog.setLooks(testSkin, testCape, true);
                check(viewer->skin() == testSkin && viewer->cape() == testCape,
                      "喂进去的皮肤进预览，披风由披风列表贴上去");
                check(dialog.selectedVariant() == AccountSkinDialog::variantSlimValue(),
                      "纤细模型 → 下拉框自动切到「纤细（Alex）」（用户多半是在当前模型上调）",
                      dialog.selectedVariant());

                dialog.setLooks(testSkin, QImage(), false);
                check(dialog.selectedVariant() == AccountSkinDialog::variantClassicValue(),
                      "经典模型 → 下拉框切回「经典（Steve）」");
                check(viewer->cape() == testCape,
                      "setLooks 的披风参数不再覆盖列表里的当前披风（旧资料图不能闪一下）");

                // 空皮肤不许把预览留成空白 —— 那个控件任何一档都得有东西可看。
                dialog.setLooks(QImage(), QImage(), false);
                check(viewer->skin() == SkinModel::defaultSteveSkin(),
                      "皮肤传空图 → 退回默认外观，不是空白");

                // 那个「模型」下拉框的滚轮防护：漏了后果比设置页那几个重 ——
                // 鼠标划过去静默换一档，接着点上传就传错模型（经典/纤细在游戏里是两套手臂）。
                {
                    const QString problem = wheelGuardProblem(
                        dialog.findChild<QComboBox *>(AccountSkinDialog::variantComboName()));
                    check(problem.isEmpty(), "X7b 那个「模型」下拉框也装了滚轮防护", problem);
                }
            }

            // ---- X8 忙碌态：按钮一起灰 ----
            // 这一段盯的是一个真缺陷：refreshButtonStates() 是按「有没有选中」定
            // 可用性的，而 setBusy() 末尾会调它一次 —— 于是忙的时候那三个按钮被重新点亮，
            // 登录还在飞（浏览器可能几十秒才回来）用户就能切账号，回来之后两条路各写一遍
            // 凭据。所以下面这条断言是**故意**盯着它们的。
            {
                dialog.setAccounts(rows);
                QList<AccountSkinDialog::CapeRow> capes;
                capes.append(AccountSkinDialog::CapeRow{QStringLiteral("cape-1"),
                                                        QStringLiteral("Migrator"), true,
                                                        QStringLiteral("https://textures.example/cape-1")});
                dialog.setCapes(capes);
                capeList->setCurrentRow(0);
                QApplication::processEvents();
                check(switchBtn->isEnabled() && removeBtn->isEnabled() &&
                          applyCapeBtn->isEnabled(),
                      "X8 前提：不忙而且都选中了，那三个按钮都是点得动的");

                dialog.setBusy(true, QStringLiteral("正在等浏览器完成登录…"));
                QApplication::processEvents();
                check(statusLabel->text() == QStringLiteral("正在等浏览器完成登录…"),
                      "忙的时候状态行说的是在等什么", statusLabel->text());
                check(!loginBtn->isEnabled() && !refreshBtn->isEnabled() &&
                          !uploadBtn->isEnabled(),
                      "登录 / 刷新外观 / 上传皮肤一起灰掉");
                check(!switchBtn->isEnabled() && !removeBtn->isEnabled() &&
                          !applyCapeBtn->isEnabled(),
                      "「切换到所选 / 删除所选 / 显示所选披风」也灰掉（不能一边登录一边切账号）");
                check(!accountList->isEnabled() && !capeList->isEnabled(),
                      "两个列表也灰掉（它们的内容正是正在被刷新的那两样）");

                dialog.setBusy(false);
                QApplication::processEvents();
                check(loginBtn->isEnabled() && refreshBtn->isEnabled() && uploadBtn->isEnabled(),
                      "忙态松开之后那三个按钮回来");
                check(switchBtn->isEnabled() && removeBtn->isEnabled() &&
                          applyCapeBtn->isEnabled(),
                      "按选中项恢复的那三个也回来（选中项还在）");
                check(statusLabel->text() == QStringLiteral("正在等浏览器完成登录…"),
                      "setBusy(false) 不碰状态行的文字 —— 写什么由调用方决定"
                      "（背景刷新失败要写失败原因，成功就写回当前身份）");
            }

            // ---- X9 离线账号那一行（输入框 / 添加按钮 / 列表里的「（离线）」） ----
            // 这一档钉的全是**交出去的东西**：名字合不合法**不在这个窗里判**（唯一出处是
            // AccountStore::addOffline()），在这里也判一遍就会出现两份判据、两套话术。
            {
                QStringList requested;
                QObject::connect(&dialog, &AccountSkinDialog::offlineAccountRequested, &dialog,
                                 [&](const QString &name) { requested.append(name); });

                offlineEdit->setText(QStringLiteral("  Steve  "));
                addOfflineBtn->click();
                check(requested.size() == 1 && requested.at(0) == QStringLiteral("Steve"),
                      "点「添加离线账号」把框里那个名字交出去，而且首尾空格已经去掉",
                      requested.join(QStringLiteral(" / ")));

                // 在框里回车 = 点那个按钮（少一次"打完名字还要去够鼠标"）。
                // NOTE: 这里直接把信号叫起来，模拟的是 QLineEdit 自己会做的事 —— 真按键盘
                // 要 QtTest 那套事件注入，这个工程里没有。
                offlineEdit->setText(QStringLiteral("Alex"));
                emit offlineEdit->returnPressed();
                check(requested.size() == 2 && requested.at(1) == QStringLiteral("Alex"),
                      "在框里回车也交一次（回车 = 点那个按钮）",
                      requested.join(QStringLiteral(" / ")));

                dialog.showOfflineAddRow();
                offlineEdit->setText(QStringLiteral("留着没提交的名字"));
                dialog.clearOfflineNameDraft();
                check(offlineEdit->text().isEmpty() && !dialog.offlineAddRowShown(),
                      "clearOfflineNameDraft()（添加成功之后）清空框并收起这一行");
                dialog.showOfflineAddRow();

                // 忙的时候这一行也得灰：它一旦成功就会把活动槽清空（切到离线身份），而正飞着的
                // 那次登录回来还要往活动槽里写 —— 两个人会打架。
                dialog.setBusy(true, QStringLiteral("正在等浏览器完成登录…"));
                QApplication::processEvents();
                check(!offlineEdit->isEnabled() && !addOfflineBtn->isEnabled(),
                      "忙的时候那个框和按钮一起灰（不许一边登录一边加离线账号）");
                dialog.setBusy(false);
                QApplication::processEvents();
                check(offlineEdit->isEnabled() && addOfflineBtn->isEnabled(),
                      "忙态松开之后它们又点得动了");

                // 列表里那条离线账号：名字后面挂「（离线）」，和「（当前）」可以同时出现。
                QList<AccountSkinDialog::AccountRow> mixed;
                mixed.append(AccountSkinDialog::AccountRow{QStringLiteral("uuid-a"),
                                                           QStringLiteral("Alice"),
                                                           QStringLiteral("2026-09-22T10:00:00Z"),
                                                           false, true});
                mixed.append(AccountSkinDialog::AccountRow{QStringLiteral("uuid-s"),
                                                           QStringLiteral("Steve"), QString(),
                                                           true, true});
                dialog.setAccounts(mixed);
                QApplication::processEvents();
                check(accountList->item(0)->text() ==
                          QStringLiteral("Alice") + AccountSkinDialog::offlineMark(),
                      "离线账号那一行挂着「（离线）」", accountList->item(0)->text());
                check(accountList->item(1)->text() ==
                          QStringLiteral("Steve") + AccountSkinDialog::offlineMark() +
                              AccountSkinDialog::currentMark(),
                      "又离线又是当前的那一行两个标记都在（「（离线）」在前）",
                      accountList->item(1)->text());
                check(accountList->item(0)->toolTip() == AccountSkinDialog::offlineRowTooltip(),
                      "离线那一行的 tooltip 说清它是哪一种（不是空串、也不是「登录取自…」）",
                      accountList->item(0)->toolTip());
                check(accountList->currentRow() == 1,
                      "选中的还是「当前」那一条（离线账号同样可以是当前）",
                      QString::number(accountList->currentRow()));
                check(dialog.selectedUuid() == QStringLiteral("uuid-s"),
                      "selectedUuid() 给的是离线那一条", dialog.selectedUuid());
            }

            // ---- X12 需要重新登录：来源行改写、行尾「重新登录」、点了走登录流程、标记清掉后恢复（issue #67） ----
            {
                AccountSkinDialog::AccountRow stale;
                stale.uuid = QStringLiteral("uuid-stale");
                stale.name = QStringLiteral("Alice");
                stale.savedAt = QStringLiteral("2026-09-22T10:00:00Z");
                stale.current = true;
                stale.needsRelogin = true;
                AccountSkinDialog::AccountRow healthy;
                healthy.uuid = QStringLiteral("uuid-healthy");
                healthy.name = QStringLiteral("Bob");
                healthy.savedAt = QStringLiteral("2026-09-22T10:00:00Z");
                AccountSkinDialog::AccountRow offlineMarked;
                offlineMarked.uuid = QStringLiteral("uuid-offline-marked");
                offlineMarked.name = QStringLiteral("Steve");
                offlineMarked.offline = true;
                offlineMarked.needsRelogin = true;  // 离线账号没有凭据，标记不生效
                dialog.setAccounts({stale, healthy, offlineMarked});
                QApplication::processEvents();
                const auto source = [&](int row) {
                    return accountList->item(row)->data(Qt::AccessibleDescriptionRole).toString();
                };
                const auto reloginButtons = [&] {
                    return dialog.findChildren<QPushButton *>(AccountSkinDialog::reloginButtonName());
                };
                check(source(0) == AccountSkinDialog::needsReloginText() &&
                          source(1).startsWith(AccountSkinDialog::microsoftAccountText()) &&
                          source(2) == AccountSkinDialog::offlineAccountText(),
                      "需要重新登录的微软账号来源行改写，正常账号与离线账号不变",
                      source(0) + " | " + source(1) + " | " + source(2));
                auto buttons = reloginButtons();
                check(buttons.size() == 1 && buttons.first()->text() == AccountSkinDialog::reloginButtonText() &&
                          buttons.first()->isVisibleTo(&dialog),
                      "只有那一行有「重新登录」，选中时也在", QString::number(buttons.size()));
                auto *copyBtn = dialog.findChild<QPushButton *>(AccountSkinDialog::copyUuidButtonName());
                QWidget *viewport = accountList->viewport();
                if (!buttons.isEmpty() && copyBtn != nullptr) {
                    QPushButton *relogin = buttons.first();
                    check(relogin->geometry().right() < copyBtn->mapTo(viewport, QPoint()).x(),
                          "选中那一行：「重新登录」在「复制 UUID」「删除」左边");
                    accountList->setCurrentRow(1);
                    QApplication::processEvents();
                    // 选中的是 healthy（非当前）：有「切换到所选」；选中需要重新登录的 stale：没有，来源那句完整显示。
                    check(switchBtn->isVisibleTo(&dialog), "选中正常的非当前账号：有「切换到所选」");
                    check(relogin->isVisibleTo(&dialog) &&
                              relogin->geometry().x() + relogin->width() + 10 == accountList->visualItemRect(accountList->item(0)).right(),
                          "不选中也显示，贴行尾",
                          QString::number(relogin->geometry().right()));
                    // 选中需要重新登录的行（非当前）：行尾只剩「重新登录」「复制」「删除」，没有「切换到所选」。
                    AccountSkinDialog::AccountRow staleOther = stale;
                    staleOther.current = false;
                    AccountSkinDialog::AccountRow currentHealthy = healthy;
                    currentHealthy.current = true;
                    dialog.setAccounts({staleOther, currentHealthy, offlineMarked});
                    accountList->setCurrentRow(0);
                    QApplication::processEvents();
                    auto reloginNow = reloginButtons();
                    check(reloginNow.size() == 1 && !switchBtn->isVisibleTo(&dialog) && removeBtn->isVisibleTo(&dialog) &&
                              copyBtn->isVisibleTo(&dialog) && reloginNow.first()->isVisibleTo(&dialog),
                          "选中需要重新登录的行：没有「切换到所选」，只剩「重新登录」「复制 UUID」「删除」");
                    if (!reloginNow.isEmpty()) {
                        // 来源那句放得下：字宽不超过「头像右 13 起、到「重新登录」按钮左 12」这一段（同 delegate 的取值）。
                        QFont subFont = accountList->font();
                        subFont.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
                        const QRect rowRect = accountList->visualItemRect(accountList->item(0));
                        const int textLeft = rowRect.left() + 12 + 32 + 13;
                        const int textRight = reloginNow.first()->geometry().left() - 12;
                        check(QFontMetrics(subFont).horizontalAdvance(AccountSkinDialog::needsReloginText()) <= textRight - textLeft,
                              "选中时「需要重新登录」那句来源文字放得下、不被省略",
                              QString("%1 > %2").arg(QFontMetrics(subFont).horizontalAdvance(AccountSkinDialog::needsReloginText())).arg(textRight - textLeft));
                    }
                    dialog.setAccounts({stale, healthy, offlineMarked});
                    accountList->setCurrentRow(1);
                    QApplication::processEvents();
                    relogin = reloginButtons().isEmpty() ? relogin : reloginButtons().first();
                    int logins = 0;
                    const auto link = QObject::connect(&dialog, &AccountSkinDialog::loginRequested, &dialog,
                                                       [&logins] { ++logins; });
                    relogin->click();
                    check(logins == 1, "点「重新登录」发 loginRequested（和「添加账号 → 微软账号」同一条）");
                    dialog.setBusy(true, QStringLiteral("x"));
                    check(!relogin->isEnabled(), "忙的时候「重新登录」置灰");
                    dialog.setBusy(false);
                    dialog.setOnlineLoginUnavailable(QStringLiteral("不可用"));
                    check(!relogin->isEnabled() && relogin->toolTip() == QStringLiteral("不可用"),
                          "微软登录用不了时「重新登录」置灰并带原因");
                    dialog.setOnlineLoginUnavailable(QString());
                    check(relogin->isEnabled(), "说明清空后「重新登录」回来");
                    QObject::disconnect(link);
                }
                stale.needsRelogin = false;
                dialog.setAccounts({stale, healthy, offlineMarked});
                QApplication::processEvents();
                check(reloginButtons().isEmpty() &&
                          source(0).startsWith(AccountSkinDialog::microsoftAccountText()),
                      "标记清掉后：按钮消失，来源行恢复「微软账号 · 登录取自…」", source(0));
            }

            // ---- X11 账号行：来源、头像、复制 UUID；请求失败行；预览说明（issue #66） ----
            {
                QImage blue(64, 64, QImage::Format_ARGB32);
                blue.fill(QColor(0x30, 0x90, 0xD0));
                AccountSkinDialog::AccountRow microsoftRow;
                microsoftRow.uuid = QStringLiteral("uuid-ms");
                microsoftRow.name = QStringLiteral("Alice");
                microsoftRow.savedAt = QStringLiteral("2026-09-22T10:00:00Z");
                microsoftRow.current = true;
                AccountSkinDialog::AccountRow offlineRow;
                offlineRow.uuid = QStringLiteral("uuid-off");
                offlineRow.name = QStringLiteral("Bob");
                offlineRow.offline = true;
                offlineRow.face = blue;  // 非当前账号也画自己的头像
                AccountSkinDialog::AccountRow letterRow;
                letterRow.uuid = QStringLiteral("uuid-ms2");
                letterRow.name = QStringLiteral("Carol");  // 没有缓存：退回首字母
                dialog.setAccounts({microsoftRow, offlineRow, letterRow});
                QApplication::processEvents();
                const auto source = [&](int row) {
                    return accountList->item(row)->data(Qt::AccessibleDescriptionRole).toString();
                };
                check(source(0).startsWith(AccountSkinDialog::microsoftAccountText()) &&
                          source(1) == AccountSkinDialog::offlineAccountText() &&
                          source(2) == AccountSkinDialog::microsoftAccountText(),
                      "每一行名字下面都有来源文字：微软账号 / 离线账号",
                      source(0) + " | " + source(1) + " | " + source(2));

                const QImage painted = accountList->viewport()->grab().toImage();
                const QRect offlineRect = accountList->visualItemRect(accountList->item(1));
                const QColor centre = painted.pixelColor(offlineRect.left() + 12 + 16, offlineRect.center().y());
                check(centre.rgb() == QColor(0x30, 0x90, 0xD0).rgb(),
                      "非当前账号画的是它自己的头像（不是首字母）", centre.name());
                const QRect letterRect = accountList->visualItemRect(accountList->item(2));
                check(painted.pixelColor(letterRect.left() + 12 + 16, letterRect.center().y()).rgb() !=
                          QColor(0x30, 0x90, 0xD0).rgb(),
                      "没有头像的行退回首字母（和有头像的行不同）");

                // 复制 UUID：选中非当前行，行尾依次是 切换 / 复制 UUID / 删除
                accountList->setCurrentRow(1);
                QApplication::processEvents();
                auto *copyBtn = dialog.findChild<QPushButton *>(AccountSkinDialog::copyUuidButtonName());
                check(copyBtn != nullptr && copyBtn->toolTip() == AccountSkinDialog::copyUuidText() &&
                          copyBtn->text().isEmpty() && !copyBtn->icon().isNull() &&
                          switchBtn->x() < copyBtn->x() && copyBtn->x() < removeBtn->x(),
                      "行尾「复制 UUID」图标钮在「切换」和「删除」之间，悬停提示是「复制 UUID」");
                if (copyBtn != nullptr) {
                    QGuiApplication::clipboard()->setText(QStringLiteral("旧内容"));
                    copyBtn->click();
                    check(QGuiApplication::clipboard()->text() == QStringLiteral("uuid-off"),
                          "点「复制 UUID」：剪贴板里是选中那一行的 uuid",
                          QGuiApplication::clipboard()->text());
                }

                // 请求失败：身份那一句不动，原因另起一行，后面跟「重试」
                dialog.setStatusText(AccountSkinDialog::identityText(QStringLiteral("Alice")));
                auto *errorLabel = dialog.findChild<QLabel *>(AccountSkinDialog::statusErrorLabelName());
                auto *retryBtn = dialog.findChild<QPushButton *>(AccountSkinDialog::retryButtonName());
                check(errorLabel != nullptr && retryBtn != nullptr && !errorLabel->isVisibleTo(&dialog) &&
                          !retryBtn->isVisibleTo(&dialog),
                      "没有失败时不显示失败行");
                if (errorLabel != nullptr && retryBtn != nullptr) {
                    dialog.setStatusError(QStringLiteral("网络不可用"));
                    check(statusLabel->text() == AccountSkinDialog::identityText(QStringLiteral("Alice")) &&
                              errorLabel->text() == QStringLiteral("网络不可用") &&
                              errorLabel->isVisibleTo(&dialog) && retryBtn->isVisibleTo(&dialog) &&
                              retryBtn->text() == AccountSkinDialog::retryButtonText() &&
                              errorLabel->mapTo(&dialog, QPoint()).y() > statusLabel->mapTo(&dialog, QPoint()).y(),
                          "失败时「已登录为…」不变，原因在它下面另起一行，后面是「重试」",
                          statusLabel->text());
                    int refreshes = 0;
                    const auto link = QObject::connect(&dialog, &AccountSkinDialog::refreshRequested, &dialog,
                                                       [&refreshes] { ++refreshes; });
                    retryBtn->click();
                    check(refreshes == 1, "点「重试」发 refreshRequested");
                    QObject::disconnect(link);
                    dialog.setStatusError(QString());
                    check(!errorLabel->isVisibleTo(&dialog) && !retryBtn->isVisibleTo(&dialog),
                          "清空原因后失败行收起");
                }

                // 披风列表所属账号：调用方靠它判断失败时留不留
                dialog.setCapesOwner(QStringLiteral("uuid-off"));
                check(dialog.capesOwner() == QStringLiteral("uuid-off"), "披风列表记着它属于谁");

                // 离线皮肤预览说明
                auto *note = dialog.findChild<QLabel *>(AccountSkinDialog::previewNoteName());
                check(note != nullptr && !note->isVisibleTo(&dialog), "预览和游戏一致时不显示说明");
                if (note != nullptr) {
                    dialog.setPreviewNote(AccountSkinDialog::previewNoteLocalSkin());
                    check(note->isVisibleTo(&dialog) && note->text() == AccountSkinDialog::previewNoteLocalSkin(),
                          "用了本地皮肤：预览下面显示那句说明");
                    dialog.setPreviewNote(AccountSkinDialog::previewNoteSteve());
                    check(note->text() == AccountSkinDialog::previewNoteSteve(), "显示 Steve：换成另一句说明");
                    dialog.setPreviewNote(QString());
                    check(!note->isVisibleTo(&dialog), "说明清空后收起");
                }
                dialog.setAccounts(rows);

                // 预览说明：默认窗口宽度（1280×800）下一行放下，可以比 3D 预览宽
                const QSize oldSize = dialog.size();
                dialog.resize(1280, 800);
                dialog.setPreviewNote(AccountSkinDialog::previewNoteLocalSkin());
                QApplication::processEvents();
                if (note != nullptr) {
                    check(note->height() < note->fontMetrics().lineSpacing() * 3 / 2 &&
                              note->width() > viewer->width(),
                          "默认窗口下预览说明一行放下，宽度比 3D 预览宽",
                          QStringLiteral("%1×%2 / 预览 %3").arg(note->width()).arg(note->height()).arg(viewer->width()));
                }
                dialog.setPreviewNote(QString());
                dialog.resize(oldSize);

                // 离开这一页（隐藏）再回来：离线名字框收起、输入清空
                dialog.showOfflineAddRow();
                offlineEdit->setText(QStringLiteral("没提交的名字"));
                dialog.hide();
                QApplication::processEvents();
                dialog.show();
                QApplication::processEvents();
                check(!dialog.offlineAddRowShown() && offlineEdit->text().isEmpty(),
                      "离开页面再回来：离线名字框收起、输入清空");
            }

            // ---- X10 信号接线 ----
            {
                int loginCount = 0;
                int refreshCount = 0;
                QObject::connect(&dialog, &AccountSkinDialog::loginRequested, &dialog,
                                 [&] { ++loginCount; });
                QObject::connect(&dialog, &AccountSkinDialog::refreshRequested, &dialog,
                                 [&] { ++refreshCount; });
                dialog.findChild<QAction *>(AccountSkinDialog::addMicrosoftActionName())->trigger();
                refreshBtn->click();
                check(loginCount == 1, "选「添加账号」里的「微软账号」发 loginRequested");
                check(refreshCount == 1, "点「刷新外观」发 refreshRequested");
                // NOTE: 「上传皮肤…」**故意不点** —— 它会开一个模态的 QFileDialog，
                // 在 offscreen 上会一直等下去（看着像死循环）。它那条路的两个半边
                // 各有落点：选项怎么读是 X1/X7，尺寸那条规矩在 verify 的 X4。
                dialog.close();
                QApplication::processEvents();
                check(!dialog.isVisible(), "点「关闭」之后窗口关掉了");
            }
        }
    }
    return 0;
}





