#include "Support.hpp"

int runVersionNameTests(const QString &sandbox) {
    section("I. 版本显示名");

    const QString id = QStringLiteral("1.20.1");
    const QString loaderName = QStringLiteral("Forge");
    const QString loaderVersion = QStringLiteral("47.2.0");

    VersionDisplayParts parts;
    parts.id = id;
    parts.loaderName = loaderName;
    parts.loaderVersion = loaderVersion;

    // I1 显示名的拼法是**固定的一份**（UI 问题 #14 删掉了「版本名格式」那四个开关）：
    // 名称槽 → 实际版本 → 加载器名。加载器版本**不进**显示名 —— 那正是原先四个开关的
    // 出厂默认（自定义名✓ 实际版本✓ 加载器名✓ 加载器版本✗），所以没改过设置的用户
    // 看到的字一个都不变。期望值是**手写**的，不是拿另一份实现算出来的。
    struct Table {
        QString customName;
        QString expected;
    };
    const QList<Table> tables = {
        {QString(), QStringLiteral("1.20.1-Forge")},
        {QStringLiteral("我的整合包"), QStringLiteral("我的整合包-1.20.1-Forge")},
    };

    for (const Table &table : tables) {
        VersionDisplayParts p = parts;
        p.customName = table.customName;
        const QString label = table.customName.isEmpty() ? QStringLiteral("<无自定义名>")
                                                         : table.customName;
        const QString got = formatVersionName(p);
        check(got == table.expected, QString("I1 %1 -> %2").arg(label, table.expected),
              got == table.expected ? QString() : QStringLiteral("实际 ") + got);
        // 畸形 = 空串、或首尾/连续的分隔符。连续 '-' 正是"去重不对称"（Forge 拼成
        // Forge-Forge）会留下的痕迹，所以这条不能省。
        check(!got.isEmpty() && !got.startsWith(QLatin1Char('-')) &&
                  !got.endsWith(QLatin1Char('-')) && !got.contains(QStringLiteral("--")),
              QString("I1 %1 拼出来非空、无多余分隔符").arg(label), got);
        // 加载器版本那一段**不进**显示名（出厂默认就关着它）。
        check(!got.contains(loaderVersion),
              QString("I1 %1 里没有加载器版本那一段").arg(label), got);
    }

    // I2 名字槽退化成实际版本：没重命名过时只显示版本号本身，不会重复一段。
    {
        VersionDisplayParts p;
        p.id = QStringLiteral("1.20.1");
        check(formatVersionName(p) == QStringLiteral("1.20.1"),
              "I2 没有自定义名、没有加载器 -> 就是版本号", formatVersionName(p));
        p.loaderName = QStringLiteral("Forge");
        p.loaderVersion = QStringLiteral("47.2.0");
        check(formatVersionName(p) == QStringLiteral("1.20.1-Forge"),
              "I2 没有自定义名 -> 版本号-加载器名（加载器版本不出现）", formatVersionName(p));

        // 名称槽和实际版本是同一个串时只留一份（去重对每一段都生效）。
        VersionDisplayParts same;
        same.id = QStringLiteral("1.18.2");
        same.realVersion = QStringLiteral("1.18.2");
        same.loaderName = QStringLiteral("Forge");
        check(formatVersionName(same) == QStringLiteral("1.18.2-Forge"),
              "I2 realVersion 与目录名相同时不拼出重复段", formatVersionName(same));

        // 加载器名恰好等于版本号时也不重复：
        VersionDisplayParts dup;
        dup.id = QStringLiteral("Forge");
        dup.loaderName = QStringLiteral("Forge");
        check(formatVersionName(dup) == QStringLiteral("Forge"),
              "I2 加载器名和版本号相同时只留一份", formatVersionName(dup));
    }

    // I3 清洗：元数据文件的意义就是让用户手写，手写的内容一律当不可信输入。
    // 这里把 id 和实际版本留空（只有名称槽有内容），量到的就是"那一段被清洗成什么样"。
    {
        VersionDisplayParts p;

        p.customName = QStringLiteral("a\nb");
        check(formatVersionName(p) == QStringLiteral("a b"),
              "I3 名字里的换行被折成空格", formatVersionName(p));

        p.customName = QStringLiteral("  padded\tname  ");
        check(formatVersionName(p) == QStringLiteral("padded name"),
              "I3 首尾空白去掉、制表符折成空格", formatVersionName(p));

        p.customName = QString(200, QLatin1Char('x'));
        const QString truncated = formatVersionName(p);
        check(truncated.size() == 64, "I3 超长名字被截到 64 字符",
              QString::number(truncated.size()));

        // 截断点**正好落在空格上**：simplified() 只保证截断前没有首尾空白，截断
        // 本身能重新制造一个尾随空格，而那个空格会被拼进显示名里变成 "xxx -1.20.1"。
        // 上面那条 200 个 'x' 的用例永远踩不到这个点，所以必须单独造一条。
        p.customName = QString(63, QLatin1Char('x')) + QLatin1Char(' ') + QString(50, QLatin1Char('y'));
        const QString cutOnSpace = formatVersionName(p);
        check(!cutOnSpace.endsWith(QLatin1Char(' ')) &&
                  cutOnSpace.size() == 63,
              "I3 截断点落在空格上时不留尾随空格",
              QString("[%1] size=%2").arg(cutOnSpace.right(5)).arg(cutOnSpace.size()));

        // 截断点**落在代理对中间**：第 64 格是 emoji 的高位、第 65 格是低位。直接
        // truncate(64) 会留下一个孤立的代理项（画出来是方块）。R16。
        p.customName = QString(63, QLatin1Char('x')) + QChar(0xD83D) + QChar(0xDE00)
                       + QString(10, QLatin1Char('y'));
        const QString cutSurrogate = formatVersionName(p);
        check(cutSurrogate.size() == 63 &&
                  !cutSurrogate.at(cutSurrogate.size() - 1).isHighSurrogate(),
              "★★ R16 截断点落在代理对中间时退回一格（不留孤立的代理项）",
              QStringLiteral("size=%1 last=U+%2")
                  .arg(cutSurrogate.size())
                  .arg(cutSurrogate.isEmpty() ? 0 : cutSurrogate.at(cutSurrogate.size() - 1).unicode(),
                       4, 16, QLatin1Char('0')));

        // 截断后拼上其他段，不能出现 " -" 这种带空格的连接
        p.customName = QString(63, QLatin1Char('x')) + QLatin1Char(' ') + QString(50, QLatin1Char('y'));
        p.id = QStringLiteral("1.20.1");
        check(!formatVersionName(p).contains(QStringLiteral(" -")),
              "I3 多段拼接时没有『空格+连字符』这种畸形连接",
              formatVersionName(p).right(20));

        // id 也要清洗 —— 在线清单和目录名同样不是我们能保证的内容
        VersionDisplayParts q;
        q.id = QStringLiteral("26.2\nsnapshot");
        check(!formatVersionName(q).contains(QLatin1Char('\n')),
              "I3 id 里的换行同样被清掉", formatVersionName(q));
    }

    // I4 displayParts() 读盘。用专门的假版本号，别碰 A~D 节摆在沙盒里的真版本。
    {
        const QString versionId = QStringLiteral("0.0.0-meta-check");
        const QString dir = sandbox + "/versions/" + versionId;
        QDir().mkpath(dir);
        const QString metaPath = dir + "/wtfml.meta";
        VersionManager vm;

        QFile::remove(metaPath);
        const VersionDisplayParts none = vm.displayParts(versionId);
        check(none.id == versionId && none.customName.isEmpty() && none.loaderName.isEmpty() &&
                  none.loaderVersion.isEmpty(),
              "I4 没有 wtfml.meta 时只有 id、其余全空");

        const auto writeMeta = [&metaPath](const QByteArray &content) {
            QFile f(metaPath);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                return false;
            }
            f.write(content);
            f.close();
            return true;
        };

        check(writeMeta(R"({"name":"我的整合包","loaderName":"Forge","loaderVersion":"47.2.0"})"),
              "I4 能写出 wtfml.meta");
        const VersionDisplayParts full = vm.displayParts(versionId);
        check(full.customName == QStringLiteral("我的整合包") &&
                  full.loaderName == QStringLiteral("Forge") &&
                  full.loaderVersion == QStringLiteral("47.2.0"),
              "I4 三个字段都读出来",
              full.customName + "/" + full.loaderName + "/" + full.loaderVersion);
        check(formatVersionName(full) == QStringLiteral("我的整合包-0.0.0-meta-check-Forge"),
              "I4 拼成 自定义名-实际版本-加载器名（加载器版本不出现）",
              formatVersionName(full));

        // realVersion：真实版本必须独立于目录名。上面那条断言同时是**回归断言** ——
        // 没写 realVersion 时参与拼装的仍是 id，与加这个字段之前逐字节一致。
        check(writeMeta(R"({"realVersion":"1.18.2"})"), "I4 能写出 realVersion");
        const VersionDisplayParts withReal = vm.displayParts(versionId);
        check(withReal.realVersion == QStringLiteral("1.18.2"), "I4 realVersion 读得出来",
              withReal.realVersion);
        check(formatVersionName(withReal) == QStringLiteral("1.18.2"),
              "I4 只有 realVersion 时，名称槽退化成它而不是目录名",
              formatVersionName(withReal));
        check(writeMeta(R"({"realVersion":123})"), "I4 能写出类型错的 realVersion");
        check(vm.displayParts(versionId).realVersion.isEmpty(),
              "I4 非字符串的 realVersion 被丢掉");

        // 畸形 JSON：整个文件当没有，但 id 必须还在（它是参数填的，不依赖文件）
        check(writeMeta(R"({"name":"断在这儿)"), "I4 能写出畸形 JSON");
        const VersionDisplayParts broken = vm.displayParts(versionId);
        check(broken.id == versionId && broken.customName.isEmpty(),
              "I4 畸形 JSON 当作没有元数据，id 不受影响");

        // 顶层是数组：同解析成功但 !isObject()，必须挡住
        check(writeMeta(R"(["name","我的整合包"])"), "I4 能写出顶层数组");
        check(vm.displayParts(versionId).customName.isEmpty(), "I4 顶层是数组时不当成元数据");

        // 字段类型不对：只丢该字段，别的字段照读
        check(writeMeta(R"({"name":123,"loaderName":"Forge"})"), "I4 能写出类型错的字段");
        const VersionDisplayParts mixed = vm.displayParts(versionId);
        check(mixed.customName.isEmpty() && mixed.loaderName == QStringLiteral("Forge"),
              "I4 非字符串字段只丢自己，不影响其他字段",
              mixed.customName + "/" + mixed.loaderName);

        // 这一条钉的是"别自作聪明"：`realVersion` 写成 `1.20.1-Forge`（PCL 那种把加载器
        // 一起塞进版本名的写法）、`loaderName` 写成 `NeoForge-Beta`、显示名两头带空格
        // —— 这些都是**用户盘上真实存在的形状**（老启动器写的、或者别人手改的）。任何
        // "看见 -Forge 就切掉""顺手 trim 一下"的猜测都会让界面上显示的名字**不是盘上那个**，
        // 而且这种改法是静默的：少一个字、多一个空格，没人会收到报错。
        check(writeMeta(R"({"name":"  我的整合包  ","realVersion":"1.20.1-Forge",)"
                        R"("loaderName":"NeoForge-Beta","loaderVersion":"21.1.0-beta"})"),
              "I4 能写出带后缀与空白的四个旧字段");
        {
            const VersionDisplayParts legacy = vm.displayParts(versionId);
            check(legacy.realVersion == QStringLiteral("1.20.1-Forge"),
                  "★★ 旧 `realVersion` **原样读**（`1.20.1-Forge` 里的 `-Forge` 不切掉）",
                  legacy.realVersion);
            check(legacy.loaderName == QStringLiteral("NeoForge-Beta"),
                  "★★ 旧 `loaderName` 原样读（`NeoForge-Beta` 不在连字符处断开）",
                  legacy.loaderName);
            check(legacy.loaderVersion == QStringLiteral("21.1.0-beta"),
                  "★★ 旧 `loaderVersion` 原样读（`-beta` 那截留着）",
                  legacy.loaderVersion);
            check(legacy.customName == QStringLiteral("  我的整合包  "),
                  "★★ 旧 `name` 原样读（**两头那几个空格不许 trim**）",
                  QStringLiteral("[%1]").arg(legacy.customName));
            const QString shown = formatVersionName(legacy);
            check(shown.contains(QStringLiteral("1.20.1-Forge")) &&
                      shown.contains(QStringLiteral("NeoForge-Beta")),
                  "★★ 拼出来的那一串里这两段**逐字可见**（读原样、拼也用它原样）",
                  shown);
            // 加载器版本**不进**显示名（出厂默认那一档就关着它，UI 问题 #14 之后固定成这样）。
            check(!shown.contains(QStringLiteral("21.1.0-beta")),
                  "★ 加载器版本不进显示名", shown);
        }

        // 撞名：<id>.json 是既定的版本 JSON 名字，元数据必须是另一个文件。
        // 这条钉的是"wtfml.meta 而不是 wtfml.json"这个决定本身。
        {
            const QString collisionId = QStringLiteral("wtfml");
            const QString collisionDir = sandbox + "/versions/" + collisionId;
            QDir().mkpath(collisionDir);
            QFile jsonFile(collisionDir + "/" + collisionId + ".json");
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                jsonFile.write(R"({"id":"wtfml","javaVersion":{"majorVersion":17}})");
                jsonFile.close();
            }
            QFile metaFile(collisionDir + "/wtfml.meta");
            if (metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                metaFile.write(R"({"name":"撞名测试"})");
                metaFile.close();
            }
            VersionManager vm2;
            check(vm2.getVersionJson(collisionId).value(QStringLiteral("id")).toString() ==
                      collisionId,
                  "I4 撞名场景: 版本 JSON 读到的仍是版本 JSON");
            check(vm2.displayParts(collisionId).customName == QStringLiteral("撞名测试"),
                  "I4 撞名场景: 元数据读到的仍是元数据");

            // 别给下一次运行留脏目录：它没有 <id>.jar 所以不会被 installedVersions()
            // 认出来，但沙盒会一直长下去。
            QDir(collisionDir).removeRecursively();
        }

        QDir(dir).removeRecursively();
    }

    // I5 LauncherSettings：真值来源是 QSettings，所以要先把 QSettings 搬进沙盒。
    // 前面 E 节用真实注册表验的是 GlobalJavaSettings；这一节的键全是新加的，
    // 在用户机器上写它们没有任何意义，还会留下垃圾。
    {
        const QString settingsDir = sandbox + "/wtfml-settings";
        QDir(settingsDir).removeRecursively();
        QDir().mkpath(settingsDir);

        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);

        // org/app 已经在 main() 开头设成 WTFML/WTFML 了。**两个都必须非空**：
        // INI 后端也会因 org 为空而 setStatus(AccessError)（qsettings.cpp:1069-1073），
        // 那样就是一个"写得进去但状态报错"的假沙盒。
        {
            QSettings probe;
            check(probe.status() == QSettings::NoError,
                  "I5 沙盒 QSettings 状态正常（org/app 都设了）",
                  QString::number(probe.status()));
        }

        LauncherSettings settings;
        check(settings.lastLaunchedVersion().isEmpty(), "I5 全新沙盒里没有『上次启动的版本』");

        settings.setLastLaunchedVersion(QStringLiteral("1.20.1"));

        // 换个实例读：验的是真的落了盘，不是留在成员变量里
        LauncherSettings reopened;
        check(reopened.lastLaunchedVersion() == QStringLiteral("1.20.1"),
              "I5 上次启动的版本跨实例读得回来", reopened.lastLaunchedVersion());
        // UI 问题 #14：那四个「版本名格式」键**不再写**（也就不再读）。
        check(!QSettings().contains(QStringLiteral("Version/NameShowCustom")) &&
                  !QSettings().contains(QStringLiteral("Version/NameShowReal")) &&
                  !QSettings().contains(QStringLiteral("Version/NameShowLoaderName")) &&
                  !QSettings().contains(QStringLiteral("Version/NameShowLoaderVersion")),
              "I5 「版本名格式」那四个键不再写回盘上");

        // 「缺失前置提醒」（界面上那个勾）。它有两件事要钉死：
        // ① 全新沙盒里**默认开着**（用户要的那个弹窗默认就弹）；
        // ② 关掉之后**跨实例读得回来** —— 也就是说读的是盘上 `Download/WarnMissingDependencies`
        // 那个键，不是留在某个成员变量里（留在内存里的话换一个实例就"自己又打开了"）。
        check(reopened.warnMissingDependencies(),
              "I5 全新沙盒里「缺失前置提醒」默认是开的（那个弹窗默认就弹）");
        reopened.setWarnMissingDependencies(false);
        {
            LauncherSettings again;
            check(!again.warnMissingDependencies(),
                  "I5 关掉「缺失前置提醒」跨实例读得回来（读的是盘上那个键）");
        }
        reopened.setWarnMissingDependencies(true);  // 写回默认，后面的用例不许看见"关着"

        QSettings::setDefaultFormat(QSettings::NativeFormat);
        QDir(settingsDir).removeRecursively();
    }

    // I6 实际版本（realVersion）：目录名里揉着加载器时，显示名不能靠它
    //
    // 真机背景：用户的目录名是 `1.18.2-Forge_40.3.12`、`1.21.1-NeoForge_21.1.210`、
    // `26.2-Fabric 0.19.5` 这种，加载器和版本早就糊在一起了。拿目录名当"实际版本"，
    // 再往 loaderName 里填 "Forge"，就会拼出 `我的整合包-1.18.2-Forge_40.3.12-Forge`
    // —— 去重只对**完全相同**的串生效，救不了 `Forge` 和 `Forge_40.3.12`。
    // PCL 用 Setup.ini 的 VersionVanillaName 解决同一个问题。I1 那一整张表用的 parts
    // 没有 realVersion，所以它同时是"不写这个字段就零变化"的回归断言。
    {
        VersionDisplayParts p;
        p.id = QStringLiteral("我的目录名");
        p.customName = QStringLiteral("我的整合包");
        p.realVersion = QStringLiteral("1.18.2");
        p.loaderName = QStringLiteral("Forge");

        check(formatVersionName(p) == QStringLiteral("我的整合包-1.18.2-Forge"),
              "I6 自定义名-实际版本-加载器名", formatVersionName(p));
        check(!formatVersionName(p).contains(QStringLiteral("我的目录名")),
              "I6 realVersion 有值时，目录名不出现在显示名里", formatVersionName(p));

        // 目录名恰好等于 realVersion（1.20.1 这种没改过名的实例是常态）：
        // 退化路径和显式路径必须给出同一个结果，不能拼出重复段（见 I2）。
        VersionDisplayParts same;
        same.id = QStringLiteral("1.18.2");
        same.realVersion = QStringLiteral("1.18.2");
        same.loaderName = QStringLiteral("Forge");
        check(formatVersionName(same) == QStringLiteral("1.18.2-Forge"),
              "I6 realVersion 与目录名相同时不拼出重复段",
              formatVersionName(same));

        // 用户手写的字段一律当不可信输入，realVersion 也要过清洗。
        // id / 自定义名留空 ⇒ 输出就是 realVersion 那一段本身。
        VersionDisplayParts dirty;
        dirty.realVersion = QStringLiteral("  1.18.2\n");
        check(formatVersionName(dirty) == QStringLiteral("1.18.2"),
              "I6 realVersion 的首尾空白和换行被清掉", formatVersionName(dirty));
        dirty.realVersion = QString(200, QLatin1Char('v'));
        check(formatVersionName(dirty).size() == 64, "I6 超长 realVersion 被截到 64 字符",
              QString::number(formatVersionName(dirty).size()));
    }

    // I7 launchStatusName()：底栏左下角「启动中...」那句里的实例名。
    //
    // 要求：「启动的时候…文本为版本名（没有就版本号-加载器名称，**不受名字
    // 显示设置影响**）启动中...」。这个函数的签名里**没有 options 参数**就是那句话的骨架
    // （想传开关都没处传），下面再用真值表把两档的具体写法钉死。
    {
        VersionDisplayParts p;
        p.id = QStringLiteral("1.21.1-NeoForge_21.1.210");  // 真机上目录名的样子
        p.realVersion = QStringLiteral("1.21.1");
        p.loaderName = QStringLiteral("NeoForge");
        p.loaderVersion = QStringLiteral("21.1.210");

        // ① 没有自定义名 → "版本号-加载器名"。**用的是 realVersion 不是目录名** ——
        // 拿目录名会拼出 `1.21.1-NeoForge_21.1.210-NeoForge`（版本号和加载器各说两遍）。
        check(launchStatusName(p) == QStringLiteral("1.21.1-NeoForge"),
              "I7 没重命名过 -> 版本号-加载器名", launchStatusName(p));
        check(!launchStatusName(p).contains(QStringLiteral("NeoForge_21.1.210")),
              "I7 不拿目录名当版本号（那会把加载器名也说两遍）", launchStatusName(p));

        // ② 有自定义名 → **只用名字**。这一条是"照 formatVersionName() 凑一个开关组合"
        // 凑不出来的地方：凑出来的会是 `我的整合包-1.21.1-NeoForge`。
        p.customName = QStringLiteral("我的整合包");
        check(launchStatusName(p) == QStringLiteral("我的整合包"),
              "I7 重命名过 -> 只有名字，不缀版本号和加载器", launchStatusName(p));
        check(!launchStatusName(p).contains(QStringLiteral("NeoForge")),
              "I7 有名字时结果里不含加载器名", launchStatusName(p));

        // ③ 「不受名字显示设置影响」：这一条现在是**结构上的**——launchStatusName() 的
        // 签名里连 options 都没有（「版本名格式」那一组设置在 UI 问题 #14 里删掉了），
        // 所以这里只留"有名字时结果里没有任何别的段"这一条实测。
        check(launchStatusName(p) == QStringLiteral("我的整合包") &&
                  !launchStatusName(p).contains(QStringLiteral("1.21.1")) &&
                  !launchStatusName(p).contains(QStringLiteral("NeoForge")),
              "I7 有名字时「启动中」那句只有名字（不带版本号 / 加载器）", launchStatusName(p));

        // ④ 没有自定义名、也没有加载器名 → 就是版本号
        VersionDisplayParts vanilla;
        vanilla.id = QStringLiteral("1.20.1");
        check(launchStatusName(vanilla) == QStringLiteral("1.20.1"),
              "I7 原版实例 -> 就是版本号", launchStatusName(vanilla));

        // ⑤ realVersion 为空时退化成目录名（和 formatVersionName() 同一条兜底：
        // 这个实例在"启动中"和下拉框里必须报同一个版本号）
        VersionDisplayParts noReal;
        noReal.id = QStringLiteral("26.2-Fabric 0.19.5");
        noReal.loaderName = QStringLiteral("Fabric");
        check(launchStatusName(noReal) == QStringLiteral("26.2-Fabric 0.19.5-Fabric"),
              "I7 realVersion 为空时用目录名兜底（和 formatVersionName 同一处退化）",
              launchStatusName(noReal));
        // ⑤b realVersion **非空、但清洗完为空**（只有空白 / 控制字符 —— wtfml.meta 是能手改
        // 的文本文件）⇒ 必须和上面那一档给同一个答案。两边判空的顺序必须一致：
        // formatVersionName() 先清洗再判空，launchStatusName() 也先清洗再判空，否则
        // 列表里显示目录名、底栏"启动中"那句显示加载器名。
        VersionDisplayParts blankReal;
        blankReal.id = QStringLiteral("26.2-Fabric 0.19.5");
        blankReal.realVersion = QStringLiteral("   ");
        blankReal.loaderName = QStringLiteral("Fabric");
        check(launchStatusName(blankReal) == launchStatusName(noReal),
              "I7 realVersion 只有空白时和「空」走同一条兜底（判空要排在清洗**之后**）",
              launchStatusName(blankReal));

        // ⑥ 目录名也是空的（理论上不会）：有什么给什么，别以分隔符打头
        VersionDisplayParts onlyLoader;
        onlyLoader.loaderName = QStringLiteral("Fabric");
        check(launchStatusName(onlyLoader) == QStringLiteral("Fabric"),
              "I7 只有加载器名时不会拼出以 - 打头的串", launchStatusName(onlyLoader));

        // ⑦ 手写的字段照旧当不可信输入清洗（这一串要摆进底栏那一格，那格宽度是钉死的）
        VersionDisplayParts dirty;
        dirty.id = QStringLiteral("1.20.1");
        dirty.customName = QStringLiteral("  我的\n整合包  ");
        check(launchStatusName(dirty) == QStringLiteral("我的 整合包"),
              "I7 自定义名里的换行折成空格、首尾空白去掉", launchStatusName(dirty));
        dirty.customName = QString(200, QLatin1Char('n'));
        check(launchStatusName(dirty).size() == 64, "I7 超长自定义名截到 64 字符",
              QString::number(launchStatusName(dirty).size()));
    }

    section("I8. 当前实例（有实例必有当前实例）");
    {
        const QStringList three{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")};
        // 首次扫描：从没启动、从没选过 → 第一个
        check(resolveCurrentInstance(three, QString(), -1, QString(), QString()) == QStringLiteral("A"),
              "I8 首次扫描：什么都没记着 → 列表第一个");
        // 首次扫描：上次启动的排在上次选中前面
        check(resolveCurrentInstance(three, QString(), -1, QStringLiteral("C"), QStringLiteral("B")) == QStringLiteral("C"),
              "I8 启动时：上次启动的优先");
        check(resolveCurrentInstance(three, QString(), -1, QStringLiteral("已删"), QStringLiteral("B")) == QStringLiteral("B"),
              "I8 启动时：上次启动的已不在 → 上次选中的");
        // 删除：当前的 B（下标 1）被删 → 顺延到同一位置的 C
        const QStringList afterDelete{QStringLiteral("A"), QStringLiteral("C")};
        check(resolveCurrentInstance(afterDelete, QStringLiteral("B"), 1, QStringLiteral("B"), QString()) == QStringLiteral("C"),
              "I8 删除当前实例：顺延到同一位置上的下一个");
        check(resolveCurrentInstance(QStringList{QStringLiteral("A")}, QStringLiteral("C"), 2, QString(), QString()) == QStringLiteral("A"),
              "I8 删除末尾那个：落到新的末尾");
        // 重命名：调用方把 previous 换成新 id 再问 → 不动；没换（刷新那一下）也不会空着
        const QStringList renamed{QStringLiteral("A"), QStringLiteral("B2"), QStringLiteral("C")};
        check(resolveCurrentInstance(renamed, QStringLiteral("B2"), 1, QString(), QString()) == QStringLiteral("B2"),
              "I8 重命名：新 id 还在 → 当前实例就是改名后的那个");
        check(!resolveCurrentInstance(renamed, QStringLiteral("B"), 1, QString(), QString()).isEmpty(),
              "I8 重命名刷新那一下：老 id 不在了也不会落到空");
        // 空列表：唯一能返回空串的情况
        check(resolveCurrentInstance(QStringList(), QStringLiteral("A"), 0, QStringLiteral("A"), QStringLiteral("A")).isEmpty(),
              "I8 空列表：没有当前实例");
        // 启动失败：LaunchController 只在成功时写 lastLaunched，失败后 previous 仍是 B
        check(resolveCurrentInstance(three, QStringLiteral("B"), 1, QStringLiteral("A"), QStringLiteral("A")) == QStringLiteral("B"),
              "I8 启动 B 失败：上次启动仍记着 A，当前实例保持 B");
    }

    return 0;
}
