#include "Support.hpp"
int UiServerSettingsChecks() {
    // ---------- U37 「服务端设置」那一整页（M4-4）----------
    //
    // 那三句话落到界面上就是这一页：**常用设置那张表单**（每一项是什么控件、值域多少，
    // 由 `ServerPropertiesForm` 那一层说了算，这一页只负责把控件摆对）、**原始文本编辑**
    // （整份 `server.properties` 原样摆出来 —— 注释、键的顺序、行尾一个都不许翻）、
    // **世界备份 + 体检**。
    //
    // 这一页**一行判据都没有**（哪几个字段、哪些值合法、体检报哪几条，全是 `MainWindow`
    // 翻好喂进来的，见它的文件头），所以这一节量得到的正好是两件事：
    //
    // * 喂进来的东西**摆对地方了没有**（哪一格是什么控件、字重和颜色对不对、左栏四页、
    // 出路按钮去重了没有）；
    // * 用户动过之后**报出去的到底是什么**（`changedOnly()` / `rawText()` / 那几条
    // `*Requested` 信号）—— 「保存真写盘」那一路在 mw_check 的 T42（真 `MainWindow`）。
    {
        ServerSettingsPage page;
        page.resize(880, 620);
        page.show();
        QApplication::processEvents();

        auto *title = page.findChild<QLabel *>(MainWindowUI::serverSettingsTitleName());
        auto *status = page.findChild<QLabel *>(MainWindowUI::serverSettingsStatusName());
        auto *sections = page.findChild<QTreeWidget *>(MainWindowUI::serverSettingsSectionListName());
        auto *start = page.findChild<QPushButton *>(QStringLiteral("serverSettingsStart"));
        auto *saveAction = page.findChild<QPushButton *>(MainWindowUI::serverSettingsSaveButtonName());
        check(start && saveAction && sections && start->parentWidget() == sections->parentWidget() &&
                  saveAction->parentWidget() == sections->parentWidget(),
              "save and start actions are anchored below server navigation");
        if (start) {
            int requests = 0;
            const auto startConnection = QObject::connect(&page, &ServerSettingsPage::startRequested,
                                                          &page, [&requests] { ++requests; });
            start->click();
            check(requests == 1, "server start action emits one request");
            page.markRawEdited();
            check(!start->isEnabled(), "unsaved edits require saving before server launch");
            page.setRawText(QString());
            page.setEditable(false, QStringLiteral("running"));
            check(!start->isEnabled(), "running server cannot be launched again from settings");
            page.setEditable(true, QString());
            QObject::disconnect(startConnection);
        }
        auto *stack = page.findChild<QStackedWidget *>(MainWindowUI::serverSettingsStackName());
        auto *form = page.findChild<QWidget *>(MainWindowUI::serverSettingsFormName());
        auto *rawEdit = page.findChild<QPlainTextEdit *>(MainWindowUI::serverSettingsRawEditName());
        auto *rawNote = page.findChild<QLabel *>(MainWindowUI::serverSettingsRawNoteName());
        auto *backupNote = page.findChild<QLabel *>(MainWindowUI::serverSettingsBackupNoteName());
        auto *backupList = page.findChild<QTreeWidget *>(MainWindowUI::serverSettingsBackupListName());
        auto *checkList = page.findChild<QTreeWidget *>(MainWindowUI::serverSettingsCheckListName());
        auto *checkSummary =
            page.findChild<QLabel *>(MainWindowUI::serverSettingsCheckSummaryName());
        auto *checkActions =
            page.findChild<QWidget *>(MainWindowUI::serverSettingsCheckActionsName());
        auto *revert = page.findChild<QPushButton *>(MainWindowUI::serverSettingsRevertButtonName());
        auto *save = page.findChild<QPushButton *>(MainWindowUI::serverSettingsSaveButtonName());
        auto *saveNote = page.findChild<QLabel *>(MainWindowUI::serverSettingsSaveNoteName());
        auto *backupNow =
            page.findChild<QPushButton *>(MainWindowUI::serverSettingsBackupNowButtonName());
        auto *restore =
            page.findChild<QPushButton *>(MainWindowUI::serverSettingsRestoreButtonName());
        auto *deleteBackup =
            page.findChild<QPushButton *>(MainWindowUI::serverSettingsDeleteBackupButtonName());
        auto *openBackups = page.findChild<QPushButton *>(
            MainWindowUI::serverSettingsOpenBackupFolderButtonName());
        auto *recheck =
            page.findChild<QPushButton *>(MainWindowUI::serverSettingsRecheckButtonName());
        const bool there = title != nullptr && status != nullptr && sections != nullptr &&
                           stack != nullptr && form != nullptr && rawEdit != nullptr &&
                           rawNote != nullptr && backupNote != nullptr && backupList != nullptr &&
                           checkList != nullptr && checkSummary != nullptr &&
                           checkActions != nullptr && revert != nullptr && save != nullptr &&
                           saveNote != nullptr && backupNow != nullptr && restore != nullptr &&
                           deleteBackup != nullptr && openBackups != nullptr && recheck != nullptr;
        check(there,
              "U37.0 前提：这一页那二十来样（标题 / 状态 / 左栏 / 堆叠页 / 表单 / 原始文本框 / "
              "备份列 / 体检列 / 保存带那两颗…）全按 objectName 找得到");
        if (there) {
            // 「哪一颗挂了哪一档」的取法同 U36：没挂属性 = `Accent`。
            const auto kindOf = [](QPushButton *button) {
                const QVariant value = button->property("wtfmlButtonKind");
                return value.isValid() ? value.toString()
                                       : Theme::buttonKindKey(Theme::ButtonKind::Accent);
            };
            const auto valuesOf = [](const ServerSettingsPage::Values &values,
                                     const QString &key) {
                for (const QPair<QString, QString> &pair : values) {
                    if (pair.first == key) {
                        return pair.second;
                    }
                }
                return QString();
            };

            // ---------- ① 这一页认得出自己 + 左栏那四页 ----------
            check(page.objectName() == MainWindowUI::serverSettingsPageName() &&
                      ServerSettingsPage::windowTitle() == QStringLiteral("服务端设置"),
                  "★ U37.1 这一页认得出自己（页眉上那行字是 `windowTitle()` 那一个出处）",
                  page.objectName());
            QStringList sectionTexts;
            for (int i = 0; i < sections->topLevelItemCount(); ++i) {
                sectionTexts.append(sections->topLevelItem(i)->text(0));
            }
            bool titlesOk = sections->topLevelItemCount() == MainWindowUI::serverSettingsSectionCount();
            for (int i = 0; titlesOk && i < sections->topLevelItemCount(); ++i) {
                titlesOk = sections->topLevelItem(i)->text(0) == MainWindowUI::serverSettingsSectionTitle(i);
            }
            check(titlesOk && stack->count() == MainWindowUI::serverSettingsSectionCount(),
                  "★★ U37.1 左栏的页数、次序就是唯一出处里的次序，右栏一页对一页"
                  "（`serverSettingsSectionCount()`）",
                  sectionTexts.join(QStringLiteral(" / ")));
            check(sections->hasMouseTracking() &&
                      qobject_cast<VersionTreeDelegate *>(sections->itemDelegate()) == nullptr,
                  "★ U37.1 那一列是**同一套列表模样**：挂着 `VersionTreeDelegate`（悬停底色那"
                  "一套由代理画），`setMouseTracking(true)` 也开着（少了它代理里那一段一个字"
                  "都不生效 —— 同实例设置那一列）");
            check(sections->width() == InstanceSettingsDialog::navigationTreeWidth(),
                  "server navigation uses the instance settings width");
            check(sections->topLevelItem(0)->sizeHint(0).height() == 36,
                  "server navigation preserves the instance settings row height");
            auto *players = sections->topLevelItem(static_cast<int>(ServerSettingsPage::Section::Players));
            int selectedList = -1;
            QObject::connect(&page, &ServerSettingsPage::playerListChanged, &page,
                             [&selectedList](int index) { selectedList = index; });
            check(players && players->childCount() == MainWindowUI::serverSettingsPlayerListCount(),
                  "player lists are nested navigation items");
            if (players && players->childCount() > 2) {
                sections->setCurrentItem(players->child(2));
                check(page.playerList() == 2 && selectedList == 2 &&
                          page.section() == ServerSettingsPage::Section::Players,
                      "player child selection updates both the list and page");
                page.setPlayerList(0);
                check(sections->currentItem() == players->child(0),
                      "programmatic player list changes keep navigation in sync");
            }

            // ---------- ② 换页：用户点 vs 程序摆 ----------
            int sectionSignals = 0;
            int lastSection = -1;
            QObject::connect(&page, &ServerSettingsPage::sectionChanged, &page,
                             [&sectionSignals, &lastSection](int index) {
                                 ++sectionSignals;
                                 lastSection = index;
                             });
            page.setSection(ServerSettingsPage::Section::Check);
            check(page.section() == ServerSettingsPage::Section::Check &&
                      stack->currentIndex() == static_cast<int>(ServerSettingsPage::Section::Check)
                      && sectionSignals == 0,
                  "★★ U37.2 `setSection()` 是**程序摆的**：页翻过去了，但**不发 "
                  "`sectionChanged`**（发出去的话 `MainWindow` 会再合一次两边的编辑）");
            sections->setCurrentItem(sections->topLevelItem(1));  // 用户点了「原始文本」
            check(sectionSignals == 1 && lastSection == 1 &&
                      stack->currentIndex() == 1 &&
                      page.section() == ServerSettingsPage::Section::RawText,
                  "★★ U37.2 用户点左栏 = 发 `sectionChanged(那一页)` 且右栏跟着翻过去",
                  QStringLiteral("%1 发 / %2").arg(sectionSignals).arg(lastSection));

            // ---------- ③ 表单：四种控件各摆各的 ----------
            const auto makeField = [](const QString &key, const QString &label,
                                      ServerSettingsPage::Kind kind) {
                ServerSettingsPage::FieldView field;
                field.key = key;
                field.label = label;
                field.kind = kind;
                return field;
            };
            QList<ServerSettingsPage::FieldView> fields;
            ServerSettingsPage::FieldView motdField =
                makeField(QStringLiteral("motd"), QStringLiteral("服务器说明"),
                          ServerSettingsPage::Kind::Text);
            // 「服务器简介」那一格挂着"游戏里长什么样"的预览。
            motdField.preview = true;
            fields.append(motdField);
            ServerSettingsPage::FieldView port =
                makeField(QStringLiteral("server-port"), QStringLiteral("端口"),
                          ServerSettingsPage::Kind::Number);
            port.min = 1;
            port.max = 65535;
            port.hint = QStringLiteral("别的程序占着这一格就得换一个");
            fields.append(port);
            fields.append(makeField(QStringLiteral("online-mode"), QStringLiteral("正版验证"),
                                    ServerSettingsPage::Kind::Switch));
            ServerSettingsPage::FieldView difficulty =
                makeField(QStringLiteral("difficulty"), QStringLiteral("难度"),
                          ServerSettingsPage::Kind::Choice);
            difficulty.choiceTokens = QStringList{QStringLiteral("peaceful"),
                                                  QStringLiteral("easy")};
            difficulty.choiceNames = QStringList{QStringLiteral("和平"), QStringLiteral("简单")};
            fields.append(difficulty);

            page.setFields(fields);
            QApplication::processEvents();
            auto *motdEdit =
                page.findChild<QLineEdit *>(MainWindowUI::serverSettingsFieldName(QStringLiteral("motd")));
            auto *portSpin =
                page.findChild<QSpinBox *>(MainWindowUI::serverSettingsFieldName(QStringLiteral("server-port")));
            auto *onlineToggle = page.findChild<ToggleSwitch *>(
                MainWindowUI::serverSettingsFieldName(QStringLiteral("online-mode")));
            auto *difficultyCombo = page.findChild<QComboBox *>(
                MainWindowUI::serverSettingsFieldName(QStringLiteral("difficulty")));
            check(motdEdit != nullptr && portSpin != nullptr && onlineToggle != nullptr &&
                      difficultyCombo != nullptr,
                  "★★ U37.3 每一格摆什么控件全看 `FieldView::kind`：文字→输入框、数字→数字框、"
                  "开关→拨动开关（不是勾选框）、下拉→下拉框 —— 认人走 "
                  "`serverSettingsFieldName(键)`（前缀 + 文件里那个键），不是行号",
                  QStringLiteral("输入框=%1 数字框=%2 开关=%3 下拉=%4")
                      .arg(motdEdit != nullptr)
                      .arg(portSpin != nullptr)
                      .arg(onlineToggle != nullptr)
                      .arg(difficultyCombo != nullptr));
            if (portSpin != nullptr && difficultyCombo != nullptr) {
                check(portSpin->minimum() == 1 && portSpin->maximum() == 65535,
                      "★★ U37.3 数字框那两头就是翻译点喂进来的值域（游戏自己的界，不是这一层"
                      "编的）",
                      QStringLiteral("%1~%2").arg(portSpin->minimum()).arg(portSpin->maximum()));
                check(difficultyCombo->count() == 2 &&
                          difficultyCombo->itemText(0) == QStringLiteral("和平") &&
                          difficultyCombo->itemData(0).toString() == QStringLiteral("peaceful"),
                      "★★ U37.3 下拉框里**存的是 token、显示的是人话**两列对齐（写回文件的是 "
                      "`peaceful` 那一列，用户看的是「和平」那一列）",
                      QStringLiteral("%1=%2").arg(difficultyCombo->itemText(0),
                                                  difficultyCombo->itemData(0).toString()));
            }

            // ---------- ③b. MOTD 那条格式化预览 ----------
            //
            // 这一块画的是**游戏那一边**的样子：`§` 码的颜色、粗斜体、两行。判据只看
            // QLabel 手里那份富文本（真正画出来的像素在 ui_shot 那边），因为这里的坑是
            // "解析完没接到预览上" —— 那一种错在 offscreen 上一点异常都没有。
            {
                auto *preview = page.findChild<QLabel *>(
                    MainWindowUI::serverSettingsMotdPreviewName());
                auto *note = page.findChild<QLabel *>(MainWindowUI::serverSettingsMotdNoteName());
                check(preview != nullptr && note != nullptr,
                      "★★ U37.15 挂着预览的那一行下面真摆出了那块深色框 + 那行小字（名字走 "
                      "`serverSettingsMotdPreviewName()`）",
                      QStringLiteral("框=%1 小字=%2")
                          .arg(preview != nullptr)
                          .arg(note != nullptr));
                if (preview != nullptr && note != nullptr) {
                    // 只有 motd 那一行挂预览：别的行要是也挂上，这一页就会多出几块画着
                    // `§` 码的框（"哪一项是 MOTD"只有一个出处：字段表那一格）。
                    const QList<QLabel *> allPreviews = page.findChildren<QLabel *>(
                        MainWindowUI::serverSettingsMotdPreviewName());
                    check(allPreviews.size() == 1,
                          "★★ U37.15 只有 `FieldView::preview` 为真的那一行挂预览（别的行不挂）",
                          QString::number(allPreviews.size()));
                    check(note->text() == MainWindowUI::serverSettingsMotdNote(),
                          "U37.15 小字说的就是「只画前两行」那一句（产品措辞只有一处）",
                          note->text());

                    // 摆进来的值画出来没有：颜色 / 粗体 / 换行三样。
                    page.setValues(ServerSettingsPage::Values{
                        {QStringLiteral("motd"), QStringLiteral("§c红§l粗\n第二行")}});
                    QApplication::processEvents();
                    const QString painted = preview->text();
                    check(painted.contains(QStringLiteral("#ff5555")) &&
                              painted.contains(QStringLiteral("font-weight:bold")) &&
                              painted.contains(QStringLiteral("<br>")),
                          "★★ U37.15 摆进来的 MOTD 当场画成富文本（`§c` → 红、`§l` → 粗、"
                          "`\\n` → 换行）",
                          painted);
                    // 行首空格要换成 `&nbsp;`：HTML 会把它吃掉，而服务器正是拿它对齐的。
                    page.setValues(ServerSettingsPage::Values{
                        {QStringLiteral("motd"), QStringLiteral("  边距")}});
                    QApplication::processEvents();
                    check(preview->text().contains(QStringLiteral("&nbsp;")),
                          "★★ U37.15 行首空格画成 `&nbsp;`（不然 HTML 把它吃掉，预览就窄了一截）",
                          preview->text());

                    // 敲一个字画一次：用户改动要当场反映到这块框上。
                    if (motdEdit != nullptr) {
                        motdEdit->setText(QStringLiteral("§a绿"));
                        QApplication::processEvents();
                        check(preview->text().contains(QStringLiteral("#55ff55")),
                              "★★ U37.15 用户在输入框里改一个字，预览当场跟着变（不是只在"
                              "开页时画一次）",
                              preview->text());
                    }
                    // 空的那一档不能是一块空白（看起来像预览坏了）。
                    page.setValues(ServerSettingsPage::Values{{QStringLiteral("motd"), QString()}});
                    QApplication::processEvents();
                    check(preview->text().contains(
                              MainWindowUI::serverSettingsMotdEmptyText().toHtmlEscaped()),
                          "★ U37.15 简介是空的时候摆一句「（空的…）」，不摆一块空白",
                          preview->text());
                }
            }

            // ---------- ④ 摆值 / 读回来 / 改过的那几项 ----------
            int editedSignals = 0;
            QObject::connect(&page, &ServerSettingsPage::edited, &page,
                             [&editedSignals] { ++editedSignals; });
            page.setValues(ServerSettingsPage::Values{
                qMakePair(QStringLiteral("motd"), QStringLiteral("欢迎")),
                qMakePair(QStringLiteral("server-port"), QStringLiteral("25565")),
                qMakePair(QStringLiteral("online-mode"), QStringLiteral("true")),
                qMakePair(QStringLiteral("difficulty"), QStringLiteral("easy")),
            });
            check(valuesOf(page.values(), QStringLiteral("motd")) == QStringLiteral("欢迎") &&
                      valuesOf(page.values(), QStringLiteral("server-port")) ==
                          QStringLiteral("25565") &&
                      valuesOf(page.values(), QStringLiteral("online-mode")) ==
                          QStringLiteral("true") &&
                      valuesOf(page.values(), QStringLiteral("difficulty")) ==
                          QStringLiteral("easy"),
                  "★★ U37.4 摆进去什么就读回来什么（数字/开关/下拉都按「写回文件那个词」的形态"
                  "回给翻译点）",
                  valuesOf(page.values(), QStringLiteral("online-mode")));
            check(editedSignals == 0 && page.changedOnly().isEmpty(),
                  "★★ U37.4 **程序摆值不算「用户改过」**：`setValues()` 既不发 `edited()`，"
                  "也不留下补丁（摆进来的那一份同时就是新的基线）",
                  QStringLiteral("edited=%1 补丁=%2")
                      .arg(editedSignals)
                      .arg(page.changedOnly().size()));

            if (portSpin != nullptr) {
                portSpin->setValue(25566);
                const ServerSettingsPage::Values dirty = page.changedOnly();
                check(editedSignals == 1 && dirty.size() == 1 &&
                          dirty.first().first == QStringLiteral("server-port") &&
                          dirty.first().second == QStringLiteral("25566"),
                      "★★ U37.4 用户在某一格里动了 ⇒ `edited()` 发一发、`changedOnly()` 里"
                      "**只有那一项**（保存时写回盘上的就是它）",
                      QStringLiteral("%1 项").arg(dirty.size()));
                page.clearChanged();
                check(page.changedOnly().isEmpty() && editedSignals == 1,
                      "★ U37.4 `clearChanged()` 把基线重设到「现在这一份」（放弃改动 / 开页那两条"
                      "路都靠它）");
            }

            // ---------- ⑤ 保存带：灰亮由两件事夹着 ----------
            page.setSaveState(false, MainWindowUI::serverSettingsDirtyFieldsNote(1));
            check(!save->isEnabled(),
                  "★★ U37.5 `MainWindow` 说「不许存」（`setSaveState(false, …)`）⇒ 「保存」"
                  "点不动");
            check(saveNote->text() == MainWindowUI::serverSettingsDirtyFieldsNote(1),
                  "★ U37.5 保存带左边那句小字**逐字**就是翻译点喂进来的（这一层一个字都不编）",
                  saveNote->text());
            if (portSpin != nullptr) {
                portSpin->setValue(25567);  // 又改了一项 ⇒ 有得可存
            }
            page.setSaveState(true, MainWindowUI::serverSettingsDirtyFieldsNote(1));
            check(save->isEnabled() && revert->isEnabled(),
                  "★★ U37.5 允许存 + 真的改过 ⇒ 「保存」和「放弃改动」这才亮（没改过的时候"
                  "按下去是白按 —— 这一层再兜一道）");
            page.clearChanged();
            check(!save->isEnabled() && !revert->isEnabled(),
                  "★★ U37.5 改的那几项又都改回来了 ⇒ 两颗一起灭（`saveAllowed` 说「允许」也"
                  "不算数，还得真改过东西）");

            // ---------- ⑥ 原始文本：行尾 + 「用户动过」那个记号 ----------
            const QString crlf = QStringLiteral("motd=欢迎\r\nserver-port=25565\r\n");
            page.setRawText(crlf);
            check(page.rawText() == crlf && !page.rawEdited(),
                  "★★★ U37.6 盘上那份是 **CRLF** ⇒ 摆进去、读回来**一个字节都不翻**（文本框"
                  "只认 `\\n`，所以行尾这一步是这一页记在手边、自己补回去的 —— 少了它，"
                  "只改一个端口会把整份文件的行尾翻掉）；而且**程序摆进来的那一份不算用户"
                  "动过**");
            const int beforeType = editedSignals;
            rawEdit->setPlainText(QStringLiteral("motd=改过了\n"));
            check(page.rawEdited() && editedSignals == beforeType + 1,
                  "★★ U37.6 用户在文本框里动了一下 ⇒ 记号打上、`edited()` 发一发（保存带那"
                  "句话要跟着变）");
            page.setRawText(crlf);
            check(!page.rawEdited(),
                  "★ U37.6 再摆一份进来（开页 / 放弃改动）⇒ 记号清掉");
            const int beforeMark = editedSignals;
            page.markRawEdited();
            check(page.rawEdited() && editedSignals == beforeMark && save->isEnabled(),
                  "★★ U37.7 `markRawEdited()` 把记号打上、保存带跟着亮（现在有得存了），"
                  "但**一发 `edited()` 都不发**（内容已经由 `setRawText()` 合好摆进来了 —— "
                  "调用方 `MainWindow` 自己就知道该重算，再发一次只会让它多算一遍）",
                  QStringLiteral("记号=%1 edited=%2 保存键=%3")
                      .arg(page.rawEdited())
                      .arg(editedSignals - beforeMark)
                      .arg(save->isEnabled()));

            // ---------- ⑦ 只读那一档（服务端在跑）----------
            page.setEditable(false, MainWindowUI::serverSettingsRunningNote());
            const bool formLocked =
                (motdEdit == nullptr || !motdEdit->isEnabled()) &&
                (portSpin == nullptr || !portSpin->isEnabled()) &&
                (onlineToggle == nullptr || !onlineToggle->isEnabled()) &&
                (difficultyCombo == nullptr || !difficultyCombo->isEnabled());
            check(formLocked,
                  "★★ U37.8 在跑的时候表单那几格一起锁上（改了也会被服务端退出时写回盘上的"
                  "那一份盖掉）");
            check(rawEdit->isReadOnly() && rawEdit->isEnabled(),
                  "★★★ U37.8 原始文本是**只读而不是禁用**：禁用连「选中复制」都不给了，而在跑"
                  "的时候用户多半正是要来抄一行配置出去");
            check(!backupNow->isEnabled() && !save->isEnabled() && !revert->isEnabled(),
                  "★★ U37.8 要动盘的那几颗（立即备份 / 保存 / 放弃改动）一并锁上");
            check(openBackups->isEnabled() && recheck->isEnabled(),
                  "★★ U37.8 而「只看一眼」的两颗（打开备份文件夹 / 重新检查）**留着能按** —— "
                  "它们只读、不改任何东西");
            check(saveNote->text() == MainWindowUI::serverSettingsRunningNote(),
                  "★★ U37.8 保存带左边那句换成「为什么按不动」（在跑那句盖过一切）",
                  saveNote->text());
            page.setEditable(true, QString());
            check(motdEdit == nullptr || motdEdit->isEnabled(),
                  "★ U37.8 服务端停了 ⇒ 又能改了");

            // ---------- ⑧ 备份列：认路径、安全快照看得出来 ----------
            check(backupList->columnCount() == MainWindowUI::serverSettingsBackupColumns().size(),
                  "★ U37.9 备份列的三格就是唯一出处里那三个（时间 / 大小 / 这一份是什么）");
            ServerSettingsPage::BackupRow row1;
            row1.path = QStringLiteral("C:/backups/world-1.zip");
            row1.when = QStringLiteral("2026-10-13 14:08");
            row1.size = QStringLiteral("12.4 MB");
            row1.scope = QStringLiteral("这个服务端的整个世界文件夹");
            ServerSettingsPage::BackupRow row2 = row1;
            row2.path = QStringLiteral("C:/backups/world-2.zip");
            row2.when = QStringLiteral("2026-10-12 09:30");
            row2.safety = true;
            row2.scope = QStringLiteral("还原前的自动快照");
            page.setBackupRows(QList<ServerSettingsPage::BackupRow>{row1, row2});
            QApplication::processEvents();
            check(backupList->topLevelItemCount() == 2 &&
                      backupList->topLevelItem(0)->text(0) == row1.when &&
                      backupList->topLevelItem(0)->data(0, Qt::UserRole).toString() == row1.path,
                  "★★ U37.9 摆进去两行就是两行，**身份（zip 路径）进 userData** —— 认路径不认"
                  "那行字（时间是会随文案/格式改的）");
            check(!backupList->topLevelItem(0)->font(0).italic() &&
                      backupList->topLevelItem(1)->font(0).italic(),
                  "★★ U37.9 「还原之前自动做的那一份」在行上是**斜体 + 浅一档**——不另编一句话，"
                  "它是什么由 `scope` 那格说清");
            check(page.selectedBackup().isEmpty() && !restore->isEnabled() &&
                      !deleteBackup->isEnabled(),
                  "★★ U37.9 一份都没选中 ⇒ 「从这一份恢复…」「删掉这一份…」点不动（这两颗都"
                  "要用到选中那一份）");
            backupList->setCurrentItem(backupList->topLevelItem(0));
            QApplication::processEvents();
            check(page.selectedBackup() == row1.path && restore->isEnabled() &&
                      deleteBackup->isEnabled(),
                  "★★ U37.9 选中一行 ⇒ 那两颗跟着亮，`selectedBackup()` 回的就是它那条路径",
                  page.selectedBackup());
            // 「恢复」要动盘（现在这一份会先被存成一张安全快照）⇒ 主题色那一档；「删掉」拿不
            // 回来 ⇒ 危险那一档的红；「立即备份」是这一页的确认性动作 ⇒ 也走主题色。
            check(kindOf(deleteBackup) == Theme::buttonKindKey(Theme::ButtonKind::Danger) &&
                      kindOf(restore) == Theme::buttonKindKey(Theme::ButtonKind::Accent) &&
                      kindOf(backupNow) == Theme::buttonKindKey(Theme::ButtonKind::Accent) &&
                      kindOf(openBackups) == Theme::buttonKindKey(Theme::ButtonKind::Ghost),
                  "★★ U37.9 四颗各挂各的档：删 = 危险（红）、恢复和立即备份 = 主题色（确认性"
                  "动作）、打开备份文件夹 = Ghost",
                  QStringLiteral("删=%1 恢复=%2 备份=%3 打开=%4")
                      .arg(kindOf(deleteBackup), kindOf(restore), kindOf(backupNow),
                           kindOf(openBackups)));
            QString restoredPath;
            int restoreSignals = 0;
            QObject::connect(&page, &ServerSettingsPage::restoreBackupRequested, &page,
                             [&restoredPath, &restoreSignals](const QString &path) {
                                 ++restoreSignals;
                                 restoredPath = path;
                             });
            emit backupList->itemDoubleClicked(backupList->topLevelItem(0), 0);
            check(restoreSignals == 1 && restoredPath == row1.path,
                  "★★ U37.9 在一行上双击 == 按「从这一份恢复…」（同一个槽），发的是那条路径",
                  restoredPath);
            QString deletedPath;
            QObject::connect(&page, &ServerSettingsPage::deleteBackupRequested, &page,
                             [&deletedPath](const QString &path) { deletedPath = path; });
            deleteBackup->click();
            check(deletedPath == row1.path,
                  "★★ U37.9 「删掉这一份…」发的是**选中那一行的路径**（不是行号 —— 行号会随"
                  "新备份插进来而变）",
                  deletedPath);
            // 在跑的时候：恢复 / 删除也点不动（它正开着世界里的文件在写）。
            page.setEditable(false, MainWindowUI::serverSettingsRunningNote());
            check(!restore->isEnabled() && !deleteBackup->isEnabled(),
                  "★★ U37.9 在跑的时候「恢复 / 删除」也一并点不动（那两颗动的正是盘上被写着"
                  "的文件）");
            page.setEditable(true, QString());

            // ---------- ⑨ 体检单：三档颜色 / 出路去重 / 只看一眼那两颗 ----------
            const auto makeCheck = [](int severity, const QString &title, const QString &token) {
                ServerSettingsPage::CheckRow row;
                row.severity = severity;
                row.title = title;
                row.detail = title + QStringLiteral("（说明）");
                row.actionToken = token;
                return row;
            };
            page.setCheckRows(QList<ServerSettingsPage::CheckRow>{
                makeCheck(0, QStringLiteral("还没同意 EULA"), QStringLiteral("reinstall")),
                makeCheck(1, QStringLiteral("端口可能被占"), QString()),
                makeCheck(2, QStringLiteral("世界在盘上"), QStringLiteral("open-settings")),
                makeCheck(0, QStringLiteral("有一个值写错了"), QStringLiteral("reinstall")),
                makeCheck(1, QStringLiteral("还没有备份"), QStringLiteral("open-backups")),
            });
            QApplication::processEvents();
            const QPalette checkPalette = checkList->palette();
            check(checkList->topLevelItemCount() == 5 &&
                      checkList->topLevelItem(0)->text(0) == QStringLiteral("还没同意 EULA") &&
                      checkList->topLevelItem(0)->text(1).contains(QStringLiteral("说明")),
                  "★ U37.10 体检单摆的是「这一条 / 说的是什么」两格（次序就是喂进来的次序 —— "
                  "谁排在前由 `ServerCheck` 说了算）");
            if (checkList->topLevelItemCount() == 5) {
                const QColor danger = Theme::dangerColor(checkPalette);
                const QColor warning = Theme::warningColor(checkPalette);
                const QColor muted = checkList->topLevelItem(2)->foreground(0).color();
                check(checkList->topLevelItem(0)->foreground(0).color().rgba() == danger.rgba() &&
                          checkList->topLevelItem(0)->font(0).bold(),
                      "★★★ U37.10 「要处理」那一条 = **危险那一档的红 + 加粗**（颜色和字重是这一"
                      "条多要紧的唯一记号 —— 次序已经由 `ServerCheck` 排好）",
                      checkList->topLevelItem(0)->foreground(0).color().name());
                check(checkList->topLevelItem(1)->foreground(0).color().rgba() == warning.rgba(),
                      "★★ U37.10 「提醒」那一条 = 警告那一档（`Theme::warningColor()`，不是自己"
                      "挑的一个黄）",
                      checkList->topLevelItem(1)->foreground(0).color().name());
                check(muted.alpha() == 150 &&
                          checkList->topLevelItem(2)->foreground(0).color().rgba() == muted.rgba(),
                      "★★ U37.10 「说明」那一条 = 正文色浅一档（不是红也不是黄 —— 它只是告诉"
                      "用户一件事）",
                      QStringLiteral("alpha=%1").arg(muted.alpha()));
            }
            auto *actionsLayout = qobject_cast<QHBoxLayout *>(checkActions->layout());
            check(actionsLayout != nullptr &&
                      checkActions->findChildren<QPushButton *>().size() == 3,
                  "★★★ U37.11 出路按钮**按记号去重**：五条里只出现了三个记号 ⇒ 三颗按钮"
                  "（`reinstall` 出现了两次也只配一颗）",
                  QString::number(checkActions->findChildren<QPushButton *>().size()));
            QList<QPushButton *> actionButtons = checkActions->findChildren<QPushButton *>();
            check(!actionButtons.isEmpty() &&
                      actionButtons.first()->parentWidget() == checkActions,
                  "★ U37.11 出路按钮活在这一格里面（`parentWidget()` 就是 `checkActions` —— "
                  "不是散在页面上别处，重摆的时候才收得干净）");
            if (actionButtons.size() == 3) {
                // 次序按记号**第一次出现**：reinstall → open-settings → open-backups。
                check(actionButtons.at(0)->text() ==
                              MainWindowUI::serverCheckActionText(QStringLiteral("reinstall")) &&
                          actionButtons.at(1)->text() ==
                              MainWindowUI::serverCheckActionText(QStringLiteral("open-settings")) &&
                          actionButtons.at(2)->text() ==
                              MainWindowUI::serverCheckActionText(QStringLiteral("open-backups")),
                      "★★ U37.11 三颗的字就是唯一出处里那三句、次序按记号第一次出现（认记号"
                      "不认这行字）",
                      QStringLiteral("%1 / %2 / %3")
                          .arg(actionButtons.at(0)->text(), actionButtons.at(1)->text(),
                               actionButtons.at(2)->text()));
                check(kindOf(actionButtons.at(1)) ==
                              Theme::buttonKindKey(Theme::ButtonKind::Neutral) &&
                          kindOf(actionButtons.at(2)) ==
                              Theme::buttonKindKey(Theme::ButtonKind::Neutral) &&
                          kindOf(actionButtons.at(0)) ==
                              Theme::buttonKindKey(Theme::ButtonKind::Accent),
                      "★★ U37.11 去「常用设置」/「世界备份」那两颗是**一般档**（只翻页、不改"
                      "任何东西），「修复」那颗是主题色（它要动盘）",
                      QStringLiteral("%1 / %2 / %3")
                          .arg(kindOf(actionButtons.at(0)), kindOf(actionButtons.at(1)),
                               kindOf(actionButtons.at(2))));
                QString actionToken;
                QObject::connect(&page, &ServerSettingsPage::actionRequested, &page,
                                 [&actionToken](const QString &token) { actionToken = token; });
                actionButtons.at(0)->click();
                check(actionToken == QStringLiteral("reinstall"),
                      "★★ U37.11 点一颗发的是它**那个记号**（产品侧拿记号认人）", actionToken);
                // 在跑的时候：要动盘那颗锁上，只翻页那两颗照旧能按。
                // `setEditable()` 会**重摆**这一格（`refreshCheckActions()` 把旧按钮删了
                // 重造）⇒ 这一段必须**重新认一遍**控件，不能抱着上面那一串指针用（悬空的）。
                page.setEditable(false, MainWindowUI::serverSettingsRunningNote());
                const QList<QPushButton *> locked =
                    checkActions->findChildren<QPushButton *>();
                check(locked.size() == 3 && !locked.at(0)->isEnabled() &&
                          locked.at(1)->isEnabled() && locked.at(2)->isEnabled(),
                      "★★★ U37.11 在跑的时候「修复」点不动，而去「常用设置」/「世界备份」那两颗照旧"
                      "能按（只翻页、不改任何东西）");
                page.setEditable(true, QString());
            }
            // 认不出来的记号：这一条照旧摆出来，只是不配按钮（别编一个「按一下就好了」）。
            page.setCheckRows(QList<ServerSettingsPage::CheckRow>{
                makeCheck(1, QStringLiteral("将来才有的毛病"), QStringLiteral("wat")),
            });
            QApplication::processEvents();
            check(checkList->topLevelItemCount() == 1 &&
                      checkActions->findChildren<QPushButton *>().isEmpty(),
                  "★★ U37.11 认不出来的记号 ⇒ 这一条照旧在单子上，但**一颗按钮都不配**（比编"
                  "一个「按一下就好了」出来强）");
            page.setCheckSummary(QStringLiteral("1 项提醒"));
            check(checkSummary->text() == QStringLiteral("1 项提醒"),
                  "★ U37.12 顶上那句总括**逐字**就是 `ServerCheck::summaryText()` 的结论");

            // ---------- 顶上两行 + 保存带那几颗各发各的信号 ----------
            page.setServerName(QStringLiteral("我的服务端"));
            page.setStatusText(ServerModel::statusName(ServerModel::Status::Stopped));
            check(title->text() == QStringLiteral("我的服务端") &&
                      status->text() == ServerModel::statusName(ServerModel::Status::Stopped),
                  "★ U37.13 名字和状态逐字就是喂进来的（状态那句话的唯一出处是 "
                  "`ServerModel::statusName()`）",
                  QStringLiteral("%1 / %2").arg(title->text(), status->text()));
            int backupNowSignals = 0;
            int openFolderSignals = 0;
            int recheckSignals = 0;
            int saveSignals = 0;
            int revertSignals = 0;
            QObject::connect(&page, &ServerSettingsPage::backupNowRequested, &page,
                             [&backupNowSignals] { ++backupNowSignals; });
            QObject::connect(&page, &ServerSettingsPage::openBackupFolderRequested, &page,
                             [&openFolderSignals] { ++openFolderSignals; });
            QObject::connect(&page, &ServerSettingsPage::recheckRequested, &page,
                             [&recheckSignals] { ++recheckSignals; });
            QObject::connect(&page, &ServerSettingsPage::saveRequested, &page,
                             [&saveSignals] { ++saveSignals; });
            QObject::connect(&page, &ServerSettingsPage::revertRequested, &page,
                             [&revertSignals] { ++revertSignals; });
            backupNow->click();
            openBackups->click();
            recheck->click();
            page.setSaveState(true, MainWindowUI::serverSettingsDirtyFieldsNote(1));
            if (portSpin != nullptr) {
                portSpin->setValue(25599);
            }
            save->click();
            revert->click();
            check(backupNowSignals == 1 && openFolderSignals == 1 && recheckSignals == 1 &&
                      saveSignals == 1 && revertSignals == 1,
                  "★★ U37.14 那几颗各发各的信号，一发不多一发不少（串线的话用户按「立即备份」"
                  "会去下载别的东西）",
                  QStringLiteral("备份=%1 打开=%2 重查=%3 保存=%4 放弃=%5")
                      .arg(backupNowSignals)
                      .arg(openFolderSignals)
                      .arg(recheckSignals)
                      .arg(saveSignals)
                      .arg(revertSignals));

            // ---------- ③ 内容（模组 / 数据包，「内容管理」）----------
            // 这一页只有名字上的规矩在判据层（`ServerContent`，ZAH 那一节钉）；这里管的是
            // **界面上那几格**：下拉框摆的是什么、行里记的是不是原样那个名字、禁用着的那一行
            // 长什么样、那颗按钮的字会不会跟着选中行翻、以及每一颗各发各的信号。
            {
                auto *contentNote = page.findChild<QLabel *>(
                    MainWindowUI::serverSettingsContentNoteName());
                auto *contentCombo = sections->topLevelItem(static_cast<int>(ServerSettingsPage::Section::Content));
                auto *contentTree = page.findChild<QTreeWidget *>(
                    MainWindowUI::serverSettingsContentTreeName());
                auto *contentAdd = page.findChild<QPushButton *>(
                    MainWindowUI::serverSettingsContentAddButtonName());
                auto *contentToggle = page.findChild<QPushButton *>(
                    MainWindowUI::serverSettingsContentToggleButtonName());
                auto *contentOpenFolder = page.findChild<QPushButton *>(
                    MainWindowUI::serverSettingsContentOpenFolderButtonName());
                auto *contentRefresh = page.findChild<QPushButton *>(
                    MainWindowUI::serverSettingsContentRefreshButtonName());
                check(contentNote != nullptr && contentCombo != nullptr &&
                          contentTree != nullptr && contentAdd != nullptr &&
                          contentToggle != nullptr && contentOpenFolder != nullptr &&
                          contentRefresh != nullptr,
                      "★★ U37.16 内容那一段七格都在（说明 / 类型 / 表 / 添加 / 启用禁用 / 打开"
                      "文件夹 / 刷新 —— 认 objectName，不认这一份布置）");
                if (contentNote != nullptr && contentCombo != nullptr &&
                    contentTree != nullptr && contentAdd != nullptr &&
                    contentToggle != nullptr && contentOpenFolder != nullptr &&
                    contentRefresh != nullptr) {
                    check(contentCombo->childCount() ==
                                  MainWindowUI::serverSettingsContentKindCount() &&
                              contentCombo->child(0)->text(0) ==
                                  MainWindowUI::serverSettingsContentKindTitle(0) &&
                              contentCombo->child(1)->text(0) ==
                                  MainWindowUI::serverSettingsContentKindTitle(1),
                          "★★ U37.16 类型下拉框的两项逐字就是唯一出处那两句（模组 / 数据包）；"
                          "**这里的下标同时就是 `ServerContent::Kind`**",
                          QStringLiteral("%1 / %2")
                              .arg(contentCombo->child(0)->text(0), contentCombo->child(1)->text(0)));
                    check(contentTree->columnCount() ==
                              MainWindowUI::serverSettingsContentColumns().size(),
                          "★ U37.16 表是三列（名字 / 大小 / 状态）",
                          QString::number(contentTree->columnCount()));

                    // 程序摆的那一下**不发** `contentKindChanged`（用户点才发）。
                    int kindSignals = 0;
                    int lastKind = -1;
                    QObject::connect(&page, &ServerSettingsPage::contentKindChanged, &page,
                                     [&kindSignals, &lastKind](int index) {
                                         ++kindSignals;
                                         lastKind = index;
                                     });
                    page.setContentKind(1);
                    check(page.contentKind() == 1 && kindSignals == 0,
                          "★★ U37.16 `setContentKind()` 只摆下拉框、**不发信号**（程序摆的"
                          "不算用户改了；发了那边会白重列一次）",
                          QString::number(kindSignals));
                    sections->setCurrentItem(contentCombo->child(0));
                    check(kindSignals == 1 && lastKind == 0 && page.contentKind() == 0,
                          "★★ U37.16 用户自己切下拉框才发一发，带的正是新下标",
                          QStringLiteral("发了 %1 发，最后一发=%2").arg(kindSignals).arg(lastKind));

                    QList<ServerSettingsPage::ContentRow> rows;
                    ServerSettingsPage::ContentRow enabledRow;
                    enabledRow.name = QStringLiteral("alpha.jar");
                    enabledRow.size = QStringLiteral("1.2 MB");
                    enabledRow.disabled = false;
                    rows << enabledRow;
                    ServerSettingsPage::ContentRow disabledRow;
                    disabledRow.name = QStringLiteral("beta.zip.disabled");
                    disabledRow.size = QStringLiteral("340 KB");
                    disabledRow.disabled = true;
                    rows << disabledRow;
                    page.setContentRows(rows);
                    QApplication::processEvents();
                    check(contentTree->topLevelItemCount() == 2,
                          "★ U37.16 两行就摆两行（禁用着的那一行也在 —— 它还在文件夹里）",
                          QString::number(contentTree->topLevelItemCount()));
                    if (contentTree->topLevelItemCount() == 2) {
                        QTreeWidgetItem *first = contentTree->topLevelItem(0);
                        QTreeWidgetItem *second = contentTree->topLevelItem(1);
                        check(first->text(0) == QStringLiteral("alpha.jar") &&
                                  first->text(1) == QStringLiteral("1.2 MB") &&
                                  first->text(2) ==
                                      MainWindowUI::serverSettingsContentStateText(false),
                              "★ U37.16 第一行的三格逐字摆进来；状态那格走唯一出处",
                              QStringLiteral("%1 / %2 / %3")
                                  .arg(first->text(0), first->text(1), first->text(2)));
                        check(first->data(0, Qt::UserRole).toString() ==
                                      QStringLiteral("alpha.jar") &&
                                  second->data(0, Qt::UserRole).toString() ==
                                      QStringLiteral("beta.zip.disabled"),
                              "★★ U37.16 认人记的是**原样那个文件名**（带 `.disabled` 尾巴的"
                              "也照原样 —— 摘尾巴那条规矩的出处是 `ServerContent`，这一层不抄）");
                        check(!first->data(0, Qt::UserRole + 1).toBool() &&
                                  second->data(0, Qt::UserRole + 1).toBool(),
                              "★★ U37.16 「禁用着没有」摆进来那一份说了算（`false`/`true` 原样）");
                        check(second->font(0).italic() && second->font(1).italic() &&
                                  second->font(2).italic() &&
                                  second->foreground(0).color().alpha() == 150 &&
                                  second->text(2) ==
                                      MainWindowUI::serverSettingsContentStateText(true),
                              "★★ U37.16 禁用着的那一行整行画灰 + 斜体（它不加载了），状态那格"
                              "写「已禁用（不加载）」",
                              QStringLiteral("italic=%1 alpha=%2")
                                  .arg(second->font(0).italic())
                                  .arg(second->foreground(0).color().alpha()));
                        check(!first->font(0).italic() &&
                                  first->text(2) ==
                                      MainWindowUI::serverSettingsContentStateText(false),
                              "★ U37.16 启用着的那一行不斜、状态写「启用中」（两类不是一个样子）");

                        // 没选中：那颗按钮没意义 ⇒ 灰着，字落在「禁用」上（选中了才翻）。
                        contentTree->setCurrentItem(nullptr);
                        check(page.selectedContentName().isEmpty() &&
                                  contentToggle->isEnabled() == false &&
                                  contentToggle->text() ==
                                      MainWindowUI::serverSettingsContentDisableText(),
                              "★★ U37.16 没选中任何一行 ⇒「启用 / 禁用」那颗灰着（点了也"
                              "不知道该动谁）");

                        QString toggledName;
                        bool toggledEnable = true;
                        int toggleSignals = 0;
                        QObject::connect(&page,
                                         &ServerSettingsPage::contentToggleRequested, &page,
                                         [&toggledName, &toggledEnable,
                                          &toggleSignals](const QString &name, bool enable) {
                                             toggledName = name;
                                             toggledEnable = enable;
                                             ++toggleSignals;
                                         });
                        contentTree->setCurrentItem(first);
                        check(page.selectedContentName() == QStringLiteral("alpha.jar") &&
                                  page.selectedContentDisabled() == false &&
                                  contentToggle->isEnabled() &&
                                  contentToggle->text() ==
                                      MainWindowUI::serverSettingsContentDisableText(),
                              "★★ U37.16 选中启用着的那一行 ⇒ 按钮的字是「禁用」");
                        contentToggle->click();
                        check(toggleSignals == 1 &&
                                  toggledName == QStringLiteral("alpha.jar") &&
                                  toggledEnable == false,
                              "★★ U37.16 按下去发的是（这一行的原样名字, `false`=要禁用）"
                              "—— 产品侧拿名字认文件、拿布尔认方向",
                              QStringLiteral("%1 / %2 / %3")
                                  .arg(toggleSignals)
                                  .arg(toggledName)
                                  .arg(toggledEnable));
                        contentTree->setCurrentItem(second);
                        check(page.selectedContentDisabled() == true &&
                                  contentToggle->text() ==
                                      MainWindowUI::serverSettingsContentEnableText(),
                              "★★ U37.16 选中禁用着的那一行 ⇒ 按钮翻成「启用」");
                        contentToggle->click();
                        check(toggleSignals == 2 && toggledEnable == true,
                              "★★ U37.16 那一发要的是「启用」（`true`，正好翻回来）",
                              QString::number(toggledEnable));

                        int addSignals = 0;
                        int openFolderSignals = 0;
                        int refreshSignals = 0;
                        QObject::connect(&page, &ServerSettingsPage::contentAddRequested,
                                         &page, [&addSignals] { ++addSignals; });
                        QObject::connect(
                            &page, &ServerSettingsPage::contentOpenFolderRequested, &page,
                            [&openFolderSignals] { ++openFolderSignals; });
                        QObject::connect(&page, &ServerSettingsPage::contentRefreshRequested,
                                         &page, [&refreshSignals] { ++refreshSignals; });
                        contentAdd->click();
                        contentOpenFolder->click();
                        contentRefresh->click();
                        check(addSignals == 1 && openFolderSignals == 1 &&
                                  refreshSignals == 1 && toggleSignals == 2,
                              "★★ U37.16 那几颗各发各的信号，一发不多一发不少（串线的话按"
                              "「刷新」会去弹文件选择框）",
                              QStringLiteral("添加=%1 打开=%2 刷新=%3 翻=%4")
                                  .arg(addSignals)
                                  .arg(openFolderSignals)
                                  .arg(refreshSignals)
                                  .arg(toggleSignals));
                    }
                    page.setContentNote(QStringLiteral("文件夹：/srv/mc/mods"));
                    check(contentNote->text() == QStringLiteral("文件夹：/srv/mc/mods"),
                          "★ U37.16 顶上那句说明逐字摆进来（文件夹在哪儿由翻译点拼好）");

                    // 服务端在跑：动盘那两颗锁上；只读那两颗（打开文件夹 / 刷新）照旧能按 ——
                    // 它们不改任何东西，看一眼盘上的情况永远是安全的。
                    const QString runningNote = MainWindowUI::serverSettingsRunningNote();
                    page.setEditable(false, runningNote);
                    check(!contentAdd->isEnabled() && !contentToggle->isEnabled() &&
                              contentOpenFolder->isEnabled() && contentRefresh->isEnabled(),
                          "★★★ U37.16 在跑的时候「添加…」和「启用 / 禁用」锁上，而「打开文件夹」"
                          "「刷新」照旧能按（只读，不改任何东西）");
                    page.setEditable(true, QString());
                    check(contentAdd->isEnabled(),
                          "★ U37.16 停下来了「添加…」就回来了");

                    page.setContentRows(QList<ServerSettingsPage::ContentRow>());
                    check(contentTree->topLevelItemCount() == 0 &&
                              contentToggle->isEnabled() == false,
                          "★ U37.16 空的文件夹 ⇒ 一行都没有、那颗按钮也跟着灰回去");
                }
            }

            // ---------- ⑤ 「踢出…」（名字不是身份键）----------
            {
                auto *playerTree = page.findChild<QTreeWidget *>(
                    MainWindowUI::serverSettingsPlayerTreeName());
                auto *kickButton = page.findChild<QPushButton *>(
                    MainWindowUI::serverSettingsPlayerKickButtonName());
                check(playerTree != nullptr && kickButton != nullptr &&
                          kickButton->text() == MainWindowUI::serverSettingsPlayerKickText(),
                      "★★ U37.16 「踢出…」那颗在玩家名单那一排里，字逐字就是唯一出处");
                if (playerTree != nullptr && kickButton != nullptr) {
                    check(kindOf(kickButton) ==
                              Theme::buttonKindKey(Theme::ButtonKind::Neutral),
                          "★ U37.16 「踢出…」是一般档（它只送一条命令，不改盘上任何文件）");

                    ServerSettingsPage::PlayerRow steve;
                    steve.identity = QStringLiteral("uuid-steve");
                    steve.name = QStringLiteral("Steve");
                    steve.detail = QStringLiteral("uuid-steve");
                    steve.kickName = QStringLiteral("Steve");
                    ServerSettingsPage::PlayerRow ipRow;
                    ipRow.identity = QStringLiteral("1.2.3.4");
                    ipRow.name = QStringLiteral("1.2.3.4");
                    ipRow.detail = QStringLiteral("1.2.3.4");
                    ipRow.kickName = QString();  // 封禁 IP 那一档没有玩家名可踢
                    page.setPlayerRows(QList<ServerSettingsPage::PlayerRow>{steve, ipRow});
                    page.setPlayerKickAvailable(true);
                    playerTree->setCurrentItem(nullptr);
                    check(!kickButton->isEnabled(),
                          "★★ U37.16 没选中人 ⇒「踢出…」灰着（不知道该踢谁）");

                    QString kickedName;
                    int kickSignals = 0;
                    QObject::connect(&page, &ServerSettingsPage::playerKickRequested, &page,
                                     [&kickedName, &kickSignals](const QString &name) {
                                         kickedName = name;
                                         ++kickSignals;
                                     });
                    if (playerTree->topLevelItemCount() == 2) {
                        playerTree->setCurrentItem(playerTree->topLevelItem(0));
                        check(kickButton->isEnabled() &&
                                  page.selectedPlayerName() == QStringLiteral("Steve"),
                              "★★ U37.16 选中一个玩家、服务端又在跑 ⇒ 能按（名字取的是"
                              "「玩家名」那一格，不是身份键）",
                              page.selectedPlayerName());
                        kickButton->click();
                        check(kickSignals == 1 && kickedName == QStringLiteral("Steve"),
                              "★★★ U37.16 按下去发的是**玩家名**（`kick <名字>` 那一句要"
                              "的就是它；身份键 uuid 不是命令里的东西）",
                              QStringLiteral("%1 / %2").arg(kickSignals).arg(kickedName));

                        // 封禁 IP 那一档：行在、选中了，但没有名字可踢 ⇒ 仍然灰着。
                        playerTree->setCurrentItem(playerTree->topLevelItem(1));
                        check(!kickButton->isEnabled() && page.selectedPlayerName().isEmpty(),
                              "★★ U37.16 封禁 IP 那一档选中了也点不动（`kick` 命令收的是"
                              "玩家名，不是地址 —— 编一个名字出来只会踢错人）");
                    }

                    // 服务端停了：踢谁都没有意义（命令没地方送）。
                    playerTree->setCurrentItem(playerTree->topLevelItem(0));
                    page.setPlayerKickAvailable(false);
                    check(!kickButton->isEnabled(),
                          "★★ U37.16 服务端没在跑 ⇒「踢出…」灰着（和「加 / 移除」正好"
                          "相反：那两颗跟着「能不能改文件」，这一颗跟着「进程在不在」）");
                    page.setPlayerKickAvailable(true);
                }
            }

            page.close();
            QApplication::processEvents();
        }
    }
    return 0;
}


