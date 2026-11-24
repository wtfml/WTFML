#include "Support.hpp"
int UiConfigEditorChecks() {
    // ---- U11 「配置文件」那一页：点一个文件 → 就地改----
    //
    // 要求：「能不能做到在启动器就直接编辑模组的 config.json（或类似的设置文件）？」
    // 补的那一句才是关键：「这些文件不都是以文本形式存储的吗，不能直接以 txt 来解析吗」
    // —— 对。所以这一页**不解析格式**，就是一个纯文本编辑器。（"勾选框 / 滑条"那种界面
    // 是做不到的：ModMenu 的 `ConfigScreenFactory` 把界面转交给模组自己的 Java 代码，
    // 那份 schema 只活在游戏进程里 —— 见 ConfigFileEditorPage.hpp 那段。）
    //
    // 分三块验：
    // ① `ConfigFileText`（纯函数：认 BOM、认行尾、认是不是 UTF-8）—— 这一层是这个功能
    // **唯一一处安全关键**。"用户改两个字、别的字节一个都不许动"这条全靠它。
    // ② 编辑器那一格（行号、脏 / 干净、只读那几档、「返回」与 Esc）。
    // ③ 对话框那两格怎么切、以及"改过就不许悄悄关掉"那道闸。
    {
        // ---------- U11a ConfigFileText：改一个值，别的字节一个都不许动 ----------
        {
            const auto roundTrips = [](const QByteArray &raw) {
                const ConfigFileText::Loaded loaded = ConfigFileText::decode(raw);
                return ConfigFileText::encode(loaded, loaded.text) == raw;
            };
            check(roundTrips(QByteArrayLiteral("a\nb\n")),
                  "★ U11a 纯 LF 的文件：往返逐字节不变");
            check(roundTrips(QByteArrayLiteral("a\r\nb\r\n")),
                  "★ U11a 纯 CRLF 的也逐字节不变");
            check(roundTrips(QByteArrayLiteral("a\r\nb")),
                  "★ U11a 末尾**没有**换行的也往返得住（多补一个或少一个都是「每存一次就多"
                  "一个空行」那种慢性病）");
            check(roundTrips(QByteArray()),
                  "★ U11a 空文件往返得住");
            check(roundTrips(QByteArrayLiteral("\xEF\xBB\xBF"
                                               "a\r\n")),
                  "★ U11a 带 BOM 的也逐字节不变（解码器会把那三个字节吃掉，所以它必须单独"
                  "记一位、写回去时补上）");
            check(roundTrips(QByteArrayLiteral("a\r\nb\nc")),
                  "★ U11a **混着来**的也往返得住（一行一个结尾符）");
            check(roundTrips(QByteArrayLiteral("\xff\xfe\x00a")),
                  "★ U11a 不是 UTF-8 的那一份也往返得住 —— 它走的是「一个字都没改 ⇒ 原样回"
                  "那些字节」那条早退，所以坏字节一个都不会被 U+FFFD 换掉");
        }

        // 这两条才是"改一个字"那条动线上真正发生的事：编辑器里的正文全是 `\n`
        // （QPlainTextEdit 的模型就是这样），写回去的时候得把 CRLF 还回来 ——
        // **包括用户新敲出来的那一行**。
        {
            const ConfigFileText::Loaded crlf =
                ConfigFileText::decode(QByteArrayLiteral("a\r\nb\r\n"));
            check(crlf.text == QStringLiteral("a\nb\n"),
                  "U11a 前提：CRLF 那一份解出来的正文里全是 `\\n`（编辑器看到的样子）");
            check(ConfigFileText::encode(crlf, QStringLiteral("a\nB\n")) ==
                      QByteArrayLiteral("a\r\nB\r\n"),
                  "★★ U11a 改一行的内容：行尾**逐字节**和原来一样（变的只有那一行正文）");
            check(ConfigFileText::encode(crlf, QStringLiteral("a\nb\nc\n")) ==
                      QByteArrayLiteral("a\r\nb\r\nc\r\n"),
                  "★★ U11a **用户新添的那一行也写成 CRLF**（不是 `\\n`）—— 不然「加一行」"
                  "这一个动作就把整个文件的行尾风格改了，git 上看着是「整个文件都重写了」");

            // 混着来的那一份：多数派是 LF，但**第 0 行原来是 CRLF，它自己那一份得保住**。
            const ConfigFileText::Loaded mixed =
                ConfigFileText::decode(QByteArrayLiteral("a\r\nb\nc\n"));
            check(mixed.dominant == ConfigFileText::Eol::Lf,
                  "U11a 前提：那一份的多数派是 LF（两票对一票）");
            check(ConfigFileText::encode(mixed, QStringLiteral("A\nb\nc\nd\n")) ==
                      QByteArrayLiteral("A\r\nb\nc\nd\n"),
                  "★★ U11a 混着来的那一份：原来那一行**按它自己那个结尾符**走（第 0 行还是 "
                  "CRLF），只有新添的第 3 行按多数派（LF）—— 不是「整份按多数派刷一遍」");
        }

        // BOM 那一趟得单独看：那三个字节不进正文，只在写回去时补。
        {
            const ConfigFileText::Loaded bom =
                ConfigFileText::decode(QByteArrayLiteral("\xEF\xBB\xBF"
                                                         "x\r\n"));
            check(bom.bom, "U11a 前提：FF BB BF 开头的那一份认得出 BOM");
            check(bom.text == QStringLiteral("x\n"),
                  "U11a 前提：解码器把那三个字节吃掉了（正文里没有它）");
            check(ConfigFileText::encode(bom, QStringLiteral("x\ny\n")) ==
                      QByteArrayLiteral("\xEF\xBB\xBF"
                                        "x\r\ny\r\n"),
                  "★ U11a 存回去时 BOM 补在最前头 —— 不补的话，那几个认 BOM 的模组读到的"
                  "第一个键就变成了一个带 U+FEFF 的怪名字");
        }

        // 三档"不给改"：不是 UTF-8 / 太大 / 正常。**只给看不给改**（不是画一个灰的保存键：
        // 灰键说的是"还没资格点"，而这儿的问题是"点了也没意义"）。
        {
            const ConfigFileText::Loaded bad =
                ConfigFileText::decode(QByteArrayLiteral("key = \xb4\xb4\n"));
            check(!bad.utf8Ok && !bad.editable(),
                  "★ U11a **不是 UTF-8 的文件不给改**（只给看）—— 改了会把里面的字整个弄坏");
            check(ConfigFileText::decode(QByteArrayLiteral("key = 1\n")).editable(),
                  "U11a 前提：正常的 UTF-8 是给改的");

            const qint64 limit = ConfigFileText::editableByteLimit();
            const ConfigFileText::Loaded huge = ConfigFileText::decode(QByteArray(limit + 1, 'x'));
            check(huge.tooBig && !huge.editable(),
                  "★ U11a 超过上限的那一份不给改",
                  QStringLiteral("上限 %1 MB").arg(limit / (1024 * 1024)));
            check(!ConfigFileText::decode(QByteArray(limit, 'x')).tooBig,
                  "★ U11a 边界：**正好到上限**还是给改的（判的是「大于」不是「大于等于」）");

            // ---- ★ 尾巴上被截断的那半个字 ----
            //
            // 这一位是 `hasError()` **一位不够**的地方：它只数"认出来的非法字节"
            // （`qstringconverter_base.h:157`），而**末尾那半截多字节序列不算非法** ——
            // 解码器把它留在 `state->remainingChars` 里等下一批（`qstringconverter.cpp:993`），
            // 一次喂完的时候它永远不碰 `invalidChars`。原来只判 hasError() ⇒ 这一档被当成
            // "可以改"，用户一改一存，那几个字节**静默地永久没了**。
            const QByteArray halfTail = QByteArrayLiteral("key = 1\n\xE4\xB8");
            check(!ConfigFileText::decode(halfTail).utf8Ok,
                  "★★ U11a 正文合法、**尾巴上缺半个字**（`\\xE4\\xB8` 是「中」的前两个字节）"
                  "也算不合法 —— 只判 hasError() 的话这一档会漏过去，改一次就静默丢掉那几个字节");
            // 反过来：**正**常的 UTF-8（含汉字、含 BOM）一个都不许被误判。
            check(ConfigFileText::decode(QByteArrayLiteral("key = 中\n")).utf8Ok,
                  "U11a 前提：完整的多字节字（`中`）是合法的 UTF-8");
            check(ConfigFileText::decode(QByteArrayLiteral("\xEF\xBB\xBFkey = 中\n")).utf8Ok,
                  "★ U11a 带 BOM 的那一份照样合法 —— 解码器把 BOM 吃掉了，往返比的是"
                  "**摘掉 BOM 之后**那串字节（比错了就会把每一份带 BOM 的配置都判成不给改）");
        }

        // ---------- U11b 编辑器那一格 ----------
        {
            ConfigFileEditorPage editor;
            editor.show();

            const ConfigFileText::Loaded crlf =
                ConfigFileText::decode(QByteArrayLiteral("a = 1\r\nb = 2\r\n"));
            editor.setFile(QStringLiteral("D:/示例/config.json"), crlf);

            check(editor.path() == QStringLiteral("D:/示例/config.json"),
                  "U11b 这一格记得住开的是哪个文件");
            check(editor.text() == QStringLiteral("a = 1\nb = 2\n"),
                  "★ U11b 摆进去的正文就是编辑器里那一份（行尾一律 `\\n`）");
            check(!editor.isDirty(),
                  "★★ U11b 刚摆进去是**干净的** —— 不干净的话，用户进来什么都没干、点一下"
                  "「返回」就被问一句「改了没保存」");
            check(editor.isEditable() && editor.saveButton() != nullptr &&
                      editor.saveButton()->isVisible(),
                  "★ U11b 可编辑时「保存」是画出来的");
            check(editor.noteLabel() != nullptr &&
                      editor.noteLabel()->text() == ConfigFileEditorPage::hintText(),
                  "U11b 底下那行字是可编辑那一档的提示");
            check(editor.gutterWidth() > 0,
                  "★ U11b 有行号那一格（宽度 > 0）", QStringLiteral("%1 px").arg(editor.gutterWidth()));

            // 行号那一格会跟着行数变宽。★ ：行数从 120 提到 **2000**。
            // 为什么：那一格有个"个位数行号也别贴着边"的**下限**（`kGutterMinWidth`），
            // 宽度是 `rightPad + max(下限, 字宽 × (位数+1))`。真 windows 平台给编辑框的
            // 等宽字体数字比 offscreen 那套桩字体**窄**，所以三位数（120 行）连同下限一起
            // 算出来还是那个下限、量下来 `40 → 40`（看着像"不跟着变"）；桩字体下三位数已经
            // 越过了下限，才量得出变宽。要量的是"位数多了会变宽"这条**不变的规矩**，
            // 就得让位数多到两种字体都肯定越限：**4 位数（2000 行）**下 `5 × 字宽 > 下限`
            // 在两套字体上都成立。
            const int widthAtTwo = editor.gutterWidth();
            QString manyLines;
            // 文楷注册进来以后 offscreen 下数字更窄，四位数也可能没越过下限 ⇒ 五位数。
            for (int i = 0; i < 20000; ++i) {
                manyLines += QStringLiteral("line\n");
            }
            editor.editor()->setPlainText(manyLines);
            check(editor.gutterWidth() > widthAtTwo,
                  "★ U11b 行数一多，行号那一格跟着变宽（四位数放不进原来那么宽）",
                  QStringLiteral("%1 → %2 px").arg(widthAtTwo).arg(editor.gutterWidth()));
            check(editor.isDirty(), "U11b 改了就是脏的");

            // 存：发出去的是**整份字节**，不是"用户改了什么"。
            QByteArray written;
            QString writtenPath;
            QObject::connect(&editor, &ConfigFileEditorPage::saveRequested, &editor,
                             [&written, &writtenPath](const QString &path, const QByteArray &bytes) {
                                 writtenPath = path;
                                 written = bytes;
                             });
            editor.setFile(QStringLiteral("D:/示例/config.json"), crlf);
            editor.editor()->setPlainText(QStringLiteral("a = 42\nb = 2\n"));
            check(editor.isDirty(), "U11b 前提：改了一个值");
            editor.saveButton()->click();
            check(writtenPath == QStringLiteral("D:/示例/config.json"),
                  "★ U11b 发出去的第一样是**这个文件**的路径（不是别的某个）");
            check(written == QByteArrayLiteral("a = 42\r\nb = 2\r\n"),
                  "★★ U11b 发出去的是**整份字节**、而且行尾还是 CRLF —— 改一个值不该把整个"
                  "文件的行尾换掉");
            check(editor.isDirty(),
                  "U11b 前提：还没人告诉它存成了没有，所以它**照旧是脏的**（写盘在"
                  "MainWindow 那边，这一格不许自己猜）");
            editor.markSaved();
            check(!editor.isDirty(),
                  "★★ U11b markSaved() 之后不脏了 —— 它的基线也跟着换成了刚写下去那一份。"
                  "不换的话，用户存完再点「返回」还会被问一句「改了没保存」，而它明明刚存过");
            editor.saveButton()->click();
            check(written == QByteArrayLiteral("a = 42\r\nb = 2\r\n"),
                  "★ U11b 紧接着再存一次，字节一模一样（存两次不会多出或少掉什么）");

            // 存过之后再改、再丢：要退回到**刚存下去那一份**，不是最初读进来那一份。
            editor.editor()->setPlainText(QStringLiteral("乱改的\n"));
            check(editor.isDirty(), "U11b 前提：又改过了");
            editor.discardChanges();
            check(!editor.isDirty() && editor.text() == QStringLiteral("a = 42\nb = 2\n"),
                  "★★ U11b discardChanges() 退回的是**上一次存下去的那一份**（存过之后基线"
                  "跟着走了，不是退回最开始读进来那一份）");

            // 两条回列表的路：那颗「返回」，和 Esc。
            int backCount = 0;
            QObject::connect(&editor, &ConfigFileEditorPage::backRequested, &editor,
                             [&backCount] { ++backCount; });
            editor.backButton()->click();
            check(backCount == 1, "★ U11b 「返回」发一发 backRequested()（这一格不自己切格子）");

            // Esc **必须在这一层被接住**：不然它会一路冒到「实例设置」那个 QDialog 上，
            // 那一下关掉的是**整个实例设置页** —— 用户想的是"退回文件列表"。
            //
            // NOTE: 事件发给**里面那个编辑框**（那是真正拿着焦点的控件），看它冒不冒上来。
            // 直接发给这一格等于把要验的那一步跳过去了。
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(editor.editor(), &esc);
            check(backCount == 2 && esc.isAccepted(),
                  "★★ U11b 在编辑框上按 Esc = 「返回」：编辑框自己不认它、它冒到这一层被接住"
                  "并吃掉 —— 不接的话它会一路冒到「实例设置」那个 QDialog 上，那一下关掉的"
                  "是**整个实例设置页**，而用户想的是「退回文件列表」",
                  QStringLiteral("收到 %1 次").arg(backCount));

            // 只读那一档：正文摆得进去，但改不了、也没有「保存」。
            const ConfigFileText::Loaded bad =
                ConfigFileText::decode(QByteArrayLiteral("key = \xb4\xb4\n"));
            editor.setFile(QStringLiteral("D:/示例/坏编码.json"), bad);
            check(!editor.isEditable(), "★ U11b 不是 UTF-8 的那一份：这一格是**只读**的");
            check(editor.saveButton() != nullptr && !editor.saveButton()->isVisible(),
                  "★★ U11b 只读时「保存」**整个不画**（不是画成灰的）");
            check(editor.editor() != nullptr && editor.editor()->isReadOnly(),
                  "★ U11b 编辑框本身也是 readOnly（不只是藏了一颗按钮）");
            check(editor.noteLabel() != nullptr &&
                      editor.noteLabel()->text().startsWith(ConfigFileEditorPage::readOnlyPrefix()),
                  "★ U11b 底下那行字换成了只读那一档、而且说清了为什么",
                  editor.noteLabel() != nullptr ? editor.noteLabel()->text() : QString());
            check(!editor.isDirty(), "U11b 只读的那一份也是干净的");
        }

        // ---------- U11c 对话框里那两格怎么切、以及"改过就不许悄悄关掉" ----------
        {
            using Dlg = InstanceSettingsDialog;
            const QString instanceDisplayName = QStringLiteral("我的整合包");
            const QString instanceDirName = QStringLiteral("我的整合包-1.20.1-Forge");
            const Dlg::GlobalValues global = [] {
                Dlg::GlobalValues g;
                g.javaPath = QStringLiteral("C:/全局/java.exe");
                g.xmx = 4096;
                g.xms = 1024;
                g.autoMemory = false;
                g.extraJvmArgs = QStringLiteral("-XX:+UseG1GC");
                return g;
            }();

            QTemporaryDir scratch;
            const QString root = scratch.path();
            const QString configDir = root + QStringLiteral("/config");
            const auto mkfile = [](const QString &path, const QByteArray &bytes) {
                QDir().mkpath(QFileInfo(path).absolutePath());
                QFile file(path);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(bytes);
                }
            };
            mkfile(configDir + QStringLiteral("/sodium-options.json"),
                   QByteArrayLiteral("{\r\n  \"quality\": 1\r\n}\r\n"));
            mkfile(configDir + QStringLiteral("/ftbquests/quests.toml"),
                   QByteArrayLiteral("[quest]\nname = \"x\"\n"));

            Dlg dialog(instanceDisplayName, instanceDirName, Dlg::Override(), global, QStringList(),
                       true);
            QHash<QString, QString> dirs;
            dirs.insert(QStringLiteral("config"), configDir);
            dialog.setInstanceDir(root);
            dialog.setFileDirs(dirs);
            dialog.show();

            // 按**键**跳页（三页收起来之后中栏的行号不等于键表的下标，见上面那条 NOTE）。
            const auto showPage = [](Dlg &d, const QString &key) { d.showPage(key); };
            const auto rowOf = [](InstanceFilesPage *page, const QString &fileName) {
                for (int row = 0; row < page->list()->count(); ++row) {
                    QListWidgetItem *item = page->list()->item(row);
                    if (item != nullptr &&
                        QFileInfo(item->data(InstanceFilesPage::pathRole()).toString()).fileName() ==
                            fileName) {
                        return row;
                    }
                }
                return -1;
            };
            // 合成一发双击，发给**视口**（行是代理画的，鼠标底下就是视口本身）。
            const auto sendDbl = [](QWidget *viewport, const QPoint &pos) {
                const auto one = [viewport, &pos](QEvent::Type type, Qt::MouseButtons buttons) {
                    QMouseEvent event(type, QPointF(pos), QPointF(viewport->mapToGlobal(pos)),
                                      Qt::LeftButton, buttons, Qt::NoModifier);
                    QApplication::sendEvent(viewport, &event);
                };
                one(QEvent::MouseButtonPress, Qt::LeftButton);
                one(QEvent::MouseButtonRelease, Qt::NoButton);
                one(QEvent::MouseButtonDblClick, Qt::LeftButton);
                one(QEvent::MouseButtonRelease, Qt::NoButton);
                QApplication::processEvents();
            };

            InstanceFilesPage *files = dialog.filesPage(QStringLiteral("config"));
            ConfigFileEditorPage *editor = dialog.configFilePage();
            showPage(dialog, QStringLiteral("config"));
            check(files != nullptr && files->list() != nullptr && files->list()->count() == 2,
                  "U11c 前提：config/ 底下列出来两条（一个 json、一个目录）",
                  QStringLiteral("%1 条").arg(files != nullptr ? files->list()->count() : -1));
            check(files != nullptr && QDir::cleanPath(files->directory()) == QDir::cleanPath(configDir),
                  "U11c 前提：这一页看的是 config/ 那个目录");
            check(editor != nullptr && !editor->isVisible(),
                  "U11c 前提：刚进来露着的是列表那一格");

            // ---- 「上级」那颗按钮：在根上不画，进去了才画 ----
            auto *up = dialog.findChild<QPushButton *>(Dlg::configUpButtonName());
            check(up != nullptr, "U11c 「上级」那颗按钮按 objectName 找得回来");
            check(up != nullptr && !up->isVisible(),
                  "★★ U11c 在 config/ 根上**不画**「上级」—— 再往上是这个实例自己（然后是整个 "
                  ".minecraft），走到那儿「这是配置文件」这件事就走丢了");
            check(up != nullptr && up->text() == Dlg::configUpText(),
                  "U11c 那颗按钮上的字走 configUpText() 那一个出处");

            // 先把列表**摆完那一趟布局**再合成双击。
            //
            // 这一条是**驱动侧**的事，不是产品的毛病：`QAbstractItemView` 的条目布局是
            // **延后**的（`scheduleDelayedItemsLayout()` 只挂一个标记），而每行的行控件
            // 摆在哪儿正是那一趟算出来的。真机上用户点不到一行"还没摆到位"的行 ——
            // 没摆到位它就没画出来，鼠标底下根本没有它；而 offscreen 这一趟没有任何
            // 绘制，于是那条延后的布局**一直挂着**，第一发合成双击的坐标就算歪了
            // （冒到视口上时落到了别的地方 / 落空），第二发才碰巧对 —— 实测
            // 就是"第一发静默无效、之后都正常"。
            // `doItemsLayout()` 是那个标记的正门（`indexAt()` / `visualItemRect()` 内部
            // 也是叫它）。写出来是因为**不写的话这一条会间歇性地红**，而红的那个样子
            // 看着像"双击没反应"—— 那正是这一整段要钉的那个洞，会把人带沟里去。
            if (files != nullptr && files->list() != nullptr) {
                files->list()->doItemsLayout();
            }

            // ---- 点一条**文件**：发出去的是"打开这个文件" ----
            //
            // 发**双击**、而且发给**那一行的行控件**（真实的鼠标就在它上面）—— 不是在
            // 视口上凭空双击。这一条钉的就是 InstanceFilesPage 里那句"行控件忽略鼠标事件、
            // 所以双击会冒到视口上"：那一侧哪天有一个行控件开始吃鼠标事件，这一条当场红。
            QString asked;
            int askCount = 0;
            QObject::connect(&dialog, &Dlg::openConfigFileRequested, &dialog,
                             [&asked, &askCount](const QString &path) {
                                 asked = path;
                                 ++askCount;
                             });
            {
                const int row = rowOf(files, QStringLiteral("sodium-options.json"));
                check(row >= 0, "U11c 前提：那一份 json 在列表里");
                QListWidgetItem *item = row >= 0 ? files->list()->item(row) : nullptr;
                const QRect rect = item != nullptr ? files->list()->visualItemRect(item) : QRect();
                check(rect.height() == MainWindowUI::instanceFileRowHeight(),
                      "U11c 前提：那一行已经**摆好位了**（这是下面那发合成双击成立的前提）",
                      QStringLiteral("%1x%2").arg(rect.width()).arg(rect.height()));
                if (item != nullptr) {
                    // 行首图标那一格（左边距 12 之内是空的）：不压着行尾任何一格。
                    sendDbl(files->list()->viewport(), QPoint(rect.x() + 20, rect.center().y()));
                }
            }
            check(askCount == 1 &&
                      QFileInfo(asked).fileName() == QStringLiteral("sodium-options.json"),
                  "★★ U11c 双击一条**文件**：发 openConfigFileRequested()，带的是那一条的"
                  "**绝对路径** —— 双击落在视口上那一行的行首（鼠标真在它上面）",
                  QStringLiteral("收到 %1 次").arg(askCount));

            // ---- 点一条**目录**：这一层自己就办得了（不发信号）----
            {
                const int row = rowOf(files, QStringLiteral("ftbquests"));
                check(row >= 0, "U11c 前提：那个子目录在列表里");
                QListWidgetItem *item = row >= 0 ? files->list()->item(row) : nullptr;
                if (item != nullptr) {
                    const QRect rect = files->list()->visualItemRect(item);
                    sendDbl(files->list()->viewport(), QPoint(rect.x() + 10, rect.center().y()));
                }
            }
            check(askCount == 1,
                  "★★ U11c 双击一条**目录**：一个信号都不发（「进去看」是纯显示层的事，"
                  "不碰盘、不联网、也不用绕到 MainWindow 去）",
                  QStringLiteral("收到 %1 次").arg(askCount));
            check(files != nullptr &&
                      QDir::cleanPath(files->directory()) ==
                          QDir::cleanPath(configDir + QStringLiteral("/ftbquests")),
                  "★★ U11c 而且它**真的进去**了（那一页换成了 config/ftbquests 再列一遍）");
            // 左栏那条导航**不许因为"拿到焦点"就自己挑一条**。
            //
            // 这一条钉的是一个 Qt 的兜底行为，不是我们写的：`QAbstractItemView::focusInEvent()`
            // 在"视图拿到焦点、而当前条目无效"时会替你选中第一条（源码里那句
            // `if (model && !d->currentIndexSet && !currentIndexValid)`）—— 而这一列上的
            // 每一次选中**都是翻页**（`currentItemChanged` → showPage()）。
            // 「配置文件」正是一个没有左栏条目的页（左栏选中 = 空），于是用户按一下 Tab
            // 进来、或者切出去再切回来（窗口重新激活走的是同一个原因），右栏摆着的配置
            // 文件就被左栏顶掉、跳回概览 —— 一次没有用户意图的翻页。挡住它的是对话框
            // `eventFilter()` 里那一发。
            //
            // NOTE: 直接**合成一发 FocusIn 发给左栏**，而不是 `setFocus()` —— offscreen
            // 底下那个窗口不是活动窗口，`setFocus()` 根本不会把 FocusIn 送到控件上，
            // 拿它当驱动这一条会**永远绿**（看着像过了，其实什么都没测）。
            //
            // NOTE: 拿**真 QTreeWidget**（`navigationTree()`）发，不拿 objectName 找 ——
            // 这一条量的就是那棵树的实际行为。
            {
                const int before = dialog.stack()->currentIndex();
                QTreeWidget *navFocus = dialog.navigationTree();
                QFocusEvent focusIn(QEvent::FocusIn, Qt::TabFocusReason);
                QApplication::sendEvent(navFocus, &focusIn);
                check(navFocus != nullptr && dialog.stack()->currentIndex() == before &&
                          navFocus->currentItem() == nullptr,
                      "★★ U11c 左栏那棵树**拿到焦点也不许自己挑一条** —— 它挑的每一条都是"
                      "一次翻页，而「配置文件」本来就没有条目：Qt 的 focusInEvent() 会在"
                      "「当前条目无效」时替你选中第一条，右栏摆着配置文件、左栏一按 Tab "
                      "就跳回概览",
                      QStringLiteral("堆叠页 %1 → %2，左栏选中 %3")
                          .arg(before)
                          .arg(dialog.stack()->currentIndex())
                          .arg(navFocus != nullptr && navFocus->currentItem() != nullptr
                                   ? navFocus->currentItem()->data(0, Qt::UserRole).toString()
                                   : QStringLiteral("<空>")));
            }
            check(up != nullptr && up->isVisible(),
                  "★★ U11c 进到子目录之后「上级」就画出来了");

            // ---- 「上级」：连着点，到根为止，不越界 ----
            if (up != nullptr) {
                up->click();
            }
            check(QDir::cleanPath(files->directory()) == QDir::cleanPath(configDir),
                  "★ U11c 点一下「上级」回到 config/ 根上");
            check(up != nullptr && !up->isVisible(),
                  "★★ U11c 回到根上之后它**又不画了**");
            if (up != nullptr) {
                up->click();
            }
            check(QDir::cleanPath(files->directory()) == QDir::cleanPath(configDir),
                  "★★ U11c 在根上再点一下**什么都不发生**（不越到 config/ 上面去）");

            // ---- 切到编辑器那一格，再切回来 ----
            const QString filePath = configDir + QStringLiteral("/sodium-options.json");
            const ConfigFileText::Loaded loaded = ConfigFileText::decode(
                QByteArrayLiteral("{\r\n  \"quality\": 1\r\n}\r\n"));
            dialog.openConfigEditor(filePath, loaded);
            check(editor != nullptr && editor->isVisible() && files != nullptr &&
                      !files->isVisible(),
                  "★★ U11c openConfigEditor() 把第 1 格切出来了（列表那一格收着）");
            check(editor != nullptr && editor->path() == filePath && !editor->isDirty(),
                  "★ U11c 摆的正是那个文件，而且刚摆进去是干净的");
            dialog.closeConfigEditor();
            check(editor != nullptr && !editor->isVisible() && files != nullptr &&
                      files->isVisible(),
                  "★ U11c closeConfigEditor() 切回列表那一格");

            // ---- 底下那颗返回箭头要关整个页面：改过没保存时**先挡住** ----
            int blocked = 0;
            QObject::connect(&dialog, &Dlg::closeBlockedByUnsavedConfigFile, &dialog,
                             [&blocked] { ++blocked; });

            // 干净的：照旧关得掉。
            dialog.reject();
            check(blocked == 0, "U11c 前提：编辑器是干净的时候，关页不弹任何东西");
            check(!dialog.isVisible(), "★ U11c 干净的页照旧关得掉（这一条是下面那条的对照）");
            dialog.show();

            // 脏的：**挡住这一关**，把话交出去。
            dialog.openConfigEditor(filePath, loaded);
            editor->editor()->setPlainText(QStringLiteral("乱改的\n"));
            check(editor->isDirty(), "U11c 前提：编辑器现在是脏的");
            editor->show();
            dialog.reject();
            check(blocked == 1,
                  "★★ U11c 编辑器里有没保存的改动时，顶上那颗返回箭头**这一关被挡住**"
                  "（发 closeBlockedByUnsavedConfigFile，让 MainWindow 去问那一句）",
                  QStringLiteral("挡住 %1 次").arg(blocked));
            check(dialog.isVisible(),
                  "★★ U11c 而且页面**没关**（用户还没回答丢不丢，这一页就不许消失）");
            check(editor->isVisible() && editor->isDirty(),
                  "★ U11c 那一格连内容都原封不动（没被顺手丢掉）");

            // Esc 按在**编辑框**上：那是「退回文件列表」，**不是**「关掉整个实例设置」。
            // 这一条是上面那条的另一半：上面钉的是"顶上那颗返回箭头被那道闸挡住"，这一条
            // 钉的是"Esc 根本走不到那道闸上"—— 走到的话，用户按一下 Esc 冒出来的是"改过
            // 还没保存，丢不丢"，而他想的是"回去看另一个文件"。
            {
                int backs = 0;
                QObject::connect(editor, &ConfigFileEditorPage::backRequested, editor,
                                 [&backs] { ++backs; });
                QKeyEvent escInEditor(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(editor->editor(), &escInEditor);
                check(backs == 1 && blocked == 1 && dialog.isVisible(),
                      "★★ U11c 在编辑框上按 Esc：走的是「返回列表」那一发（backRequested），"
                      "**不是**关掉整个实例设置 —— 页面还在，也没冒出「改过还没保存」那一问",
                      QStringLiteral("返回 %1 次 / 挡住 %2 次").arg(backs).arg(blocked));
            }

            // 用户在 MainWindow 那边答了"丢掉"：discardChanges() 之后再 done(Rejected)。
            editor->discardChanges();
            dialog.done(QDialog::Rejected);
            check(!dialog.isVisible() && blocked == 1,
                  "★★ U11c 答「丢掉」之后拿 done(Rejected) 关掉 —— 走 reject() 的话会又回到"
                  "那道闸上，于是问不完地问（所以调用点必须是 done 而不是 reject）",
                  QStringLiteral("挡住 %1 次").arg(blocked));
        }

        // ---------- U12 「显示哪些页」那张勾选表----------
        //
        // 要求：「针对每个实例，在实例设置里个性化自定义，概览里面调整是否显示」。
        // 这一节钉的就是那张表本身：**它在概览页里**、**七条与 optionalPageKeys() 逐位
        // 对齐**、**不可用的那一条不勾 + 灰着 + tooltip 说为什么**、**勾掉一条 = 左栏少
        // 一条 + 当前页被赶回概览**，以及"这一份选择记在实例自己身上"（写盘那条路在
        // mw_check 里真写一遍）。
        {
            using Dlg = InstanceSettingsDialog;
            const Dlg::GlobalValues global = [] {
                Dlg::GlobalValues g;
                g.javaPath = QStringLiteral("C:/全局/java.exe");
                g.xmx = 4096;
                g.xms = 1024;
                g.autoMemory = false;
                g.extraJvmArgs = QStringLiteral("-XX:+UseG1GC");
                return g;
            }();

            Dlg dialog(QStringLiteral("我的整合包"), QStringLiteral("我的整合包-1.20.1-Forge"),
                       Dlg::Override(), global, QStringList(), true);
            dialog.show();

            // 页是**第一次切过去才建**的（见 InstanceSettingsDialog::ensurePage()）：堆叠页
            // 第 0 格是概览（打开对话框那一刻就建好），外面套着**它自己那层滚动区**
            // （`scrollName()`），真正的面板（`panelName()`）在它肚子里 —— 搬东西 / 找归属按
            // `isAncestorOf()` 走，所以拿外面这一层当根是对的。
            auto *overview = dialog.findChild<QScrollArea *>(
                Dlg::scrollName(QStringLiteral("overview")));
            const QWidget *overviewPanel = overview != nullptr ? overview->widget() : nullptr;
            check(overview != nullptr && overviewPanel != nullptr &&
                      overviewPanel->objectName() == Dlg::panelName(QStringLiteral("overview")),
                  "U12 前提：概览那一页建好了（外层滚动区 + 里面那张面板）",
                  overview != nullptr ? overview->objectName() : QStringLiteral("<没有>"));

            // ---- ① 七条：都在概览页里，而且与 optionalPageKeys() 逐位对齐 ----
            //
            // 对齐是**逐位**的（文字也是 optionalPageTexts() 那一串）：勾选表里的次序
            // 就是用户点名的那一串（存档 服务器列表 资源包 数据包 模组 光影包 原理图），
            // 谁把它按堆叠页的次序重排一遍，这一条当场红。
            {
                const QStringList keys = Dlg::optionalPageKeys();
                const QStringList texts = Dlg::optionalPageTexts();
                int matched = 0;
                int inOverview = 0;
                QStringList mismatched;
                for (int i = 0; i < keys.size(); ++i) {
                    auto *box = dialog.findChild<QCheckBox *>(Dlg::pageCheckName(keys.at(i)));
                    if (box == nullptr) {
                        mismatched << QStringLiteral("%1:<找不到>").arg(keys.at(i));
                        continue;
                    }
                    if (box->text() != texts.value(i)) {
                        mismatched << QStringLiteral("%1:%2").arg(keys.at(i), box->text());
                        continue;
                    }
                    ++matched;
                    if (overview != nullptr && overview->isAncestorOf(box)) {
                        ++inOverview;
                    }
                }
                check(matched == keys.size(),
                      "U12 七条勾选框按 pageCheckName(键) 一个个都找得回来，且字与 "
                      "optionalPageTexts() 逐位相同",
                      mismatched.isEmpty() ? QStringLiteral("%1 条").arg(matched)
                                           : mismatched.join(QLatin1Char(' ')));
                check(inOverview == keys.size(),
                      "★★ U12 而且这七条**全都在概览页里**（用户原话：「概览里面调整是否显示」）",
                      QStringLiteral("%1/%2 条在概览页里").arg(inOverview).arg(keys.size()));
                // 钉死的那两条 + 「配置文件」都不该有勾选框：钉死的是关不掉的，
                // 「配置文件」是根本没有入口的那一页（见 configPageKey()）。
                check(dialog.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("overview"))) == nullptr &&
                          dialog.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("launch"))) == nullptr &&
                          dialog.findChild<QCheckBox *>(Dlg::pageCheckName(Dlg::configPageKey())) == nullptr,
                      "U12 「概览 / 启动 / 配置文件」都没有勾选框（前两条关不掉、最后那条没有入口）");
                // 那一小块的行名。
                int titled = 0;
                const QList<QLabel *> labels = overview->findChildren<QLabel *>();
                for (QLabel *label : labels) {
                    if (label->text() == Dlg::showPagesLabelText()) {
                        ++titled;
                    }
                }
                check(titled == 1,
                      "U12 表头上那句「显示哪些页」在概览页里正好一份（走 showPagesLabelText()）",
                      QStringLiteral("%1 份").arg(titled));
                // 那一排常用目录按钮也在概览页里（要求：「快捷方式"放到概览的下方」）——
                // 这一条与 U0h 是同一件事的两半：那边按 objectName 找、这边按归属找。
                const QList<QWidget *> shortcuts = dialog.shortcutButtons();
                int shortcutsHere = 0;
                for (QWidget *button : shortcuts) {
                    if (overview->isAncestorOf(button)) {
                        ++shortcutsHere;
                    }
                }
                check(!shortcuts.isEmpty() && shortcutsHere == shortcuts.size(),
                      "U12 五颗常用目录按钮也都在概览页里（「快捷方式」并进概览那一句）",
                      QStringLiteral("%1/%2 颗").arg(shortcutsHere).arg(shortcuts.size()));
            }

            // ---- ② 默认：可用就显示（七条全勾上、左栏九条、一个键都不写）----
            {
                QStringList unchecked;
                QStringList disabled;
                for (const QString &key : Dlg::optionalPageKeys()) {
                    auto *box = dialog.findChild<QCheckBox *>(Dlg::pageCheckName(key));
                    if (box == nullptr) {
                        continue;
                    }
                    if (!box->isChecked()) {
                        unchecked << key;
                    }
                    if (!box->isEnabled()) {
                        disabled << key;
                    }
                    if (box->isEnabled() && box->toolTip() != Dlg::pageShownHint()) {
                        disabled << QStringLiteral("%1:tooltip 不对").arg(key);
                    }
                }
                check(unchecked.isEmpty() && disabled.isEmpty(),
                      "U12 默认全可用 ⇒ 七条全勾上、全都点得动、tooltip 都是「勾上左边那一列"
                      "里就有这一页」那一句",
                      QStringLiteral("没勾 %1 / 不对 %2")
                          .arg(unchecked.join(QLatin1Char(',')))
                          .arg(disabled.join(QLatin1Char(','))));
                QStringList expected = Dlg::pinnedPageKeys();
                expected += Dlg::optionalPageKeys();
                check(dialog.shownEntryKeys() == expected,
                      "U12 默认那一列 = pinnedPageKeys() + optionalPageKeys()（九条，上下有序）",
                      dialog.shownEntryKeys().join(QLatin1Char(' ')));
                check(dialog.pageChoices().isEmpty(),
                      "U12 一页都没勾掉 ⇒ pageChoices() 是空的（写盘那边按空对象**删键**）",
                      QString::fromUtf8(QJsonDocument(dialog.pageChoices()).toJson(QJsonDocument::Compact)));
            }

            // ---- ③ 用不上的一条：**不勾 + 灰着 + tooltip 说为什么**（ 起它
            // **仍然留在左栏的树上**，由那一页自己解释 —— 见 U10f 那一段的说明）----
            //
            // 要求：「如果没有对应的加载器，保持对应选项不可勾选的状态，悬停文本解释」，
            // 补的那句是「对于没有数据包的版本，数据包也默认隐藏且不可勾选」。
            {
                dialog.setDatapackUsable(false);
                auto *box = dialog.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("datapack")));
                check(box != nullptr && !box->isEnabled() && !box->isChecked(),
                      "★★ U12 「数据包」在这个实例上用不上 ⇒ 灰着 + 不勾",
                      QStringLiteral("enabled=%1 checked=%2")
                          .arg(box != nullptr && box->isEnabled() ? 1 : 0)
                          .arg(box != nullptr && box->isChecked() ? 1 : 0));
                check(box != nullptr && box->toolTip() == Dlg::datapackUnavailableText(),
                      "★★ U12 而且悬停文本解释为什么 —— 走 pageUnavailableText() 那一个分发处",
                      box != nullptr ? box->toolTip() : QStringLiteral("<没有那个控件>"));
                check(!dialog.shownEntryKeys().contains(QStringLiteral("datapack")),
                      "★★ U12 「能用」那份账里没有它（shownEntryKeys 少掉这一条 —— 另一半"
                      "「树上仍然点得进去」在 U10f 那一段量）",
                      dialog.shownEntryKeys().join(QLatin1Char(' ')));
                check(dialog.shownEntryKeys().size() ==
                          Dlg::pinnedPageKeys().size() + Dlg::optionalPageKeys().size() - 1,
                      "U12 少掉的正好是它一条（其余八条照旧）",
                      QStringLiteral("%1 条").arg(dialog.shownEntryKeys().size()));

                // 灰着的那一条**点不动**、而且**程序化那一趟一个字都不写盘**。
                {
                    int edited = 0;
                    QObject::connect(&dialog, &Dlg::settingsEdited, &dialog,
                                     [&edited] { ++edited; });
                    // ① 可用性变化本身是**画**，不是用户改的：四条 setter 走一遍，不许发。
                    dialog.setModUsable(false);
                    dialog.setShaderUsable(false);
                    dialog.setSchematicUsable(false);
                    dialog.setDatapackUsable(false);
                    QApplication::processEvents();
                    check(edited == 0,
                          "★★ U12 四条可用性 setter 走一遍，settingsEdited **一次都不发** —— "
                          "它是「画」（setChecked 不发 clicked），不是用户改的。发了的话，"
                          "开一次窗就白写一次盘",
                          QStringLiteral("发了 %1 次").arg(edited));
                    // ② 灰着那一条 `click()` 也没反应。
                    if (box != nullptr) {
                        box->click();
                        QApplication::processEvents();
                    }
                    check(edited == 0 && box != nullptr && !box->isChecked(),
                          "★★ U12 灰着那一条点了什么都不发生（一颗禁用控件的 click() 是"
                          "空动作，一个信号都不发）",
                          QStringLiteral("发了 %1 次").arg(edited));
                }

                // 另外三条各自都有话说，而且**三句话互不相同**（唯一分发处那个 if 链的
                // 意义就在这儿：同一句「用不上」套在模组上会把人往错的方向带）。
                dialog.setModUsable(false);
                dialog.setShaderUsable(false);
                dialog.setSchematicUsable(false);
                const QStringList missing = {
                    dialog.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("mod"))) != nullptr
                        ? dialog.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("mod")))->toolTip()
                        : QString(),
                    dialog.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("shader"))) != nullptr
                        ? dialog.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("shader")))->toolTip()
                        : QString(),
                    dialog.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("schematic"))) != nullptr
                        ? dialog.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("schematic")))->toolTip()
                        : QString()};
                check(missing.at(0) == Dlg::modUnavailableText() &&
                          missing.at(1) == Dlg::shaderUnavailableText() &&
                          missing.at(2) == Dlg::schematicUnavailableText(),
                      "U12 模组 / 光影包 / 原理图 三条的悬停文本各自走自己那一个出处",
                      missing.join(QStringLiteral(" | ")));
                check(missing.at(0) != missing.at(1) && missing.at(1) != missing.at(2) &&
                          missing.at(0) != missing.at(2),
                      "★★ U12 而且三句**互不相同**（照抄一句的症状是「照着提示去装错东西」）",
                      missing.join(QStringLiteral(" | ")));
                // 四页都不可用，但「存档 / 服务器列表 / 资源包」这三条**照旧可用**
                // （资源包到现在为止没有闸，见 .hpp 里 optionalPageKeys() 那一段）。
                QStringList stillThere = Dlg::pinnedPageKeys();
                stillThere << QStringLiteral("save") << QStringLiteral("server")
                           << QStringLiteral("resourcepack");
                check(dialog.shownEntryKeys() == stillThere,
                      "U12 那四条都不可用之后，左栏里只剩下钉死的两条 + 三条没闸的",
                      dialog.shownEntryKeys().join(QLatin1Char(' ')));

                // 装回来 ⇒ 自己就回来了（可用性不落盘，每次现算）。
                dialog.setDatapackUsable(true);
                dialog.setModUsable(true);
                dialog.setShaderUsable(true);
                dialog.setSchematicUsable(true);
                check(dialog.shownEntryKeys().contains(QStringLiteral("datapack")) &&
                          dialog.shownEntryKeys().contains(QStringLiteral("mod")) &&
                          dialog.shownEntryKeys().contains(QStringLiteral("schematic")),
                      "U12 可用性回来了 ⇒ 那几条自己就回来了（它从来不写进 meta）",
                      dialog.shownEntryKeys().join(QLatin1Char(' ')));
            }

            // ---- ④ 勾掉一条：左栏少一条、当前页被赶回概览、选择进 pageChoices() ----
            {
                int edited = 0;
                QObject::connect(&dialog, &Dlg::settingsEdited, &dialog, [&edited] { ++edited; });

                // 先把当前页摆到「存档」上，再勾掉它 —— 这样才测得到"当前页被赶回去"那一半。
                dialog.showPage(QStringLiteral("save"));
                const int before = dialog.stack()->currentIndex();
                check(before == Dlg::stackPageIndexOf(QStringLiteral("save")),
                      "U12 前提：当前页已经摆到「存档」上了",
                      QStringLiteral("%1").arg(before));

                auto *box = dialog.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("save")));
                if (box != nullptr) {
                    box->click();
                    QApplication::processEvents();
                }
                check(edited == 1,
                      "★ U12 勾一下**只发一次** settingsEdited（发两次 = 同一次改动写两遍盘）",
                      QStringLiteral("发了 %1 次").arg(edited));
                check(!dialog.shownEntryKeys().contains(QStringLiteral("save")),
                      "★★ U12 勾掉「存档」⇒ 左栏那一列里当场少一条",
                      dialog.shownEntryKeys().join(QLatin1Char(' ')));
                // 左栏那条导航已经从那一列（`pageList()`，构造里就 `hide()`
                // 了、只剩"旧访问接口"这一个身份）搬到树上 ⇒ 这一句量的是**树上选中了
                // 哪一条**。U0 那一节量过：树就是界面导航的唯一出处。
                const auto navKeyNow = [&dialog]() {
                    QTreeWidget *tree =
                        dialog.findChild<QTreeWidget *>(Dlg::navigationTreeName());
                    const QTreeWidgetItem *item = tree != nullptr ? tree->currentItem() : nullptr;
                    return item != nullptr ? item->data(0, Qt::UserRole).toString() : QString();
                };
                check(dialog.stack()->currentIndex() == 0 &&
                          navKeyNow() == Dlg::overviewCategoryKey(),
                      "★★ U12 而且当前那一页被赶回概览 —— 右栏摆着一页、左栏里却没有它，是"
                      "最容易让人以为「点坏了」的一种状态",
                      QStringLiteral("堆叠页 %1 / 树上选中 %2")
                          .arg(dialog.stack()->currentIndex())
                          .arg(navKeyNow()));
                const QJsonObject choices = dialog.pageChoices();
                check(choices.size() == 1 && choices.value(QStringLiteral("save")) == false,
                      "★★ U12 pageChoices() 里**只**记着这一条 false（存下来的只可能是 false）",
                      QString::fromUtf8(QJsonDocument(choices).toJson(QJsonDocument::Compact)));

                // 再勾回来 ⇒ 那个键**消失**（不是记一个 true）。
                if (box != nullptr) {
                    box->click();
                    QApplication::processEvents();
                }
                check(edited == 2 && dialog.pageChoices().isEmpty() &&
                          dialog.shownEntryKeys().contains(QStringLiteral("save")),
                      "★★ U12 再勾回来 ⇒ pageChoices() 又空了（「勾上」就是把键去掉，"
                      "不是记一个 true —— 盘上留一堆 true 的话，默认永远是「显示」这件事就"
                      "再也改不动了）",
                      QString::fromUtf8(QJsonDocument(dialog.pageChoices()).toJson(QJsonDocument::Compact)));
            }

            // ---- ⑤ 这一份选择是"这个实例自己的"：灌回来还记着 ----
            {
                Dlg other(QStringLiteral("还是那个整合包"),
                          QStringLiteral("我的整合包-1.20.1-Forge"), Dlg::Override(), global,
                          QStringList(), true);
                other.show();
                other.setPageChoices(QJsonObject{{QStringLiteral("save"), false},
                                                 {QStringLiteral("shader"), false},
                                                 // 认不出来的键（以后删掉一页之后盘上留下的那种）
                                                 {QStringLiteral("某个删掉的页"), false}});
                const QStringList shown = other.shownEntryKeys();
                auto *saveCheck =
                    other.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("save")));
                auto *shaderCheck =
                    other.findChild<QCheckBox *>(Dlg::pageCheckName(QStringLiteral("shader")));
                check(!shown.contains(QStringLiteral("save")) &&
                          !shown.contains(QStringLiteral("shader")) &&
                          shown.contains(QStringLiteral("mod")),
                      "★★ U12 灌回来的那份选择照样生效：勾掉的两条不在左栏里、没勾过的照旧在",
                      shown.join(QLatin1Char(' ')));
                check(saveCheck != nullptr && !saveCheck->isChecked() && shaderCheck != nullptr &&
                          !shaderCheck->isChecked(),
                      "U12 而且那两条勾选框的**样子**也跟着（不是只有左栏变了）");
                check(other.pageChoices().size() == 2 &&
                          other.pageChoices().value(QStringLiteral("save")) == false &&
                          other.pageChoices().value(QStringLiteral("shader")) == false,
                      "★★ U12 认不出来的键**丢掉**（前向兼容：以后删掉一页之后，盘上那条"
                      "不该跟着在这儿复活）",
                      QString::fromUtf8(
                          QJsonDocument(other.pageChoices()).toJson(QJsonDocument::Compact)));
            }
        }

        // ---------- U11d ConfigEntries：一条 `键 = 值` 是怎么认出来的 ----------
        // 用户后面那句「能不能通过外接解析库来做到条目-设置值来一一对应」的落点。
        // 为什么不用现成的库、这份扫描器认什么不认什么，见 ConfigEntries.hpp 那段；
        // 这里只钉"四类格式各认成什么"。
        {
            using namespace ConfigEntries;

            // ---- TOML：键、值、区间、注释、`#Range:` 一次全在 ----
            {
                const QString text = QStringLiteral("# 每多少 tick 重算一次\n"
                                                    "#Range: 1 ~ 32767\n"
                                                    "interval = 20\n"
                                                    "\n"
                                                    "[general]\n"
                                                    "enabled = true\n"
                                                    "name = \"hi\"\n");
                const QList<Entry> list = scan(text);
                check(list.size() == 3, "U11d TOML：顶层一条 + 段头下面两条",
                      QStringLiteral("%1 条").arg(list.size()));
                if (list.size() == 3) {
                    const Entry &first = list.at(0);
                    check(first.path == QStringLiteral("interval") &&
                              first.key == QStringLiteral("interval"),
                          "U11d 键路径就是那个键", first.path);
                    check(first.kind == Kind::Number && first.value == QStringLiteral("20"),
                          "★ U11d 裸数字认成数字（值是原文 `20`，不带引号）",
                          first.value);
                    // 这条是**区间**这件事的根：切出来的必须正好是那串值。
                    check(text.mid(first.valueBegin, first.valueEnd - first.valueBegin) ==
                              QStringLiteral("20"),
                          "★★ U11d 那个区间切出来**正好是这串值**（差一个字符，点一条就会选"
                          "歪一格）");
                    check(first.line == 2, "★ U11d 值在第 2 行（0 起）—— 界面上的「第 N 行」"
                                           "就是它 +1",
                          QStringLiteral("第 %1 行").arg(first.line));
                    check(first.note.contains(QStringLiteral("每多少 tick 重算一次")),
                          "★★ U11d 键上面那几行注释接成了这一条的**说明**（那份 schema 常常"
                          "只活在这里）",
                          first.note);
                    check(first.range == QStringLiteral("1 ~ 32767"),
                          "★★ U11d `#Range: 1 ~ 32767` 这条**范围**单独认出来了（Forge 那批"
                          " TOML 的量纲就写在注释里）",
                          first.range);

                    const Entry &second = list.at(1);
                    check(second.path == QStringLiteral("general.enabled") &&
                              second.segments.join(QLatin1Char('.')) == second.path,
                          "★ U11d 段头下面的键带着段名（`general.enabled`），树按它分组",
                          second.path);
                    check(second.kind == Kind::Bool, "U11d `true` 认成布尔");
                    check(text.mid(second.valueBegin, second.valueEnd - second.valueBegin) ==
                              QStringLiteral("true"),
                          "U11d 布尔那一条的值区间也对得上");

                    const Entry &third = list.at(2);
                    check(third.kind == Kind::String &&
                              third.value == QStringLiteral("\"hi\""),
                          "★ U11d 带引号的认成字符串，而且**值里留着引号**（界面上要显示的是"
                          "文件里那串原话）",
                          third.value);
                    check(third.note.isEmpty(),
                          "U11d 前提：段头这一条上面没有注释，说明就是空的（不是把上一段的"
                          "注释接了过来）");
                }
            }

            // ---- JSON：一层 + 嵌套对象（`.`) + 一行写完的数组 ----
            {
                const QString text = QStringLiteral("{\n"
                                                    "  \"quality\": 1,\n"
                                                    "  \"features\": {\n"
                                                    "    \"leaves\": \"smart\"\n"
                                                    "  },\n"
                                                    "  \"list\": [1, 2]\n"
                                                    "}\n");
                const QList<Entry> list = scan(text);
                check(list.size() == 3, "U11d JSON：两个标量 + 一个数组 = 三条（嵌套对象自己"
                                        "不是一条）",
                      QStringLiteral("%1 条").arg(list.size()));
                if (list.size() == 3) {
                    check(list.at(0).path == QStringLiteral("quality"),
                          "★ U11d 带引号的**键**是键（引号剥掉），不是值", list.at(0).path);
                    check(list.at(1).path == QStringLiteral("features.leaves"),
                          "★★ U11d 下潜过的对象：键路径是 `features.leaves`（树拿它分组，"
                          "用户才知道自己改的是哪一层）",
                          list.at(1).path);
                    check(list.at(2).path == QStringLiteral("list") &&
                              list.at(2).kind == Kind::Array,
                          "★ U11d 一行写完的 `[1, 2]` 认成数组、值就是整串");
                    check(text.mid(list.at(2).valueBegin,
                                   list.at(2).valueEnd - list.at(2).valueBegin) ==
                              QStringLiteral("[1, 2]"),
                          "U11d 数组那一条的区间切出来是整串 `[1, 2]`（含两头的方括号）");
                }
            }

            // ---- JSON5：`//` 与块注释、尾逗号 ----
            {
                const QString text = QStringLiteral("{\n"
                                                    "  // 说明\n"
                                                    "  \"a\": 1,\n"
                                                    "  /* 块注释 */\n"
                                                    "  \"b\": [1, 2, 3],\n"
                                                    "}\n");
                const QList<Entry> list = scan(text);
                check(list.size() == 2,
                      "★★ U11d **带注释的 JSON 照样认**（`//` 和块注释都不当正文）—— 盘上"
                      " 460 个 `.json5` 加 380 个带注释的 `.json` 就靠这一条；用 Qt 自带的 "
                      "QJsonDocument 它们一个都读不动",
                      QStringLiteral("%1 条").arg(list.size()));
                if (list.size() == 2) {
                    check(list.at(0).note == QStringLiteral("说明"),
                          "U11d `//` 那行注释落在下面那一条的说明上", list.at(0).note);
                    check(list.at(1).note == QStringLiteral("块注释"),
                          "U11d 块注释也一样（注释符剥掉了）", list.at(1).note);
                    check(list.at(1).value == QStringLiteral("[1, 2, 3]"),
                          "U11d 尾逗号（`],` 后面那个 `}`）不影响认值", list.at(1).value);
                }
            }

            // ---- CFG：`B:` / `I:` / `S:` 那种类型前缀 ----
            {
                const QString text =
                    QStringLiteral("B:shouldShowDuplicateTrees=true\nI:maxDepth=3\nS:name=hello\n");
                const QList<Entry> list = scan(text);
                check(list.size() == 3, "U11d CFG：三条",
                      QStringLiteral("%1 条").arg(list.size()));
                if (list.size() == 3) {
                    // 前缀**不剥**：盘上那串字就是 `B:shouldShowDuplicateTrees`，
                    // 而"和文件里一模一样"是这个功能的底线（用户拿它去 Ctrl+F 找得到）。
                    check(list.at(0).path == QStringLiteral("B:shouldShowDuplicateTrees"),
                          "★★ U11d CFG 那一行**整串都是键**（`B:` 这个类型前缀不剥）—— 剥了"
                          "的话，用户拿索引上那个键回文件里搜会一个都搜不到",
                          list.at(0).path);
                    check(list.at(0).kind == Kind::Bool && list.at(0).value == QStringLiteral("true"),
                          "★ U11d 而值照旧只有 `true` —— 冒号是键那一头的，不是值的（这条是"
                          "「`:` 什么时候是键、什么时候是分隔符」那个判断的落点）");
                    check(list.at(2).value == QStringLiteral("hello") &&
                              list.at(2).kind == Kind::Other,
                          "U11d 裸词（`hello`）认成「未识别」而不是字符串 —— 文件里就是没有"
                          "引号，这一层不替它猜");
                }
            }

            // ---- INI / properties：`;` 注释 + 段头 ----
            {
                const QString text =
                    QStringLiteral("[General]\nname=hello\n; 注释\nenabled=true\n");
                const QList<Entry> list = scan(text);
                check(list.size() == 2, "U11d INI：段头下面两条",
                      QStringLiteral("%1 条").arg(list.size()));
                if (list.size() == 2) {
                    check(list.at(0).path == QStringLiteral("General.name"),
                          "U11d INI 的段名也带进键路径", list.at(0).path);
                    check(list.at(1).note == QStringLiteral("注释"),
                          "★ U11d `;` 开头那行也是注释（`#` 之外还有这一种）", list.at(1).note);
                }
            }

            // ---- 多行数组：`mods` 自己不是一条，里面那些对象才是 ----
            {
                const QString text = QStringLiteral("mods = [\n"
                                                    "  { id = \"aaa\", enabled = true },\n"
                                                    "]\n");
                const QList<Entry> list = scan(text);
                check(list.size() == 2,
                      "★ U11d 多行数组**自己不出条目**（整串 `[\\n … \\n]` 摆到界面上既看不"
                      "全也改不了），下潜之后里面那些键才是条目",
                      QStringLiteral("%1 条").arg(list.size()));
                if (list.size() == 2) {
                    check(list.at(0).path == QStringLiteral("mods[0].id"),
                          "★★ U11d 下潜出来的路径带下标 `mods[0].id` —— 两条同名的键靠它才分"
                          "得开",
                          list.at(0).path);
                    check(text.mid(list.at(0).valueBegin,
                                   list.at(0).valueEnd - list.at(0).valueBegin) ==
                              QStringLiteral("\"aaa\""),
                          "U11d 下潜到的那一格值，区间照样准");
                    check(list.at(1).path == QStringLiteral("mods[0].enabled"),
                          "U11d 同一个对象里第二条也带同一个下标", list.at(1).path);
                }
            }

            // ---- 接得住的那些"不是配置"的正文 ----
            {
                check(scan(QString()).isEmpty(), "U11d 空文件：0 条（不崩）");
                check(scan(QStringLiteral("# 什么都没有\n# 还是什么都没有\n")).isEmpty(),
                      "★ U11d **整份都是注释**：0 条 —— 盘上真有这种文件（锂的 "
                      "`lithium.properties` 就是一堆注释），界面上该说的是「没认出设置项」"
                      "而不是摆一棵空树");
                check(scan(QStringLiteral("{ 半截 JSON")).size() <= 1,
                      "★ U11d 半截 JSON 不崩（这一层不是解析器，不查语法对不对）");
                check(maxEntries() > 0, "U11d 有上限（到点就停，不接着认）",
                      QStringLiteral("%1 条").arg(maxEntries()));
                check(kindName(Kind::Bool) == QStringLiteral("布尔") &&
                          kindName(Kind::Number) == QStringLiteral("数字") &&
                          kindName(Kind::String) == QStringLiteral("字符串") &&
                          kindName(Kind::Array) == QStringLiteral("数组") &&
                          kindName(Kind::Object) == QStringLiteral("对象") &&
                          kindName(Kind::Other) == QStringLiteral("未识别"),
                      "U11d 六类的名字就这六个字（界面上 tooltip 那颗「类型」和这里同一个"
                      "出处）");
            }

            // ---------- U11w 「值读到哪儿为止」（ 补的三处）----------
            //
            // 这三处都是**真的把设置项读丢/读残**的那一类，原文都在盘上（量法见 hpp 的
            // Dialect 那一段）：① `S:hexblack=#1e1b1b` 那种以 `#` 起头的值被当注释吞掉、
            // ② `.properties` 的值被第一个逗号截断（后半截还被当成正文重扫一遍）、
            // ③ 三引号里那个**转义引号**被当成串结束、后面半截串体变成假条目。
            {
                // ---- ① 紧贴分隔符的 `#` 是值，不是注释 ----
                //
                // 这条判据**不按方言分**（JSON5 里裸 `#` 起头的值一样是值），所以两种方言
                // 各量一遍：只有 `#` 前面有空白的时候它才是行内注释。
                const QList<Entry> cfgColor = scan(QStringLiteral("S:hexblack=#1e1b1b\n"),
                                                   Dialect::KeyValue);
                check(cfgColor.size() == 1 && cfgColor.at(0).value == QStringLiteral("#1e1b1b"),
                      "★★ U11w `S:hexblack=#1e1b1b` 是一条设置项、值是 `#1e1b1b`（1.12 Forge "
                      "的颜色就是这么写的；修之前整条**凭空消失**）",
                      QStringLiteral("%1 条%2")
                          .arg(cfgColor.size())
                          .arg(cfgColor.isEmpty() ? QString()
                                                  : QStringLiteral("，值 ") + cfgColor.at(0).value));
                const QList<Entry> jsonColor = scan(QStringLiteral("color = #ff0000\n"),
                                                    Dialect::Jsonish);
                check(jsonColor.size() == 1 && jsonColor.at(0).value == QStringLiteral("#ff0000"),
                      "★ U11w JSON 那一族里紧贴分隔符的 `#` 也是值（这条判据两种方言共用）",
                      QStringLiteral("%1 条").arg(jsonColor.size()));
                const QList<Entry> commented = scan(QStringLiteral("x = 1 # 说明\n"), Dialect::Jsonish);
                check(commented.size() == 1 && commented.at(0).value == QStringLiteral("1"),
                      "★ U11w 而**前面有空白**的 `#` 照旧是行内注释（值还是 `1`，不是 "
                      "`1 # 说明`）—— 别为了上面那两条把这条判据整个拿掉",
                      QStringLiteral("%1 条").arg(commented.size()));

                // ---- ② 逗号按方言分派 ----
                //
                // `.properties` / `.cfg` 那一族没有容器语法，逗号就是值里的一个字符；
                // JSON/JSON5/TOML 里它是元素分隔符（现有行为，不能一起改）。
                const QString commaLine =
                    QStringLiteral("particle$noCulling=com.lowdragmc.x,com.brandon3055.y\n");
                const QList<Entry> kvComma = scan(commaLine, Dialect::KeyValue);
                check(kvComma.size() == 1 &&
                          kvComma.at(0).value ==
                              QStringLiteral("com.lowdragmc.x,com.brandon3055.y"),
                      "★★ U11w `.properties` 的值**一路读到行尾**（盘上那条 "
                      "`particle$noCulling=…,…` 否则只剩第一个逗号前面那半截）",
                      kvComma.isEmpty() ? QStringLiteral("0 条") : kvComma.at(0).value);
                const QList<Entry> jsComma = scan(QStringLiteral("list = 1,2,3\n"), Dialect::Jsonish);
                check(jsComma.size() == 1 && jsComma.at(0).value == QStringLiteral("1"),
                      "★ U11w JSON 那一族逗号照旧是元素分隔符",
                      jsComma.isEmpty() ? QStringLiteral("0 条") : jsComma.at(0).value);

                // ---- 后缀 → 方言（只看字符串，不碰盘）----
                check(dialectForPath(QStringLiteral("C:/mc/config/chisel.cfg")) == Dialect::KeyValue &&
                          dialectForPath(QStringLiteral("C:/mc/config/a.properties")) ==
                              Dialect::KeyValue &&
                          dialectForPath(QStringLiteral("C:/mc/config/a.ini")) == Dialect::KeyValue &&
                          dialectForPath(QStringLiteral("C:/mc/config/a.conf")) == Dialect::KeyValue,
                      "U11w `.cfg` / `.properties` / `.ini` / `.conf` 都是「逗号是普通字符」那一族");
                check(dialectForPath(QStringLiteral("C:/mc/config/a.CFG")) == Dialect::KeyValue,
                      "U11w 后缀大小写不敏感（`A.CFG` 也算）");
                check(dialectForPath(QStringLiteral("C:/mc/config/a.toml")) == Dialect::Jsonish &&
                          dialectForPath(QStringLiteral("C:/mc/config/a.json5")) == Dialect::Jsonish &&
                          dialectForPath(QStringLiteral("C:/mc/config/a.jsonc")) == Dialect::Jsonish,
                      "U11w `.toml` / `.json5` / `.jsonc` 是结构化那一族（逗号是分隔符）");
                // 拿不准的走**现有行为**：这一条是"这次改动不许改变没预料到的文件的读法"。
                check(dialectForPath(QStringLiteral("C:/mc/config/a.txt")) == Dialect::Jsonish &&
                          dialectForPath(QStringLiteral("C:/mc/config/a.config")) == Dialect::Jsonish,
                      "★ U11w 认不出来的后缀（`.txt` / `.config`）走现有行为 —— 宁可那样，也不要"
                      "让一份没人预料到的文件换一种读法");
                check(dialectForPath(QStringLiteral("C:/mc/1.20.1/config/foo")) == Dialect::Jsonish &&
                          dialectForPath(QStringLiteral("C:/mc/1.20.1/config/foo.json")) ==
                              Dialect::Jsonish,
                      "★ U11w 点号在**目录名**里的时候不算后缀（`1.20.1/config/foo` 没有后缀）");
                // NOTE: `Entry` 没有 operator==（它是纯数据），所以这里逐位比那两样有意义的。
                const QList<Entry> noArg = scan(QStringLiteral("a = 1,2\n"));
                const QList<Entry> asJsonish =
                    scan(QStringLiteral("a = 1,2\n"), Dialect::Jsonish);
                check(noArg.size() == 1 && asJsonish.size() == 1 &&
                          noArg.at(0).value == asJsonish.at(0).value &&
                          noArg.at(0).path == asJsonish.at(0).path,
                      "U11w 不传方言 = Jsonish（老调用方一个字节都不变）");

                // ---- ③ 三引号里的转义引号 ----
                //
                // TOML 的 `"""`（basic，认转义）和 `'''`（literal，一个转义都不认）规矩不同。
                // 原来两档都不认转义：`\"""` 被当成串结束，后面那半截串体**被当成正文重扫
                // 一遍** —— 串里写着一句 `y = 999`，索引里就会凭空多出一条 `y`。
                const QList<Entry> triple = scan(
                    QStringLiteral("msg = \"\"\"hello \\\"\"\" y = 999 \"\"\"\n"));
                check(triple.size() == 1 && triple.at(0).key == QStringLiteral("msg"),
                      "★★ U11w 三引号里那个**转义引号**不算串结束（修之前这一份会多出一条 "
                      "**假条目** `y`：串体被当成正文重扫了）",
                      QStringLiteral("%1 条").arg(triple.size()));
                // 单引号那一档（literal）**不认**转义 —— 那是对的，别跟着双引号一起改。
                const QList<Entry> literal =
                    scan(QStringLiteral("msg = '''a\\'''\nb = 1\n"));
                check(literal.size() == 2,
                      "★ U11w 三**单**引号是 literal（反斜杠就是反斜杠）：`'''a\\'''` 到那个 `'''` "
                      "就结束，下面那条 `b` 照旧认出来",
                      QStringLiteral("%1 条").arg(literal.size()));

                // ---- ④ 下潜到容器里面之后，`[` 不再是段头 ----
                //
                // `[2]` 单独一行、前后都没有别的记号 —— 原来四条判据一条都不拦，被当成 TOML
                // 段头读掉，而段头会把键路径**整个清空**，外面那层 `\"a\": [...]` 的前缀就没了。
                const QList<Entry> inArray = scan(QStringLiteral("{\n  \"a\": [\n    [2],\n"
                                                                 "    \"b\": 1\n  ]\n}\n"));
                check(inArray.size() == 1 && inArray.at(0).path == QStringLiteral("a.b"),
                      "★★ U11w 容器里面的 `[2]` 不是段头（键路径不许被它清空：修之前这一条的"
                      "路径是 `2.b`）",
                      inArray.isEmpty() ? QStringLiteral("0 条") : inArray.at(0).path);

                // ---- ⑤ 找 `.cfg` 的类型前缀时，`=` 不能数引号里面的那个 ----
                const QList<Entry> quotedEq = scan(QStringLiteral("{ a: \"x=y\" }\n"));
                check(quotedEq.size() == 1 && quotedEq.at(0).path == QStringLiteral("a") &&
                          quotedEq.at(0).value == QStringLiteral("\"x=y\""),
                      "U11w `{ a: \"x=y\" }` 里那个 `=` 在引号里面 ⇒ 短键 `a` 不是类型前缀"
                      "（值照旧是 `\"x=y\"`）",
                      quotedEq.isEmpty() ? QStringLiteral("0 条") : quotedEq.at(0).path);
            }
        }

        // ---------- U11e 左边那一格「条目」：点一条跳过去 ----------
        {
            ConfigFileEditorPage editor;
            editor.show();

            const QString json = QStringLiteral("{\n"
                                                "  \"quality\": 1,\n"
                                                "  \"leaves\": \"smart\"\n"
                                                "}\n");
            editor.setFile(QStringLiteral("D:/示例/sodium-options.json"),
                           ConfigFileText::decode(json.toUtf8()));

            auto *tree = editor.indexTree();
            check(editor.indexToggle() != nullptr && tree != nullptr &&
                      editor.indexFilter() != nullptr,
                  "U11e 那一格的三个控件都按 objectName 找得回来");
            check(tree != nullptr && tree->topLevelItemCount() > 0,
                  "★★ U11e 摆一份文件进去，树上就有条目了 —— 没有这一下，左边那一格永远是"
                  "空的",
                  QStringLiteral("%1 个顶层项").arg(tree != nullptr ? tree->topLevelItemCount() : -1));
            check(editor.indexEntryCount() == 2,
                  "★ U11e 认出来的**叶子**是两条（顶层项那个数不是条目数：树是按路径分了组"
                  "的，别拿它当条数）",
                  QStringLiteral("%1 条").arg(editor.indexEntryCount()));
            check(editor.indexVisible() && editor.indexToggle()->isEnabled(),
                  "★ U11e 认出东西来了 ⇒ 那一格露着、那颗「条目」点得动");

            // ---- 点一条：编辑框里那一格值被选中 ----
            if (tree != nullptr && editor.indexEntryCount() == 2) {
                // 找到「leaves」那一条（第二行，值 `"smart"`）。
                QTreeWidgetItem *leaf = nullptr;
                for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
                    if ((*it)->data(0, ConfigFileEditorPage::entryPathRole()).toString() ==
                        QStringLiteral("leaves")) {
                        leaf = *it;
                    }
                }
                check(leaf != nullptr, "U11e 前提：树上找得到 `leaves` 那一条");
                check(leaf != nullptr && leaf->data(0, ConfigFileEditorPage::valueBeginRole())
                                             .toInt() == json.indexOf(QStringLiteral("\"smart\"")),
                      "★ U11e 那一条身上记的区间起点 = `\"smart\"` 在正文里的下标（索引和"
                      "正文是拿同一份字算的）");
                if (leaf != nullptr) {
                    editor.jumpToEntry(leaf);
                    const QString selected = editor.editor()->textCursor().selectedText();
                    check(selected == QStringLiteral("\"smart\""),
                          "★★ U11e 点一条 ⇒ 编辑框里**正好那一格值被选中**（就是用户要改的"
                          "那串字）—— 选多一个字符就会连着逗号一起删掉",
                          selected);
                    // NOTE: 问的是 `focusWidget()` 不是 `hasFocus()`：后者在**非活动窗口**
                    // 上恒为假，而 offscreen 下这一格是不会被激活的（见下面 O 段里
                    // 同一句 NOTE）—— 拿它量会跟着平台跑。
                    check(editor.focusWidget() == editor.editor(),
                          "★ U11e 焦点交给编辑框：点索引是为了接着改它，不是为了让索引亮着");
                    check(editor.indexTree() != nullptr &&
                              editor.focusWidget() != editor.indexTree(),
                          "U11e 前提：焦点**不在**索引那棵树上（不然上面那条是白过的）");
                }

                // 分组那一行没有值：点它**什么都不该发生**（不是"选到一段风马牛不相及的字"）。
                editor.editor()->setPlainText(json);
                editor.editor()->textCursor().clearSelection();
                QTreeWidgetItem *group = tree->topLevelItem(0);
                // 只有真的分了组才试（这一份文件全是顶层键，分组行可能是别的形状）。
                if (group != nullptr && group->childCount() > 0 &&
                    !group->data(0, ConfigFileEditorPage::valueBeginRole()).isValid()) {
                    editor.jumpToEntry(group);
                    check(!editor.editor()->textCursor().hasSelection(),
                          "★ U11e 点分组那一行：**不选任何东西**（那不是一个设置项，身上没有"
                          "值的区间）");
                }
            }

            // ---- 筛选框：只在树上藏几行，不动正文 ----
            {
                const QString before = editor.text();
                editor.indexFilter()->setText(QStringLiteral("leaves"));
                int visibleLeaves = 0;
                for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
                    if ((*it)->childCount() == 0 && !(*it)->isHidden()) {
                        ++visibleLeaves;
                    }
                }
                check(visibleLeaves == 1 && editor.indexEntryCount() == 2,
                      "★★ U11e 筛 `leaves` ⇒ 树上只剩一条，而**认出来的条数没变**（筛选藏的"
                      "是行，不是重新认一遍）",
                      QStringLiteral("露着 %1 条 / 一共 %2 条")
                          .arg(visibleLeaves)
                          .arg(editor.indexEntryCount()));
                check(editor.text() == before && !editor.isDirty(),
                      "★★ U11e 筛选**一个字节都不动正文**（它就是藏几行；顺手改正文的话，"
                      "用户筛一下就脏了）");
                editor.indexFilter()->clear();
                int backAgain = 0;
                for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
                    if ((*it)->childCount() == 0 && !(*it)->isHidden()) {
                        ++backAgain;
                    }
                }
                check(backAgain == 2, "U11e 清掉筛选，两条都回来了",
                      QStringLiteral("%1 条").arg(backAgain));
            }

            // ---- 那颗「条目」：收起来 / 放出去 ----
            {
                editor.indexToggle()->setChecked(false);
                check(!editor.indexVisible() && editor.indexEntryCount() == 2,
                      "★ U11e 按下「条目」⇒ 那一格收起来（**条目并没有丢**：再放出来还在，"
                      "不用重认）");
                editor.indexToggle()->setChecked(true);
                check(editor.indexVisible(), "U11e 再按一下放出来");
            }

            // ---- 没认出东西的那一份：那一格收着，那颗按钮**点不动** ----
            {
                editor.setFile(QStringLiteral("D:/示例/lithium.properties"),
                               ConfigFileText::decode(QByteArrayLiteral("# 一堆注释\n")));
                check(editor.indexEntryCount() == 0 && !editor.indexVisible(),
                      "★★ U11e 整份都是注释 ⇒ 那一格收着（摆一棵空树在那里，用户只会以为"
                      "界面坏了）");
                check(!editor.indexToggle()->isEnabled(),
                      "★★ U11e 而且那颗「条目」**点不动**（不是「点了没反应」）—— 认不出"
                      "条目就是认不出，展开一个空的索引没有第二种意思");
                check(editor.indexToggle()->toolTip() ==
                          ConfigFileEditorPage::indexEmptyHint(),
                      "★ U11e 为什么点不动写在它的 tooltip 上（灰按钮得说得出理由）",
                      editor.indexToggle()->toolTip());
                check(editor.indexTree()->topLevelItemCount() == 0,
                      "U11e 前提：树上真的清了（换文件不许留着上一个文件的条目 —— 那些区间"
                      "是拿**另一个文件**算的，点一下会选到这一份的风马牛不相及的一段字）");
            }

            // ---- 换文件之后区间是新的 ----
            {
                editor.setFile(QStringLiteral("D:/示例/a.json"),
                               ConfigFileText::decode(QByteArrayLiteral("{\n  \"aa\": 1\n}\n")));
                editor.setFile(QStringLiteral("D:/示例/b.json"),
                               ConfigFileText::decode(
                                   QByteArrayLiteral("{\n  \"b\": 2\n}\n")));
                QTreeWidgetItem *leaf = nullptr;
                for (QTreeWidgetItemIterator it(editor.indexTree()); *it != nullptr; ++it) {
                    leaf = *it;
                }
                check(leaf != nullptr &&
                          leaf->data(0, ConfigFileEditorPage::entryPathRole()).toString() ==
                              QStringLiteral("b"),
                      "★ U11e 换了文件之后树上摆的是**新文件**的条目");
                if (leaf != nullptr) {
                    editor.jumpToEntry(leaf);
                    check(editor.editor()->textCursor().selectedText() == QStringLiteral("2"),
                          "★★ U11e 而且跳的那个区间是拿**新正文**算的（留着上一份的区间的话，"
                          "点一下会选到这一份里的另一段字）",
                          editor.editor()->textCursor().selectedText());
                }
            }
        }

        // ---------- U11f 世代备份那一套**纯函数**----------
        // 位置 / 名字 / 清理都在这儿量：真正读写盘的那几下在 MainWindow（mw_check T20.10），
        // 而"落在哪个文件夹、挑哪一份删"是这一节的事 —— 这一节错了的话，用户看到的是
        // "还原上一次"取回了一份更老的、或者清理删错了东西（那是唯一一处会主动删东西的地方）。
        {
            using namespace ConfigFileText;

            // ---- Stamp：两样一起量过才算数 ----
            const Stamp blank;
            const Stamp one{100, 1700000000000LL};
            const Stamp same{100, 1700000000000LL};
            const Stamp other{101, 1700000000000LL};
            const Stamp noTime{100, -1};
            check(!blank.valid() && one.valid() && !noTime.valid(),
                  "★ U11f 没量过的 Stamp 不算数（**两样都得有**：只有大小是不知道它是哪一刻"
                  "的那一份）",
                  QStringLiteral("空=%1 一份=%2 缺时间=%3")
                      .arg(int(blank.valid()))
                      .arg(int(one.valid()))
                      .arg(int(noTime.valid())));
            check(one == same && one != other && blank != one,
                  "★ U11f 比的是「大小 + 时间」两样（**只改一个字节**也算变了 —— 只看大小的话 "
                  "`4096` → `4097` 看不出来）");

            // ---- 文件夹名：定长、可排序、撞了就挂序号 ----
            const QDateTime when = QDateTime::fromString(QStringLiteral("2026-09-27T14:30:12"),
                                                         Qt::ISODate);
            check(backupFolderName(when, {}) == QStringLiteral("20260927-143012"),
                  "★ U11f 备份文件夹名就是那一刻（`yyyyMMdd-HHmmss`）",
                  backupFolderName(when, {}));
            const QStringList oneTaken{QStringLiteral("20260927-143012")};
            const QString second = backupFolderName(when, oneTaken);
            check(second == QStringLiteral("20260927-143012-002"),
                  "★★ U11f 同一秒里存第二次**不覆盖上一步**（挂一个序号）—— 覆盖的话，用户"
                  "按的第二下「还原」取回来的其实是第一下之前那一份",
                  second);
            check(backupFolderName(when, {oneTaken.first(), second, QStringLiteral("20260927-143012-003")}) ==
                      QStringLiteral("20260927-143012-004"),
                  "★ U11f 序号一直往下排（撞了第二第三个也不会退回去覆盖）");
            // 序号**补零到三位**：名字排序 == 时间排序这条约定全靠定长，"…-2" 和 "…-10"
            // 按字典序是 `-10` 在前 —— 清理那一趟就会把最新那份当成最老那份删掉。
            check(second > QStringLiteral("20260927-143012") &&
                      backupFolderName(when, {oneTaken.first(), second, QStringLiteral("20260927-143012-003")}) >
                          second,
                  "★★ U11f 挂过序号的名字**照样按字典序排得出先后**（补零的意义就在这儿）",
                  QStringLiteral("%1 < %2").arg(second, backupFolderName(
                      when, {oneTaken.first(), second, QStringLiteral("20260927-143012-003")})));

            // ---- 认不认识"这是我们的文件夹" ----
            check(isBackupFolderName(QStringLiteral("20260927-143012")) &&
                      isBackupFolderName(QStringLiteral("20260927-143012-002")),
                  "★ U11f 我们生成的两种形状都认得出来（没挂序号 / 挂了序号）");
            const QStringList notOurs{QStringLiteral("config"), QString(), QStringLiteral("2026092714301"),
                                      QStringLiteral("2026-09-27143012"),
                                      QStringLiteral("20260927-143012-7z"),
                                      QStringLiteral("备份-2026")};
            bool allRejected = true;
            for (const QString &name : notOurs) {
                if (isBackupFolderName(name)) {
                    allRejected = false;
                }
            }
            check(allRejected,
                  "★★ U11f 不是那个形状的一律不认（14 位数字、日期写成 `2026-09-27`、序号不是"
                  "数字…）—— **清理只按这一个判据删东西**，放行一个就会删掉用户自己放进去的",
                  notOurs.join(QStringLiteral(" / ")));

            // ---- 清理：留最新 N 份，认不出的一个都不碰 ----
            const QStringList folders{QStringLiteral("20260101-000001"), QStringLiteral("读我"),
                                      QStringLiteral("20260927-143012"),
                                      QStringLiteral("20250101-120000"),
                                      QStringLiteral("20260927-140000")};
            const QStringList doomed = backupFoldersToRemove(folders, 2);
            check(doomed.size() == 2 &&
                      doomed.contains(QStringLiteral("20250101-120000")) &&
                      doomed.contains(QStringLiteral("20260101-000001")),
                  "★★ U11f 清理留**最新**的两份（按名字倒序 = 时间倒序），更老的两份删掉 —— "
                  "把最新那份删了等于用户刚存完就没了退路",
                  doomed.join(QStringLiteral(" / ")));
            check(!doomed.contains(QStringLiteral("读我")),
                  "★★ U11f **认不出的名字一个都不删**（用户自己在那个目录里放了个文件夹，"
                  "那是他的东西）");
            check(backupFoldersToRemove(folders, 5).isEmpty() &&
                      backupFoldersToRemove({}, 2).isEmpty(),
                  "★ U11f 没超过上限（或者那一层还没有东西）时一份都不删");

            // ---- 落点：保住"这个文件原来在 config/ 底下的哪儿" ----
            const QString root = QStringLiteral("D:/数据/cache/config-backups");
            const QString configDir = QStringLiteral("D:/数据/versions/我的实例/config");
            check(backupPathFor(root, QStringLiteral("我的实例"), configDir,
                                configDir + QStringLiteral("/ftbquests/quests.toml"),
                                QStringLiteral("20260927-143012")) ==
                      QStringLiteral("D:/数据/cache/config-backups/我的实例/20260927-143012/"
                                     "ftbquests/quests.toml"),
                  "★★ U11f 备份里那一条**留着它相对 config/ 的路径**（`ftbquests/quests.toml`）"
                  "—— 只留文件名的话，两个子目录里同名的 `config.toml` 在备份里就撞成一个了",
                  backupPathFor(root, QStringLiteral("我的实例"), configDir,
                                configDir + QStringLiteral("/ftbquests/quests.toml"),
                                QStringLiteral("20260927-143012")));
            const QString escaped = backupPathFor(root, QStringLiteral("我的实例"), configDir,
                                                  QStringLiteral("D:/别处/evil.toml"),
                                                  QStringLiteral("20260927-143012"));
            check(escaped == QStringLiteral("D:/数据/cache/config-backups/我的实例/"
                                            "20260927-143012/evil.toml") &&
                      !escaped.contains(QStringLiteral("..")),
                  "★★ U11f 传进来的路径要是不在 config/ 底下（理论上到不了），退化成文件名 ——"
                  "**绝不许把 `../..` 拼进去**：那会把备份写到备份根外面，而清理只认根底下的"
                  "东西 ⇒ 那些文件永远收不回来",
                  escaped);
        }

        // ---------- U11g 「还原上一次」那一颗 + 底下那行字----------
        {
            ConfigFileEditorPage editor;
            editor.show();
            check(editor.restoreButton() != nullptr &&
                      editor.restoreButton()->objectName() ==
                          ConfigFileEditorPage::restoreButtonName(),
                  "U11g 「还原上一次」那颗按钮按 objectName 找得回来",
                  editor.restoreButton() != nullptr ? editor.restoreButton()->objectName()
                                                    : QStringLiteral("(没有)"));
            check(!editor.restoreButton()->isVisible(),
                  "★ U11g 还没摆文件的时候不画它（和「保存」同一个规矩：点了也没意义的按钮"
                  "不摆出来）");

            const QByteArray raw = QByteArrayLiteral("{\r\n  \"quality\": 1\r\n}\r\n");
            const ConfigFileText::Loaded loaded = ConfigFileText::decode(raw);
            ConfigFileText::Stamp stamp;
            stamp.size = raw.size();
            stamp.mtimeMs = 1700000000000LL;
            editor.setFile(QStringLiteral("D:/示例/sodium-options.json"), loaded, stamp);
            check(!editor.restoreButton()->isVisible(),
                  "★★ U11g 打开一个**还没留过备份**的文件：那颗按钮不画（`setFile()` 会把"
                  "上一条的答案清掉 —— 用户从「有备份的 A」切到「没备份的 B」时，B 上不许挂着"
                  "一颗取回 A 的备份的按钮）",
                  QStringLiteral("找得到备份=%1").arg(int(editor.backupAvailable())));
            check(editor.stamp() == stamp,
                  "★ U11g 打开时量的那一眼**留着**（保存前要比的正是它）");

            editor.setBackupAvailable(true);
            check(editor.backupAvailable() && editor.restoreButton()->isVisible(),
                  "★ U11g 留过备份 ⇒ 那颗按钮画出来");

            // 取回备份：只摆进框里（变脏、底下那行字说清它是哪来的、盘上那份一点没动）
            const QString older = QStringLiteral("{\n  \"quality\": 0\n}\n");
            editor.restoreContent(older, QStringLiteral("2026-09-27 14:30:12"));
            check(editor.text() == older && editor.isDirty(),
                  "★★ U11g 取回来的内容摆进框里、而且**变脏了**（于是「返回」会问一句、「保存」"
                  "才真的写盘）—— 悄悄写进去的话，用户按一下就叫「还原」，而「刚才改的那一份」"
                  "已经被盖掉了，再想回去没有路",
                  QStringLiteral("脏=%1").arg(int(editor.isDirty())));
            check(editor.noteLabel()->text().startsWith(
                      ConfigFileEditorPage::restoredPrefix()) &&
                      editor.noteLabel()->text().contains(QStringLiteral("2026-09-27 14:30:12")),
                  "★★ U11g 底下那行字**说清这一份是哪来的、还没写回盘上**（它是这一页唯一"
                  "一处「正文变了但盘上没变」的情形，不说的话用户会以为已经落盘了）",
                  editor.noteLabel()->text());

            // 索引跟着换：区间是拿正文算的，正文整份换了它就全不作数了
            check(editor.indexEntryCount() == 1,
                  "★ U11g 取回之后索引立刻重认了一遍（拿的是取回来那份正文，不是原来那份）",
                  QStringLiteral("%1 条").arg(editor.indexEntryCount()));

            // 取回来的那一份又原样存下去：脏位跟着内容走，不跟着"动过没有"走
            editor.editor()->setPlainText(loaded.text);
            check(!editor.isDirty(),
                  "★ U11g 取回之后又改回原样 ⇒ 不脏了（脏位比的是**内容**，不是「动过没有」）");

            // 存成功：那行字换成"已保存"，而且"取回来的"这件事到此为止
            editor.restoreContent(older, QStringLiteral("2026-09-27 14:30:12"));
            editor.markSaved();
            check(!editor.isDirty() &&
                      editor.noteLabel()->text().startsWith(ConfigFileEditorPage::savedPrefix()),
                  "★★ U11g 存成功之后那行字换成「已保存（时:分:秒）」，**不再说「取回来的、还没"
                  "写回盘上」**（它已经写回去了）",
                  editor.noteLabel()->text());

            // 只读的文件上不画它：取回来也写不回去
            const QByteArray bad = QByteArrayLiteral("bad \xb4 byte\n");
            editor.setFile(QStringLiteral("D:/示例/broken.cfg"), ConfigFileText::decode(bad));
            editor.setBackupAvailable(true);
            check(!editor.isEditable() && !editor.restoreButton()->isVisible(),
                  "★★ U11g **只读的文件上不画它**（不是 UTF-8 / 太大 ⇒ 取回来也写不回去，"
                  "摆一颗按下去没结果的按钮只是噪音）");
        }
    }  // U11
    return 0;
}


