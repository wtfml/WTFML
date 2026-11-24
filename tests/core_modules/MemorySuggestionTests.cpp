#include "Support.hpp"

int runMemorySuggestionTests(const QString &sandbox) {
    Q_UNUSED(sandbox);
    section("L. 内存建议值（自动档按系统内存算）");

    // ---- L1: Xmx 真值表 ----
    // 规则：min(总/4, 可用/2) → 夹 [1024, 8192] → 再套"物理内存一半"的硬顶 →
    // 向下取整到 256，但不小于 256。
    struct XmxCase {
        qint64 total;
        qint64 available;
        int expect;
        const char *why;
    };
    const XmxCase xmxCases[] = {
        {0, 0, 4096, "读不到系统内存 → 内置默认值（不拿可用内存硬凑）"},
        {16384, 12288, 4096, "16G 空闲 → 16G/4"},
        {16384, 4096, 2048, "16G 但只剩 4G 可用 → 改成按可用算"},
        {32768, 24576, 8192, "32G → 32G/4 = 8G，正好到上限"},
        {65536, 49152, 8192, "64G → 被上限 8192 压住（不是 16G）"},
        {8192, 6144, 2048, "8G → 2G"},
        {4096, 3000, 1024, "4G → 1G，正好到下限"},
        {2048, 1500, 1024, "2G → 算出 512 被下限抬到 1024，又受 1G 硬顶"},
        {1024, 800, 512, "1G → 硬顶 512 压过下限（按 1024 硬凑会让 JVM 连堆都开不起来）"},
        {2048, 0, 1024, "可用内存读不到 → 只按总量算"},
        {65536, 2048, 1024, "可用内存极低 → 只剩 1024 可给"},
        {10000, 8000, 2304, "非 256 整数倍 → 向下取整（框里不出现 2500 这种数）"},
        {512, 400, 256, "极小机器 → 硬顶 256，但不再往下（setXmx 的下限）"},
    };
    for (const XmxCase &c : xmxCases) {
        const int got = GlobalJavaSettings::suggestXmxMb(c.total, c.available);
        check(got == c.expect,
              QStringLiteral("L1 suggestXmxMb(%1, %2) = %3")
                  .arg(c.total)
                  .arg(c.available)
                  .arg(c.expect),
              got == c.expect ? QString::fromUtf8(c.why)
                              : QStringLiteral("实际 %1（%2）").arg(got).arg(QString::fromUtf8(c.why)));
    }
    // 越低配的机器越不该给出"超过一半物理内存"的值（这一条是上面整张表的共同不变量）
    for (const XmxCase &c : xmxCases) {
        if (c.total <= 0) {
            continue;
        }
        const int got = GlobalJavaSettings::suggestXmxMb(c.total, c.available);
        check(got <= c.total / 2 || got == 256,
              QStringLiteral("L1 (%1, %2) 不会要超过一半物理内存").arg(c.total).arg(c.available),
              QString::number(got));
    }
    // 一律是 256 的整数倍：和界面两个框的步长一致
    for (const XmxCase &c : xmxCases) {
        const int got = GlobalJavaSettings::suggestXmxMb(c.total, c.available);
        check(got % 256 == 0, QStringLiteral("L1 (%1, %2) 是 256 的整数倍").arg(c.total).arg(c.available),
              QString::number(got));
    }

    // ---- L1b: 「这个实例装了多少模组」那一档（见 docs/FEATURES.md 待办"自动档不认整合包规模"）----
    // 规则加在**第 1 步**上：total/4 + 每 64 模组 512 MB（最多 +4096），之后照旧走
    // min(可用/2) → 夹 [1024, 8192] → min(总量/2) → 向下取整 256。数字同样是手算的。
    struct ModCase {
        qint64 total;
        qint64 available;
        int mods;
        int expect;
        const char *why;
    };
    const ModCase modCases[] = {
        {16384, 12288, 0, 4096, "一个模组都没有 → 和加这一档之前逐字节一样"},
        {16384, 12288, 63, 4096, "不满一档（64）不加"},
        {16384, 12288, 64, 4608, "一档 → 4096 + 512"},
        {16384, 12288, 200, 5632, "三档 → 4096 + 3*512（「200 模组」正是待办里那个例子）"},
        {16384, 12288, 1000, 6144, "加成封顶 +4096 → 8192，再被「可用/2」压回 6144"},
        {32768, 32768, 1000, 8192, "机器够大时顶到自动档的上限 8192（加成封顶 +4096 正是它）"},
        {32768, 6144, 200, 3072, "机器只有 6G 可用 → 可用/2 把加成整档吃掉（机器现实说了算）"},
        {4096, 3000, 128, 1280, "小机器：加成照样透出来一点（1024 → 1280），但超不过总量的一半"},
        {0, 0, 256, 4096, "读不到系统内存 → 内置默认值，加成不加"},
        {16384, 12288, -5, 4096, "负数当「没有模组」（不拿它去算减法）"},
    };
    for (const ModCase &c : modCases) {
        const int got = GlobalJavaSettings::suggestXmxMb(c.total, c.available, c.mods);
        check(got == c.expect,
              QStringLiteral("L1b suggestXmxMb(%1, %2, mods=%3) = %4")
                  .arg(c.total)
                  .arg(c.available)
                  .arg(c.mods)
                  .arg(c.expect),
              got == c.expect ? QString::fromUtf8(c.why)
                              : QStringLiteral("实际 %1（%2）").arg(got).arg(QString::fromUtf8(c.why)));
    }
    // 同两条不变量，对有模组的那一档照样成立
    for (const ModCase &c : modCases) {
        const int got = GlobalJavaSettings::suggestXmxMb(c.total, c.available, c.mods);
        check(got % 256 == 0,
              QStringLiteral("L1b (%1, %2, mods=%3) 是 256 的整数倍").arg(c.total).arg(c.available).arg(c.mods),
              QString::number(got));
        if (c.total > 0) {
            check(got <= c.total / 2 || got == 256,
                  QStringLiteral("L1b (%1, %2, mods=%3) 不会要超过一半物理内存")
                      .arg(c.total)
                      .arg(c.available)
                      .arg(c.mods),
                  QString::number(got));
        }
    }
    // 同一个实例、同一个机器，模组多的**不会**拿到比模组少的小（单调不降）
    for (int mods = 0; mods <= 512; mods += 16) {
        const int few = GlobalJavaSettings::suggestXmxMb(16384, 12288, mods);
        const int many = GlobalJavaSettings::suggestXmxMb(16384, 12288, mods + 64);
        check(many >= few,
              QStringLiteral("L1b mods=%1 → %2，mods=%3 → %4（多的不该更小）")
                  .arg(mods)
                  .arg(few)
                  .arg(mods + 64)
                  .arg(many),
              QStringLiteral("%1 / %2").arg(few).arg(many));
    }

    // ---- L2: Xms 真值表 ----
    struct XmsCase {
        int xmx;
        int expect;
    };
    const XmsCase xmsCases[] = {
        {1024, 512}, {2048, 512}, {4096, 1024}, {8192, 2048},
        {256, 256},   // Xmx 比下限还小时跟着退到 Xmx（Xms > Xmx 会让 JVM 拒绝启动）
        {512, 512},   // 同上：下限不超过 Xmx 本身
    };
    for (const XmsCase &c : xmsCases) {
        const int got = GlobalJavaSettings::suggestXmsMb(c.xmx);
        check(got == c.expect, QStringLiteral("L2 suggestXmsMb(%1) = %2").arg(c.xmx).arg(c.expect),
              QString::number(got));
    }
    // 不变量：Xms 永远不大于 Xmx（这条错了就是 JVM 直接起不来）。
    // 逐档扫一遍，只在出错时点出是哪一档，避免刷屏。
    {
        int badXmx = 0;
        for (int xmx = 256; xmx <= 16384; xmx += 256) {
            if (GlobalJavaSettings::suggestXmsMb(xmx) > xmx) {
                badXmx = xmx;
                break;
            }
        }
        check(badXmx == 0, "L2 Xms 永远 <= Xmx（256 到 16384 逐档扫过）",
              badXmx == 0 ? QString() : QStringLiteral("xmx=%1").arg(badXmx));
    }

    // ---- L3: 自动档的重算与落盘 ----
    // 真值来源是 QSettings，所以照 I5 那套先把 QSettings 搬进沙盒（**必须在第一个
    // QSettings 对象出现之前**），跑完再搬回去。这一节的键在真机器上写一遍没有意义，
    // 还会给用户留下垃圾。
    {
        const QString settingsDir = sandbox + "/memory-settings";
        QDir(settingsDir).removeRecursively();
        QDir().mkpath(settingsDir);

        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);

        section("L3 自动/手动档：读回、重算、快照与回滚");

        // 老配置（没有 Java/MemoryAuto 这个键）默认手动档，照旧原样读回那两个数。
        {
            QSettings raw;
            raw.clear();
            raw.setValue(QStringLiteral("Java/Xmx"), 4096);
            raw.setValue(QStringLiteral("Java/Xms"), 1024);
            raw.sync();
        }
        {
            GlobalJavaSettings old;
            check(!old.autoMemory(), "L3 老配置默认手动档");
            check(old.xmx() == 4096 && old.xms() == 1024, "L3 老配置的值原样读回",
                  QStringLiteral("%1/%2").arg(old.xmx()).arg(old.xms()));
        }

        // 手动档：重算**一个数字都不许动**，但仍然读系统内存给界面写说明。
        {
            QSettings raw;
            raw.clear();
            raw.setValue(QStringLiteral("Java/Xmx"), 8192);
            raw.setValue(QStringLiteral("Java/Xms"), 2048);
            raw.sync();
            GlobalJavaSettings custom;
            check(!custom.autoMemory(), "L3 没有自动键的配置是手动档");
            check(custom.xmx() == 8192 && custom.xms() == 2048, "L3 手动档的值原样读回",
                  QStringLiteral("%1/%2").arg(custom.xmx()).arg(custom.xms()));
            const SystemMemory::Usage usage = custom.refreshSuggestedMemory();
            check(custom.xmx() == 8192 && custom.xms() == 2048,
                  "L3 手动档下重算不动用户的数字",
                  QStringLiteral("%1/%2").arg(custom.xmx()).arg(custom.xms()));
            check(usage.totalMb > 0, "L3 手动档下也照样读了系统内存（界面拿它写说明）",
                  QString::number(usage.totalMb));
        }

        // 盘上写着 Java/MemoryAuto=true：自动档重新有入口了，读回来就应该是自动。
        // 自动档下 refreshSuggestedMemory() 按当前系统内存重算 Xmx/Xms。
        {
            QSettings raw;
            raw.clear();
            raw.setValue(QStringLiteral("Java/MemoryAuto"), true);
            raw.setValue(QStringLiteral("Java/Xmx"), 5120);
            raw.setValue(QStringLiteral("Java/Xms"), 1536);
            raw.sync();
            GlobalJavaSettings flagged;
            check(flagged.autoMemory(), "L3 盘上写着「自动」就读成自动档");
            check(flagged.manualXmx() == 5120 && flagged.manualXms() == 1536,
                  "L3 没有手动快照时，读回来的当前值就是回滚快照",
                  QStringLiteral("%1/%2").arg(flagged.manualXmx()).arg(flagged.manualXms()));

            const SystemMemory::Usage usage = flagged.refreshSuggestedMemory();
            check(usage.totalMb > 0, "L3 自动档读到了系统内存", QString::number(usage.totalMb));
            check(flagged.xmx() ==
                      GlobalJavaSettings::suggestXmxMb(usage.totalMb, usage.availableMb) &&
                      flagged.xms() == GlobalJavaSettings::suggestXmsMb(flagged.xmx()),
                  "L3 自动档下重算落到 suggestXmxMb()/suggestXmsMb() 那一对上",
                  QStringLiteral("%1/%2").arg(flagged.xmx()).arg(flagged.xms()));
            check(flagged.manualXmx() == 5120 && flagged.manualXms() == 1536,
                  "L3 重算不会把回滚快照改成自动值",
                  QStringLiteral("%1/%2").arg(flagged.manualXmx()).arg(flagged.manualXms()));
        }

        // 手动 → 自动时记现场；自动 → 手动时原样回滚。快照落盘，重启之后还在。
        {
            QSettings raw;
            raw.clear();
            raw.sync();
            GlobalJavaSettings manual;
            manual.setXmx(3072);
            manual.setXms(768);
            manual.save();
            check(!manual.autoMemory(), "L3 起点是手动档");
            manual.setAutoMemory(true);
            check(manual.autoMemory() && manual.manualXmx() == 3072 && manual.manualXms() == 768,
                  "L3 切到自动档的一瞬间记下手动现场",
                  QStringLiteral("%1/%2").arg(manual.manualXmx()).arg(manual.manualXms()));
            check(manual.xmx() == 3072 && manual.xms() == 768,
                  "L3 setAutoMemory() 自己不动 Xmx/Xms（重算由 refreshSuggestedMemory() 负责）",
                  QStringLiteral("%1/%2").arg(manual.xmx()).arg(manual.xms()));
            manual.refreshSuggestedMemory();
            // 再调一次 true 不能把快照改成自动算出来的数。
            manual.setAutoMemory(true);
            check(manual.manualXmx() == 3072 && manual.manualXms() == 768,
                  "L3 已经在自动档上再调 setAutoMemory(true) 不覆盖手动静照",
                  QStringLiteral("%1/%2").arg(manual.manualXmx()).arg(manual.manualXms()));

            GlobalJavaSettings reopened;
            check(reopened.autoMemory(), "L3 重启后仍然是自动档");
            check(reopened.manualXmx() == 3072 && reopened.manualXms() == 768,
                  "L3 回滚快照跟着落盘、跨重启还在",
                  QStringLiteral("%1/%2").arg(reopened.manualXmx()).arg(reopened.manualXms()));

            reopened.restoreManualMemory();
            GlobalJavaSettings afterRestore;
            check(!afterRestore.autoMemory() && afterRestore.xmx() == 3072 &&
                      afterRestore.xms() == 768,
                  "L3 切回手动档恢复的是开启自动之前那一对",
                  QStringLiteral("%1/%2 auto=%3")
                      .arg(afterRestore.xmx()).arg(afterRestore.xms())
                      .arg(afterRestore.autoMemory()));
        }

        // 老键仍然写得进去、读得出来（数据格式没变）。
        {
            QSettings raw;
            raw.clear();
            raw.sync();
            GlobalJavaSettings s;
            s.setAutoMemory(true);
            check(!QSettings().value(QStringLiteral("Java/MemoryAuto"), QVariant()).isNull(),
                  "L3 Java/MemoryAuto 这个键照旧写进盘");
            GlobalJavaSettings reopened;
            check(reopened.autoMemory(), "L3 新版本读回来就是自动档");
        }

        // ---- L4: 两份启动器共用同一份全局 Java 设置：各改各的，互不覆盖 ----
        // 和 ZD6 同一个场景（计划里 P0-2 那一半）：全局设置那棵注册表按机器共用，
        // 各安装目录的两份启动器同时开着时，整份写回会把对方刚改的一项打回去。
        // 这里甲只改 Java 路径、乙只改内存，两个方向各验一次。
        section("L4 两份启动器共用全局设置：各改各的，谁也不把对方刚改的那一项打回去");
        {
            QSettings raw;
            raw.clear();
            raw.sync();

            // 甲、乙都在**任何一次保存之前**读盘（两份快照都已经在内存里）。
            GlobalJavaSettings first;
            GlobalJavaSettings second;

            first.setJavaPath(QStringLiteral("D:/Java/jdk21/bin/java.exe"));
            first.save();
            // 乙这时还不知道甲改了路径（它的快照还是启动时那份空的），只改内存。
            second.setXmx(6144);
            second.save();

            GlobalJavaSettings probe;
            check(probe.javaPath() == QStringLiteral("D:/Java/jdk21/bin/java.exe"),
                  "★★ L4 乙保存它那一项时，没有把甲刚改的 Java 路径打回去",
                  probe.javaPath());
            check(probe.xmx() == 6144,
                  "★★ L4 甲保存时也没有把乙的内存打回去",
                  QString::number(probe.xmx()));

            // 收尾：清成空配置，别给后面（或以后加的）断言留值。
            raw.clear();
            raw.sync();
        }

        // ---- L5: 切回手动之后，另一扇窗里"旧的自动档"不许把手动现场盖回自动值 ----
        // 症状（2026-11-04 mw_check 那条间歇失败）：两扇窗同时开着；A 切回手动、把现场
        // 5120 写回盘，B 还停在它启动时读到的"自动档"上。B 下一次自动刷新（那 2 秒定时器）
        // 若 save()，就把自动算出来的数写回盘 —— 用户看到的是"切回手动后内存还是自动值"。
        section("L5 切回手动之后，旧的自动档实例不许盖掉手动现场");
        {
            QSettings raw;
            raw.clear();
            raw.sync();

            GlobalJavaSettings a;
            a.setXmx(5120);
            a.setXms(512);
            a.save();
            a.setAutoMemory(true);          // 记下现场 5120/512
            a.refreshSuggestedMemory();     // 自动值进内存
            a.save();

            GlobalJavaSettings stale;       // 另一扇窗：这时读盘，记下"自动档"
            check(stale.autoMemory(), "L5 前提：第二份读到的是自动档");

            a.restoreManualMemory();        // 用户切回手动 ⇒ 盘上回到 5120
            GlobalJavaSettings afterRestore;
            check(!afterRestore.autoMemory() && afterRestore.xmx() == 5120,
                  "L5 切回手动之后盘上就是现场",
                  QStringLiteral("%1 auto=%2")
                      .arg(afterRestore.xmx())
                      .arg(afterRestore.autoMemory()));

            // 第二份按自动档又算了一遍，而且**这一趟算出来的数和它读盘时不一样**
            // （系统可用内存变了就会这样）—— 这正是 mw_check 那次失败的条件；
            // 手动 +256 把它摆成确定会写盘的形状，不用等机器上真的变。
            stale.refreshSuggestedMemory();
            stale.setXmx(stale.xmx() + 256);
            stale.save();

            GlobalJavaSettings probe;
            check(!probe.autoMemory() && probe.xmx() == 5120 && probe.xms() == 512,
                  "★★ L5 旧的自动档实例不许把手动现场盖成自动值",
                  QStringLiteral("%1/%2 auto=%3")
                      .arg(probe.xmx())
                      .arg(probe.xms())
                      .arg(probe.autoMemory()));
            raw.clear();
            raw.sync();
        }

        QSettings::setDefaultFormat(QSettings::NativeFormat);
        QDir(settingsDir).removeRecursively();
    }

    return 0;
}

