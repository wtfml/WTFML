#include "Support.hpp"
int UiGroupEditorChecks() {

    {
        using Group = GroupDialog::Group;
        const QString packId = QStringLiteral("g-整合包");
        const QString modId = QStringLiteral("g-模组");

        QList<Group> groups;
        groups.append({packId, QStringLiteral("整合包"), true});
        // 「模组」建的时候就是**收着的**：下面验"勾选框跟着选中的那一组走"要有两种状态可比。
        groups.append({modId, QStringLiteral("模组"), false});
        QHash<QString, int> counts;
        counts.insert(packId, 3);
        counts.insert(modId, 1);

        // 五个默认分组（"恢复默认"要换成它们）。id 照产品里那五个写死的值 —— 这一节不
        // 依赖 InstanceGroupStore（ui/ 不 include src/ 那条规矩），但**形状**要和它一样。
        QList<Group> defaults;
        defaults.append({QStringLiteral("default-pinned"), QStringLiteral("收藏"), true});
        defaults.append({QStringLiteral("default-moddable"), QStringLiteral("可安装mod"), true});
        defaults.append({QStringLiteral("default-vanilla"), QStringLiteral("原版"), true});
        defaults.append({QStringLiteral("default-snapshot"), QStringLiteral("快照版"), true});
        defaults.append({QStringLiteral("default-old"), QStringLiteral("远古版"), true});
        QHash<QString, int> defaultCounts;
        defaultCounts.insert(QStringLiteral("default-vanilla"), 2);

        // 「名字不合法」「重名」「恢复默认」那几条路都会**说一句为什么**，而那句话从
        // 起开在**内容区中间那一块**（`InlineMessage`），不再是一个独立的
        // QMessageBox 窗。要让这一句弹得出来，这个对话框自己得先"在覆盖层里" ——
        // 产品那边它就是被 MainWindow 当一页塞进覆盖层的（见 OverlayHost::showPage），
        // 所以这里也照那条路摆一个**只服务于这一节的**覆盖层（不动上面那个 ui 的状态，
        // 那边 O 段还要断言"刚搭好的时候它是关着的"）。
        //
        // NOTE: 这里**不给布局**，只 `setParent`。这一节里那些断言量的是对话框**自己**
        // 内部的东西（组清单的行高、行尾那两颗三角的像素、尺寸提示），让它被外面
        // 拉大只会把它们搅了；这一节要验的是"那句话开在哪一层"，而那一件事由
        // `OverlayHost::forWidget()` 沿父链找得到父控件就够了。
        QWidget overlayHome;
        overlayHome.resize(900, 620);
        QWidget overlayAnchor(&overlayHome);
        overlayAnchor.setGeometry(0, 0, 900, 620);
        OverlayHost overlayHost(&overlayAnchor, &overlayHome);
        QWidget *groupPage = overlayHost.openPage(QStringLiteral("分组设置"));
        const auto putInOverlay = [groupPage](QWidget *page) {
            if (groupPage == nullptr) {
                return;  // 上面那条断言已经报过了；这里再 setParent(nullptr) 就把它变成独立窗了
            }
            page->setParent(groupPage);
            page->show();
        };
        check(groupPage != nullptr, "分组设置那一页在覆盖层里摆出来了（下面那些提示才有地方弹）");
        QApplication::processEvents();

        GroupDialog dialog(groups, counts, defaults, defaultCounts);
        putInOverlay(&dialog);
        check(dialog.groups() == groups, "没动过的话，出参就是进参那一份（连展开状态一起）");
        check(dialog.groups().at(1).expanded == false,
              "「默认展开/关闭」是**组自己的一格**，不是界面上的显示状态：进参里收着的那一组"
              "原样带出来");

        auto *list = dialog.findChild<QListWidget *>(GroupDialog::groupListName());
        auto *addButton = dialog.findChild<QPushButton *>(GroupDialog::addGroupButtonName());
        auto *renameButton =
            dialog.findChild<QPushButton *>(GroupDialog::renameGroupButtonName());
        auto *removeButton =
            dialog.findChild<QPushButton *>(GroupDialog::removeGroupButtonName());
        auto *expandedBox = dialog.findChild<QCheckBox *>(GroupDialog::expandedCheckName());
        auto *restoreButton =
            dialog.findChild<QPushButton *>(GroupDialog::restoreDefaultsButtonName());
        check(list != nullptr && addButton != nullptr && renameButton != nullptr &&
                  removeButton != nullptr && expandedBox != nullptr && restoreButton != nullptr &&
                  dialog.findChild<QPushButton *>(GroupDialog::confirmButtonName()) != nullptr &&
                  dialog.findChild<QPushButton *>(GroupDialog::cancelButtonName()) != nullptr,
              "组列表、四个动作按钮、「默认展开」勾选框、确定/取消，全都按 objectName 找得回来");
        check(dialog.findChild<QPushButton *>(QStringLiteral("moveGroupUpButton")) == nullptr &&
                  dialog.findChild<QPushButton *>(QStringLiteral("moveGroupDownButton")) == nullptr,
              "「上移」「下移」那两颗按钮已经不在了（挪动改成点行尾那两颗三角）");

        check(list != nullptr && list->count() == 2, "组列表里两条",
              QString::number(list != nullptr ? list->count() : -1));
        check(list != nullptr && list->item(0) != nullptr &&
                  list->item(0)->text() ==
                      GroupDialog::groupItemText(QStringLiteral("整合包"), 3) &&
                  list->item(1) != nullptr &&
                  list->item(1)->text() == GroupDialog::groupItemText(QStringLiteral("模组"), 1),
              "每条的文字带着实例数（「组名 (N)」）",
              list != nullptr && list->item(0) != nullptr ? list->item(0)->text() : QString());
        check(list != nullptr && list->item(0) != nullptr &&
                  list->item(0)->data(Qt::UserRole).toString() == packId,
              "组 id 存在 UserRole 里（不拿显示文本当身份 —— 组名是用户起的）");

        // 进参里没给某个组的实例数时当 0（调用方只数得出来树上画着的那些，藏起来的组
        // 本来就一个实例都没有）。
        {
            GroupDialog noCounts(groups, QHash<QString, int>(), defaults, defaultCounts);
            auto *bare = noCounts.findChild<QListWidget *>(GroupDialog::groupListName());
            check(bare != nullptr && bare->item(0) != nullptr &&
                      bare->item(0)->text() ==
                          GroupDialog::groupItemText(QStringLiteral("整合包"), 0),
                  "进参里没有这个组的实例数时按 0 显示",
                  bare != nullptr && bare->item(0) != nullptr ? bare->item(0)->text() : QString());
        }

        // ---- 一开窗停在第一组（步骤 137：右边那张卡片写的是"选中那一组"，空着没意义）----
        check(list != nullptr && list->currentRow() == 0 && renameButton != nullptr &&
                  renameButton->isEnabled(),
              "★ 开窗就选中第一组，「重命名…」能点",
              QString::number(list != nullptr ? list->currentRow() : -2));
        // ---- 没选中任何一行：除了「新建」，别的都点不动 ----
        // （点一个不存在的组没有意义；「新建」是唯一一个不依赖选中项的动作 —— 一个组
        // 都没有的时候它是唯一的出路，而"组一个都没有"是删得出来的状态。）
        if (list != nullptr) list->setCurrentRow(-1);
        check(addButton != nullptr && addButton->isEnabled(), "没选中时「新建」照样能点");
        check(renameButton != nullptr && !renameButton->isEnabled() &&
                  removeButton != nullptr && !removeButton->isEnabled() &&
                  expandedBox != nullptr && !expandedBox->isEnabled(),
              "一行都没选中时，其余那三个都点不动（「新建」是唯一一个不依赖选中项的）");

        const auto rowOfGroup = [list](const QString &groupId) {
            if (list == nullptr) {
                return -1;
            }
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->data(Qt::UserRole).toString() == groupId) {
                    return i;
                }
            }
            return -1;
        };

        // ---- 选中一行之后：能点的都能点，挪到头/尾那一边灰着 ----
        if (list != nullptr && list->count() == 2) {
            list->setCurrentRow(0);
        }
        check(renameButton != nullptr && renameButton->isEnabled() &&
                  removeButton != nullptr && removeButton->isEnabled(),
              "选中一行之后「重命名」「删除」可点");
        check(list != nullptr && list->count() == 2 &&
                  !GroupListDelegate::canMoveUp(list->model()->index(0, 0)) &&
                  GroupListDelegate::canMoveDown(list->model()->index(0, 0)),
              "第一条在最上面：「上移」挪不动（那一颗**不画**，不是画成灰的 —— 原来那两颗"
              "按钮的可用性就是这个判据，只是换了个说法）");

        // ---- 新建 ----
        // 被拒的这几条都会弹出警告框（"出错必须弹窗"）。每一条都先把关窗的 timer 支好，
        // 顺便把弹出来的那句话收下来核对 —— 只说"被拒了"是不够的，用户得知道为什么。
        dismissNextInline(&overlayHost);
        check(!dialog.createGroup(QStringLiteral("   ")), "纯空格的名字建不出来");
        check(!g_modalText.isEmpty(), "而且弹了框说明为什么", g_modalTitle + " / " + g_modalText);

        dismissNextInline(&overlayHost);
        check(!dialog.createGroup(QStringLiteral("整合包")), "重名建不出来");
        check(g_modalText.contains(QStringLiteral("整合包")) &&
                  g_modalText.contains(QStringLiteral("已经有一个")),
              "框里点名说的是哪个组重了", g_modalText);

        dismissNextInline(&overlayHost);
        check(!dialog.createGroup(QStringLiteral("整合包 ")), "去掉首尾空格后重名，也建不出来");

        dismissNextInline(&overlayHost);
        check(!dialog.createGroup(QString(GroupDialog::maxGroupNameLength() + 8,
                                          QLatin1Char('x'))),
              "超过上限的名字建不出来");
        check(g_modalText.contains(QString::number(GroupDialog::maxGroupNameLength())),
              "框里说了上限是多少", g_modalText);
        check(dialog.groups().size() == 2, "被拒的这几次一个组都没加上",
              QString::number(dialog.groups().size()));

        check(dialog.createGroup(QStringLiteral("  原版  ")), "有内容的名字能建出来");
        check(dialog.groups().size() == 3 && dialog.groups().last().name == QStringLiteral("原版"),
              "组清单多了一个，名字去掉了首尾空格");
        const QString newId = dialog.groups().last().id;
        check(!newId.isEmpty() && newId != packId && newId != modId,
              "新组有自己的 id，不和已有的撞", newId);
        check(dialog.groups().last().expanded, "新建的组默认是展开的");
        check(list != nullptr && list->count() == 3, "左边列表跟着多一条");
        check(list != nullptr && list->currentRow() == 2,
              "新建完左边列表选中它（下一步多半是给它改名或往里放东西）");

        // ---- 重命名 ----
        check(dialog.renameGroup(newId, QStringLiteral("原版 1.21")), "改组名成功");
        check(dialog.groups().last().name == QStringLiteral("原版 1.21") &&
                  dialog.groups().last().id == newId,
              "名字改了、id 没动（归属挂的是 id，所以改组名不会把组里的实例抖出去）");
        check(list != nullptr && list->item(2) != nullptr &&
                  list->item(2)->text() ==
                      GroupDialog::groupItemText(QStringLiteral("原版 1.21"), 0),
              "列表里那一行的字跟着换了（没有整张重建，选中项和滚动位置都不抖）",
              list != nullptr && list->item(2) != nullptr ? list->item(2)->text() : QString());
        dismissNextInline(&overlayHost);
        check(!dialog.renameGroup(newId, QStringLiteral("模组")), "改成和别的组重名，拒绝");
        check(g_modalTitle.contains(QStringLiteral("重命名")), "弹的是「重命名分组失败」那个框",
              g_modalTitle);
        dismissNextInline(&overlayHost);
        check(!dialog.renameGroup(newId, QString()), "改成空名，拒绝");
        dismissNextInline(&overlayHost);
        check(!dialog.renameGroup(newId, QString(GroupDialog::maxGroupNameLength() + 8,
                                                 QLatin1Char('y'))),
              "超长的名字也拒绝（和「新建」同一把尺子）");
        check(!dialog.renameGroup(QStringLiteral("没有这个组"), QStringLiteral("x")),
              "组不存在时返回 false（不弹窗：那不是用户此刻做的操作）");
        check(dialog.groups().at(rowOfGroup(modId)).name == QStringLiteral("模组") &&
                  dialog.groups().at(rowOfGroup(packId)).name == QStringLiteral("整合包"),
              "改了一圈组名，别的组一个字节没动");

        // ---- 排顺序（上移/下移，一次一格） ----
        if (list != nullptr) {
            list->setCurrentRow(rowOfGroup(newId));
        }
        check(dialog.moveGroupUp(newId), "把最后一条往上挪一格");
        check(dialog.groups().at(1).id == newId && dialog.groups().at(2).id == modId,
              "它和上面那一格换了位置（顺序就是界面上的顺序、也是树上的顺序）");
        check(list != nullptr && list->currentRow() == 1,
              "选中项跟着那一组走（按 id 找回来，不是按行号）");
        check(list != nullptr &&
                  GroupListDelegate::canMoveUp(list->model()->index(rowOfGroup(newId), 0)) &&
                  GroupListDelegate::canMoveDown(list->model()->index(rowOfGroup(newId), 0)),
              "挪到中间之后两个方向都能挪（两颗三角都画出来）");
        check(dialog.moveGroupDown(newId), "再挪回来");
        check(dialog.groups().at(2).id == newId, "回到最后一条");
        check(dialog.moveGroupDown(modId), "最后一条还能再往下挪一格");
        check(dialog.groups().last().id == modId, "「模组」挪到了最下面");
        check(!dialog.moveGroupDown(modId), "已经在最下面了：再往下返回 false（不弹窗）");
        check(dialog.groups().size() == 3, "上面那两下没有凭空多出/少掉组",
              QString::number(dialog.groups().size()));

        // ---- 置顶 / 置底（按住 Shift 点那两颗按钮的那一档， 加的） ----
        //
        // 要求：「按住 shift 时点击就直接放最顶上/底下」。界面那条路是 clicked 里读
        // QApplication::keyboardModifiers()，而 offscreen 下摆不出"按着 Shift"—— 所以真正
        // 办事的是那两个槽，这里直接驱动它们（和"重命名拆成问/做两半"同一个理由）。
        check(dialog.groups().at(0).id == packId && dialog.groups().at(1).id == newId &&
                  dialog.groups().last().id == modId,
              "跑上面那几下的顺序： [整合包][新建的那个][模组]");
        check(dialog.moveGroupToTop(modId), "把最后一条**直接挪到最上面**");
        check(dialog.groups().first().id == modId && dialog.groups().size() == 3,
              "它到了第一条，组数没变", QString::number(dialog.groups().size()));
        check(dialog.groups().at(1).id == packId && dialog.groups().at(2).id == newId,
              "★ 中间那两条的相对顺序没乱（是「一步到位」，不是 swap 好几下）",
              QStringLiteral("%1 / %2")
                  .arg(dialog.groups().at(1).name, dialog.groups().at(2).name));
        check(list != nullptr && list->currentRow() == rowOfGroup(newId),
              "★ 选中项没被挪动那一下弄丢（它还是原来那一组，按 id 找回来而不是按行号）",
              QStringLiteral("当前第 %1 行，新建的那一组在第 %2 行")
                  .arg(list != nullptr ? list->currentRow() : -1)
                  .arg(rowOfGroup(newId)));
        check(!dialog.moveGroupToTop(modId), "已经在最上面了：再置顶返回 false（不弹窗）");
        check(dialog.moveGroupToBottom(modId), "再**直接挪到最下面**");
        check(dialog.groups().last().id == modId && dialog.groups().first().id == packId &&
                  dialog.groups().at(1).id == newId,
              "★ 到了最后一条，中间那两条回到原样（置顶再置底 = 回到出发点）",
              QStringLiteral("%1 / %2 / %3")
                  .arg(dialog.groups().at(0).name, dialog.groups().at(1).name,
                       dialog.groups().at(2).name));
        check(!dialog.moveGroupToBottom(modId), "已经在最下面了：返回 false");
        check(!dialog.moveGroupToTop(QStringLiteral("没有这个组")) &&
                  !dialog.moveGroupToBottom(QStringLiteral("没有这个组")),
              "组不存在时那两个也返回 false（不弹窗：那不是用户此刻做的操作）");
        check(dialog.groups().size() == 3, "上面这几下没有凭空多出/少掉组",
              QString::number(dialog.groups().size()));
        // Shift 那一条手势界面上**没有别的痕迹**（两个槽本身长得和上移/下移一样），
        // 所以提示语是它唯一的出处 —— 少一句用户就永远不知道有这一档。
        if (list != nullptr && list->count() == 3) {
            // 提示挂在**行尾那两格**上（GroupListDelegate::rowActionTip()，走代理的
            // helpEvent()）—— 行矩形在这儿是现摆的（这一节没有真视口，那张窗没摆出来），
            // 判据本身只吃"行矩形 + 点在哪"，和真视口给的那一份是同一个函数。
            const QRect tipRow(0, 0, 320, MainWindowUI::versionTreeRowHeight());
            const QModelIndex tipIndex = list->model()->index(1, 0);  // 中间那条：两边都挪得动
            const QString upTip = GroupListDelegate::rowActionTip(
                tipIndex, tipRow, GroupListDelegate::moveUpRect(tipRow).center());
            const QString downTip = GroupListDelegate::rowActionTip(
                tipIndex, tipRow, GroupListDelegate::moveDownRect(tipRow).center());
            check(upTip.contains(QStringLiteral("Shift")) &&
                      downTip.contains(QStringLiteral("Shift")),
                  "行尾那两格的悬停提示都写明了「按住 Shift = 直接放到最上/最下面」",
                  upTip + QStringLiteral(" / ") + downTip);
        }

        // ---- 行尾那两颗上下移三角（ 傍晚从实例树搬进来的） ----
        //
        // 要求：「...这个箭头不应该在这里，而应该在 分组设置 里面，这里的删了，去分组
        // 设置里面加上，设置那个框下方的上移下移的按钮就删了」。上面那一节（组行那棵树）
        // 钉的是"这里没有了"；这一节钉另一半：它们**真在这张表里**，长什么样、点一下挪不挪
        // 得动、以及那一下**不会**顺手把这一行选中（用户报的「点击一下就同时展开和调整了」
        // 正是这个形状）。
        //
        // NOTE: 这一段**另开一个** GroupDialog —— 上面那半个的组序后面还要按
        // 「[整合包][新建的那个][模组]」接着往下验。组数用三个：两条的时候就凑不齐
        // "第一格只有下移、中间两格都有、最后一格只有上移"这三档。
        {
            using Group = GroupDialog::Group;
            const QString shaderId = QStringLiteral("g-光影");
            QList<Group> arrowGroups;
            arrowGroups.append({packId, QStringLiteral("整合包"), true});
            arrowGroups.append({modId, QStringLiteral("模组"), true});
            arrowGroups.append({shaderId, QStringLiteral("光影"), true});
            QHash<QString, int> arrowCounts;
            arrowCounts.insert(packId, 3);
            arrowCounts.insert(modId, 1);
            arrowCounts.insert(shaderId, 2);
            // 真的摆出来：行尾那两格要视图给的行矩形才量得了（`visualItemRect()`；没摆出来
            // 的窗布局还没算过，那个矩形是空的）。
            GroupDialog arrowDialog(arrowGroups, arrowCounts, defaults, defaultCounts);
            arrowDialog.show();
            QApplication::processEvents();
            auto *glist = arrowDialog.findChild<QListWidget *>(GroupDialog::groupListName());
            check(glist != nullptr && glist->count() == 3,
                  "这一段的前提：另开的那张分组设置里三条、而且真的摆出来了",
                  QString::number(glist != nullptr ? glist->count() : -1));
            if (glist != nullptr && glist->count() == 3) {
                const auto rowRect = [glist](int row) { return glist->visualItemRect(glist->item(row)); };
                // 画出来的那一块（底下那条缝不算）—— 量底色和量几何都按它。
                const auto drawnRect = [&rowRect](int row) {
                    return GroupListDelegate::contentRect(rowRect(row));
                };
                const auto orderNow = [&arrowDialog] {
                    QStringList names;
                    for (const Group &group : arrowDialog.groups()) {
                        names.append(group.name);
                    }
                    return names.join(QStringLiteral(" / "));
                };
                const auto selectedId = [glist] {
                    const QListWidgetItem *item = glist->currentItem();
                    return item != nullptr ? item->data(Qt::UserRole).toString() : QString();
                };

                // ① 条目矩形 = 画出来的那一块（`versionTreeRowHeight()`）+ 底下一条缝；
                // 行尾那个方框里上箭头在上、下箭头在下，整个框贴着行右边。
                //
                // （要求：「能不能把箭头都放到最右边，上面是上箭头，下面是
                // 下箭头，拿方框框起来，同时给组和组之间留一点距离，不然粘在一起分辨不开」）
                const QRect rFirst = rowRect(0);
                const QRect cFirst = GroupListDelegate::contentRect(rFirst);
                check(rFirst.width() > 0 &&
                          rFirst.height() ==
                              MainWindowUI::versionTreeRowHeight() + GroupListDelegate::rowGap() &&
                          cFirst.height() == MainWindowUI::versionTreeRowHeight() &&
                          cFirst.top() == rFirst.top(),
                      "★ 条目比画出来的那一块高一条缝（rowGap()）：画的那一块仍然是"
                      "versionTreeRowHeight()、而且贴着条目的上边（缝留在底下）",
                      QStringLiteral("条目 %1x%2 / 画的 %3x%4")
                          .arg(rFirst.width()).arg(rFirst.height())
                          .arg(cFirst.width()).arg(cFirst.height()));
                check(GroupListDelegate::contentRect(rowRect(1)).top() -
                              GroupListDelegate::contentRect(rowRect(0)).bottom() - 1 ==
                          GroupListDelegate::rowGap(),
                      "★ 相邻两条**画出来的**那一块之间正好裂开 rowGap() 那么高"
                      "（用户原话「给组和组之间留一点距离，不然粘在一起分辨不开」）",
                      QStringLiteral("缝 %1 px（要 %2）")
                          .arg(GroupListDelegate::contentRect(rowRect(1)).top() -
                               GroupListDelegate::contentRect(rowRect(0)).bottom() - 1)
                          .arg(GroupListDelegate::rowGap()));
                const QRect boxFirst = GroupListDelegate::moveBoxRect(rFirst);
                check(GroupListDelegate::rowGap() >= 4,
                      "★ 这条缝不能是 0：`rowGap()` 两头读的是同一个常数（行高和这里），"
                      "把它改成 0 的话上面两条会自洽地全绿 —— 而用户要的正是「看得见的"
                      "一条缝」。4 px 是下限（一眼能看出来是一条缝），具体挑几个数是这一层的事",
                      QString::number(GroupListDelegate::rowGap()));
                check(GroupListDelegate::moveUpRect(rFirst).left() ==
                          GroupListDelegate::moveDownRect(rFirst).left() &&
                      GroupListDelegate::moveUpRect(rFirst).width() ==
                          GroupListDelegate::moveDownRect(rFirst).width() &&
                      GroupListDelegate::moveUpRect(rFirst).bottom() + 1 ==
                          GroupListDelegate::moveDownRect(rFirst).top() &&
                      GroupListDelegate::moveUpRect(rFirst).top() == boxFirst.top() &&
                      GroupListDelegate::moveDownRect(rFirst).bottom() == boxFirst.bottom() &&
                      boxFirst.right() <=
                          cFirst.right() - MainWindowUI::versionTreeRowPadding() &&
                      cFirst.contains(boxFirst),
                      "★ 行尾那个方框：上箭头在上、下箭头在下（两半一样宽、上下叠着，合起来"
                      "正好是整框），整个框贴着行右边、装在画出来的那一块里",
                      QStringLiteral("框 x=%1..%2 y=%3..%4 / 行右=%5")
                          .arg(boxFirst.left()).arg(boxFirst.right())
                          .arg(boxFirst.top()).arg(boxFirst.bottom())
                          .arg(cFirst.right()));
                // ② 画出来的那一圈装在**自己的那一格**里（上下两格、互不重叠），行矮就跟着
                // 缩那条在实例树上早就有了，这里补的是"换了个宿主之后仍然是这个式子"。
                const QRect shortRow(0, 0, 400, MainWindowUI::versionTreeArrowSize());
                check(cFirst.contains(GroupListDelegate::moveUpGlyphRect(rFirst)) &&
                          cFirst.contains(GroupListDelegate::moveDownGlyphRect(rFirst)) &&
                          GroupListDelegate::moveUpRect(rFirst)
                              .contains(GroupListDelegate::moveUpGlyphRect(rFirst)) &&
                          GroupListDelegate::moveDownRect(rFirst)
                              .contains(GroupListDelegate::moveDownGlyphRect(rFirst)) &&
                          shortRow.contains(GroupListDelegate::moveUpGlyphRect(shortRow)) &&
                          shortRow.contains(GroupListDelegate::moveDownGlyphRect(shortRow)) &&
                          !GroupListDelegate::moveUpGlyphRect(rFirst)
                               .intersects(GroupListDelegate::moveDownGlyphRect(rFirst)),
                      "两颗三角画出来的那一圈各装在自己那一格里（互不重叠），行矮的时候"
                      "跟着那半格一起缩（不留半颗在格子外面）");

                // ③ 能不能挪是**算出来的**：这张表里行序就是组序，没有角色位这一说。
                const QModelIndex idxFirst = glist->model()->index(0, 0);
                const QModelIndex idxMiddle = glist->model()->index(1, 0);
                const QModelIndex idxLast = glist->model()->index(2, 0);
                check(!GroupListDelegate::canMoveUp(idxFirst) &&
                          GroupListDelegate::canMoveDown(idxFirst) &&
                          GroupListDelegate::canMoveUp(idxMiddle) &&
                          GroupListDelegate::canMoveDown(idxMiddle) &&
                          GroupListDelegate::canMoveUp(idxLast) &&
                          !GroupListDelegate::canMoveDown(idxLast),
                      "第一条只能往下、中间那条两颗都有、最后一条只能往上");
                check(!GroupListDelegate::canMoveUp(QModelIndex()) &&
                          !GroupListDelegate::canMoveDown(QModelIndex()),
                      "无效的 index（点在空白处）两边都挪不动");
                check(GroupListDelegate::showsMoveBox(idxFirst) &&
                          GroupListDelegate::showsMoveBox(idxMiddle) &&
                          GroupListDelegate::showsMoveBox(idxLast) &&
                          !GroupListDelegate::showsMoveBox(QModelIndex()),
                      "三条都画那个方框 —— 第一条只有下移、最后一条只有上移，但框照画"
                      "（画的是「这两格在哪儿」，不是「两颗都在」）");
                // 表里只剩一条分组时：整块**不画**（一个空框看着像坏了），而且那一带也
                // 不认、不弹（判据是同一个 showsMoveBox()，画和点不会错开）。
                {
                    QListWidget lone;
                    new QListWidgetItem(QStringLiteral("唯一的一条"), &lone);
                    const QModelIndex only = lone.model()->index(0, 0);
                    const QRect loneRow(0, 0, 320, MainWindowUI::versionTreeRowHeight());
                    check(!GroupListDelegate::showsMoveBox(only) &&
                              GroupListDelegate::rowActionTip(
                                  only, loneRow,
                                  GroupListDelegate::moveUpRect(loneRow).center()).isEmpty(),
                          "★ 表里只剩一条分组时那个方框整块不画（它本来在的那一格也不弹"
                          "话、点下去也不该被吃掉）");
                }

                // ④ 画哪一颗就是那一对函数说了算（量像素，不看代码）：第一条只有下移那一格
                // 有墨、上移那一格一个像素都没有 —— 挪不动的一侧是**不画**，不是画成灰的。
                // 开窗默认选中第一组（步骤 137）：这里量的是"画哪一颗"，不是选中配色 ——
                // 这个测试没套主题，选中那一档的墨是按深色底配的，压在未套主题的浅底上量不出来。
                glist->clearSelection();
                const QImage listShot = captureWidget(glist->viewport());
                // 底色按**整行数票**取多数色（dominantColor()），不取某一个像素：
                // 这张表上还有别的东西会改那一片的颜色 —— 第 0 行挂着"当前项"那一圈
                // （这张表一建出来 currentRow() 就是 0）、行里还有组名和那两颗三角。
                // 取一个点的话，点落在哪一样上量出来的结论就跟着翻（ 日志窗
                // 那几条底色断言踩过同一个坑）。
                const auto inkIn = [&listShot](const QRect &drawn, const QRect &cell) {
                    // 基准是**画出来的那一块**的多数色，不是整条条目矩形：条目底下那条缝是
                    // 视口的白底，把它一起数进去的话白底会赢得「多数色」（那道渐变一行一个色、
                    // 每种色只有一行那么多，散在直方图里），于是整片渐变都被算成墨（实测
                    // 那一格从 0 px 变成满格 144 px）。
                    const QColor bg = dominantColor(listShot, drawn);
                    const QRect clip = cell.intersected(listShot.rect());
                    int n = 0;
                    for (int y = clip.top(); y <= clip.bottom(); ++y) {
                        for (int x = clip.left(); x <= clip.right(); ++x) {
                            // 判据是"**看得出来不一样**"，不是"逐位相等"：这张表上第 0 行
                            // 是**当前项**那一行，底色是**渐变**的（量到 e7f3fb → e7f2f9，
                            // 整个区块的多数色是 e6f0f6）—— 逐位比会把整格都算成墨。
                            // 漂移是 10 上下、那两颗三角是 300 上下（0x7f7f7f 压在白底上），
                            // 24 这一刀两边都够远（和日志窗那几条底色断言同一个量法）。
                            if (colorDistance(QColor(listShot.pixel(x, y)), bg) > 24) {
                                ++n;
                            }
                        }
                    }
                    return n;
                };
                check(inkIn(cFirst, GroupListDelegate::moveUpGlyphRect(rFirst)) == 0 &&
                          inkIn(cFirst, GroupListDelegate::moveDownGlyphRect(rFirst)) > 0,
                      "★ 第一条只画**下移**那一颗（上移那一格一个像素的墨都没有）",
                      QStringLiteral("上格 %1 px / 下格 %2 px")
                          .arg(inkIn(cFirst, GroupListDelegate::moveUpGlyphRect(rFirst)))
                          .arg(inkIn(cFirst, GroupListDelegate::moveDownGlyphRect(rFirst))));
                // 步骤 137：那圈原生线框不画了 —— 框左沿那一列一个像素的墨都没有
                // （三角照画，上面那条已经量过）。
                {
                    const QRect frameProbe(boxFirst.left(), boxFirst.top(), 1,
                                           boxFirst.height());
                    check(inkIn(cFirst, frameProbe) == 0,
                          "★ 行尾两颗三角外面不再画线框（框左沿那一列没有墨）",
                          QStringLiteral("框沿 %1 px").arg(inkIn(cFirst, frameProbe)));
                }
                check(inkIn(drawnRect(2), GroupListDelegate::moveUpGlyphRect(rowRect(2))) > 0 &&
                          inkIn(drawnRect(2), GroupListDelegate::moveDownGlyphRect(rowRect(2))) ==
                              0,
                      "★ 最后一条反过来：只有上移那一颗",
                      QStringLiteral("上格 %1 px / 下格 %2 px")
                          .arg(inkIn(drawnRect(2),
                                     GroupListDelegate::moveUpGlyphRect(rowRect(2))))
                          .arg(inkIn(drawnRect(2),
                                     GroupListDelegate::moveDownGlyphRect(rowRect(2)))));
                check(inkIn(drawnRect(1), GroupListDelegate::moveUpGlyphRect(rowRect(1))) > 0 &&
                          inkIn(drawnRect(1), GroupListDelegate::moveDownGlyphRect(rowRect(1))) >
                              0,
                      "中间那条两颗都画");

                // ⑤ 提示语：挂在**那两格**上，而且只有真画了东西的那一侧才说话。
                const QRect rMiddle0 = rowRect(1);
                const QString upTip = GroupListDelegate::rowActionTip(
                    idxMiddle, rMiddle0, GroupListDelegate::moveUpRect(rMiddle0).center());
                const QString downTip = GroupListDelegate::rowActionTip(
                    idxMiddle, rMiddle0, GroupListDelegate::moveDownRect(rMiddle0).center());
                check(upTip.contains(QStringLiteral("Shift")) &&
                          downTip.contains(QStringLiteral("Shift")),
                      "行尾那两格的悬停提示都写明了「按住 Shift = 直接放到最上/最下面」",
                      upTip + QStringLiteral(" / ") + downTip);
                check(GroupListDelegate::rowActionTip(
                          idxFirst, rFirst, GroupListDelegate::moveUpRect(rFirst).center())
                              .isEmpty() &&
                          GroupListDelegate::rowActionTip(
                              idxMiddle, rMiddle0,
                              QPoint(rMiddle0.left() + 8, rMiddle0.center().y()))
                              .isEmpty(),
                      "挪不动的那一侧不弹提示（没画东西的一格说「点我挪一格」是骗人），"
                      "一行字那一带上也不弹");

                // ⑥ 手势：一次一格、按住 Shift 直接到最上/最下、连点每一发都算，而且那一下
                // **不会**顺手把这一行选中。
                const auto sendMouse = [glist](const QPoint &pos, QEvent::Type type,
                                               Qt::KeyboardModifiers mods) {
                    QMouseEvent event(type, QPointF(pos),
                                      QPointF(glist->viewport()->mapToGlobal(pos)),
                                      Qt::LeftButton, Qt::LeftButton, mods);
                    QApplication::sendEvent(glist->viewport(), &event);
                    QApplication::processEvents();
                };
                // 【"那一发有没有到这一行上"怎么量：数 QListWidget::itemDoubleClicked，不数框】
                //
                // 双击一行 = 重命名（产品那一条是"按一下「重命名」那颗按钮"，一按就弹一个模态
                // 的 QInputDialog）。**不能拿"框弹出来了没有"当判据**：offscreen 上
                // QApplication::activeModalWidget() 恒是空的（它先看 active_window，而这里没有
                // 任何窗口是"活动"的 —— 量到 activeWindow=0x0），照它判的话"框弹了"和"框没弹"
                // 看起来一模一样（实测：框真弹了，两条定时器都报 nullptr，最后是那个框自己
                // 在 ~287 ms 上关掉的，判据还是"没看见"）。
                //
                // 所以直接量**事件到没到这一行上**：接一条 itemDoubleClicked 数一发。为了让
                // 这一数不被"接着就弹一个框"挡住，量之前先把那颗按钮**关掉**（产品那一条是
                // `if (m_renameButton->isEnabled()) 按一下` —— 关掉它，这一发就纯粹是"到没到"，
                // 弹框那一段和这里要验的事无关）。量完马上恢复。
                int dblEmitted = 0;
                QObject::connect(glist, &QListWidget::itemDoubleClicked, glist,
                                 [&dblEmitted] { ++dblEmitted; });
                QPushButton *renameButton =
                    arrowDialog.findChild<QPushButton *>(GroupDialog::renameGroupButtonName());
                check(renameButton != nullptr,
                      "这一段的前提：找得到那颗「重命名」按钮（哨兵要靠它把弹框那一段摁住）");

                // ⑥a 哨兵：在**一行字**上双击，那一发必须到这一行上（量法本身有效）。
                glist->setCurrentRow(0);
                if (renameButton != nullptr) {
                    renameButton->setEnabled(false);
                }
                dblEmitted = 0;
                sendMouse(QPoint(rowRect(0).left() + 8, rowRect(0).center().y()),
                          QEvent::MouseButtonPress, Qt::NoModifier);
                sendMouse(QPoint(rowRect(0).left() + 8, rowRect(0).center().y()),
                          QEvent::MouseButtonDblClick, Qt::NoModifier);
                check(dblEmitted == 1,
                      "哨兵：在一行**字**上双击，那一发真的到了这一行上（下面那几条「双击行尾那"
                      "两格不算数」的量法本身有效）",
                      QStringLiteral("收到 %1 发").arg(dblEmitted));
                if (renameButton != nullptr) {
                    renameButton->setEnabled(true);
                }

                // ⑥b 第一条那一侧的三角**画都没画**：点上去什么都不做，而且这一下**被整个
                // 吃掉**（不吃的话会顺手把第一行选中）。
                glist->setCurrentRow(2);
                sendMouse(GroupListDelegate::moveUpRect(rowRect(0)).center(),
                          QEvent::MouseButtonPress, Qt::NoModifier);
                sendMouse(GroupListDelegate::moveUpRect(rowRect(0)).center(),
                          QEvent::MouseButtonRelease, Qt::NoModifier);
                check(arrowDialog.groups().at(0).id == packId &&
                          arrowDialog.groups().at(1).id == modId,
                      "第一条那一侧点上去不挪（组序原样）", orderNow());
                check(glist->currentRow() == 2 && selectedId() == shaderId,
                      "★ 而且那一下**被整个吃掉了**：选中项还停在原来那一行 —— 漏出去的话这一下"
                      "会顺手把第一行选中（「点击一下就同时展开和调整了」"
                      "就是这个形状）",
                      QStringLiteral("选中第 %1 行 / %2").arg(glist->currentRow()).arg(selectedId()));

                // ⑥c 真点一下「下移」：它和下一个邻居换一格。选中项跟着**那一组**走。
                sendMouse(GroupListDelegate::moveDownRect(rowRect(0)).center(),
                          QEvent::MouseButtonPress, Qt::NoModifier);
                sendMouse(GroupListDelegate::moveDownRect(rowRect(0)).center(),
                          QEvent::MouseButtonRelease, Qt::NoModifier);
                check(orderNow() == QStringLiteral("模组 / 整合包 / 光影"),
                      "点第一条的「下移」：它和下一个邻居换了一格", orderNow());
                check(selectedId() == shaderId,
                      "★ 那一行挪走之后选中项跟着**那一组**走（按 id 找回来，不是按行号），"
                      "也没有被这一下点去别处",
                      selectedId());

                // ⑥d 按住 Shift 点「下移」= 直接放到最下面（一步到位，不是换一格）。
                sendMouse(GroupListDelegate::moveDownRect(rowRect(0)).center(),
                          QEvent::MouseButtonPress, Qt::ShiftModifier);
                check(orderNow() == QStringLiteral("整合包 / 光影 / 模组"),
                      "★ 按住 Shift 点「下移」：「模组」直接到了最底下（换一格的话它只在第二条）",
                      orderNow());
                // ⑥e 反过来：Shift +「上移」把最后一条直接放到最上面。
                sendMouse(GroupListDelegate::moveUpRect(rowRect(2)).center(),
                          QEvent::MouseButtonPress, Qt::ShiftModifier);
                check(orderNow() == QStringLiteral("模组 / 整合包 / 光影"),
                      "★ 按住 Shift 点「上移」：「模组」又从最底下直接到了最上面",
                      orderNow());

                // ⑥f 点得快的时候第二发到的是**双击**事件：两发都要算，而且那两发**一发都没
                // 有漏到这一行上**（漏了的话这一行会被选中、双击还会弹重命名框）。
                // 同一格上连点两下：第一发把这一行那一组挪下去，第二发落在换上来那一组
                // 的同一格上，两下正好换回来 —— 少算一发就会停在「模组 / 光影 / 整合包」。
                const QPoint fastPos = GroupListDelegate::moveDownRect(rowRect(1)).center();
                dblEmitted = 0;
                sendMouse(fastPos, QEvent::MouseButtonPress, Qt::NoModifier);
                sendMouse(fastPos, QEvent::MouseButtonDblClick, Qt::NoModifier);
                check(orderNow() == QStringLiteral("模组 / 整合包 / 光影"),
                      "★ 连点两下（第二发是双击事件）两发都算：先把它挪下去、再落到换上来那一组"
                      "的同一格上，两下正好换回来",
                      orderNow());
                check(dblEmitted == 0,
                      "★ 而且那两发**都没有漏到这一行上**（行尾那两格上的双击不算数 —— "
                      "用户连点两下想挪两格，中间弹出个重命名框、或者顺手把这一行选中，"
                      "都是错的）",
                      QStringLiteral("收到 %1 发").arg(dblEmitted));

                // ⑥g 反面：那两格**之外**的地方一个字节都没动（点一行字照样选中它）。
                sendMouse(QPoint(rowRect(1).left() + 8, rowRect(1).center().y()),
                          QEvent::MouseButtonPress, Qt::NoModifier);
                sendMouse(QPoint(rowRect(1).left() + 8, rowRect(1).center().y()),
                          QEvent::MouseButtonRelease, Qt::NoModifier);
                check(glist->currentRow() == 1,
                      "点在那一行**字**上照样把它选中（被吃掉的只有行尾那两格）",
                      QString::number(glist->currentRow()));

                // ⑦ 表里**只剩一条分组**那一档：整块既不画、也不认。上面那几条都在三条的
                // 表上量的，而三条的表里 showsMoveBox() 恒为真 —— 代理里那一句、
                // eventFilter() 里那一句，谁都验不到。这一节另开一张一条的窗，两边都量。
                {
                    using LGroup = GroupDialog::Group;
                    QList<LGroup> lone;
                    lone.append({packId, QStringLiteral("整合包"), true});
                    QHash<QString, int> loneCounts;
                    loneCounts.insert(packId, 3);
                    GroupDialog loneDialog(lone, loneCounts, defaults, defaultCounts);
                    loneDialog.show();
                    QApplication::processEvents();
                    auto *llist =
                        loneDialog.findChild<QListWidget *>(GroupDialog::groupListName());
                    check(llist != nullptr && llist->count() == 1,
                          "⑦ 前提：另开的那张分组设置里只有一条分组",
                          QString::number(llist != nullptr ? llist->count() : -1));
                    if (llist != nullptr && llist->count() == 1) {
                        const QRect loneRow = llist->visualItemRect(llist->item(0));
                        const QRect loneBox = GroupListDelegate::moveBoxRect(loneRow);
                        const QRect loneDrawn =
                            GroupListDelegate::contentRect(loneRow);
                        const QImage loneShot = captureWidget(llist->viewport());
                        int boxInk = 0;
                        const QColor loneBg = dominantColor(loneShot, loneDrawn);
                        const QRect loneClip = loneBox.intersected(loneShot.rect());
                        for (int y = loneClip.top(); y <= loneClip.bottom(); ++y) {
                            for (int x = loneClip.left(); x <= loneClip.right(); ++x) {
                                if (colorDistance(QColor(loneShot.pixel(x, y)), loneBg) > 24) {
                                    ++boxInk;
                                }
                            }
                        }
                        check(boxInk == 0,
                              "★ 只剩一条分组时那个方框**不画**（本来就挪不动，画个空框看着像"
                              "坏了）—— 方框该在的那一整块一个像素的墨都没有",
                              QStringLiteral("框里 %1 px").arg(boxInk));
                        // 而且那一带**不吃**：点上去就是普通的一行（这一下要把它选中）。
                        llist->setCurrentRow(-1);
                        QPoint lonePos = GroupListDelegate::moveUpRect(loneRow).center();
                        for (QEvent::Type t : {QEvent::MouseButtonPress,
                                               QEvent::MouseButtonRelease}) {
                            QMouseEvent ev(t, QPointF(lonePos),
                                           QPointF(llist->viewport()->mapToGlobal(lonePos)),
                                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                            QApplication::sendEvent(llist->viewport(), &ev);
                            QApplication::processEvents();
                        }
                        check(llist->currentRow() == 0,
                              "★ 而且那一带**不吃**鼠标：只剩一条分组时它不是按钮，点上去照样"
                              "把这一行选中（eventFilter() 里那句 showsMoveBox() 的守卫）",
                              QStringLiteral("选中第 %1 行").arg(llist->currentRow()));
                    }
                }
            }
        }

        // ---- 行高：和版本下载那棵树一样（要求「分组设置界面也是」），再加一条缝 ----
        //
        // 条目矩形 = versionTreeRowHeight() + rowGap()；**画出来的那一块**仍然是那个常数
        // （代理按 contentRect() 收掉底下那一条）。两条一起量：只量前者的话，代理把整条
        // 条目铺满（缝白留）也能绿。
        const int listRowHeight =
            MainWindowUI::versionTreeRowHeight() + GroupListDelegate::rowGap();
        check(list != nullptr && list->count() == 3 &&
                  list->item(0)->sizeHint().height() == listRowHeight &&
                  list->item(1)->sizeHint().height() == listRowHeight &&
                  list->item(2)->sizeHint().height() == listRowHeight,
              "列表每一条的行高都是 versionTreeRowHeight() + rowGap()（底下那条缝）",
              QStringLiteral("item0=%1 要=%2")
                  .arg(list != nullptr && list->item(0) != nullptr
                           ? list->item(0)->sizeHint().height()
                           : -1)
                  .arg(listRowHeight));
        check(list != nullptr && list->item(0) != nullptr &&
                  GroupListDelegate::contentRect(QRect(0, 0, 100,
                                                 list->item(0)->sizeHint().height()))
                          .height() == MainWindowUI::versionTreeRowHeight(),
              "★ 而且画出来的那一块仍然是 versionTreeRowHeight()（多出来的那一条是缝，"
              "不是把行撑高了）",
              QString::number(list == nullptr || list->item(0) == nullptr
                                  ? -1
                                  : GroupListDelegate::contentRect(
                                        QRect(0, 0, 100,
                                              list->item(0)->sizeHint().height()))
                                        .height()));
        check(list != nullptr && list->sizeHintForRow(0) == listRowHeight,
              "真的画出来也是这么高（不只是 item 上挂着那个数）",
              QString::number(list == nullptr ? -1 : list->sizeHintForRow(0)));

        // ---- 「默认展开/关闭」：勾选框跟着**选中的那一组**走 ----
        //
        // 这里真正要钉住的是那个守卫（m_updatingExpanded）：摆勾选框的那一下会发一次
        // toggled，那一次**不能**被当成用户点的 —— 否则"看一眼别的组"就会把上一个组的
        // 状态改掉，而用户什么都没做。
        if (list != nullptr) {
            list->setCurrentRow(rowOfGroup(modId));
        }
        check(expandedBox != nullptr && !expandedBox->isChecked(),
              "选中「模组」（它是收着的）→ 勾选框空着");
        check(dialog.groups().at(rowOfGroup(modId)).expanded == false,
              "只是**看**了一眼别的组，谁的状态都没被改掉（摆勾选框那一下不算用户操作）");
        if (list != nullptr) {
            list->setCurrentRow(rowOfGroup(packId));
        }
        check(expandedBox != nullptr && expandedBox->isChecked(),
              "切回「整合包」（它展开着）→ 勾选框又亮着");
        check(dialog.groups().at(rowOfGroup(packId)).expanded,
              "来回换选中的组，两边的状态各自原样");

        if (expandedBox != nullptr) {
            expandedBox->setChecked(false);  // = 用户点了它
        }
        check(dialog.groups().at(rowOfGroup(packId)).expanded == false,
              "点了勾选框：选中的那一组变成收着的");
        check(dialog.groups().at(rowOfGroup(modId)).expanded == false &&
                  dialog.groups().at(rowOfGroup(newId)).expanded,
              "只动选中那一组（别的组的状态不受影响）");
        if (expandedBox != nullptr) {
            expandedBox->setChecked(true);
        }
        check(dialog.groups().at(rowOfGroup(packId)).expanded, "再点一次能切回来");

        // ---- 删组 ----
        check(!dialog.deleteGroup(QStringLiteral("没有这个组")), "删一个不存在的组返回 false");
        const int beforeDelete = dialog.groups().size();
        check(dialog.deleteGroup(modId), "删掉一个组");
        check(dialog.groups().size() == beforeDelete - 1, "组清单少一个");
        check(rowOfGroup(modId) == -1, "列表里也没有它那一行了");
        check(dialog.deleteGroup(modId) == false, "同一个组删两次，第二次返回 false");
        check(dialog.groups().size() == beforeDelete - 1, "第二次没删掉别的东西");

        // ---- 「恢复默认分组设置…」：那颗按钮**先问一句** ----
        // 这一趟点的是**第二颗**按钮（「否」）⇒ 清单必须原样。
        {
            GroupDialog ask(groups, counts, defaults, defaultCounts);
            putInOverlay(&ask);  // 问话也开在覆盖层里，同上
            auto *restore =
                ask.findChild<QPushButton *>(GroupDialog::restoreDefaultsButtonName());
            dismissNextInline(&overlayHost, 1);  // 1 = 第二颗 = 「否」
            if (restore != nullptr) {
                restore->click();
            }
            check(ask.groups() == groups,
                  "答「否」的时候一个组都没换（问过才做，取消 = 磁盘和清单都原样）");
            check(g_modalText.contains(QStringLiteral("收藏")),
                  "那个框里点名说了会换成哪几个默认分组", g_modalText);
        }

        // 直接驱动那一半（产品里走的是上面那个确认框，确认之后调的就是它）。
        const QList<Group> beforeRestore = dialog.groups();
        check(dialog.restoreDefaults(), "恢复默认分组设置");
        check(dialog.groups() == defaults, "组清单换成五个默认分组（顺序也照默认那份）");
        check(dialog.groups() != beforeRestore, "对照：换之前确实是另一份（这条防的是测试自己写错）");
        check(list != nullptr && list->count() == 5, "列表里五条",
              QString::number(list != nullptr ? list->count() : -1));
        check(list != nullptr && list->item(0) != nullptr &&
                  list->item(0)->text() ==
                      GroupDialog::groupItemText(QStringLiteral("收藏"), 0) &&
                  list->item(2) != nullptr &&
                  list->item(2)->text() ==
                      GroupDialog::groupItemText(QStringLiteral("原版"), 2),
              "实例数跟着换成**恢复之后**那几个（进参里给的 defaultCounts），"
              "不是原来那些组里的数字",
              list != nullptr && list->item(2) != nullptr ? list->item(2)->text() : QString());

        // ---- 确定 / 取消 ----
        {
            GroupDialog acceptDialog(groups, counts, defaults, defaultCounts);
            auto *confirm =
                acceptDialog.findChild<QPushButton *>(GroupDialog::confirmButtonName());
            auto *cancel = acceptDialog.findChild<QPushButton *>(GroupDialog::cancelButtonName());
            if (confirm != nullptr) {
                confirm->click();
            }
            check(acceptDialog.result() == QDialog::Accepted, "「确定」接的是 accept()");
            if (cancel != nullptr) {
                cancel->click();
            }
            check(acceptDialog.result() == QDialog::Rejected, "「取消」接的是 reject()");
        }
    }
 return 0;
}


