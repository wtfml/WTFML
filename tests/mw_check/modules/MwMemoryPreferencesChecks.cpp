#include "Support.hpp"
int MwMemoryPreferencesChecks(const bool settingsWired, const QString &kDecorated, MainWindow &window, QComboBox * &installed, QComboBox * &memoryMode, QLabel * &memoryHint, QSpinBox * &maxSpin, QComboBox * &behaviourCombo, QComboBox * &sourceCombo, QPlainTextEdit * &jvmEdit, SettingsParts::CommitOnFocusOut * &applyJvm, MemorySlider * &maxSlider) {
    if (settingsWired) {
        // ---- 内存：改一个数就落盘 ----
        // 默认是手动档：条和数字框开窗就能改。这里量的第一件事是"它开窗时确实已经能改"，
        // 然后才是改一下立刻落盘。
        {
            GlobalJavaSettings probe;
            check(!probe.autoMemory(), "盘上记的是手动档（默认档）");
            check(maxSpin->isEnabled() && maxSlider->isEnabled(), "手动档下条和数字框开窗就能编辑");
            check(maxSpin->value() == probe.xmx(),
                  "开窗时框里就是盘上那个数，没有退回内置默认值",
                  QString("界面 %1，盘上 %2").arg(maxSpin->value()).arg(probe.xmx()));
            check(!memoryHint->text().contains(QStringLiteral("自动分配")),
                  "手动档说明文字里不提「自动分配」", memoryHint->text());
        }

        const int editTargetMb = qMin(8192, maxSpin->maximum());
        maxSpin->setValue(editTargetMb);
        {
            GlobalJavaSettings probe;
            check(probe.xmx() == editTargetMb,
                  "改完最大内存立刻落盘（另起一个读盘者读到的是新值）",
                  QString::number(probe.xmx()));
        }

        // ---- 改数字框：条跟着走（两件东西说的是同一个数） ----
        {
            const QList<int> steps = MemorySlider::valueSteps(maxSpin->minimum(), maxSpin->maximum());
            check(steps.value(maxSlider->value(), -1) == editTargetMb,
                  "在数字框里打一个数，条跟着挪到那一格上",
                  QString("条 %1 / 框 %2").arg(steps.value(maxSlider->value(), -1))
                      .arg(maxSpin->value()));
        }

        // ---- 拖条：数字框跟着走，而且**松手才落盘** ----
        // 拖动中 valueChanged 会发几十上百发，每一发都写一遍注册表是不行的 —— 落盘那一下
        // 留给 sliderReleased（见 ctor 里那两条线）。这里用 setValue + 合成 press/release
        // 把"拖动中"和"松手"两种时刻分开量：setValue 时 isSliderDown() 还是假（等价于
        // 键盘 / 滚轮那一路），press 之后才是拖动中。
        {
            const int capMb = maxSpin->maximum();
            const QList<int> steps = MemorySlider::valueSteps(maxSpin->minimum(), capMb);
            const int bigIndex = MemorySlider::nearestIndex(steps, capMb);
            const int midIndex = MemorySlider::nearestIndex(steps, 4096);

            // ① 键盘那一路（= 非拖动）：立刻落盘
            maxSlider->setValue(midIndex);
            QApplication::processEvents();
            {
                GlobalJavaSettings probe;
                check(maxSpin->value() == steps.at(midIndex) && probe.xmx() == steps.at(midIndex),
                      "条挪一格，数字框和盘上跟着变成那一格的值",
                      QString("框 %1 / 盘上 %2").arg(maxSpin->value()).arg(probe.xmx()));
            }

            // ② 拖动中：数字跟着走、盘上**先不动**
            // 「拖动中」用 setSliderDown(true) 摆出来 —— 那正是产品读的那一位
            // （onMemorySliderMoved 里判的 isSliderDown()），不用合成鼠标事件：
            // 合成一发 press 只会先被"点滑槽直接跳过去"那条路吃掉（Fusion 的
            // SH_Slider_AbsoluteSetButtons 就是 LeftButton），量到的落盘不是拖动那一路的。
            {
                GlobalJavaSettings before;
                const int beforeXmx = before.xmx();
                maxSlider->setSliderDown(true);
                maxSlider->setValue(bigIndex);   // 拖动中又挪到了头
                QApplication::processEvents();
                GlobalJavaSettings during;
                check(maxSpin->value() == steps.at(bigIndex),
                      "拖动中数字框跟着走（松手才更新的话，用户拖的时候那个数一直不动）",
                      QString::number(maxSpin->value()));
                check(during.xmx() == beforeXmx,
                      "拖动中一个字节都不落盘（一次拖动几十发 valueChanged，写不起）",
                      QString("拖动前 %1 / 拖动中 %2").arg(beforeXmx).arg(during.xmx()));

                maxSlider->setSliderDown(false);   // 松手
                QApplication::processEvents();
                GlobalJavaSettings after;
                check(after.xmx() == steps.at(bigIndex),
                      "松手那一发才落盘，落的就是最后停的那一格",
                      QString("松手后盘上 %1（期望 %2）").arg(after.xmx()).arg(steps.at(bigIndex)));
                check(maxSpin->value() == after.xmx(),
                      "松手之后框和盘上还是同一个数", QString("%1/%2")
                          .arg(maxSpin->value()).arg(after.xmx()));
            }
        }
        maxSpin->setValue(8192);
        QApplication::processEvents();

        // ---- 最大 < 最小：把最小拉下来，不去顶用户刚填的那个 ----
        // 界面上**没有**最小内存那一格了，但 Xms 照样发给 JVM ⇒ 这道夹子
        // 挪到了 onMemoryChanged()/refreshMemoryUi() 里（见 clampXmsToXmx）。判据只能
        // 看盘上：把最大内存拖到下限（256）时，那个比它还大的最小内存必须跟着降下来
        // —— 不降的话下一次启动 JVM 直接报 "Initial heap size set to a larger value
        // than the maximum heap size" 然后退出，而用户在界面上什么都看不出来。
        {
            GlobalJavaSettings before;
            check(before.xms() > 256,
                  "前提：这会儿盘上的最小内存比 256 大（不然这条断言白写）",
                  QString::number(before.xms()));
            maxSpin->setValue(256);
            QApplication::processEvents();
            GlobalJavaSettings after;
            check(after.xmx() == 256 && after.xms() == 256,
                  "最大内存降到 256 时，最小内存跟着降到 256（JVM 会拒绝相反的组合）",
                  QString("%1/%2").arg(after.xmx()).arg(after.xms()));
            check(memoryHint->text().contains(
                      QStringLiteral("最小内存由启动器设为 %1 MB").arg(after.xms())),
                  "说明文字里那句「最小内存由启动器设为 … MB」跟着重画了",
                  memoryHint->text());
            check(maxSpin->value() == 256 && maxSlider->value() == 0,
                  "条和数字框都在下限那一格上",
                  QString("框 %1 / 条下标 %2").arg(maxSpin->value()).arg(maxSlider->value()));
        }

        // ---- 自动分配：展示自动值、禁用输入、切回手动时回滚 ----
        // 用户点名的那条路 —— 抓「自动分配 / 手动分配」那颗下拉框。
        // 自动档下条和数字框禁用，但仍然展示自动算出来的内存；进度条（虚条）跟着
        // 自动值走，圆圈停在自动之前的手动位置。切回手动时把那一对原样还回去。
        const int manualTargetMb = qMin(5120, maxSpin->maximum());
        maxSpin->setValue(manualTargetMb);
        QApplication::processEvents();
        int manualTargetXms = 0;
        {
            GlobalJavaSettings probe;
            manualTargetXms = probe.xms();
            check(probe.xmx() == manualTargetMb && !probe.autoMemory(),
                  "手动档那个数存下去了，盘上记的是手动档",
                  QString("%1 / %2").arg(probe.xmx()).arg(probe.autoMemory()));
        }

        int autoDisplayedXmx = 0;
        memoryMode->setCurrentIndex(memoryMode->findData(true));
        QApplication::processEvents();
        {
            GlobalJavaSettings probe;
            autoDisplayedXmx = probe.xmx();
            check(probe.autoMemory(), "切到自动分配后立刻落盘");
            check(probe.manualXmx() == manualTargetMb && probe.manualXms() == manualTargetXms,
                  "自动之前的现场（Xmx/Xms）记下来了",
                  QString("%1/%2").arg(probe.manualXmx()).arg(probe.manualXms()));
            check(autoDisplayedXmx >= 256 && autoDisplayedXmx <= maxSpin->maximum(),
                  "自动档给出了一个在界面范围内的建议值",
                  QString::number(autoDisplayedXmx));
            check(!maxSpin->isEnabled() && !maxSlider->isEnabled(),
                  "自动档下最大内存数字框和条都禁用");
            check(maxSpin->value() == autoDisplayedXmx,
                  "数字框里显示的是自动值（虽然禁用着，但仍然要能看见）",
                  QString("框 %1 / 自动 %2").arg(maxSpin->value()).arg(autoDisplayedXmx));
            check(maxSlider->hasAutoValue() &&
                      maxSlider->autoValueMb() == autoDisplayedXmx,
                  "条上的虚条展示自动值",
                  QString("虚条 %1 / 自动 %2")
                      .arg(maxSlider->autoValueMb()).arg(autoDisplayedXmx));
            // 圆圈必须落在**虚条的头**（自动值那一格）：停在"上次手动"那个位置时，自动值
            // 比它小的话圆圈就浮在一段没填的轨道上，用户报的就是"圆圈跟条脱开了"。
            const QList<int> autoSteps =
                MemorySlider::valueSteps(maxSpin->minimum(), maxSpin->maximum());
            const int autoStop =
                autoSteps.value(MemorySlider::nearestIndex(autoSteps, autoDisplayedXmx), -1);
            check(maxSlider->valueMb() == autoStop,
                  "圆圈落在虚条头上（自动值那一格），不浮在没填的轨道上",
                  QString("圆圈 %1 / 自动应停 %2").arg(maxSlider->valueMb()).arg(autoStop));
            check(memoryHint->text().contains(QStringLiteral("自动分配")) &&
                      memoryHint->text().contains(QStringLiteral("恢复")),
                  "说明文字讲清了这是自动值、以及切回手动会恢复原值",
                  memoryHint->text());
        }

        // 自动档下再开一扇窗：档位、禁用状态、自动展示值、手动圆圈都得跟现在一致。
        {
            MainWindow again;
            again.show();
            QApplication::processEvents();
            QComboBox *reopenedMode =
                byObjectName<QComboBox>(&again, MainWindowUI::memoryModeComboName());
            QSpinBox *reopenedSpin =
                byObjectName<QSpinBox>(&again, MainWindowUI::memoryMaxSpinName());
            auto *reopenedSlider = static_cast<MemorySlider *>(
                byObjectName<QSlider>(&again, MainWindowUI::memoryMaxSliderName()));
            GlobalJavaSettings reopenedProbe;
            check(reopenedMode != nullptr && reopenedMode->currentData().toBool(),
                  "自动档跨开窗保留（下拉框还停在自动分配）");
            check(reopenedProbe.autoMemory(), "新窗口读到的盘上也是自动档");
            check(reopenedSpin != nullptr && !reopenedSpin->isEnabled() &&
                      reopenedSpin->value() == reopenedProbe.xmx(),
                  "新窗口里数字框仍禁用、显示自动值",
                  reopenedSpin != nullptr ? QString::number(reopenedSpin->value()) : QString());
            const QList<int> reopenSteps =
                reopenedSpin != nullptr
                    ? MemorySlider::valueSteps(reopenedSpin->minimum(), reopenedSpin->maximum())
                    : QList<int>();
            const int reopenStop =
                reopenSteps.value(MemorySlider::nearestIndex(reopenSteps, reopenedProbe.xmx()), -1);
            check(reopenedSlider != nullptr && !reopenedSlider->isEnabled() &&
                      reopenedSlider->hasAutoValue() &&
                      reopenedSlider->autoValueMb() == reopenedProbe.xmx() &&
                      reopenedSlider->valueMb() == reopenStop,
                  "新窗口的条也画着自动虚条，圆圈落在自动值那一格",
                  reopenedSlider != nullptr
                      ? QString("虚条 %1 / 圆圈 %2 / 应停 %3")
                            .arg(reopenedSlider->autoValueMb())
                            .arg(reopenedSlider->valueMb())
                            .arg(reopenStop)
                      : QString());
        }

        // 切回手动分配：恢复开启自动之前的那一对，控件重新可用。
        memoryMode->setCurrentIndex(memoryMode->findData(false));
        QApplication::processEvents();
        {
            GlobalJavaSettings probe;
            check(!probe.autoMemory(), "切回手动分配后盘上记的是手动档");
            check(probe.xmx() == manualTargetMb && probe.xms() == manualTargetXms,
                  "Xmx/Xms 回滚到开启自动之前的那一对",
                  QString("现在 %1/%2 / 期望 %3/%4")
                      .arg(probe.xmx()).arg(probe.xms())
                      .arg(manualTargetMb).arg(manualTargetXms));
            check(maxSpin->isEnabled() && maxSlider->isEnabled(),
                  "切回手动之后数字框和条重新可用");
            check(maxSpin->value() == manualTargetMb &&
                      !maxSlider->hasAutoValue() && maxSlider->valueMb() == manualTargetMb,
                  "数字框和圆圈都回到手动现场，虚条清掉",
                  QString("框 %1 / 圆圈 %2 / 虚条 %3")
                      .arg(maxSpin->value()).arg(maxSlider->valueMb())
                      .arg(maxSlider->hasAutoValue() ? QStringLiteral("有")
                                                     : QStringLiteral("无")));
            check(!memoryHint->text().contains(QStringLiteral("自动分配")),
                  "说明文字回到手动档那一句", memoryHint->text());
        }

        // 手动档再开窗：显示的还是用户那个数（不重算）。
        {
            MainWindow again;
            again.show();
            QApplication::processEvents();
            QComboBox *reopenedMode =
                byObjectName<QComboBox>(&again, MainWindowUI::memoryModeComboName());
            QSpinBox *spin = byObjectName<QSpinBox>(&again, MainWindowUI::memoryMaxSpinName());
            QSlider *reopenedSlider =
                byObjectName<QSlider>(&again, MainWindowUI::memoryMaxSliderName());
            check(reopenedMode != nullptr && !reopenedMode->currentData().toBool(),
                  "手动档跨开窗保留（下拉框停在手动分配）");
            check(spin != nullptr && spin->isEnabled() && spin->value() == manualTargetMb,
                  "新开一扇设置窗，框里还是用户拖出来的那个数（开窗不重算）",
                  spin != nullptr ? QString::number(spin->value()) : QString());
            check(spin != nullptr && reopenedSlider != nullptr &&
                      MemorySlider::valueSteps(spin->minimum(), spin->maximum())
                              .value(reopenedSlider->value(), -1) == manualTargetMb,
                  "那根条也停在同一格上",
                  reopenedSlider != nullptr ? QString::number(reopenedSlider->value())
                                            : QString());
        }
        // 到这里盘上和界面都停在 manualTargetMb，后面几节接着跑时起点是确定的。

        // ---- 启动后：落盘，且存的是约定的字符串 ----
        // 下面两条故意写死键名和存进去的串：LauncherSettings.cpp 里那两句注释说了
        // "这几个串是持久化格式的一部分"，改掉它们等于让用户盘上已经存好的档位静默退回
        // 默认。写死在这里，改了就会红。
        behaviourCombo->setCurrentIndex(
            behaviourCombo->findData(int(LauncherBehaviour::CloseAndRestore)));
        {
            LauncherSettings probe;
            QSettings raw;
            check(probe.launcherBehaviour() == LauncherBehaviour::CloseAndRestore,
                  "改「启动后」立刻落盘");
            check(raw.value(QStringLiteral("Launcher/AfterLaunch")).toString() ==
                      LauncherSettings::behaviourKey(LauncherBehaviour::CloseAndRestore),
                  "存的是约定的字符串（不是枚举下标），键名 Launcher/AfterLaunch",
                  raw.value(QStringLiteral("Launcher/AfterLaunch")).toString());
        }

        // ---- 下载源：落盘 + 存成字符串 ----
        sourceCombo->setCurrentIndex(sourceCombo->findData(int(DownloadSource::Mirror)));
        {
            LauncherSettings probe;
            QSettings raw;
            check(probe.downloadSource() == DownloadSource::Mirror, "改「下载源」立刻落盘");
            check(raw.value(QStringLiteral("Download/Source")).toString() ==
                      LauncherSettings::downloadSourceKey(DownloadSource::Mirror),
                  "存的是字符串（换源要重下几个 GB，把 old 值读歪的代价太大）",
                  raw.value(QStringLiteral("Download/Source")).toString());
        }
        // NOTE: 这个开关的最后一跳（VersionManager::setUseMirror → DownloadManager 真的
        // 换掉 URL 主机）在这里**看不到**：VersionManager 不往回暴露这个开关。
        // 而且这一跳**目前没有任何断言覆盖** —— mirrorUrl()/mirrorVersionJsonUrl()
        // 那一组静态断言并不存在。这里只钉"下拉框 → 盘"这一跳，以及下面 N 节
        // "盘 → 新窗口的下拉框"那一跳。

        // ---- 显示名的拼法是固定的（UI 问题 #14：那一组开关撤掉了）----
        // 显示名就是 自定义名-实际版本-加载器名（原先是那四个开关的出厂档），加载器版本
        // 不进显示名；实例列表和下拉框是同一份数据的两种看法，写法必须一致。
        {
            const int at = installed->findData(QStringLiteral("1.20.1"));
            check(installed->itemText(at) == kDecorated,
                  "实例下拉框里的显示名是 自定义名-实际版本-加载器名", installed->itemText(at));
            check(!installed->itemText(at).contains(QStringLiteral("47.2.0")),
                  "加载器版本不进显示名", installed->itemText(at));
            bool listMatches = false;
            QTreeWidget *instanceListView =
                byObjectName<QTreeWidget>(&window, MainWindowUI::instanceListName());
            for (QTreeWidgetItem *item : instanceItemsOf(instanceListView)) {
                if (item->data(0, Qt::UserRole).toString() == QStringLiteral("1.20.1")) {
                    listMatches = item->text(0) == installed->itemText(at);
                }
            }
            check(listMatches, "实例列表里的同一个版本写法一致（同一份数据的两种看法）");
        }

        // ---- JVM 参数：失焦（committed）才落盘，且去首尾空白 ----
        jvmEdit->setPlainText(QStringLiteral("  -Xss1M  "));
        {
            GlobalJavaSettings probe;
            check(probe.extraJvmArgs() == QStringLiteral("-XX:+UseG1GC"),
                  "只改文本框、还没失焦时盘上的值不动（半条参数不能进命令行）",
                  probe.extraJvmArgs());
        }
        emit applyJvm->committed();
        {
            GlobalJavaSettings probe;
            check(probe.extraJvmArgs() == QStringLiteral("-Xss1M"),
                  "失焦提交后落盘，且首尾空白被去掉", probe.extraJvmArgs());
        }
    }
    return 0;
}


