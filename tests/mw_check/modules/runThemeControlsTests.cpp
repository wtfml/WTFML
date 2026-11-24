#include "Support.hpp"
#include "ThemeAdvancedEditor.hpp"
#include "Glass.hpp"
void runThemeControlsTests() {
    const QString originalDir = AppPaths::minecraftDir();
    const Theme::Spec themeBefore = Theme::current();
    const QPalette paletteBefore = QApplication::palette();
    QTemporaryDir root;
    check(root.isValid(), "T39.0 临时数据根目录建出来了");
    if (!root.isValid()) {
        return;
    }

    QSettings sandbox;
    const QVariant foldersBefore = sandbox.value(QStringLiteral("Launcher/Folders"));
    const QVariant currentBefore = sandbox.value(QStringLiteral("Launcher/CurrentFolder"));
    const auto restore = [&] {
        sandbox.setValue(QStringLiteral("Launcher/Folders"), foldersBefore);
        sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), currentBefore);
        sandbox.remove(QStringLiteral("Theme"));
        sandbox.sync();
        // 主题要**连调色板一起**铺回去：这一节把预设拨到过深色，光 `setSpec()` 是收不回
        // 来的（深色那一份还挂在应用上，后面每一节量的都会是深底）。
        Theme::setSpec(themeBefore);
        Theme::apply(*qApp);
        AppPaths::setMinecraftDir(originalDir);
    };
    sandbox.setValue(QStringLiteral("Launcher/Folders"), QStringList({root.path()}));
    sandbox.setValue(QStringLiteral("Launcher/CurrentFolder"), root.path());
    sandbox.sync();
    AppPaths::setMinecraftDir(root.path());

    {
        MainWindow w;
        w.show();
        QApplication::processEvents();

        MainWindowUI *ui = w.ui();
        check(ui != nullptr, "T39.1 设置页在（这一节整段都要用它的主题那一节）");
        if (ui == nullptr) {
            restore();
            return;
        }

        auto *presetCombo = byObjectName<QComboBox>(&w, MainWindowUI::themePresetComboName());
        auto *accentSwatch = byObjectName<QPushButton>(&w, MainWindowUI::themeAccentButtonName());
        auto *opacitySlider =
            byObjectName<QSlider>(&w, MainWindowUI::themeTextOpacitySliderName());
        auto *opacityPercent =
            byObjectName<QLabel>(&w, MainWindowUI::themeTextOpacityPercentName());
        QVector<QPushButton *> semanticSwatches;
        for (int i = 0; i < MainWindowUI::themeSemanticCount(); ++i) {
            semanticSwatches.append(
                byObjectName<QPushButton>(&w, MainWindowUI::themeSemanticButtonName(i)));
        }
        int foundSwatches = 0;
        for (QPushButton *swatch : semanticSwatches) {
            if (swatch != nullptr) {
                ++foundSwatches;
            }
        }
        check(presetCombo != nullptr && accentSwatch != nullptr && opacitySlider != nullptr &&
                  opacityPercent != nullptr &&
                  foundSwatches == MainWindowUI::themeSemanticCount(),
              "T39.1 这一节那几颗控件都按 objectName 找得回来",
              QStringLiteral("语义色块 %1/%2")
                  .arg(foundSwatches)
                  .arg(MainWindowUI::themeSemanticCount()));
        if (presetCombo == nullptr || accentSwatch == nullptr || opacitySlider == nullptr) {
            restore();
            return;
        }

        // ---------- T39.1b 建页面这一趟不许往盘上写 ----------
        //
        // 2026-11-13 用户真实的 `Theme/BackdropTint` / `Theme/BackdropBlur` 被写成了 0
        //（出厂 20 / 50，用户说自己没动过）。产品这一侧唯一会写这两个键的是设置页那两根
        // 强度滑块的 setter，而它们的 `setValue()` 是在 connect **之前**做的，所以"把页面
        // 搭出来"这一趟一个键都不该落盘；只有值真的变了才写（下面第二发钉这条）。
        // objectName 是 `ui/MainWindowSettingsPage.cpp` 里那两行字面量（这一层拿不到头文件里的
        // 常量，改名字时两边一起改）。
        {
            QSettings disk;
            check(!disk.contains(QStringLiteral("Theme/BackdropTint")) &&
                      !disk.contains(QStringLiteral("Theme/BackdropBlur")) &&
                      !disk.contains(QStringLiteral("Theme/Backdrop")),
                  "T39.1b 建完设置页，磨砂那三个键一个都没写盘（构造期不许发 valueChanged(0)）",
                  disk.fileName());
            auto *tint = byObjectName<QSlider>(&w, QStringLiteral("backdropTintSlider"));
            auto *blur = byObjectName<QSlider>(&w, QStringLiteral("backdropBlurSlider"));
            check(tint != nullptr && blur != nullptr, "T39.1b 着色 / 模糊两根强度滑块都找得回来");
            if (tint != nullptr && blur != nullptr) {
                check(tint->value() == Glass::tintStrength() && blur->value() == Glass::blurStrength() &&
                          tint->value() == Glass::kDefaultTintStrength && blur->value() == 50,
                      "T39.1b 干净沙盒里两根滑块停在出厂 20 / 50（不是 0）",
                      QStringLiteral("着色 %1 / 模糊 %2").arg(tint->value()).arg(blur->value()));
                // 用户真的把它拖到底才算改动：这一发之后盘上必须有 0，收尾由 `restore()` 清。
                tint->setValue(0);
                check(QSettings().value(QStringLiteral("Theme/BackdropTint"), -1).toInt() == 0,
                      "T39.1b 值真的变了才落盘（滑块是「改完立刻生效」那条路）",
                      QSettings().fileName());
                // 收尾：把这一节自己拨下去的那一档放回出厂，别让后面的绘制量到一个 0 度着色。
                tint->setValue(Glass::kDefaultTintStrength);
                QSettings().remove(QStringLiteral("Theme/BackdropTint"));
            }
        }

        // ---------- ① 构造完那一刻：控件照着规格画出来了 ----------
        //
        // 这一条量的正是 `refreshThemeControls()` 那一发的产物 —— 控件照着内存里那一份
        // 规格画：主题色是规格里那一支、四个语义色是**按当前深浅现推**的那四个、预设那一格
        // 停在规格里那一个（新安装是暮色雪山）。
        //
        // 四块语义色**必须真的被涂过**（`wtfmlSwatchColor` 那条动态属性是
        // `Theme::markColorSwatch()` 挂的）：不涂的话它们是四颗空白按钮，而屏幕上只是
        // "四块白方块"，不报错也不崩（ui_check 那边量得到形状，涂色这一步只有这里有
        // `MainWindow` 才看得见）。
        const auto swatchColor = [](QPushButton *swatch) {
            return swatch == nullptr ? QColor()
                                     : swatch->property("wtfmlSwatchColor").value<QColor>();
        };
        const auto sameColor = [](const QColor &a, const QColor &b) {
            return a.isValid() && b.isValid() &&
                   a.name(QColor::HexArgb) == b.name(QColor::HexArgb);
        };
        const QPalette palette = QApplication::palette();
        const QColor expectedSemantic[] = {
            Theme::dangerColor(palette), Theme::warningColor(palette), Theme::okColor(palette),
            Theme::errorTextColor(palette)};
        int paintedOk = 0;
        for (int i = 0; i < semanticSwatches.size(); ++i) {
            if (sameColor(swatchColor(semanticSwatches.at(i)), expectedSemantic[i])) {
                ++paintedOk;
            }
        }
        check(paintedOk == MainWindowUI::themeSemanticCount() &&
                  sameColor(swatchColor(accentSwatch), Theme::current().accent),
              "★★ T39.2 建完那一刻五块色板都**涂上了色**：主题色是规格里那一支、四个语义色是"
              "按当前深浅现推的那四个（没涂的话它们是四颗空白按钮 —— 不报错、不崩）",
              QStringLiteral("对了 %1/%2 块；主题色那块 %3 / 要 %4")
                  .arg(paintedOk)
                  .arg(MainWindowUI::themeSemanticCount())
                  .arg(swatchColor(accentSwatch).name(), Theme::current().accent.name()));
        check(presetCombo->currentData().toString() == Theme::current().presetId &&
                  opacitySlider->value() == Theme::current().textOpacity &&
                  opacityPercent->text() ==
                      MainWindowUI::themePercentText(Theme::current().textOpacity),
              "T39.2 预设那一格停在规格里那一档、不透明度那根条和右边那个百分比也是",
              QStringLiteral("%1 / %2 / %3")
                  .arg(presetCombo->currentData().toString())
                  .arg(opacitySlider->value())
                  .arg(opacityPercent->text()));

        // ---------- ② 拨预设 → 规格 + 盘 + 界面三样一起动 ----------
        //
        // 这是这一节最要紧的一条：`MainWindow` 那一侧要是漏了"落盘"或者"铺开"，症状都是
        // "改了没生效 / 下次启动又变回去"，而屏幕上看着一切正常。
        //
        // 这一条里"调色板深吗"那半**必须真机跑**（这个工程平时就是真机跑的，别加
        // `QT_QPA_PLATFORM=offscreen`）：深浅那一档是 `styleHints()->setColorScheme()`
        // 往下传的，而 **offscreen 插件不认这一档**（实测：`standardPalette()` 照旧是浅的）
        // —— 在 offscreen 底下跑会得到一条看起来像产品坏了的**假红**。
        const int darkIndex = presetCombo->findData(QStringLiteral("dark"));
        check(darkIndex >= 0, "T39.3 配色那格里找得到「深色」那一档");
        // 出厂默认已经是暮色雪山：这一栏要是本来就停在它上面，`setCurrentIndex()` 同一个
        // index 不发 `currentIndexChanged`，下面那三样就一样都不会动 —— 先拨到别的
        // 一档，让"拨一下深色"真的是一次切换。
        const int awayIndex = presetCombo->findData(QStringLiteral("emerald"));
        if (darkIndex >= 0 && awayIndex >= 0 &&
            presetCombo->currentData().toString() == QStringLiteral("dark")) {
            presetCombo->setCurrentIndex(awayIndex);
            QApplication::processEvents();
        }
        if (darkIndex >= 0) {
            presetCombo->setCurrentIndex(darkIndex);
            QApplication::processEvents();
            QSettings written;
            // CTest 为了避免弹出真窗口固定使用 offscreen，而该平台插件不实现
            // styleHints()->setColorScheme()（上面的注释已说明）。规格和落盘在任何
            // 平台都必须验；调色板只在真正支持系统深浅提示的平台上验，避免伪失败。
            const bool canApplyColorScheme =
                QGuiApplication::platformName() != QLatin1String("offscreen");
            const bool paletteMatches = Theme::isDarkPalette(QApplication::palette());
            check(Theme::current().presetId == QStringLiteral("dark") &&
                      (!canApplyColorScheme || paletteMatches) &&
                      written.value(QStringLiteral("Theme/Preset")).toString() ==
                          QStringLiteral("dark"),
                  "★★ T39.3 拨一下「深色」：内存里那份规格、应用调色板、盘上那一位**三样"
                  "一起变**（少了任何一样都是「改了没生效」或「下次启动又变回去」）",
                  QStringLiteral("规格 %1 / 调色板深吗 %2 / 盘上 %3")
                      .arg(Theme::current().presetId)
                      .arg(paletteMatches ? 1 : 0)
                      .arg(written.value(QStringLiteral("Theme/Preset")).toString()));
            // 深色下那四个语义色**当场换成另一套**（`dangerColor()` 是按参数那份调色板算的），
            // 而色板是**画的**、不会自己跟着调色板走 ⇒ 得由 `refreshThemeControls()` 重涂。
            // 漏了这一涂，屏幕上就是"深色主题下那四块还是浅色档的红黄绿"。
            const QPalette darkPalette = QApplication::palette();
            const QColor darkDanger = Theme::dangerColor(darkPalette);
            check(sameColor(swatchColor(semanticSwatches.value(0)), darkDanger),
                  "★★ T39.3 换成深色之后那四块色板也**重涂过**（深底上要用深底那一档的红"
                  "—— 色块是画上去的，不跟着调色板走）",
                  QStringLiteral("块上是 %1 / 深底那一档是 %2")
                      .arg(swatchColor(semanticSwatches.value(0)).name(), darkDanger.name()));
            // 覆盖项叠加在预设上面：拨预设**不许**把用户自己改的那几项抹掉。
            QMetaObject::invokeMethod(&w, "onThemeAccentResetClicked", Qt::DirectConnection);
            QApplication::processEvents();
            check(presetCombo->currentData().toString() == QStringLiteral("dark") &&
                      Theme::current().presetId == QStringLiteral("dark"),
                  "★ T39.3 「主题色 → 恢复默认」只把那一个色还回去，**预设还是深色**"
                  "（覆盖项是叠在预设上面的，不是二选一）",
                  Theme::current().presetId);
        }

        // ---------- ③ 拖不透明度 → 规格 + 盘 + 那格百分比 ----------
        opacitySlider->setValue(60);
        QApplication::processEvents();
        {
            QSettings written;
            check(Theme::current().textOpacity == 60 &&
                      written.value(QStringLiteral("Theme/TextOpacity")).toInt() == 60 &&
                      opacityPercent->text() == MainWindowUI::themePercentText(60),
                  "★★ T39.4 拖不透明度：规格 / 盘上那一位 / 右边那格百分比三样一起跟着走",
                  QStringLiteral("规格 %1 / 盘上 %2 / 那格「%3」")
                      .arg(Theme::current().textOpacity)
                      .arg(written.value(QStringLiteral("Theme/TextOpacity")).toInt())
                      .arg(opacityPercent->text()));
        }

        // ---------- ④ 「恢复默认」把它们还回去 ----------
        //
        // 两条路各一句，都是**按名字调那一发槽**（产品里它们是那两颗按钮的槽，底下没有
        // 需要用户输入的东西，所以这一发就是用户点下去同一发）。
        QMetaObject::invokeMethod(&w, "onThemeAccentResetClicked", Qt::DirectConnection);
        QApplication::processEvents();
        const QColor presetBrand = Theme::tokens(Theme::current().presetId).primary;
        check(sameColor(Theme::current().accent, presetBrand) &&
                  sameColor(swatchColor(accentSwatch), presetBrand),
              "★★ T39.5 「主题色 → 恢复默认」：规格回到**当前预设那一支**，那块色也**当场重涂**"
              "（预设照旧是深色，不跟着一起跳回出厂）",
              QStringLiteral("规格 %1 / 块上 %2 / 预设那一支 %3")
                  .arg(Theme::current().accent.name(), swatchColor(accentSwatch).name(),
                       presetBrand.name()));

        // 语义色：先把四个色分别点成"用户挑过的"（直接改规格 + 重画，就是 `pickColor()`
        // 之后那一发），再按「跟随预设」把它们清掉。
        {
            Theme::Spec spec = Theme::current();
            spec.danger = QColor(0x10, 0x20, 0x30);
            spec.warning = QColor(0x40, 0x50, 0x60);
            spec.ok = QColor(0x70, 0x80, 0x90);
            spec.errorText = QColor(0xa0, 0xb0, 0xc0);
            Theme::setSpec(spec);
            Theme::save(Theme::current());
            Theme::apply(*qApp);
            check(QMetaObject::invokeMethod(&w, "refreshThemeControls", Qt::DirectConnection),
                  "T39.6 那一发重画调得动（产品里灌值和每次改完调的就是它）");
            check(sameColor(swatchColor(semanticSwatches.value(0)), QColor(0x10, 0x20, 0x30)) &&
                      sameColor(swatchColor(semanticSwatches.value(3)), QColor(0xa0, 0xb0, 0xc0)),
                  "★ T39.6 点名过的语义色，色板上就是那一个（不是「按现在深浅现推」那个）",
                  swatchColor(semanticSwatches.value(0)).name());

            QMetaObject::invokeMethod(&w, "onThemeSemanticResetClicked", Qt::DirectConnection);
            QApplication::processEvents();
            const Theme::Spec &afterReset = Theme::current();
            const QPalette nowPalette = QApplication::palette();
            check(!afterReset.danger.isValid() && !afterReset.warning.isValid() &&
                      !afterReset.ok.isValid() && !afterReset.errorText.isValid() &&
                      sameColor(swatchColor(semanticSwatches.value(0)),
                                Theme::dangerColor(nowPalette)),
                  "★★ T39.6 「跟随预设」把四项清成**没点名**（不是「设成今天这个色」——"
                  "那样换了深浅它就跟不动了），色板当场回到按当前深浅现推的那一套",
                  QStringLiteral("规格里还有几个：%1")
                      .arg((afterReset.danger.isValid() ? 1 : 0) +
                           (afterReset.warning.isValid() ? 1 : 0) +
                           (afterReset.ok.isValid() ? 1 : 0) +
                           (afterReset.errorText.isValid() ? 1 : 0)));
        }

        auto *advanced = ui->getThemeAdvancedEditor();
        check(advanced != nullptr, "T39.7a 高级配色入口已接入设置页");
        if (advanced) {
            const int role = int(Theme::Role::Base);
            const QColor custom(QStringLiteral("#dce9e3"));
            advanced->setRoleColor(role, custom);
            check(Theme::current().roles.value(role) == custom &&
                      Theme::load().roles.value(role) == custom,
                  "T39.7b 高级配色通过真实控制器生效并保存");
            advanced->setRoleColor(role, QColor());
            check(!Theme::current().roles.contains(role) &&
                      !Theme::load().roles.contains(role),
                  "T39.7c 跟随主题会清除自定义角色颜色并保存");
        }

        restore();
        check(Theme::current().presetId == themeBefore.presetId &&
                  Theme::current().textOpacity == themeBefore.textOpacity &&
                  AppPaths::minecraftDir() == originalDir &&
                  QApplication::palette() == paletteBefore,
              "T39.8 收尾：主题规格 / 调色板 / 数据根目录都还原了（这一节不留痕迹）",
              QStringLiteral("预设 %1（该是 %2）")
                  .arg(Theme::current().presetId, themeBefore.presetId));
    }

    // ---------- ⑥ 两份启动器共用同一份主题设置：各改各的，谁也不把对方刚改的那一项打回去 ----------
    //
    // 主题以前是**整组清掉重写**（`remove("Theme")` 之后逐项写回）：装在两个目录、同时开着
    // 的两份启动器各改一项时，后存的那个把对方刚改的抹回自己启动时看到的样子 ——
    // 计划 P0-2 里记的那条已知边界。现在 `save()` 只写与 `load()` 快照不同的项。
    //
    // NOTE: 这里"两个进程"是这么造的 —— 甲用本进程的 `Theme::load()` 拿到快照、`save()`
    // 落盘；乙那一趟直接用 `QSettings` 往同一个键写（另一份启动器保存时做的事就是这个）。
    // 整组重写的话第三条必红（念出来是 100）。
    {
        sandbox.remove(QStringLiteral("Theme"));
        sandbox.sync();

        {
            auto initial = Theme::load();
            QSettings other;
            other.setValue(QStringLiteral("Theme/TextOpacity"), 37); other.sync();
            initial.accent = QColor(QStringLiteral("#123456"));
            Theme::save(initial);
            QSettings reader;
            check(reader.value(QStringLiteral("Theme/TextOpacity")).toInt() == 37,
                  "T39.9 首次保存不把未落盘的默认值当作用户修改覆盖其它窗口");
            auto beforeDelete = Theme::load();
            other.remove(QStringLiteral("Theme/TextOpacity")); other.sync();
            beforeDelete.accent = QColor(QStringLiteral("#654321"));
            Theme::save(beforeDelete);
            QSettings afterDelete;
            check(!afterDelete.contains(QStringLiteral("Theme/TextOpacity")),
                  "T39.9 其它窗口删除以恢复默认的未改项不被复活");
            sandbox.remove(QStringLiteral("Theme")); sandbox.sync();
        }

        Theme::Spec first = Theme::load();
        first.accent = QColor(QStringLiteral("#112233"));
        Theme::save(first);
        // 每次读都新建一个 `QSettings`（别拿一个对象一直读：它自己那份缓存可能还没刷）。
        const auto readKey = [](const QString &key) {
            QSettings reader;
            return reader.value(key);
        };
        check(readKey(QStringLiteral("Theme/Accent")).toString() == QStringLiteral("#ff112233"),
              "★★ T39.9 甲改的主题色落了盘（`#aarrggbb`）",
              readKey(QStringLiteral("Theme/Accent")).toString());

        // 乙：另一个安装目录里的那份启动器，它启动时读到的也是上面那一份，
        // 之后只改「文字不透明度」并保存。
        {
            QSettings other;
            other.setValue(QStringLiteral("Theme/TextOpacity"), 37);
            other.sync();
        }

        // 甲再改一项（换预设）并保存：乙刚写的 37 **不许**被打回去。
        // 两个 id 都写成**认识的**那一档（`light` 是旧名字，落盘时会被 `normalize()`
        // 收成 `emerald`，拿它当"改成了什么"对不上盘上的值）。
        Theme::Spec second = first;
        second.presetId =
            second.presetId == QStringLiteral("dark") ? QStringLiteral("emerald") : QStringLiteral("dark");
        Theme::save(second);
        check(readKey(QStringLiteral("Theme/TextOpacity")).toInt() == 37,
              "★★★ T39.9 甲第二次保存没有把乙刚改的不透明度打回去",
              readKey(QStringLiteral("Theme/TextOpacity")).toString());
        check(readKey(QStringLiteral("Theme/Preset")).toString() == second.presetId,
              "★★ T39.9 甲自己改的那一项照样落了盘（不覆盖 ≠ 不写）",
              readKey(QStringLiteral("Theme/Preset")).toString());

        // 反方向：乙改主题色、甲改不透明度 —— 两条对称。
        {
            QSettings other;
            other.setValue(QStringLiteral("Theme/Accent"), QStringLiteral("#ff445566"));
            other.sync();
        }
        Theme::Spec third = Theme::load();
        third.textOpacity = 61;
        Theme::save(third);
        check(readKey(QStringLiteral("Theme/Accent")).toString() == QStringLiteral("#ff445566") &&
                  readKey(QStringLiteral("Theme/TextOpacity")).toInt() == 61,
              "★★ T39.9 反方向同样互不覆盖",
              QStringLiteral("主题色=%1 不透明度=%2")
                  .arg(readKey(QStringLiteral("Theme/Accent")).toString())
                  .arg(readKey(QStringLiteral("Theme/TextOpacity")).toString()));

        // 「改回默认」还得把那一项**删掉**：不删的话重开一次它又复活（这是原来整组
        // `remove("Theme")` 干的那件事，逐项写之后得单独守住）。
        {
            Theme::Spec withDanger = Theme::load();
            withDanger.danger = QColor(0x10, 0x20, 0x30);
            Theme::save(withDanger);
            check(readKey(QStringLiteral("Theme/Accents/Danger")).isValid(),
                  "★ T39.9 点名过的语义色写进了 `Theme/Accents/Danger`");
            Theme::Spec back = Theme::load();
            back.danger = QColor();  // 用户点了「跟随预设」
            Theme::save(back);
            check(!readKey(QStringLiteral("Theme/Accents/Danger")).isValid(),
                  "★★ T39.9 改回「跟随预设」⇒ 那个键真的从盘上删掉了（不是留着一个旧值）");
        }

        {
            auto local = Theme::load();
            local.danger = QColor(QStringLiteral("#112233"));
            Theme::save(local);
            local = Theme::load();
            QSettings other;
            other.setValue(QStringLiteral("Theme/Accents/Danger"), QStringLiteral("#ff445566"));
            other.sync();
            local.danger = QColor();
            Theme::save(local);
            check(readKey(QStringLiteral("Theme/Accents/Danger")).toString() == QStringLiteral("#ff445566"),
                  "T39.9 清除本地覆盖不删除外部刚修改的同键");
        }

        // 收尾：这一块自己写过的东西一个都不留（`restore()` 已经跑过一遍，这里补第二遍）。
        {
            QSettings future;
            future.setValue(QStringLiteral("Theme/Future/Option"), QStringLiteral("keep"));
            future.sync();
            auto known = Theme::load();
            known.textOpacity = 73;
            Theme::save(known);
            check(readKey(QStringLiteral("Theme/Future/Option")).toString() == QStringLiteral("keep"),
                  "T39.9 保存已知主题项不删除未来版本未知键");
        }
        sandbox.remove(QStringLiteral("Theme"));
        sandbox.sync();
        Theme::setSpec(themeBefore);
        Theme::apply(*qApp);
    }
}

