#include "Support.hpp"

void runStartupDiagnosisTests() {
    section("DX. 启动诊断（「综合启动诊断报告」）");

    QTemporaryDir dir;
    check(dir.isValid(), "DX0 临时目录");
    if (!dir.isValid()) {
        return;
    }
    const QString root = dir.path();
    const auto textOf = [](const StartupDiagnosis::Report &report) { return report.toText(); };
    const auto hasTitle = [](const StartupDiagnosis::Report &report, const QString &needle) {
        for (const StartupDiagnosis::Finding &finding : report.findings) {
            if (finding.title.contains(needle)) return true;
        }
        return false;
    };
    const auto baseInputs = [&] {
        StartupDiagnosis::Inputs inputs;
        inputs.instanceName = QStringLiteral("测试实例");
        inputs.versionId = QStringLiteral("1.20.1-Forge");
        inputs.gameVersion = QStringLiteral("1.20.1");
        inputs.loaderName = QStringLiteral("Forge");
        inputs.loaderVersion = QStringLiteral("47.2.0");
        inputs.requiredJavaMajor = 17;
        inputs.autoSelectJava = true;
        inputs.autoMemory = true;
        inputs.xmxMb = 2048;
        inputs.systemTotalMb = 8192;
        inputs.systemAvailableMb = 4096;
        return inputs;
    };

    // ---------- DX1. 一切都正常 ----------
    {
        const StartupDiagnosis::Inputs inputs = baseInputs();
        const StartupDiagnosis::Report report = StartupDiagnosis::analyze(inputs);
        check(report.problems == 0 && report.warnings == 0,
              "★ DX1 一份没毛病的实例：0 个问题、0 条提醒",
              QStringLiteral("%1 / %2").arg(report.problems).arg(report.warnings));
        check(report.headline() == QStringLiteral("启动诊断：未发现明显问题"),
              "DX1 顶上那一句", report.headline());
        check(report.instanceLine.contains(QStringLiteral("测试实例"))
                  && report.instanceLine.contains(QStringLiteral("1.20.1"))
                  && report.instanceLine.contains(QStringLiteral("Forge 47.2.0")),
              "DX1 第二行说这是哪个实例（名字 / 版本 / 加载器版本）", report.instanceLine);
        check(hasTitle(report, QStringLiteral("加载器：Forge 47.2.0")),
              "DX1 加载器那一条");
        check(hasTitle(report, QStringLiteral("暂无崩溃报告"))
                  && hasTitle(report, QStringLiteral("暂无启动日志")),
              "DX1 没崩溃报告、没日志时如实说「没有」，不编");
        check(textOf(report).contains(QStringLiteral("【正常】"))
                  && textOf(report).contains(QStringLiteral("【信息】")),
              "DX1 报告里带严重度标题");
    }

    // ---------- DX2. 原版实例 + mods 里有东西 ----------
    {
        StartupDiagnosis::Inputs inputs = baseInputs();
        inputs.loaderName.clear();
        inputs.loaderVersion.clear();
        const QString mods = root + QStringLiteral("/mods-noloader");
        QDir().mkpath(mods);
        writeFileAt(mods + QStringLiteral("/a.jar"), QByteArray("x"));
        writeFileAt(mods + QStringLiteral("/b.jar"), QByteArray("x"));
        inputs.modsDir = mods;
        const StartupDiagnosis::Report report = StartupDiagnosis::analyze(inputs);
        check(report.problems == 1 && hasTitle(report, QStringLiteral("未安装模组加载器")),
              "★★ DX2 原版实例却有两个 .jar ⇒ **问题**（游戏不会读它们），不是一句提醒",
              textOf(report));
        check(hasTitle(report, QStringLiteral("启用了 2 个模组")),
              "DX2 数出来的是 2 个", textOf(report));
    }

    // ---------- DX3. Java 这一档 ----------
    {
        StartupDiagnosis::Inputs missing = baseInputs();
        missing.configuredJavaPath = root + QStringLiteral("/no/such/java.exe");
        const StartupDiagnosis::Report report = StartupDiagnosis::analyze(missing);
        check(report.problems == 1
                  && hasTitle(report, QStringLiteral("指定的 Java 不可用")),
              "★ DX3 指定的 Java 路径不存在 ⇒ 问题", textOf(report));

        StartupDiagnosis::Inputs manualEmpty = baseInputs();
        manualEmpty.autoSelectJava = false;
        manualEmpty.configuredJavaPath.clear();
        const StartupDiagnosis::Report report2 = StartupDiagnosis::analyze(manualEmpty);
        check(report2.problems == 1
                  && hasTitle(report2, QStringLiteral("手动指定")),
              "★ DX3 手动档却空着路径 ⇒ 问题（手动档不会自己去挑）", textOf(report2));

        StartupDiagnosis::Inputs autoNoPath = baseInputs();
        autoNoPath.configuredJavaPath.clear();
        const StartupDiagnosis::Report report3 = StartupDiagnosis::analyze(autoNoPath);
        check(report3.problems == 0 && hasTitle(report3, QStringLiteral("自动选择")),
              "DX3 自动档、还没挑过 ⇒ 只是一条「信息」", textOf(report3));
    }

    // ---------- DX4. 内存那几档 ----------
    {
        StartupDiagnosis::Inputs over = baseInputs();
        over.xmxMb = 9000;
        over.systemTotalMb = 8192;
        const StartupDiagnosis::Report report = StartupDiagnosis::analyze(over);
        check(report.problems == 1 && hasTitle(report, QStringLiteral("超过设备总内存")),
              "★★ DX4 Xmx 比机器总内存还大 ⇒ 问题", textOf(report));

        StartupDiagnosis::Inputs high = baseInputs();
        high.xmxMb = 7000;
        high.systemTotalMb = 8192;
        const StartupDiagnosis::Report report2 = StartupDiagnosis::analyze(high);
        check(report2.warnings == 1 && hasTitle(report2, QStringLiteral("过多")),
              "★ DX4 超过五分之四 ⇒ 提醒（不是问题）", textOf(report2));

        StartupDiagnosis::Inputs tiny = baseInputs();
        tiny.xmxMb = 256;
        const StartupDiagnosis::Report report3 = StartupDiagnosis::analyze(tiny);
        check(report3.warnings == 1 && hasTitle(report3, QStringLiteral("过小")),
              "★ DX4 256 MB ⇒ 提醒「偏小」", textOf(report3));

        StartupDiagnosis::Inputs unknownMemory = baseInputs();
        unknownMemory.systemTotalMb = 0;
        const StartupDiagnosis::Report report4 = StartupDiagnosis::analyze(unknownMemory);
        check(hasTitle(report4, QStringLiteral("无法读取设备物理内存"))
                  && !hasTitle(report4, QStringLiteral("超过设备总内存")),
              "★ DX4 读不到系统内存：明说读不到，**不拿 0 当总内存**", textOf(report4));
    }

    // ---------- DX5. mods 的禁用那一格 ----------
    {
        StartupDiagnosis::Inputs inputs = baseInputs();
        const QString mods = root + QStringLiteral("/mods-disabled");
        QDir().mkpath(mods);
        writeFileAt(mods + QStringLiteral("/on.jar"), QByteArray("x"));
        for (int i = 0; i < 7; ++i) {
            writeFileAt(mods + QStringLiteral("/off%1.jar.disabled").arg(i), QByteArray("x"));
        }
        inputs.modsDir = mods;
        const StartupDiagnosis::Report report = StartupDiagnosis::analyze(inputs);
        check(hasTitle(report, QStringLiteral("已启用 1 个模组")),
              "DX5 启用中的只算 `.jar`（`.jar.disabled` 不算）", textOf(report));
        check(hasTitle(report, QStringLiteral("有 7 个模组处于禁用状态")),
              "DX5 禁用数按 `.jar.disabled` 数", textOf(report));
        check(textOf(report).contains(QStringLiteral("…还有 2 个")),
              "★ DX5 禁用名字只列前 5 个，余下用「还有 N 个」收尾"
              "（**绝不出现「还有 1 个未列出」那种别扭说法**）",
              textOf(report));
    }

    {
        const QString crashDir = root + QStringLiteral("/crash");
        QDir().mkpath(crashDir);
        writeFileAt(crashDir + QStringLiteral("/crash-2026-10-18_12.00.00-client.txt"),
                    QByteArrayLiteral("---- Minecraft Crash Report ----\n"
                                      "Description: Rendering overlay\n"
                                      "Time: 2026-10-18 12:00:00\n"
                                      "java.lang.OutOfMemoryError: Java heap space\n"));
        StartupDiagnosis::Inputs inputs = baseInputs();
        inputs.crashDir = crashDir;
        const StartupDiagnosis::Report report = StartupDiagnosis::analyze(inputs);
        check(hasTitle(report, QStringLiteral("内存不足")),
              "★ DX6 崩溃报告里是 OutOfMemoryError ⇒ 说「内存不足」，并把报告里那两行带上",
              textOf(report));
        check(textOf(report).contains(QStringLiteral("Description: Rendering overlay")),
              "DX6 证据是崩溃报告里的原文");

        const QString crashDir2 = root + QStringLiteral("/crash-class");
        QDir().mkpath(crashDir2);
        writeFileAt(crashDir2 + QStringLiteral("/crash-2.txt"),
                    QByteArrayLiteral("Description: Exception in server tick loop\n"
                                      "java.lang.NoClassDefFoundError: foo/Bar\n"));
        StartupDiagnosis::Inputs inputs2 = baseInputs();
        inputs2.crashDir = crashDir2;
        const StartupDiagnosis::Report report2 = StartupDiagnosis::analyze(inputs2);
        check(hasTitle(report2, QStringLiteral("缺类异常")),
              "★ DX6 缺类异常 ⇒ 提醒是依赖不齐（只说事实，不点名模组）", textOf(report2));

        const QString crashDir3 = root + QStringLiteral("/crash-java");
        QDir().mkpath(crashDir3);
        writeFileAt(crashDir3 + QStringLiteral("/crash-3.txt"),
                    QByteArrayLiteral("Description: Initializing game\n"
                                      "UnsupportedClassVersionError\n"));
        StartupDiagnosis::Inputs inputs3 = baseInputs();
        inputs3.crashDir = crashDir3;
        const StartupDiagnosis::Report report3 = StartupDiagnosis::analyze(inputs3);
        check(hasTitle(report3, QStringLiteral("Java 版本不匹配")),
              "★ DX6 UnsupportedClassVersionError ⇒ 问题（Java 太旧）", textOf(report3));
    }

    // ---------- DX7. 上一次启动的日志 ----------
    {
        const QString logPath = root + QStringLiteral("/wtfml.log");
        writeFileAt(logPath,
                    QByteArrayLiteral("[12:00:00] [Game Error] Could not find main class\n"
                                      "[12:00:01] Exit code: 1\n"));
        StartupDiagnosis::Inputs inputs = baseInputs();
        inputs.logPath = logPath;
        const StartupDiagnosis::Report report = StartupDiagnosis::analyze(inputs);
        check(hasTitle(report, QStringLiteral("上一次游戏以退出码 1 结束")),
              "★★ DX7 日志末尾的 `Exit code: 1` ⇒ 提醒（这是启动失败最直接的一条证据）",
              textOf(report));
        check(textOf(report).contains(QStringLiteral("Could not find main class")),
              "DX7 证据带上了最后那条 [Game Error]");

        writeFileAt(logPath, QByteArrayLiteral("[12:00:01] Exit code: 0\n"));
        const StartupDiagnosis::Report report2 = StartupDiagnosis::analyze(inputs);
        check(hasTitle(report2, QStringLiteral("正常退出"))
                  && report2.warnings == 0,
              "DX7 退出码 0 ⇒ 「正常」那一条，不是提醒", textOf(report2));

        writeFileAt(logPath, QByteArrayLiteral("java.lang.UnsupportedClassVersionError: oops\n"));
        const StartupDiagnosis::Report report3 = StartupDiagnosis::analyze(inputs);
        check(hasTitle(report3, QStringLiteral("UnsupportedClassVersionError")),
              "★ DX7 日志里有 UnsupportedClassVersionError ⇒ 问题", textOf(report3));

        writeFileAt(logPath, QByteArrayLiteral("java.lang.OutOfMemoryError: Java heap space\n"));
        const StartupDiagnosis::Report report4 = StartupDiagnosis::analyze(inputs);
        check(hasTitle(report4, QStringLiteral("OutOfMemoryError")),
              "★ DX7 日志里有 OutOfMemoryError ⇒ 提醒（哪怕没有崩溃报告）", textOf(report4));
    }

    // ---------- DX8. 排序与计数 ----------
    {
        StartupDiagnosis::Inputs inputs = baseInputs();
        inputs.loaderName.clear();  // 有模组 + 没加载器 ⇒ 问题
        const QString mods = root + QStringLiteral("/mods-order");
        QDir().mkpath(mods);
        writeFileAt(mods + QStringLiteral("/a.jar"), QByteArray("x"));
        inputs.modsDir = mods;
        inputs.xmxMb = 256;  // 偏小 ⇒ 提醒
        const StartupDiagnosis::Report report = StartupDiagnosis::analyze(inputs);
        check(report.problems == 1 && report.warnings == 1,
              "DX8 计数（1 个问题 / 1 条提醒）",
              QStringLiteral("%1 / %2").arg(report.problems).arg(report.warnings));
        check(!report.findings.isEmpty()
                  && report.findings.first().severity == StartupDiagnosis::Severity::Problem,
              "★ DX8 最严重的排最前（首条是「问题」）");
        check(report.headline().contains(QStringLiteral("1 个问题"))
                  && report.headline().contains(QStringLiteral("1 条提醒")),
              "DX8 顶上那一句把两个数都说了", report.headline());
    }
}

