#include "Support.hpp"
#include "MainWindowPageLayout.hpp"
int UiSettingsLayoutChecks(MainWindowUI &ui, QTabWidget * &tabs) {

    {
        tabs->setCurrentIndex(MainWindowUI::settingsTabIndex());
        QApplication::processEvents();
        QWidget *settingsPage = tabs->widget(MainWindowUI::settingsTabIndex());
        check(settingsPage != nullptr &&
                  settingsPage->objectName() == MainWindowUI::settingsTabName(),
              "最后一页就是设置页", settingsPage != nullptr ? settingsPage->objectName()
                                                             : QString());

        // 设置页 = 页头（标题 + 4 个分段页签 + 搜索）+ 四页堆叠，每页一层滚动区、一块最宽 720 居中的表单。
        // 第 N 个页签 = 堆叠页第 N 页；没有左侧索引栏，也没有二级目录。
        auto *settingsTabs =
            settingsPage->findChild<SettingsParts::SlidingTabs *>(MainWindowUI::settingsTabsName());
        auto *categoryStack =
            settingsPage->findChild<QStackedWidget *>(MainWindowUI::settingsCategoryStackName());
        check(settingsTabs != nullptr && categoryStack != nullptr, "设置页有顶部页签和堆叠页");
        check(settingsPage->findChild<QListWidget *>(QStringLiteral("settingsCategoryList")) == nullptr &&
                  settingsPage->findChild<QWidget *>(QStringLiteral("settingsSubIndex")) == nullptr,
              "没有左侧索引栏，也没有二级目录");
        const QStringList categoryKeys = MainWindowUI::settingsCategoryKeys();
        const QStringList categoryTexts = MainWindowUI::settingsCategoryTexts();
        check(categoryKeys == QStringList({QStringLiteral("launch"), QStringLiteral("warn"),
                                           QStringLiteral("look"), QStringLiteral("download")}) &&
                  categoryTexts == QStringList({QStringLiteral("启动"), QStringLiteral("提示"),
                                                QStringLiteral("个性化"), QStringLiteral("下载")}),
              "一级分类只有 启动 / 提示 / 个性化 / 下载（游戏并入启动，账户撤出）");
        if (settingsTabs != nullptr && categoryStack != nullptr) {
            check(settingsTabs->count() == categoryKeys.size() &&
                      categoryStack->count() == categoryKeys.size(),
                  "页签数 == 堆叠页数 == 分类表条数",
                  QStringLiteral("%1 / %2 / %3")
                      .arg(settingsTabs->count())
                      .arg(categoryStack->count())
                      .arg(categoryKeys.size()));
            int pagesOk = 0;
            int emptyPages = 0;
            int formWidthOk = 0;
            for (int i = 0; i < categoryStack->count() && i < categoryKeys.size(); ++i) {
                const QString key = categoryKeys.at(i);
                auto *pageScroll = qobject_cast<QScrollArea *>(categoryStack->widget(i));
                auto *pageForm = pageScroll != nullptr
                                     ? pageScroll->findChild<QWidget *>(MainWindowUI::settingsFormName(key))
                                     : nullptr;
                pagesOk += (pageScroll != nullptr &&
                            pageScroll->objectName() == MainWindowUI::settingsPanelName(key) &&
                            pageScroll->widgetResizable() && pageForm != nullptr)
                               ? 1
                               : 0;
                emptyPages += (pageForm != nullptr && !pageForm->findChildren<QLabel *>().isEmpty()) ? 0 : 1;
                formWidthOk += (pageForm != nullptr &&
                                pageForm->maximumWidth() == MainWindowUI::settingsFormMaxWidth())
                                   ? 1
                                   : 0;
            }
            check(pagesOk == categoryKeys.size(), "每一页都是「一层滚动区 + 一块表单」，objectName 按键拼出来",
                  QStringLiteral("%1/%2").arg(pagesOk).arg(categoryKeys.size()));
            check(emptyPages == 0, "没有空页", QStringLiteral("空的 %1 页").arg(emptyPages));
            check(formWidthOk == categoryKeys.size() && MainWindowUI::settingsFormMaxWidth() == 720,
                  "★ 每页表单限宽 720（窗口再宽也不跟着长，居中摆）");

            // 点第 N 个页签 = 翻到第 N 页。
            for (int i = categoryKeys.size() - 1; i >= 0; --i) {
                settingsTabs->setCurrentIndex(i);
                QApplication::processEvents();
                check(categoryStack->currentIndex() == i, "点第 N 个页签，堆叠页翻到第 N 页",
                      QStringLiteral("%1 → %2").arg(i).arg(categoryStack->currentIndex()));
            }
            settingsTabs->setCurrentIndex(0);
            QApplication::processEvents();

            // 行结构：每个分组 = 小标题（sectionTitle 角色）+ 一列 SettingsRow；
            // 行 = 左标签（settingsItemLabel）+ 右控件列。
            int groups = 0;
            int groupsBad = 0;
            int rows = 0;
            int rowsBad = 0;
            QStringList bad;
            for (int i = 0; i < categoryStack->count(); ++i) {
                for (auto *group : categoryStack->widget(i)->findChildren<SettingsParts::SettingsGroup *>()) {
                    ++groups;
                    bool titleOk = false;
                    for (QLabel *label : group->findChildren<QLabel *>(Qt::FindDirectChildrenOnly))
                        titleOk |= label->property(Theme::textRolePropertyName().toUtf8().constData()).toString() ==
                                   Theme::sectionTitleTextRoleValue();
                    groupsBad += titleOk ? 0 : 1;
                    for (auto *row : group->rowsHost()->findChildren<SettingsParts::SettingsRow *>(
                             QString(), Qt::FindDirectChildrenOnly)) {
                        ++rows;
                        if (row->label() == nullptr || !row->label()->property("settingsItemLabel").toBool() ||
                            row->control() == nullptr) {
                            ++rowsBad;
                            bad << (row->label() != nullptr ? row->label()->text() : QStringLiteral("<无标签>"));
                        }
                    }
                }
            }
            check(groups >= 12 && groupsBad == 0, "每个分组都有小标题（分组标题角色）",
                  QStringLiteral("分组 %1，缺标题 %2").arg(groups).arg(groupsBad));
            check(rows >= 30 && rowsBad == 0, "每一行都是「标签 + 控件」",
                  QStringLiteral("行 %1，坏 %2：%3").arg(rows).arg(rowsBad).arg(bad.join(QStringLiteral("、"))));
        }

        // 设置页这些控件都得能按 objectName 找回来（MainWindow 靠名字取它们，
        // 名字掉了就是"设置项点了没反应"）。
        // NOTE: 这一条是**递归**找的，所以设置页分栏对它没有影响 —— 控件
        // 换了哪一栏、套了几层容器都不管，只要还在设置页底下、名字没丢。真正受分栏
        // 影响的是上面那组"落在第 1 列"的断言（它们要认出所在那一栏）。
        const QStringList settingNames = {
            // 「外观」那一组（「主页标题」那一格在 UI 问题 #3 里撤掉了）。
            MainWindowUI::accountSkinSettingsButtonName(),
            MainWindowUI::javaManageButtonName(),      MainWindowUI::javaPathLabelName(),
            MainWindowUI::javaWarnMismatchCheckName(),
            MainWindowUI::memoryHintName(),
            MainWindowUI::memoryMaxSpinName(),         MainWindowUI::memoryMaxSliderName(),
            MainWindowUI::memoryModeComboName(),
            MainWindowUI::launcherBehaviourComboName(), MainWindowUI::extraJvmArgsEditName(),
            MainWindowUI::downloadSourceComboName(),
            // 「同时最多下几个」：和「下载源」同一组（这一批文件怎么走）。
            MainWindowUI::downloadConcurrencyComboName(),
            // CurseForge 的 API key：和「下载源」同一组，可留空。
            MainWindowUI::curseForgeApiKeyEditName(),
            // 「启动选项」那一组。
            MainWindowUI::showGameLogCheckName(),      MainWindowUI::processPriorityComboName(),
            MainWindowUI::gameWindowWidthSpinName(),   MainWindowUI::gameWindowHeightSpinName(),
            // 「窗口模式」那一格：和上面那两格是一组。
            MainWindowUI::gameWindowModeComboName(),
            // 「启动提醒」那一组：启动前那两条确认各配一个开关。
            MainWindowUI::warnDuplicateLaunchCheckName(), MainWindowUI::warnWorldLossCheckName(),
            // 「下载的文件」那一组：从实例设置里点「下载新模组」时要不要问存哪儿。
            MainWindowUI::downloadAutoIntoInstanceCheckName()};
        QStringList missing;
        for (const QString &name : settingNames) {
            if (settingsPage->findChild<QWidget *>(name) == nullptr) {
                missing << name;
            }
        }
        check(missing.isEmpty(), "二十六个设置控件都能按 objectName 找回来", missing.join(", "));

        // 「CurseForge API key」那一格（摆在下拉框那一组里）。它和别处
        // 不同的地方有两条，都值得单独钉住：
        // - 它是**明文**（不是 QLineEdit::Password）。理由是贴错了要能一眼看见：它本来
        // 就明文躺在配置文件里，用户看不见的输入框只会让他把 key 贴错两次，
        // 而查的人（他自己）连"贴进去的是什么"都看不到；
        // - 它**可以留空**（空 = 走社区镜像），所以占位文字说的必须是"留空会怎样"，
        // 不是必填项那种"请输入…"。
        if (auto *cfKey =
                settingsPage->findChild<QLineEdit *>(MainWindowUI::curseForgeApiKeyEditName())) {
            check(cfKey->echoMode() == QLineEdit::Normal,
                  "CurseForge 那一格是明文显示的（贴错了得看得见）",
                  QString::number(static_cast<int>(cfKey->echoMode())));
            check(cfKey->placeholderText() == MainWindowUI::curseForgeApiKeyPlaceholder() &&
                      !cfKey->placeholderText().isEmpty(),
                  "空着的时候写着「留空使用社区镜像」（这一格不是必填项）",
                  cfKey->placeholderText());
            check(cfKey->text().isEmpty(),
                  "刚搭好时框里是空的（值由 MainWindow 从盘上灌，界面层不自己猜）",
                  cfKey->text());
        }
        if (auto *autoCheck = ui.getDownloadAutoIntoInstanceCheck()) {
            // **界面层不自己猜**（和上面 CurseForge 那一格同一条规矩）：刚搭好时是**没勾**的，
            // 值由 MainWindow 从盘上灌（见 applySettingsToUi）。"默认勾上"是**设置那一层**的
            // 默认值（LauncherSettings::downloadAutoIntoInstance()），在 mw_check 里钉着 ——
            // 在这一层断言"默认勾上"等于要求界面层自己编一个默认值出来。
            check(!autoCheck->isChecked(),
                  "刚搭好时那颗是没勾的（值由 MainWindow 灌，界面层不自己猜）");
            check(autoCheck->text() == MainWindowUI::downloadAutoIntoInstanceCheckText() &&
                      !autoCheck->text().isEmpty(),
                  "那一格的字（勾上管的是哪一件事）", autoCheck->text());
            const QString hint = autoCheck->accessibleDescription();
            check(hint.contains(QStringLiteral("目标实例")) && hint.contains(QStringLiteral("系统文件夹")),
                  "按需帮助写明开启和关闭时各用哪种选择器", hint);
        } else {
            check(false, "找得到「内置目标实例选择器」那颗勾选框");
        }

        // 「同时最多下几个」那颗下拉框。界面层这里只钉一件**界面层管得着**
        // 的事：**刚搭好时是一个空框**。四档那个表在 src/ 的 `DownloadConcurrency.hpp` 里
        // （唯一出处），界面层不许 include src/ ⇒ 项只能由 MainWindow 灌（和「下载源」
        // 「窗口模式」同一个形状）。在这一层断言"四档都在"等于要求界面层自己抄一份表出来。
        // （"四档都在、选中的那一档是盘上那个"由 mw_check 盯。）
        if (auto *concCombo = ui.getDownloadConcurrencyCombo()) {
            check(concCombo->count() == 0,
                  "刚搭好时那颗「并发数」是**空框**（项由 MainWindow 灌，界面层不自己抄表）",
                  QString::number(concCombo->count()));
            check(concCombo->objectName() == MainWindowUI::downloadConcurrencyComboName(),
                  "它挂的就是那个 objectName（验证工程按名字找它）", concCombo->objectName());
        } else {
            check(false, "找得到「并发数」那颗下拉框");
        }

        // 行标题也找一份：这是**静态文字**（没有 objectName 可取），只能用文字找。
        // 钉它的理由是"两句话说的是同一件事的两面"：行标题写的是这一格管什么、
        // 占位文字写的是不填会怎样 —— 行标题被改成别的东西，用户就不知道该填什么了。
        {
            bool rowFound = false;
            for (QLabel *label : settingsPage->findChildren<QLabel *>()) {
                if (label->text() == MainWindowUI::curseForgeApiKeyRowText()) {
                    rowFound = true;
                    break;
                }
            }
            check(rowFound, "设置页上真的摆着那一行（行标题就是那一句）",
                  MainWindowUI::curseForgeApiKeyRowText());
        }

        // 「主页标题」那一格在 UI 问题 #3 里撤掉了：商标固定，设置页里不该再有那一格输入框。
        check(settingsPage != nullptr &&
                  settingsPage->findChild<QLineEdit *>(QStringLiteral("titleTextEdit")) == nullptr,
              "★ 设置页里没有「主页标题」那一格（条上那行字固定是商标）");

        // 「提示」栏里那两条启动前的确认（分栏时并到一栏）。
        // 这里钉的是**这一组特有的东西** —— 两句文案各自只此一处、行标题是用户点名的
        // 那两个词（「重复启动」/「进度提醒」，提示框上那句小字就是照着它们指路的，
        // 所以这两个词一改，小字就指错地方了）。控件本身能不能找回来在上面那条
        // 上面那份控件清单里；勾选状态由 MainWindow 从盘上灌，界面层不自己猜。
        {
            auto *duplicate = settingsPage->findChild<QCheckBox *>(
                MainWindowUI::warnDuplicateLaunchCheckName());
            auto *worldLoss = settingsPage->findChild<QCheckBox *>(
                MainWindowUI::warnWorldLossCheckName());
            check(duplicate != nullptr && worldLoss != nullptr,
                  "找得到「提示」栏里那两个勾选框");
            if (duplicate != nullptr && worldLoss != nullptr) {
                check(duplicate->text() == MainWindowUI::warnDuplicateLaunchCheckText(),
                      "「重复启动」那一格的字就是那唯一一处", duplicate->text());
                check(worldLoss->text() == MainWindowUI::warnWorldLossCheckText(),
                      "「进度提醒」那一格的字就是那唯一一处", worldLoss->text());
                check(!duplicate->isChecked() && !worldLoss->isChecked(),
                      "刚搭好时两个都没勾（值是 MainWindow 从盘上灌的，界面不自己猜）");
                // 两个勾选框都在右边那一列、各占自己那一行 —— 这一组的形状和「启动选项」
                // 那几格一样（控件在上、说明在下，装着它们的那一格在右列）。
                check(duplicate->parentWidget() != worldLoss->parentWidget(),
                      "两条各占一行（不是挤在同一格里）");
            }
        }
        // 那三个词得真在表单上画着：行标题由 `addSettingRow()` 落成一个 QLabel，
        // 这里按文字去找 —— 改行标题的人会当场看到这条红。
        //
        // NOTE: 分栏之后这三条并进「提示」栏，组标题也随之改名 —— 三条管的是同一件事
        // （"启动那一下要不要再问一句"）。
        auto *warnForm = settingsPage->findChild<QWidget *>(
            MainWindowUI::settingsFormName(QStringLiteral("warn")));
        check(warnForm != nullptr, "「提示」栏那张表单找得到",
              MainWindowUI::settingsFormName(QStringLiteral("warn")));
        if (warnForm != nullptr) {
            const QList<QLabel *> labelsInForm = warnForm->findChildren<QLabel *>();
            const auto hasLabelText = [&labelsInForm](const QString &text) {
                for (const QLabel *label : labelsInForm) {
                    if (label->text() == text) {
                        return true;
                    }
                }
                return false;
            };
            check(warnForm->findChild<QCheckBox *>(MainWindowUI::javaWarnMismatchCheckName()) &&
                      warnForm->findChild<QCheckBox *>(MainWindowUI::warnDuplicateLaunchCheckName()) &&
                      warnForm->findChild<QCheckBox *>(MainWindowUI::warnWorldLossCheckName()),
                  "安全提醒的三个开关直接显示，不重复添加行标题");
            check(hasLabelText(QStringLiteral("启动前")) && hasLabelText(QStringLiteral("下载时")),
                  "这一页的分组标题「启动前」「下载时」画出来了");
            check(!hasLabelText(QStringLiteral("启动提醒")) && !hasLabelText(QStringLiteral("安全提醒")),
                  "老分组名不再画");
        }

        {
            // 每个设置控件都长在某一行（SettingsRow）里；「最大内存」下面的说明和那根条同在一个容器里。
            const QList<QWidget *> controls = {
                ui.getMemoryHint(),                ui.getMemoryMaxSpin(),
                ui.getMemoryMaxSlider(),           ui.getLauncherBehaviourCombo(),
                ui.getExtraJvmArgsEdit(),          ui.getDownloadSourceCombo(),
                ui.getShowGameLogCheck(),          ui.getProcessPriorityCombo(),
                ui.getGameWindowWidthSpin(),
                ui.getGameWindowHeightSpin(),      ui.getWarnDuplicateLaunchCheck(),
                ui.getWarnWorldLossCheck()};
            int inRow = 0;
            for (QWidget *control : controls) {
                for (QWidget *p = control; p != nullptr; p = p->parentWidget())
                    if (qobject_cast<SettingsParts::SettingsRow *>(p) != nullptr) {
                        ++inRow;
                        break;
                    }
            }
            check(inRow == controls.size(), "每个设置控件都长在某一行里",
                  QString("%1/%2").arg(inRow).arg(controls.size()));
            check(ui.getMemoryHint() != nullptr && ui.getMemoryMaxSlider() != nullptr &&
                      ui.getMemoryHint()->parentWidget() == ui.getMemoryMaxSlider()->parentWidget()->parentWidget(),
                  "「最大内存」下面的说明和那根条装在同一个容器里（说明跟着条走）");
            if (ui.getMemoryHint() != nullptr && ui.getMemoryMaxSlider() != nullptr) {
                const QPoint sliderBottom = ui.getMemoryMaxSlider()->mapTo(
                    ui.getMemoryHint()->parentWidget(), QPoint(0, ui.getMemoryMaxSlider()->height()));
                check(ui.getMemoryHint()->y() >= sliderBottom.y(), "那句说明在条下面（不是并排）",
                      QString("bar.bottom=%1 hint.y=%2").arg(sliderBottom.y()).arg(ui.getMemoryHint()->y()));
            }
        }

        // 「最大内存」那一行现在是**一根条 + 一个数字框**：两件东西说的是
        // 同一个数，范围就必须是同一个，而且和 GlobalJavaSettings 的钳位对得上 ——
        // "填了不生效"和"拖到头和打得进去的不是一个数"都是同一件事两个答案。
        // 「最小内存」那一格从这一页撤掉了（用户要求），下限 128 那条跟着撤走的断言。
        check(ui.getMemoryMaxSpin()->minimum() == 256 &&
                  ui.getMemoryMaxSlider()->minimumMb() == 256,
              "最大内存的下限 256（数字框和条说的是同一个数）",
              QString("%1 / %2").arg(ui.getMemoryMaxSpin()->minimum())
                  .arg(ui.getMemoryMaxSlider()->minimumMb()));
        check(ui.getMemoryMaxSpin()->maximum() == MainWindowUI::memoryLimitMaxMb() &&
                  ui.getMemoryMaxSlider()->maximumMb() == MainWindowUI::memoryLimitMaxMb(),
              "上限都是 65536（界面单独搭出来时的天花板 —— 真跑起来 MainWindow 会按这台机器"
              "的物理内存再收一次口，见 MainWindow::applyMemoryLimits）",
              QString("%1 / %2").arg(ui.getMemoryMaxSpin()->maximum())
                  .arg(ui.getMemoryMaxSlider()->maximumMb()));
        check(ui.getMemoryMaxSpin()->singleStep() == MainWindowUI::memoryStepMb(),
              "步长是 256（拖一格不会拖出个奇怪的值）",
              QString::number(ui.getMemoryMaxSpin()->singleStep()));
        check(ui.getMemoryModeCombo() != nullptr &&
                  ui.getMemoryModeCombo()->count() == 2 &&
                  ui.getMemoryModeCombo()->itemText(0) == QStringLiteral("自动分配") &&
                  ui.getMemoryModeCombo()->itemText(1) == QStringLiteral("手动分配") &&
                  !ui.getMemoryModeCombo()->currentData().toBool(),
              "「内存分配」最右边那颗下拉框只有自动 / 手动两项，默认手动",
              ui.getMemoryModeCombo() != nullptr
                  ? QString("%1 项 / 当前 %2")
                        .arg(ui.getMemoryModeCombo()->count())
                        .arg(ui.getMemoryModeCombo()->currentText())
                  : QStringLiteral("<没有下拉框>"));

        // 档位表本身（纯函数，不用真去点一根条 —— 点条只能验到"拖动是通的"，验不到
        // "每一格都是该有的那个数"）。见 MemorySlider.hpp 里那段。
        {
            const QList<int> steps =
                MemorySlider::valueSteps(256, MainWindowUI::memoryLimitMaxMb());
            bool increasing = steps.size() >= 2;
            bool onGrid = true;
            for (int i = 0; i < steps.size(); ++i) {
                if (i > 0 && steps.at(i) <= steps.at(i - 1)) {
                    increasing = false;
                }
                // 每一格都落在 256 的整数倍上；**最右端那一格除外** —— 上限本身不在格上时
                // 它就停在那个数上（见 valueSteps 里那段：不然最右边够不到上限）。
                // 每一格都得落在**最细那一档**（128 MB）的整数倍上；最右端那一格除外
                // —— 上限本身不在格上时它就停在那个数上（见 valueSteps 里那段：不然最
                // 右边够不到上限）。NOTE: 判据不是 256 —— 2 GB 以下那一带的步长就是 128
                // （256 → 384 → 512…），拿 256 当判据是把"最小的那一档"当成"最大的那一档"
                // 去用了。
                if (i + 1 < steps.size() && steps.at(i) % 128 != 0) {
                    onGrid = false;
                }
            }
            check(increasing && steps.constFirst() == 256 &&
                      steps.constLast() == MainWindowUI::memoryLimitMaxMb(),
                  "档位表：升序、没有重复项、两头就是下限和上限",
                  QString("%1 格，%2 … %3").arg(steps.size()).arg(steps.constFirst())
                      .arg(steps.constLast()));
            check(onGrid, "档位表：每一格都是 128 的整数倍（最右端那一格可以是上限本身）");
            // 步长随值变大 —— 这才是"灵活变通"：小内存 128 一格（4 GB 的机器上 1152 /
            // 1280 那些档位是真有人用的），大内存 512 一格（32 GB 那台上不然要划两百多格）。
            check(MemorySlider::valueSteps(256, 2048).at(1) - 256 == 128 &&
                      MemorySlider::valueSteps(8192, 10240).at(1) - 8192 == 512,
                  "档位：2 GB 以下 128 一格、8 GB 以上 512 一格",
                  QString("%1 / %2").arg(MemorySlider::valueSteps(256, 2048).at(1) - 256)
                      .arg(MemorySlider::valueSteps(8192, 10240).at(1) - 8192));
            // 就近吸附：同样近时取**小**的那一格（往下压比往上顶安全：内存给大了 JVM
            // 直接起不来，给小了只是慢一点）。
            check(MemorySlider::nearestIndex(QList<int>{1024, 1280}, 1152) == 0,
                  "同样近时吸附到小的那一格（1152 → 1024）");
            check(MemorySlider::nearestIndex(QList<int>{1024, 1280}, 1280) == 1,
                  "正好在格上就停在那一格");
            check(MemorySlider::nearestIndex(QList<int>(), 4096) == -1, "空表回 -1");
            // 上限比下限高不到一格：表里就是两个数，不会退化成一串重复值
            const QList<int> narrow = MemorySlider::valueSteps(256, 300);
            check(narrow.size() == 2 && narrow.constFirst() == 256 && narrow.constLast() == 300,
                  "上下限之间不到一格时，表里就是两个数（下限、上限）",
                  QString("%1 格").arg(narrow.size()));
        }

        // 自动档展示值：虚条和圆圈都画在自动值那一格。
        //
        // 圆圈停在"上次手动"那个位置时，自动值比它小的话圆圈就浮在一段没填的轨道上，
        // 看着像圆圈跟条脱开了（用户报的就是这个）。画法这一层钉的是"圆圈画在哪"：
        // valueMb 停在 4096 与停在 8192 两种画出来必须**一模一样**。
        {
            MemorySlider autoProbe;
            autoProbe.setRangeMb(256, 16384);
            autoProbe.resize(240, 20);
            autoProbe.show();
            autoProbe.setValueMb(4096);
            autoProbe.setAutoValueMb(8192);
            QApplication::processEvents();
            const QImage knobAtManualValue = autoProbe.grab().toImage();
            autoProbe.setValueMb(8192);
            QApplication::processEvents();
            const QImage knobAtAutoValue = autoProbe.grab().toImage();
            check(!knobAtManualValue.isNull() && knobAtManualValue == knobAtAutoValue,
                  "自动档的圆圈画在虚条头上（自动值那一格），与 value() 停在哪无关",
                  QString("两张图 %1 x %2").arg(knobAtManualValue.size().width())
                      .arg(knobAtManualValue.size().height()));
            check(autoProbe.valueMb() == 8192 && autoProbe.hasAutoValue() &&
                      autoProbe.autoValueMb() == 8192,
                  "valueMb 是调用方自己设的那一格，autoValueMb 是自动值",
                  QString("圆圈 %1 / 自动 %2").arg(autoProbe.valueMb()).arg(autoProbe.autoValueMb()));
            autoProbe.setValueMb(4096);
            autoProbe.clearAutoValue();
            check(!autoProbe.hasAutoValue() && autoProbe.valueMb() == 4096,
                  "清掉自动展示值之后回到手动画法，圆圈位置不变",
                  QString("圆圈 %1 / 自动 %2").arg(autoProbe.valueMb()).arg(autoProbe.autoValueMb()));
        }

        // JVM 参数那一格是 QPlainTextEdit 而不是 QLineEdit：参数列表天然是多行的。
        check(ui.getExtraJvmArgsEdit() != nullptr &&
                  ui.getExtraJvmArgsEdit()->parentWidget() != nullptr,
              "全局 JVM 参数是个多行输入框");
        check(ui.getExtraJvmArgsEdit() != nullptr && ui.getExtraJvmArgsEdit()->height() <= 96,
              "它只有三四行高（这一格放的是顺手加的几条，不是一整份命令行）",
              QString::number(ui.getExtraJvmArgsEdit() != nullptr
                                  ? ui.getExtraJvmArgsEdit()->height()
                                  : -1));
        // 界面只搭壳：下拉框的条目和值由 MainWindow 灌（见 mw_check）
        // NOTE: 起「Java 版本」那一格是颗按钮（`javaManageButton`）而不是
        // 下拉框了，所以它不再出现在这一串"刚搭好时是空的"里。
        check(ui.getLauncherBehaviourCombo()->count() == 0 &&
                  ui.getDownloadSourceCombo()->count() == 0 &&
                  // 「进程优先级」尤其要在这儿：它是**界面里唯一一个条目由 MainWindow 灌的
                  // 下拉框**，而 applySettingsToUi() 里那句 addItem 是**不去重**的 ——
                  // 界面层哪天自己先灌一遍，用户看到的就是十档。
                  ui.getProcessPriorityCombo()->count() == 0 &&
                  ui.getMemoryMaxSpin()->value() == ui.getMemoryMaxSpin()->minimum(),
              "设置页刚搭好时全是空的（值由 MainWindow 从盘上灌进来）",
              QString("%1/%2/%3")
                  .arg(ui.getLauncherBehaviourCombo()->count())
                  .arg(ui.getDownloadSourceCombo()->count())
                  .arg(ui.getProcessPriorityCombo()->count()));
        check(settingsPage->findChild<QCheckBox *>(QStringLiteral("memoryAutoCheck")) == nullptr &&
                  ui.getMemoryModeCombo() != nullptr &&
                  ui.getMemoryModeCombo()->count() == 2,
              "设置页的自动/手动是最右边那颗两项下拉框（不是旧勾选框）",
              ui.getMemoryModeCombo() != nullptr
                  ? QString("%1 项").arg(ui.getMemoryModeCombo()->count())
                  : QStringLiteral("<没有下拉框>"));
        // 同一个形状，另一项：「实例指定的 Java 版本不符时提醒」这个总闸同样只搭壳。
        // NOTE: 产品里的默认值是"提醒"（true），但那条是**产品逻辑**、不是界面的事 ——
        // 界面层自己默认打开的话，MainWindow 万一忘了灌值就会显示一个假的"开"。
        check(ui.getJavaWarnMismatchCheck() != nullptr &&
                  !ui.getJavaWarnMismatchCheck()->isChecked(),
              "「实例指定的 Java 版本不符时提醒」刚搭好时也是未勾选的（值由 MainWindow 灌）");
        check(ui.getJavaWarnMismatchCheck() != nullptr &&
                  !ui.getJavaWarnMismatchCheck()->text().isEmpty(),
              "它也自带文字", ui.getJavaWarnMismatchCheck() != nullptr
                                  ? ui.getJavaWarnMismatchCheck()->text()
                                  : QString());
        // 那句说明要有占位文字：空标签会把行高压成 0，灌值的时候布局会跳一下。
        check(ui.getMemoryHint() != nullptr && !ui.getMemoryHint()->text().isEmpty(),
              "「最大内存」的说明有占位文字（免得布局跳）",
              ui.getMemoryHint() != nullptr ? ui.getMemoryHint()->text() : QString());
    }
    {
        auto *page = tabs->widget(MainWindowUI::settingsTabIndex());
        auto *categories = page->findChild<QStackedWidget *>(MainWindowUI::settingsCategoryStackName());
        auto *look = page->findChild<QScrollArea *>(MainWindowUI::settingsPanelName(QStringLiteral("look")));
        QWidget *window = tabs->window();
        const QSize oldSize = window->size();
        const int oldCategory = categories->currentIndex();
        categories->setCurrentWidget(look);
        for (const int width : {900, 1100}) {
            window->resize(width, 720);
            QApplication::processEvents();
            bool fits = look && look->widget()->width() <= look->viewport()->width() && look->horizontalScrollBar()->maximum() == 0;
            for (const auto part : {Theme::Part::PageBackground}) {
                const QString key = Theme::partKey(part);
                auto *choose = page->findChild<QPushButton *>(MainWindowUI::themeImageChooseName(key));
                auto *fit = page->findChild<QComboBox *>(MainWindowUI::themeImageFitName(key));
                auto *opacity = page->findChild<QSlider *>(MainWindowUI::themeImageOpacityName(key));
                if (!choose || !fit || !opacity) { fits = false; continue; }
                QWidget *form = look->widget();
                fits &= fit->mapTo(form, QPoint()).y() >= choose->mapTo(form, QPoint()).y() + choose->height();
                fits &= opacity->mapTo(form, QPoint()).y() >= fit->mapTo(form, QPoint()).y() + fit->height();
                fits &= opacity->mapTo(form, QPoint()).x() + opacity->width() <= form->width();
            }
            check(fits, "background controls reflow vertically without horizontal scrolling", QString::number(width));
        }
        window->resize(oldSize);
        categories->setCurrentIndex(oldCategory);
        QApplication::processEvents();
    }
    {
        // ---- 重排验收：任何页签、两个尺寸下都没有横向溢出；窄表单行上下叠放；失焦即存 ----
        QWidget *page = tabs->widget(MainWindowUI::settingsTabIndex());
        tabs->setCurrentIndex(MainWindowUI::settingsTabIndex());
        QWidget *window = tabs->window();
        const QSize oldSize = window->size();
        auto *settingsTabs =
            page->findChild<SettingsParts::SlidingTabs *>(MainWindowUI::settingsTabsName());
        auto *stack = page->findChild<QStackedWidget *>(MainWindowUI::settingsCategoryStackName());
        const QStringList keys = MainWindowUI::settingsCategoryKeys();
        const auto pump = [] {
            QApplication::processEvents();
            QApplication::processEvents();
        };
        // 「高级」展开行里的东西也要量：全部展开，量完收回。
        const QList<SettingsParts::ExpandSection *> sections = page->findChildren<SettingsParts::ExpandSection *>();
        for (auto *section : sections) section->setExpanded(true, false);

        for (const QSize size : {QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight), QSize(1280, 800)}) {
            window->resize(size);
            pump();
            QWidget *header = page->findChild<QWidget *>(MainWindowUI::pageHeaderName(QStringLiteral("settings")));
            auto *search = page->findChild<QLineEdit *>(MainWindowUI::settingsSearchEditName());
            check(header != nullptr && header->width() <= page->width() && search != nullptr &&
                      search->mapTo(page, QPoint(search->width(), 0)).x() <= page->width() &&
                      settingsTabs != nullptr &&
                      settingsTabs->mapTo(page, QPoint(settingsTabs->width(), 0)).x() <= search->mapTo(page, QPoint(0, 0)).x(),
                  QStringLiteral("%1×%2：页头（标题 / 页签 / 搜索）在页宽内，页签不压到搜索").arg(size.width()).arg(size.height()));
            for (int i = 0; settingsTabs != nullptr && stack != nullptr && i < keys.size(); ++i) {
                settingsTabs->setCurrentIndex(i);
                pump();
                auto *scroll = qobject_cast<QScrollArea *>(stack->widget(i));
                if (scroll == nullptr) continue;
                const int viewportWidth = scroll->viewport()->width();
                int overflow = 0;
                QStringList detail;
                for (auto *row : scroll->findChildren<SettingsParts::SettingsRow *>()) {
                    if (!row->isVisibleTo(scroll)) continue;
                    const QPoint topLeft = row->mapTo(scroll->viewport(), QPoint(0, 0));
                    if (topLeft.x() < 0 || topLeft.x() + row->width() > viewportWidth) {
                        ++overflow;
                        detail << QStringLiteral("行「%1」%2..%3 > %4").arg(row->label()->text()).arg(topLeft.x()).arg(topLeft.x() + row->width()).arg(viewportWidth);
                    }
                    for (QWidget *child : {static_cast<QWidget *>(row->label()), row->control()}) {
                        if (child == nullptr) continue;
                        const QPoint right = child->mapTo(row, QPoint(child->width(), 0));
                        if (right.x() > row->width()) {
                            ++overflow;
                            detail << QStringLiteral("行「%1」里 %2 超出行宽 %3 > %4").arg(row->label()->text(), child->metaObject()->className()).arg(right.x()).arg(row->width());
                        }
                    }
                }
                // 非空判据：表单真的铺开了（宽窗 = 720 上限；窄窗 = 可视宽减两侧留白），不是缩成内容那么窄。
                auto *formWidget = scroll->findChild<QWidget *>(MainWindowUI::settingsFormName(keys.at(i)));
                const int expectForm = qMin(MainWindowUI::settingsFormMaxWidth(), viewportWidth - Theme::pageInsetLeft() - Theme::contentToScrollBarGap());
                check(formWidget != nullptr && qAbs(formWidget->width() - expectForm) <= 2,
                      QStringLiteral("%1×%2「%3」页：表单宽 = min(720, 可视宽 − 左边距 − 滑条间隙)")
                          .arg(size.width()).arg(size.height()).arg(keys.at(i)),
                      QStringLiteral("表单 %1，期望 %2，可视 %3")
                          .arg(formWidget != nullptr ? formWidget->width() : -1).arg(expectForm).arg(viewportWidth));
                check(scroll->horizontalScrollBar()->maximum() == 0 && scroll->widget()->width() <= viewportWidth && overflow == 0,
                      QStringLiteral("%1×%2「%3」页：内容宽 ≤ 可视宽，没有横向溢出")
                          .arg(size.width()).arg(size.height()).arg(keys.at(i)),
                      QStringLiteral("横向滚动 %1，内容 %2 / 可视 %3，溢出 %4 处 %5")
                          .arg(scroll->horizontalScrollBar()->maximum()).arg(scroll->widget()->width()).arg(viewportWidth)
                          .arg(overflow).arg(detail.join(QStringLiteral("；"))));
            }
        }

        // 行叠放：内容宽 < 600 时，标签在上、控件在下；回到 720 后恢复左右。
        if (settingsTabs != nullptr && stack != nullptr) {
            window->resize(1280, 800);
            settingsTabs->setCurrentIndex(0);
            pump();
            auto *scroll = qobject_cast<QScrollArea *>(stack->widget(0));
            QWidget *form = scroll != nullptr ? scroll->findChild<QWidget *>(MainWindowUI::settingsFormName(QStringLiteral("launch"))) : nullptr;
            if (form != nullptr) {
                const auto countRows = [form](bool *allStacked, bool *noneStacked, int *bad) {
                    *allStacked = true;
                    *noneStacked = true;
                    *bad = 0;
                    int total = 0;
                    for (auto *row : form->findChildren<SettingsParts::SettingsRow *>()) {
                        if (!row->isVisibleTo(form)) continue;
                        ++total;
                        *allStacked &= row->stacked();
                        *noneStacked &= !row->stacked();
                        const QPoint labelPos = row->label()->mapTo(row, QPoint(0, row->label()->height()));
                        const QPoint controlPos = row->control()->mapTo(row, QPoint(0, 0));
                        const bool below = controlPos.y() >= labelPos.y();
                        const bool beside = controlPos.x() > row->label()->mapTo(row, QPoint(row->label()->width(), 0)).x() - 1;
                        if (row->stacked() ? !below : (!beside && !below)) ++*bad;
                    }
                    return total;
                };
                bool allStacked = false;
                bool noneStacked = false;
                int bad = 0;
                QStringList stackedRows;
                for (auto *row : form->findChildren<SettingsParts::SettingsRow *>())
                    if (row->isVisibleTo(form) && row->stacked())
                        stackedRows << QStringLiteral("%1(w=%2)").arg(row->label()->text()).arg(row->width());
                check(countRows(&allStacked, &noneStacked, &bad) >= 5 && noneStacked && bad == 0,
                      "表单 720 宽：行是左标签右控件（没有叠放）",
                      QStringLiteral("form=%1 叠放：%2 错位 %3").arg(form->width()).arg(stackedRows.join(QLatin1Char(','))).arg(bad));
                form->setMaximumWidth(560);
                pump();
                const int total = countRows(&allStacked, &noneStacked, &bad);
                check(total >= 5 && allStacked && bad == 0,
                      "内容宽 560 < 600：每一行都上下叠放，控件在标签下面",
                      QStringLiteral("行 %1，全叠放 %2，错位 %3").arg(total).arg(allStacked).arg(bad));
                int widest = 0;
                for (auto *row : form->findChildren<SettingsParts::SettingsRow *>())
                    if (row->isVisibleTo(form) && row->control() != nullptr &&
                        qobject_cast<QComboBox *>(row->control()) != nullptr)
                        widest = qMax(widest, row->control()->width());
                check(widest > 0 && widest <= SettingsParts::kStackedControlMax,
                      "叠放时下拉框最宽 320", QString::number(widest));
                form->setMaximumWidth(MainWindowUI::settingsFormMaxWidth());
                pump();
                countRows(&allStacked, &noneStacked, &bad);
                check(noneStacked, "表单回到 720 宽，行恢复左右并排");
            }
        }

        // 失焦即存：改过的单行输入 / 多行文本在失焦或回车后出现「已保存」，1.2s 后淡出；没改过不出现。
        {
            window->resize(1280, 800);
            pump();
            auto *toast = ui.getSettingsSavedToast();
            auto *proxyEdit = ui.getDownloadProxyManualEdit();
            auto *jvm = ui.getExtraJvmArgsEdit();
            auto *commit = ui.getExtraJvmArgsCommit();
            check(toast != nullptr && proxyEdit != nullptr && jvm != nullptr && commit != nullptr &&
                      !toast->isVisible(),
                  "「已保存」平时不显示");
            if (toast != nullptr && proxyEdit != nullptr && jvm != nullptr && commit != nullptr) {
                const auto waitMs = [](int ms) {
                    QEventLoop loop;
                    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
                    loop.exec();
                };
                const auto showTab = [&](QWidget *w) {
                    for (int i = 0; i < keys.size(); ++i)
                        if (stack != nullptr && stack->widget(i)->isAncestorOf(w)) settingsTabs->setCurrentIndex(i);
                    pump();
                };
                showTab(proxyEdit);
                // 没改过就失焦：不提示。
                emit proxyEdit->editingFinished();
                check(!toast->isVisible(), "没改过的输入框失焦不提示「已保存」");
                emit proxyEdit->textEdited(QStringLiteral("x"));
                emit proxyEdit->editingFinished();
                check(toast->isVisible() && toast->opacity() > 0.99, "改过的输入框失焦后出现「已保存」");
                waitMs(1500);
                check(!toast->isVisible(), "「已保存」1.2 秒后淡出");

                // JVM 参数：失焦即存（信号来自失焦过滤器）。
                showTab(jvm);
                int committedCount = 0;
                QObject::connect(commit, &SettingsParts::CommitOnFocusOut::committed, commit, [&committedCount] { ++committedCount; });
                jvm->insertPlainText(QStringLiteral("-Xss1M"));
                QFocusEvent out(QEvent::FocusOut);
                QApplication::sendEvent(jvm, &out);
                check(committedCount == 1 && toast->isVisible(), "JVM 参数框失焦即存，并提示「已保存」",
                      QString::number(committedCount));
                waitMs(1500);
                // 开关：用户点一下就提示。
                auto *switchCheck = ui.getShowGameLogCheck();
                if (switchCheck != nullptr) {
                    showTab(switchCheck);
                    switchCheck->click();
                    check(toast->isVisible(), "点开关后出现「已保存」");
                    switchCheck->click();
                    waitMs(1500);
                }
                jvm->clear();
                jvm->document()->setModified(false);
            }
        }

        for (auto *section : sections) section->setExpanded(false, false);
        if (settingsTabs != nullptr) settingsTabs->setCurrentIndex(0);
        window->resize(oldSize);
        pump();
    }

 return 0;
}


