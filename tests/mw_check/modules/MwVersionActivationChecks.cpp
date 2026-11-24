#include "Support.hpp"
int MwVersionActivationChecks(MainWindow &window, QTreeWidget * &versionTree, QStackedWidget * &gameStack, QLineEdit * &installNameEdit, QLabel * &installTargetLabel, QPushButton * &installBackBtn) {
    // ------------------------------------------------------------------
    // 要求：「四个分类（正式版 快照版 远古版 愚人节版），每一个占一条，点开后展开
    // 对应版本，再点击具体版本后进入一个新页面」。这一节钉的是**两件事之间的那条线**：
    // · 点**分类**（顶层条目）→ 只是展开/折叠，**不许**翻到安装页；
    // · 点**版本号**（子条目）→ 翻到安装页，而且那一份是"基于它装"。
    // 前者错了用户会莫名其妙进到安装页、以为自己点错了；后者错了就是"点了没反应"
    // —— 两种都不会报错，只会让人以为启动器坏了。
    {
        // 四个名字是用户点名的（正式版 / 快照版 / 远古版 / 愚人节版）。这里**写死**着比：
        // 产品自己那份分类表（VersionCategory）要数 915 条清单才分得出来，那份数据的正确性
        // 由 verify 里的真清单钉住，这里钉的是"窗上的四条就是那四个名字、顺序也是这个顺序"。
        const QStringList expectedChannelTexts = {QStringLiteral("正式版"), QStringLiteral("快照版"),
                                                  QStringLiteral("远古版"),
                                                  QStringLiteral("愚人节版")};
        const QStringList expectedTexts =
            QStringList{VersionManager::latestGroupText()} + expectedChannelTexts;
        check(versionTree != nullptr && gameStack != nullptr,
              "版本树和两层堆叠都找得到（这条是下面那些的前提）");

        // 下面要借加载器那棵树（验"进安装页会把选择清回不装"）。Y1/Y2 段已经验过它的
        // 条数、顺序和归属，这里只是按 objectName 把它再拿回来用一次。
        auto *loaderTree = byObjectName<QTreeWidget>(&window, MainWindowUI::loaderTreeName());
        const int kForgeRow = 1;  // Y1 段钉过：第 1 条是 Forge
        const int kNoneRow = MainWindowUI::modLoaderNoneIndex();
        const auto loaderRow = [loaderTree](int index) {
            const int row = index >= 0 && index < MainWindowUI::modLoaderNoneIndex() ? index
                                                                                      : index + 1;
            return loaderTree != nullptr && index >= 0 && row < loaderTree->topLevelItemCount()
                       ? loaderTree->topLevelItem(row)
                       : nullptr;
        };
        const auto loaderSubText = [](QTreeWidgetItem *item) {
            return item != nullptr
                       ? item->data(MainWindowUI::loaderTreeKeyColumn(),
                                    MainWindowUI::rowSubTextRole())
                             .toString()
                       : QString();
        };
        check(loaderRow(kForgeRow) != nullptr && loaderRow(kNoneRow) != nullptr,
              "加载器那棵树拿得回来（下面借它验「进安装页会清掉选择」）");
        // 选中一条加载器（走真信号路）。 起**点顶层那一条只管展开/收起**，
        // 选中是"点它底下的版本号"那一下的事 —— 所以这里得先有一条版本号可点：
        // 那一条底下有能点的条目就用真的，没有（离线、或者列表还没回来）就临时插一条
        // 「自动（推荐）」进去，点完再摘掉。插进去那条走的是**和真版本号完全相同的
        // 信号路**（itemClicked → 产品那个共用 lambda），所以判据不会因此变松。
        const auto selectLoaderRow = [&window, loaderTree, &loaderRow](int index) {
            QTreeWidgetItem *row = loaderRow(index);
            if (row == nullptr) {
                return;
            }
            QTreeWidgetItem *target = nullptr;
            for (int i = 0; i < row->childCount(); ++i) {
                // 「正在获取…」/「列表没取到」那两个占位是灰的、点不动 —— 跳过它们。
                if (!row->child(i)->isDisabled()) {
                    target = row->child(i);
                    break;
                }
            }
            const bool synthetic = target == nullptr;
            if (synthetic) {
                target = new QTreeWidgetItem(row);
                target->setText(MainWindowUI::loaderTreeKeyColumn(),
                                MainWindowUI::loaderVersionStableText());
                target->setData(MainWindowUI::loaderTreeKeyColumn(), Qt::UserRole,
                                LoaderMeta::loaderChannelKey(LoaderMeta::LoaderChannel::Stable));
            }
            QMetaObject::invokeMethod(loaderTree, "itemClicked", Qt::DirectConnection,
                                      Q_ARG(QTreeWidgetItem *, target), Q_ARG(int, 0));
            QApplication::processEvents();
            if (synthetic) {
                row->removeChild(target);
                delete target;
            }
        };

        if (versionTree != nullptr && gameStack != nullptr) {            // 先让树按"清单到了（空的）"刷一次 —— 真窗口上这一步是 onVersionsFetched()
            // 干的（清单到达 / 失败都会走它）。直接调它，是因为"清单真的到了"要联网。
            QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                      Q_ARG(QStringList, QStringList()));
            QApplication::processEvents();

            check(versionTree->topLevelItemCount() == expectedTexts.size(),
                  "树上正好五条顶层（「最新版本」那一组 + 四条分类，一条不少也不多）",
                  QString::number(versionTree->topLevelItemCount()));

            int distinctKeys = 0;
            int indicatorPoisoned = 0;
            int hasChildrenAgrees = 0;
            int withVersions = 0;
            int versionRows = 0;
            // 「最新版本」那一组的孩子**单独记**：它们本来就该和"自己那一栏里那一条"重名
            // （同一个版本号在树上有两份），不能并进下面那笔"版本号不重名"的账里。
            int latestRows = 0;
            QStringList treeTexts;
            QSet<QString> keys;
            QSet<QString> versionIds;
            QSet<QString> latestIds;
            for (int i = 0; i < versionTree->topLevelItemCount(); ++i) {
                QTreeWidgetItem *top = versionTree->topLevelItem(i);
                if (top == nullptr) {
                    treeTexts << QStringLiteral("<null>");
                    continue;
                }
                treeTexts << top->text(MainWindowUI::versionTreeKeyColumn());
                const QString key =
                    top->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toString();
                if (!key.isEmpty()) {
                    keys.insert(key);
                }
                // 展开箭头那一格：分类**有**版本时右边该有箭头（能点开）、**没有**时
                // 一个都不许画（画了就骗人说点得开）—— 那是代理画的，由 ui_check 按像素管。
                // 这里管的是"分类条目在模型层没有说谎"：
                // · 分类一律**不许**被设成 DontShowIndicator（左边那个三角的开关）。
                // 坑在这里：想去掉左边那个三角就写了它，而
                // QTreeModel::hasChildren() 是按 policy 直接 return false 的 ——
                // 那个分类的版本号**一条都排不进布局**，界面上只剩四行空的分类，
                // 点开也没东西，而所有断言（身份、条数、itemClicked）照样全绿。
                // 去掉三角的正路是 versionTree->setRootIsDecorated(false)。
                // · 有没有孩子这件事，模型层说的必须和条目上实际的 childCount() 一致。
                if (top->childIndicatorPolicy() == QTreeWidgetItem::DontShowIndicator) {
                    ++indicatorPoisoned;
                }
                const QModelIndex topIndex = versionTree->model()->index(i, 0);
                if (versionTree->model()->hasChildren(topIndex) == (top->childCount() > 0)) {
                    ++hasChildrenAgrees;
                }
                const bool hasVersions = top->childCount() > 0;
                const bool isLatestRow = (i == 0);
                if (hasVersions && isLatestRow) {
                    for (int j = 0; j < top->childCount(); ++j) {
                        QTreeWidgetItem *child = top->child(j);
                        if (child == nullptr) {
                            continue;
                        }
                        ++latestRows;
                        const QString id =
                            child->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole)
                                .toString();
                        if (!id.isEmpty()) {
                            latestIds.insert(id);
                        }
                    }
                } else if (hasVersions) {
                    ++withVersions;
                    for (int j = 0; j < top->childCount(); ++j) {
                        QTreeWidgetItem *child = top->child(j);
                        if (child == nullptr) {
                            continue;
                        }
                        ++versionRows;
                        const QString id =
                            child->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole)
                                .toString();
                        if (!id.isEmpty()) {
                            versionIds.insert(id);
                        }
                    }
                }
            }
            distinctKeys = keys.size();
            check(treeTexts == expectedTexts,
                  "五条依次是 最新版本 / 正式版 / 快照版 / 远古版 / 愚人节版",
                  treeTexts.join(QLatin1Char('/')));
            // 身份在 userData 上、而且五条互不相同：拿条目文字当身份的话，改一个字的说法
            // 就会把"记住哪条是展开的"那件事打断（refreshAvailableVersions 就是按它记的）。
            // NOTE: 那五条住的是**同一个角色**，所以「最新版本」的键和四个 channelKey() 撞了
            // 的话，这里就会少一格（而症状是"展开状态串到另一条上"，很难认）。
            check(distinctKeys == expectedTexts.size(),
                  "五条的身份（userData）都非空且互不相同",
                  QString::number(distinctKeys));

            // 「最新版本」那一组（要求见上面 expectedTexts 那一段）。
            // 这一节**不挑环境** —— 清单没拉回来时那一组照旧在，只是里面空的（和四条空分类
            // 同一个规矩），所以下面凡是要有版本号才成立的那几条都只在"里面真有东西"时跑。
            const QString latestKey = VersionManager::latestGroupKey();
            {
                const auto latestRow = [versionTree]() -> QTreeWidgetItem * {
                    return versionTree != nullptr && versionTree->topLevelItemCount() > 0
                               ? versionTree->topLevelItem(0)
                               : nullptr;
                };
                QTreeWidgetItem *latestTop = latestRow();
                check(latestTop != nullptr &&
                          latestTop->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole)
                                  .toString() == latestKey,
                      "「最新版本」是树上的**第一条**，身份是它自己的键（不是某一个分类的）",
                      latestTop != nullptr
                          ? latestTop->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole)
                                .toString()
                          : QStringLiteral("<没有第 0 条>"));
                check(latestTop != nullptr && latestTop->isExpanded(),
                      "「最新版本」那一组**默认展开**（用户原话：「默认展开」）");
                check(latestRows <= 2,
                      "那一组里最多两条（一个最新正式版、一个最新快照）",
                      QString::number(latestRows));

                // 那一组里的每一条，在它**自己那一栏**里也必须有一条同名版本 —— 两处各挑
                // 各的，早晚会出现"最新版本里写着 A、正式版那一栏里却没有 A"。顺带按它落在
                // 哪一栏推出它该长哪张图（正式版草方块 / 快照版覆雪草块）。
                int missingFromChannels = 0;
                int wrongMarkInGroup = 0;
                for (int j = 0; latestTop != nullptr && j < latestTop->childCount(); ++j) {
                    QTreeWidgetItem *child = latestTop->child(j);
                    if (child == nullptr) {
                        continue;
                    }
                    const QString id =
                        child->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toString();
                    int foundIn = -1;
                    for (int c = 0; c < expectedChannelTexts.size(); ++c) {
                        QTreeWidgetItem *channelTop = versionTree->topLevelItem(c + 1);
                        if (channelTop == nullptr) {
                            continue;
                        }
                        for (int k = 0; k < channelTop->childCount(); ++k) {
                            if (channelTop->child(k)
                                    ->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole)
                                    .toString() == id) {
                                foundIn = c;
                                break;
                            }
                        }
                        if (foundIn >= 0) {
                            break;
                        }
                    }
                    if (foundIn < 0) {
                        ++missingFromChannels;
                        continue;
                    }
                    const QImage expectedMark =
                        VersionIcons::versionChannelMark(foundIn, MainWindowUI::versionTreeIconSize())
                            .toImage();
                    const QPixmap pixmap =
                        child->data(MainWindowUI::versionTreeKeyColumn(), MainWindowUI::rowIconRole())
                            .value<QPixmap>();
                    if (pixmap.isNull() || pixmap.toImage() != expectedMark) {
                        ++wrongMarkInGroup;
                    }
                }
                check(missingFromChannels == 0,
                      "「最新版本」里那几条，在它自己那一栏里也各有一条同名版本"
                      "（两处是同一份数据，不是各挑各的）",
                      QString("%1 条在分类里找不到").arg(missingFromChannels));
                check(wrongMarkInGroup == 0,
                      "那一组里每条前面的图标按**它自己属于哪一类**给（正式版草方块 / 快照版覆雪草块）",
                      QString("%1 行不对").arg(wrongMarkInGroup));

                // 「默认展开」只摆一次：用户把它收起来之后，清单刷新（onVersionsFetched —— 每次
                // 启动那一次、以及手动刷新拉回来那一次）**不该**又把它撑开。这条和上一条是一对
                // —— 少了它，"每次刷新都强行展开"也能过上一条。
                // NOTE: 刷新会把树整个重灌，所以下面每量一次都要**重新把条目捞回来**（老的
                // QTreeWidgetItem* 那一瞬间就全成野指针了）。
                if (latestTop != nullptr) {
                    versionTree->collapseItem(latestTop);
                    QApplication::processEvents();
                    QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                              Q_ARG(QStringList, QStringList()));
                    QApplication::processEvents();
                    QTreeWidgetItem *afterCollapse = latestRow();
                    check(afterCollapse != nullptr && !afterCollapse->isExpanded(),
                          "用户把它收起来之后，清单刷新**不会**又把它撑开（默认展开只摆一次）",
                          afterCollapse == nullptr
                              ? QStringLiteral("<没有第 0 条>")
                              : (afterCollapse->isExpanded() ? QStringLiteral("展开着")
                                                             : QStringLiteral("收着")));
                    // 再撑开、再刷一次：用户自己展开的那一档得**留得住**（这条是上一条的另一半
                    // —— 少了它，一个"永远收着"的实现也能过上一条）。
                    if (afterCollapse != nullptr) {
                        versionTree->expandItem(afterCollapse);
                    }
                    QApplication::processEvents();
                    QMetaObject::invokeMethod(&window, "onVersionsFetched", Qt::DirectConnection,
                                              Q_ARG(QStringList, QStringList()));
                    QApplication::processEvents();
                    QTreeWidgetItem *afterExpand = latestRow();
                    check(afterExpand != nullptr && afterExpand->isExpanded(),
                          "用户自己展开着的那一档，清单刷新之后还是展开的",
                          afterExpand == nullptr
                              ? QStringLiteral("<没有第 0 条>")
                              : (afterExpand->isExpanded() ? QStringLiteral("展开着")
                                                           : QStringLiteral("收着")));
                }
            }
            check(!versionTree->rootIsDecorated(),
                  "树上左边那个系统三角不画（展开/收起的提示只在右边那个箭头上）");
            check(indicatorPoisoned == 0,
                  "四条分类里没有一条被设成 DontShowIndicator"
                  "（那个值会让 QTreeModel::hasChildren 直接返回 false，整条分类的版本号都排不进布局）",
                  QString("%1 条被设上了（共 %2 条）")
                      .arg(indicatorPoisoned)
                      .arg(versionTree->topLevelItemCount()));
            check(hasChildrenAgrees == versionTree->topLevelItemCount(),
                  "模型层说的「有没有孩子」和条目上实际的 childCount() 一致",
                  QString("%1/%2 条对上了（其中 %3 条有版本，共 %4 个版本号）")
                      .arg(hasChildrenAgrees)
                      .arg(versionTree->topLevelItemCount())
                      .arg(withVersions)
                      .arg(versionRows));
            if (withVersions > 0) {
                // 子条目也拿 userData（= 版本号）当身份：「点哪一条装哪一版」全看它，
                // 所以既不许有空身份、也不许两条重名。清单拉不到时这一段整段跳过
                // （那种环境下树上一个版本号都没有，这条断言没有可断言的对象）。
                // NOTE: 这一条**只算四条分类**（`versionRows`/`versionIds` 那一趟在收集时就把
                // 「最新版本」那一组跳过了）：那两条本来就该和"自己那一栏里那一条"重名
                // —— 同一个版本号在树上确实有两份，那不是缺陷（用户要的就是把它顶到最上面）。
                check(versionRows == versionIds.size(),
                      "四条分类里的版本号身份都非空、不重复（「最新版本」那一份是刻意重名的，不算）",
                      QString("%1 条，%2 个不同的身份").arg(versionRows).arg(versionIds.size()));

                // 版本号前面那个小图标**按分类**给（：「快照版的草用白色，
                // 远古版就土块，愚人节版用幸运方块，原版不变」）。这里钉的是**接线**：
                // 每条版本号身上的那个图，必须逐像素等于"它自己那一栏"该有的那张 ——
                // 分类下标的顺序就是界面上那四条的顺序（上面那条 treeTexts 已经钉过）。
                // 判据写成和 VersionIcons::versionChannelMark(i, …) 逐像素比，而不是
                // "非空就行"：把下标写歪（比如四条都传 0）、或者漏设某一条，都会在这里红。
                int rowsWithRightMark = 0;
                int wrongMark = 0;
                // NOTE: 四条分类现在是 `topLevelItem(1..4)` —— 第 0 条让给「最新版本」那一组了。
                // 这里的 `i` 同时是"分类下标"和"那一栏该长的图"，所以两条都得跟着挪一格。
                for (int i = 0; i < 4; ++i) {
                    QTreeWidgetItem *top = versionTree->topLevelItem(i + 1);
                    if (top == nullptr) {
                        continue;
                    }
                    const QImage expected =
                        VersionIcons::versionChannelMark(i, MainWindowUI::versionTreeIconSize())
                            .toImage();
                    for (int j = 0; j < top->childCount(); ++j) {
                        QTreeWidgetItem *child = top->child(j);
                        if (child == nullptr) {
                            continue;
                        }
                        const QVariant mark = child->data(MainWindowUI::versionTreeKeyColumn(),
                                                          MainWindowUI::rowIconRole());
                        const QPixmap pixmap = mark.value<QPixmap>();
                        if (pixmap.isNull() || pixmap.toImage() != expected) {
                            ++wrongMark;
                        } else {
                            ++rowsWithRightMark;
                        }
                    }
                }
                check(wrongMark == 0 && rowsWithRightMark > 0,
                      "版本号前面的图标按分类给对了（快照白草 / 远古土块 / 愚人节幸运方块 / "
                      "原版草方块）",
                      QStringLiteral("%1 行对、%2 行不对").arg(rowsWithRightMark).arg(wrongMark));

                // 上面那些都是**模型层**的事（数条目、看身份）—— 模型里有、界面上没有，
                // 是完全可能发生的：`DontShowIndicator` 那种写法就是"四条分类都在、
                // 900 多个版本号一条都排不进布局"，而上面每一条断言照样全绿。
                // 所以这里量一次**真的排进布局了没有**：翻开一条有版本的分类，
                // 子条目必须有一个落在视口里的矩形（看得见才点得到）。
                //
                // NOTE: 要先把下载页翻到前面、并把 gameStack 停到树那一层 —— 隐藏的
                // 控件不排布局，那种情况下量出来的空矩形说明不了任何事。
                QTabWidget *downloadTabs =
                    window.findChild<QTabWidget *>(MainWindowUI::tabWidgetName());
                const int tabBefore = downloadTabs != nullptr ? downloadTabs->currentIndex() : -1;
                const int stackBefore =
                    gameStack != nullptr ? gameStack->currentIndex() : -1;
                if (downloadTabs != nullptr) {
                    downloadTabs->setCurrentIndex(MainWindowUI::downloadTabIndex());
                }
                if (gameStack != nullptr) {
                    gameStack->setCurrentIndex(MainWindowUI::downloadGameTreeIndex());
                }
                QTreeWidgetItem *withKids = nullptr;
                for (int i = 0; i < versionTree->topLevelItemCount(); ++i) {
                    if (versionTree->topLevelItem(i)->childCount() > 0) {
                        withKids = versionTree->topLevelItem(i);
                        break;
                    }
                }
                const bool wasExpanded = withKids != nullptr && withKids->isExpanded();
                if (withKids != nullptr) {
                    versionTree->expandItem(withKids);
                }
                QApplication::processEvents();
                QTreeWidgetItem *firstChild = withKids != nullptr ? withKids->child(0) : nullptr;
                const QRect childRect =
                    firstChild != nullptr ? versionTree->visualItemRect(firstChild) : QRect();
                const QRect parentRect =
                    withKids != nullptr ? versionTree->visualItemRect(withKids) : QRect();
                check(firstChild != nullptr && !childRect.isEmpty() && childRect.height() > 0,
                      "展开分类之后，版本号那一条**真的排进了布局**（在视口里有个矩形）",
                      QString("%1x%2（分类那行 %3x%4）")
                          .arg(childRect.width())
                          .arg(childRect.height())
                          .arg(parentRect.width())
                          .arg(parentRect.height()));
                check(!childRect.isEmpty() && childRect.top() > parentRect.top(),
                      "版本号排在它那个分类**下面**（不是和分类叠在同一行上）",
                      QString("子 y=%1 / 分类 y=%2").arg(childRect.top()).arg(parentRect.top()));
                if (!wasExpanded && withKids != nullptr) {
                    versionTree->collapseItem(withKids);  // 还原成进来时那个样子
                }
                if (gameStack != nullptr && stackBefore >= 0) {
                    gameStack->setCurrentIndex(stackBefore);
                }
                if (downloadTabs != nullptr && tabBefore >= 0) {
                    downloadTabs->setCurrentIndex(tabBefore);
                }
                QApplication::processEvents();
            }

            // 点**顶层那几条**：只当展开/折叠，**不许**翻页。这条是那个守卫的全部意义 ——
            // 少了它，用户点一下"正式版"就进了一个写着"将基于原版 正式版 安装"的页面。
            // NOTE: 起顶层有五条（「最新版本」那一组 + 四条分类），两种各点一下
            // —— 那一组走的是**同一个** lambda（判据是"有没有爹"），它要是漏了，
            // 规律是"点最新版本进安装页、装一个叫 latest 的版本"。
            gameStack->setCurrentIndex(MainWindowUI::downloadGameTreeIndex());
            QApplication::processEvents();
            for (int row = 0; row < 2 && row < versionTree->topLevelItemCount(); ++row) {
                QTreeWidgetItem *top = versionTree->topLevelItem(row);
                const bool wasExpanded = top->isExpanded();
                QMetaObject::invokeMethod(versionTree, "itemClicked", Qt::DirectConnection,
                                          Q_ARG(QTreeWidgetItem *, top), Q_ARG(int, 0));
                QApplication::processEvents();
                check(gameStack->currentIndex() == MainWindowUI::downloadGameTreeIndex(),
                      QStringLiteral("点顶层第 %1 条（%2）不进安装页（它只是展开/折叠）")
                          .arg(row)
                          .arg(top->text(MainWindowUI::versionTreeKeyColumn())),
                      QString::number(gameStack->currentIndex()));
                check(top->isExpanded() != wasExpanded,
                      QStringLiteral("而且第 %1 条那一下**真的翻了**（展开 ↔ 收起）").arg(row),
                      top->isExpanded() ? QStringLiteral("展开着") : QStringLiteral("收着"));
                // 翻回去，别把上面那节（"默认展开"）摆好的状态带走。
                top->setExpanded(wasExpanded);
                QApplication::processEvents();
            }

            // 点**版本号**：翻到安装页，而且那一份是"基于它装"。优先点一条**真的**版本号
            // （清单拉到了树上就有）；清单没拉到的机器上，自己插一条假的进去
            // —— 插完走的是**和真的一条完全相同的信号路径**
            // （itemClicked → 那个共用 lambda → openInstallPage()）。
            // NOTE: 起这里点的是**第 0 条**（=「最新版本」那一组）底下的第一条，
            // 所以顺带也钉住了"那一组里那两条点得动"（在线的机器上跑的就是这一条）。
            if (versionTree->topLevelItemCount() > 0) {
                QTreeWidgetItem *top = versionTree->topLevelItem(0);
                QTreeWidgetItem *child = top->child(0);
                const bool synthetic = child == nullptr;
                if (synthetic) {
                    child = new QTreeWidgetItem(top);
                    child->setData(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole,
                                   QStringLiteral("1.21.4"));
                    child->setText(MainWindowUI::versionTreeKeyColumn(), QStringLiteral("1.21.4"));
                }
                const QString pickedId =
                    child->data(MainWindowUI::versionTreeKeyColumn(), Qt::UserRole).toString();
                check(!pickedId.isEmpty(), "点的那一条带着版本号（身份在 userData 上）", pickedId);

                // 先故意选上 Forge：下面那一问验的是**进安装页这一步自己会把选择清回
                // 「不装」**，而不是"上一节碰巧留对了"。上一个实例选的是哪条加载器跟
                // 这一个没关系，留着的话下一个版本会顶着一个"上次顺手点的 Forge"进来。
                selectLoaderRow(kForgeRow);
                check(!loaderSubText(loaderRow(kForgeRow)).isEmpty(),
                      "点版本号之前先选上 Forge（下面要看它被清掉）",
                      loaderSubText(loaderRow(kForgeRow)));

                QMetaObject::invokeMethod(versionTree, "itemClicked", Qt::DirectConnection,
                                          Q_ARG(QTreeWidgetItem *, child), Q_ARG(int, 0));
                QApplication::processEvents();
                check(gameStack->currentIndex() == MainWindowUI::downloadGameInstallIndex(),
                      "点一条**版本号**翻到安装页",
                      QString::number(gameStack->currentIndex()));
                check(installTargetLabel != nullptr &&
                          installTargetLabel->text().contains(pickedId),
                      "安装页顶上写着是基于哪个原版装的（点的是哪一个，看得见）",
                      installTargetLabel != nullptr ? installTargetLabel->text() : QString());
                check(installNameEdit != nullptr && installNameEdit->text() == pickedId,
                      "名称框里默认写的就是那个版本号（勾了 Forge 又被清掉，所以是原版号本身）",
                      installNameEdit != nullptr ? installNameEdit->text() : QString());
                check(loaderSubText(loaderRow(kNoneRow)).isEmpty() &&
                          loaderSubText(loaderRow(kForgeRow)).isEmpty(),
                      "进安装页之后加载器回到「不装」（不继承上一个实例的选择）",
                      loaderSubText(loaderRow(kForgeRow)).isEmpty()
                          ? QStringLiteral("不装")
                          : loaderSubText(loaderRow(kForgeRow)));

                // 收拾：翻回树那一层、把那条（要是假的）版本号拿掉
                // （后面几节拿窗口当开局那个样子用）。
                if (installBackBtn != nullptr) {
                    installBackBtn->click();
                    QApplication::processEvents();
                }
                if (synthetic) {
                    top->removeChild(child);
                    delete child;
                }
                check(gameStack->currentIndex() == MainWindowUI::downloadGameTreeIndex(),
                      "Y6 段收尾：回到树那一层");
            }
        }
    }
    return 0;
}


