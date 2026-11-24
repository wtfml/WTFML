#include "Support.hpp"

void runServerSettingsTests() {
    section("ZV. 服务端设置：那十几张表的表 / 收不收 / 合回文件 / 体检");

    // ---------- ZV0. 表本身 ----------
    {
        const QList<ServerPropertiesForm::Field> &fields = ServerPropertiesForm::commonFields();
        check(fields.size() == 21, "ZV0 常用字段一共 21 项（「那十几个旋钮」+ 资源包两项）",
              QString::number(fields.size()));

        // 键唯一、每项该有的字都有。
        QSet<QString> keys;
        bool dupKey = false;
        bool blankText = false;
        for (const ServerPropertiesForm::Field &field : fields) {
            if (keys.contains(field.key)) {
                dupKey = true;
            }
            keys.insert(field.key);
            if (field.key.isEmpty() || field.label.isEmpty() || field.hint.isEmpty()) {
                blankText = true;
            }
        }
        check(!dupKey, "★★ ZV0 十九个键互不相同（键是身份，重了界面上就摆两行同一个东西）");
        check(!blankText, "★★ ZV0 每一项都有键、行首那个中文名、和那行小字");

        // 只有 `motd` 那一项挂"游戏里长什么样"的预览：挂错一项就是那一行下面
        // 多出一块画着 `§` 码的深色框；一项都不挂就是"格式化预览"又没了。
        QStringList previewKeys;
        for (const ServerPropertiesForm::Field &field : fields) {
            if (field.preview) {
                previewKeys.append(field.key);
            }
        }
        check(previewKeys == QStringList{QStringLiteral("motd")},
              "★★ ZV0 预览只挂在 motd 那一项上（界面按这一格决定挂不挂那块深色框）",
              previewKeys.join(QStringLiteral(",")));
        const ServerPropertiesForm::Field *motd =
            ServerPropertiesForm::fieldFor(QStringLiteral("motd"));
        check(motd != nullptr && motd->preview && motd->kind == ServerPropertiesForm::Kind::Text,
              "ZV0 挂了预览的那一格是个文本框（预览跟着一格一格的文字走）");

        // 表里写的那几个 `fallback` **必须过得了自己的 `accepts()`** —— 这是"按下保存会
        // 不会把文件写坏"的第一道保险。错在这儿的后果是：用户什么都没改、只按了一下保存，
        // 那一项就被写成了 `accepts()` 拒绝的东西（或者干脆写不进去、报一句他看不懂的错）。
        int badFallback = 0;
        for (const ServerPropertiesForm::Field &field : fields) {
            QString normalized;
            QString problem;
            if (!ServerPropertiesForm::accepts(field, field.fallback, &normalized, &problem)) {
                ++badFallback;
                check(false, QStringLiteral("★★ ZV0 缺省值「%1」自己过不了 accepts()").arg(field.key),
                      problem);
            } else if (!field.fallback.isEmpty() && normalized != field.fallback) {
                // 数字那几档会被规范化（`007` → `7`），出现这种说明表里写歪了。
                check(false, QStringLiteral("★★ ZV0 缺省值「%1」收下来变了样").arg(field.key),
                      normalized);
            }
        }
        check(badFallback == 0, "★★ ZV0 十九项缺省值**每一项**都收得住");

        // 每一档自己的形状：数字落在界里、下拉框的缺省就是第一个候选、开关只有两档。
        int badShape = 0;
        for (const ServerPropertiesForm::Field &field : fields) {
            switch (field.kind) {
            case ServerPropertiesForm::Kind::Number: {
                bool ok = false;
                const int value = field.fallback.toInt(&ok);
                if (!ok || value < field.min || value > field.max) {
                    ++badShape;
                }
                break;
            }
            case ServerPropertiesForm::Kind::Switch:
                if (field.fallback != QStringLiteral("true")
                    && field.fallback != QStringLiteral("false")) {
                    ++badShape;
                }
                break;
            case ServerPropertiesForm::Kind::Choice: {
                // 缺省值必须是候选**之一**，不一定是第一个（`difficulty` 的缺省是 `easy`，
                // 它在候选里排第二 —— 游戏自己的顺序是 和平/简单/普通/困难，缺省却落在
                // "简单"上）。真正要钉的是"缺省值不会掉到候选外面去"：那种值落盘之后，
                // 服务端读到的是一档它不认识的难度。
                bool inChoices = false;
                for (const ServerPropertiesForm::Choice &choice : field.choices) {
                    if (choice.token == field.fallback) {
                        inChoices = true;
                    }
                }
                if (field.choices.isEmpty() || !inChoices) {
                    ++badShape;
                }
                break;
            }
            case ServerPropertiesForm::Kind::Text:
                break;
            }
        }
        check(badShape == 0, "★★ ZV0 每一项的值域 / 候选 / 缺省三样互相对得上"
                             "（数字在界里、下拉框的缺省是第一个候选、开关只有 true/false）");

        // 下拉框每一项都得有那两个词（写进文件的那个 + 摆给用户看的那个）。
        int badChoice = 0;
        for (const ServerPropertiesForm::Field &field : fields) {
            for (const ServerPropertiesForm::Choice &choice : field.choices) {
                if (choice.token.isEmpty() || choice.name.isEmpty()) {
                    ++badChoice;
                }
            }
        }
        check(badChoice == 0, "★ ZV0 下拉框每个候选都有「写进文件的那个词」和「显示的那个词」");

        // 次序：日常（8）→ 资源包（2）→ 规则（8）→ 性能（3）。资源包那两项贴着世界目录/种子，
        // 因为建完服紧接着就是「要不要让大家下个包」；规则组和性能组各往后挪了两格。
        const auto keyAt = [&fields](int index) { return fields.value(index).key; };
        check(keyAt(0) == QStringLiteral("motd") && keyAt(7) == QStringLiteral("level-seed"),
              "ZV0 头八项是日常那几项（简介 / 端口 / 人数 / 难度 / 模式 / 极限 / 世界目录 / 种子）",
              keyAt(7));
        check(keyAt(8) == QStringLiteral("resource-pack")
                  && keyAt(9) == QStringLiteral("resource-pack-sha1"),
              "ZV0 第九、十项是资源包（地址 / SHA-1）", keyAt(8));
        check(keyAt(10) == QStringLiteral("online-mode") && keyAt(17) == QStringLiteral("spawn-protection"),
              "ZV0 中间八项是规则那几项（正版验证 / 白名单 / PVP … 出生点保护）", keyAt(10));
        check(keyAt(18) == QStringLiteral("view-distance")
                  && keyAt(20) == QStringLiteral("player-idle-timeout"),
              "ZV0 最后三项是性能（视距 / 模拟距离 / 挂机踢出）", keyAt(18));

        // 只有 `level-name` 是"要当目录名用"的那一项（值里带路径分隔符就等于把世界写到
        // 服务端目录外面去 —— 见 `Field::dirName` 那段）。
        int dirNameCount = 0;
        QString dirNameKey;
        for (const ServerPropertiesForm::Field &field : fields) {
            if (field.dirName) {
                ++dirNameCount;
                dirNameKey = field.key;
            }
        }
        check(dirNameCount == 1 && dirNameKey == QStringLiteral("level-name"),
              "★★ ZV0 十九项里只有 level-name 是目录名（别的项都不按路径规矩管）", dirNameKey);

        // `fieldFor` 认人认键（大小写不敏感），认不出来回 nullptr。
        check(ServerPropertiesForm::fieldFor(QStringLiteral("server-port")) != nullptr
                  && ServerPropertiesForm::fieldFor(QStringLiteral("Server-Port"))
                         == ServerPropertiesForm::fieldFor(QStringLiteral("server-port")),
              "★ ZV0 反查那一项的规格时大小写不敏感（用户手写 `Server-Port` 也认）");
        check(ServerPropertiesForm::fieldFor(QStringLiteral("nope")) == nullptr,
              "ZV0 认不出来的键回 nullptr（不是「随便给一项」）");

        // `knownKeys()` 就是表里那十九个键，次序也一致。
        const QStringList known = ServerPropertiesForm::knownKeys();
        QStringList fromFields;
        for (const ServerPropertiesForm::Field &field : fields) {
            fromFields.append(field.key);
        }
        check(known == fromFields, "★ ZV0 knownKeys() 和表**同一份**（不是另一处抄一遍）",
              known.join(QLatin1Char(',')));
    }

    // ---------- ZV1. 收不收（`accepts()`） ----------
    {
        const ServerPropertiesForm::Field *port =
            ServerPropertiesForm::fieldFor(QStringLiteral("server-port"));
        const ServerPropertiesForm::Field *view =
            ServerPropertiesForm::fieldFor(QStringLiteral("view-distance"));
        const ServerPropertiesForm::Field *pvp =
            ServerPropertiesForm::fieldFor(QStringLiteral("pvp"));
        const ServerPropertiesForm::Field *mode =
            ServerPropertiesForm::fieldFor(QStringLiteral("gamemode"));
        const ServerPropertiesForm::Field *level =
            ServerPropertiesForm::fieldFor(QStringLiteral("level-name"));
        const ServerPropertiesForm::Field *motd =
            ServerPropertiesForm::fieldFor(QStringLiteral("motd"));
        check(port != nullptr && view != nullptr && pvp != nullptr && mode != nullptr
                  && level != nullptr && motd != nullptr,
              "ZV1 前置：要拿的那六项都在表里");

        const auto accept = [](const ServerPropertiesForm::Field &field, const QString &raw,
                               QString *out) {
            QString problem;
            return ServerPropertiesForm::accepts(field, raw, out, &problem);
        };
        const auto saysProblem = [](const ServerPropertiesForm::Field &field,
                                    const QString &raw) {
            QString normalized;
            QString problem;
            ServerPropertiesForm::accepts(field, raw, &normalized, &problem);
            return problem;
        };

        // 数字：只收**严格**的整数（`toInt()` 会把 `1e3` / `10.0` 静默读成 0 —— 那 0 落在
        // 视距那一格上是个合法的烂值，见 `accepts()` 那段）。
        QString out;
        check(accept(*port, QStringLiteral(" 25566 "), &out) && out == QStringLiteral("25566"),
              "★ ZV1 数字两侧的空白裁掉（裁空白是安全的）", out);
        check(accept(*port, QStringLiteral("007"), &out) && out == QStringLiteral("7"),
              "★ ZV1 前导零规范化成十进制", out);
        check(accept(*port, QStringLiteral("+25566"), &out) && out == QStringLiteral("25566"),
              "ZV1 带正号也认", out);
        for (const QString &bad : {QStringLiteral("1e3"), QStringLiteral("10.0"),
                                   QStringLiteral("0x10"), QStringLiteral("abc"),
                                   QStringLiteral("1 0"), QStringLiteral("--1")}) {
            check(!accept(*port, bad, &out),
                  QStringLiteral("★★ ZV1 「%1」这种「看着像数字」的一律不收（不许替他猜）").arg(bad),
                  saysProblem(*port, bad));
        }
        // 空串也收不住（数字那几档）。
        check(!accept(*port, QString(), &out), "★★ ZV1 数字那几格空着不收（空值落盘之后服务端会崩）",
              saysProblem(*port, QString()));
        // 两头钳在游戏自己的界上。
        check(accept(*port, QStringLiteral("1"), &out) && accept(*port, QStringLiteral("65535"), &out)
                  && !accept(*port, QStringLiteral("0"), &out)
                  && !accept(*port, QStringLiteral("65536"), &out),
              "★★ ZV1 端口 1 ~ 65535 两头都收、两头外面都不收");
        check(accept(*view, QStringLiteral("3"), &out) && accept(*view, QStringLiteral("32"), &out)
                  && !accept(*view, QStringLiteral("2"), &out)
                  && !accept(*view, QStringLiteral("33"), &out),
              "★★ ZV1 视距 3 ~ 32（抄的是游戏自己的 clamp，**不是我们觉得合适的界**）");

        // 开关：Minecraft 自己写的是 true/false，但用户手改出来的 `yes` / `1` / `ON` 也得认 ——
        // 读进来认不出来会被当成"开着"，那是个把正版验证悄悄关掉的默认值。
        for (const QString &yes : {QStringLiteral("true"), QStringLiteral("TRUE"),
                                   QStringLiteral("yes"), QStringLiteral("on"),
                                   QStringLiteral("1")}) {
            check(accept(*pvp, yes, &out) && out == QStringLiteral("true"),
                  QStringLiteral("★ ZV1 开关认得「%1」，一律规范成 true").arg(yes), out);
        }
        for (const QString &no : {QStringLiteral("false"), QStringLiteral("FALSE"),
                                  QStringLiteral("no"), QStringLiteral("off"),
                                  QStringLiteral("0")}) {
            check(accept(*pvp, no, &out) && out == QStringLiteral("false"),
                  QStringLiteral("★ ZV1 开关认得「%1」，一律规范成 false").arg(no), out);
        }
        for (const QString &bad : {QStringLiteral("maybe"), QStringLiteral(""), QStringLiteral("2")}) {
            check(!accept(*pvp, bad, &out),
                  QStringLiteral("★★ ZV1 开关那两档之外的（「%1」）不收").arg(bad),
                  saysProblem(*pvp, bad));
        }

        // 下拉框：回**表里拼的那个词**（用户敲 `CREATIVE` 落盘也是小写那个）。
        check(accept(*mode, QStringLiteral("CREATIVE"), &out) && out == QStringLiteral("creative"),
              "★ ZV1 下拉框大小写不敏感，收下来的是表里那个词", out);
        check(!accept(*mode, QStringLiteral("survival-ish"), &out),
              "★ ZV1 候选之外的值不收", saysProblem(*mode, QStringLiteral("survival-ish")));
        check(saysProblem(*mode, QStringLiteral("nope")).contains(QStringLiteral("创造")),
              "★ ZV1 不收那句把候选**摆出来**（不是光说「不对」）",
              saysProblem(*mode, QStringLiteral("nope")));
        // 难度那一档的候选是另一套词（两个下拉框各说各的，别串台）。
        const ServerPropertiesForm::Field *difficulty =
            ServerPropertiesForm::fieldFor(QStringLiteral("difficulty"));
        check(difficulty != nullptr && accept(*difficulty, QStringLiteral("HARD"), &out)
                  && out == QStringLiteral("hard"),
              "★ ZV1 难度那一档独立认那几个词（`HARD` → `hard`）", out);
        check(!accept(*difficulty, QStringLiteral("creative"), &out),
              "★ ZV1 游戏模式那一套词不会在难度那一格上蒙对", saysProblem(*difficulty,
                                                                        QStringLiteral("creative")));

        // 文本：随便填，裁空白。
        check(accept(*motd, QStringLiteral("  hi there  "), &out) && out == QStringLiteral("hi there"),
              "ZV1 文本那几格裁两侧空白", out);
        check(accept(*motd, QString(), &out) && out.isEmpty(),
              "★ ZV1 文本那几格可以是空的（motd 空着就是空着）");

        // 换行：一口下去会把这一项切成两行，落盘之后那份文件再也解不回来 —— 四种控件
        // 一律挡在这儿。
        for (const ServerPropertiesForm::Field *field : {motd, port, pvp, mode}) {
            check(!accept(*field, QStringLiteral("a\nb"), &out),
                  QStringLiteral("★★ ZV1 「%1」收不下换行（落盘会把这一项切成两行）").arg(field->key),
                  saysProblem(*field, QStringLiteral("a\nb")));
            check(!accept(*field, QStringLiteral("a\r\nb"), &out),
                  QStringLiteral("★★ ZV1 「%1」CRLF 也一样不收").arg(field->key));
        }

        // 目录名那一项：空 / `.` / `..` / 带路径字符的全不收。
        check(!accept(*level, QString(), &out) && !accept(*level, QStringLiteral("  "), &out),
              "★★ ZV1 世界目录名不能是空的");
        for (const QString &bad : {QStringLiteral("."), QStringLiteral(".."),
                                   QStringLiteral("../外面的世界"),
                                   QStringLiteral("my/world"), QStringLiteral("C:world"),
                                   QStringLiteral("world?"), QStringLiteral("wor|ld")}) {
            check(!accept(*level, bad, &out),
                  QStringLiteral("★★ ZV1 世界目录名不收「%1」（它会变成 <服务端目录>/<值>）").arg(bad),
                  saysProblem(*level, bad));
        }
        check(accept(*level, QStringLiteral("我的世界"), &out) && out == QStringLiteral("我的世界"),
              "★ ZV1 中文目录名收（Windows 上没问题）", out);
    }

    // ---------- ZV2. `valuesOf` / `isInFile` ----------
    {
        const ServerModel::Properties props = ServerModel::parseProperties(
            QStringLiteral("motd=hi\nserver-port=25566\n"));
        const QList<ServerPropertiesForm::Field> &fields = ServerPropertiesForm::commonFields();
        const ServerPropertiesForm::Values values = ServerPropertiesForm::valuesOf(props, fields);
        check(values.size() == fields.size(),
              "ZV2 valuesOf 一项不少（表有十九项就回十九项）", QString::number(values.size()));
        // 次序跟着**表**走（不是跟着文件里的键序）—— 界面照它从上往下摆。
        bool orderOk = true;
        for (int i = 0; i < values.size(); ++i) {
            if (values.at(i).first != fields.at(i).key) {
                orderOk = false;
            }
        }
        check(orderOk, "★★ ZV2 valuesOf 的次序跟着**表**走（界面照它从上往下摆）");
        check(values.value(1).second == QStringLiteral("25566"),
              "ZV2 文件里写着的那一项读出来是它", values.value(1).second);
        check(values.value(0).second == QStringLiteral("hi"), "ZV2 头一项也是文件里的值");
        // 文件里没有的那一项 ⇒ 缺省值（这是"新装的、还没跑过的服务端"那一页打开时的样子）。
        check(values.value(2).second == QStringLiteral("20"),
              "★★ ZV2 文件里没写的那一项回缺省值（不是空串）", values.value(2).second);
        check(!ServerPropertiesForm::isInFile(props, fields.at(2))
                  && ServerPropertiesForm::isInFile(props, fields.at(1)),
              "★ ZV2 isInFile 分得清「文件里写着」和「用的缺省」（界面拿它说「这一项还没落盘」）");
    }

    // ---------- ZV3. `applyEdits`：只碰点名的键 ----------
    {
        // 一份"什么花样都有"的文件：注释、空行、我们不认识的键、CRLF、键序打乱。
        const QString raw = QStringLiteral("#Minecraft server properties\r\n"
                                           "#Fri Oct 13 12:00:00 CST 2026\r\n"
                                           "motd=A Minecraft Server\r\n"
                                           "server-port=25565\r\n"
                                           "\r\n"
                                           "level-name=world\r\n"
                                           "some-plugin-option=nonsense value\r\n");
        ServerModel::Properties props = ServerModel::parseProperties(raw);
        const ServerPropertiesForm::Values edits = {
            {QStringLiteral("server-port"), QStringLiteral("25566")},
            {QStringLiteral("level-name"), QStringLiteral("world")},   // 值没变 ⇒ 不算改
        };
        const int changed = ServerPropertiesForm::applyEdits(props, edits);
        check(changed == 1, "★ ZV3 只算**真改了**的那一项（值一样的不算）", QString::number(changed));
        const QString expected = QStringLiteral("#Minecraft server properties\r\n"
                                                "#Fri Oct 13 12:00:00 CST 2026\r\n"
                                                "motd=A Minecraft Server\r\n"
                                                "server-port=25566\r\n"
                                                "\r\n"
                                                "level-name=world\r\n"
                                                "some-plugin-option=nonsense value\r\n");
        check(props.toString() == expected,
              "★★ ZV3 改一项之后**逐字**是这样：注释、空行、我们不认识的键、键序、CRLF 一个字节都没动",
              props.toString());
        check(props.value(QStringLiteral("server-port")) == QStringLiteral("25566"),
              "ZV3 被改的那一项真的变了");
        check(props.value(QStringLiteral("some-plugin-option")) == QStringLiteral("nonsense value"),
              "★★ ZV3 我们不认识的键原样留着（整合包和插件往里加的键一个都不动）");

        // 表里没有的键：跳过（这一层只认那十九项）。
        ServerModel::Properties other = ServerModel::parseProperties(QStringLiteral("a=1\n"));
        check(ServerPropertiesForm::applyEdits(other,
                                               {{QStringLiteral("not-in-table"), QStringLiteral("x")}})
                  == 0,
              "★ ZV3 表里没有的键不写（不是「谁传进来就写谁」）");

        // 值不合法：跳过、不写 —— 这是给"调用方忘了先过 accepts()"准备的兜底。
        ServerModel::Properties bad = ServerModel::parseProperties(QStringLiteral("server-port=1\n"));
        check(ServerPropertiesForm::applyEdits(bad,
                                               {{QStringLiteral("server-port"), QStringLiteral("abc")}})
                  == 0
                  && bad.value(QStringLiteral("server-port")) == QStringLiteral("1"),
              "★★ ZV3 不合法的值**跳过**（宁可留着旧的，也不写一个服务端起不来的数进去）");

        // 文件里没有这一项 ⇒ 追加到末尾。
        ServerModel::Properties appended =
            ServerModel::parseProperties(QStringLiteral("server-port=25565\n"));
        check(ServerPropertiesForm::applyEdits(appended,
                                               {{QStringLiteral("view-distance"),
                                                 QStringLiteral("8")}})
                  == 1
                  && appended.toString().endsWith(QStringLiteral("view-distance=8\n")),
              "★ ZV3 文件里没有这一项 ⇒ 追到末尾", appended.toString());

        // 开关 / 下拉框走的是规范化之后那个词。
        ServerModel::Properties norm = ServerModel::parseProperties(QStringLiteral("pvp=1\n"));
        ServerPropertiesForm::applyEdits(norm, {{QStringLiteral("pvp"), QStringLiteral("OFF")}});
        check(norm.value(QStringLiteral("pvp")) == QStringLiteral("false"),
              "★ ZV3 写进去的是规范化之后那个词（`OFF` → `false`）",
              norm.value(QStringLiteral("pvp")));
    }

    // ---------- ZV4. `problemsIn` / `duplicateKeys` / `unknownKeys` / `worldDirName` ----------
    {
        // 只查**文件里真写着的**项：缺省值按定义就是合法的，报出来只会制造噪音。
        const ServerModel::Properties clean =
            ServerModel::parseProperties(QStringLiteral("server-port=25565\nview-distance=10\n"));
        check(ServerPropertiesForm::problemsIn(clean).isEmpty(),
              "★★ ZV4 都对的时候一条都不报（新装的服务端打开这一页不该看见红字）");
        const ServerModel::Properties dirty = ServerModel::parseProperties(
            QStringLiteral("server-port=99999\nview-distance=abc\npvp=true\n"));
        const QList<ServerPropertiesForm::ValueProblem> problems =
            ServerPropertiesForm::problemsIn(dirty);
        check(problems.size() == 2,
              "★★ ZV4 只报**写歪了**的那两项（合法的第三项不报）",
              QString::number(problems.size()));
        check(problems.value(0).key == QStringLiteral("server-port")
                  && problems.value(0).label == QStringLiteral("端口")
                  && problems.value(0).value == QStringLiteral("99999")
                  && !problems.value(0).problem.isEmpty(),
              "★ ZV4 报出来的那一条带着：键、中文名、**文件里现在写着的那个值**、为什么不收",
              problems.value(0).problem);
        check(problems.value(1).key == QStringLiteral("view-distance"),
              "★ ZV4 次序跟着表走（端口在视距前面）", problems.value(1).key);

        // 重复的键：值不一样才报（值一样的是无意义的重复）。
        check(ServerPropertiesForm::duplicateKeys(ServerModel::parseProperties(
                  QStringLiteral("server-port=25565\nserver-port=25566\n")))
                  == QStringList{QStringLiteral("server-port")},
              "★★ ZV4 同一个键两行、值还不一样 ⇒ 报出来（Java 的规矩是后面盖前面，用户多半没意识到）");
        check(ServerPropertiesForm::duplicateKeys(ServerModel::parseProperties(
                  QStringLiteral("server-port=25565\nserver-port=25565\n")))
                  .isEmpty(),
              "★ ZV4 值一样的两行不报（那是无意义的重复）");
        check(ServerPropertiesForm::duplicateKeys(ServerModel::parseProperties(
                  QStringLiteral("Server-Port=1\nserver-port=2\n")))
                  == QStringList{QStringLiteral("server-port")},
              "★ ZV4 大小写写歪的同一个键也算（`Server-Port` 十有八九是想写 `server-port`）");

        // 不认识的键：不是错，只是说一声（这一层一个都不动）。
        check(ServerPropertiesForm::unknownKeys(ServerModel::parseProperties(
                  QStringLiteral("motd=hi\nfoo=1\nbar=2\n")))
                  == QStringList({QStringLiteral("foo"), QStringLiteral("bar")}),
              "★ ZV4 不认识的键按出现次序报出来、同一个键只报一次");
        check(ServerPropertiesForm::unknownKeys(ServerModel::parseProperties(
                  QStringLiteral("MOTD=hi\n")))
                  .isEmpty(),
              "★ ZV4 认键大小写不敏感（`MOTD` 不报成「不认识的」）");

        // 世界目录名：认得出就是它，认不出**回空串**（调用方要靠"空"去拒绝备份 / 恢复）。
        check(ServerPropertiesForm::worldDirName(
                  ServerModel::parseProperties(QStringLiteral("level-name=mc\n")))
                  == QStringLiteral("mc"),
              "ZV4 世界目录名认得出就是它");
        check(ServerPropertiesForm::worldDirName(
                  ServerModel::parseProperties(QStringLiteral("motd=hi\n")))
                  == QStringLiteral("world"),
              "★ ZV4 文件里没写 level-name ⇒ 按游戏自己的缺省算（world）");
        check(ServerPropertiesForm::worldDirName(
                  ServerModel::parseProperties(QStringLiteral("level-name=../跑出去\n")))
                  .isEmpty(),
              "★★ ZV4 值不合法 ⇒ **回空串**（不猜 —— 猜出来的路径可能指着别人的目录）");
    }

    // ---------- ZV5. 体检：报哪几条、按什么次序 ----------
    {
        // 全好 ⇒ 一条都不报，而且有一句"没问题"的话（别摆一张空表）。
        ServerCheck::Facts good;
        good.installed = true;
        good.metaKnown = true;
        good.launchable = true;
        good.eulaAccepted = true;
        good.port = 25565;
        good.portState = ServerPortCheck::State::Free;
        good.worldName = QStringLiteral("world");
        good.worldExists = true;
        good.worldBytes = 12 * 1024 * 1024;
        good.worldBackupCount = 2;
        const QList<ServerCheck::Finding> none = ServerCheck::inspect(good);
        // "全都对"**不等于**一条都不报：世界在盘上那一条是**说明**（"它在哪儿、多大"），
        // 它永远在（只要世界在盘上）。真正一条都没有的情形是"这个服务端我们连世界名叫什么
        // 都说不出来"—— 见下面那一段。这一页怕的是**空着**（用户以为页坏了）。
        check(none.size() == 1 && none.first().severity == ServerCheck::Severity::Info,
              "★★ ZV5 全都对 ⇒ 只剩那条说明（世界在盘上），一条要处理 / 提醒都没有",
              QString::number(none.size()));
        check(!none.isEmpty() && none.first().title.contains(QStringLiteral("world"))
                  && none.first().detail.contains(QStringLiteral("MB")),
              "★ ZV5 那条说明把世界名和**它多大**都写出来", none.isEmpty() ? none.first().detail
                                                                          : QString());
        check(ServerCheck::summaryText(none) == QStringLiteral("1 项说明"),
              "★ ZV5 那一档有点东西时，空的那两档一个字都不出现",
              ServerCheck::summaryText(none));
        // 真的一条都没有时：那句"没问题"（别摆一句「0 项要处理」）。
        ServerCheck::Facts silent;
        silent.installed = true;
        silent.metaKnown = true;
        silent.launchable = true;
        silent.eulaAccepted = true;
        silent.port = 25565;
        silent.portState = ServerPortCheck::State::Free;
        const QList<ServerCheck::Finding> nothing = ServerCheck::inspect(silent);
        check(nothing.isEmpty() && ServerCheck::summaryText(nothing) == ServerCheck::allGoodText()
                  && !ServerCheck::allGoodText().isEmpty(),
              "★ ZV5 一条都没有时那句总括 = 「没问题」那句（不摆「0 项要处理」）",
              ServerCheck::summaryText(nothing));

        // 一路错到底：看**次序**（拦路的在最上面）。
        ServerCheck::Facts worst;
        worst.installed = true;
        worst.metaKnown = true;
        worst.launchable = false;
        worst.launchProblem = QStringLiteral("我们记的那一份里缺了启动要用的版本信息。");
        worst.eulaAccepted = false;
        worst.running = false;
        worst.port = 25565;
        worst.portState = ServerPortCheck::State::InUse;
        worst.worldName = QStringLiteral("world");
        worst.worldExists = true;
        worst.worldBytes = 1024;
        worst.modCount = 3;
        worst.worldBackupCount = 0;
        worst.valueProblems = ServerPropertiesForm::problemsIn(
            ServerModel::parseProperties(QStringLiteral("view-distance=abc\n")));
        worst.duplicateKeys = QStringList{QStringLiteral("motd")};
        worst.unknownKeys = QStringList{QStringLiteral("foo")};
        const QList<ServerCheck::Finding> all = ServerCheck::inspect(worst);
        check(all.size() == 9, "ZV5 这一份最坏的情形报 9 条", QString::number(all.size()));
        // 前五条都是「要处理」，次序：拼不出命令 → EULA → 值不合法 → 端口被占。
        const auto titleAt = [&all](int i) { return all.value(i).title; };
        check(all.value(0).severity == ServerCheck::Severity::Problem
                  && all.value(0).action == ServerCheck::Action::Reinstall,
              "★★ ZV5 头一条是「拼不出启动命令」（拦着它起不来），出路是重装", titleAt(0));
        check(all.value(1).action == ServerCheck::Action::AcceptEula
                  && !all.value(1).title.isEmpty(),
              "★★ ZV5 第二条是 EULA（没人替你同意），出路是那道闸", titleAt(1));
        check(all.value(2).action == ServerCheck::Action::FixValues,
              "★★ ZV5 第三条是「某一项的值不对」，出路是改回缺省值", titleAt(2));
        check(all.value(3).action == ServerCheck::Action::OpenSettings,
              "★★ ZV5 第四条是「端口被占」（在跑时不报 —— 见下面那一条），出路是去改端口", titleAt(3));
        check(!all.value(3).title.isEmpty() && all.value(3).title.contains(QStringLiteral("25565")),
              "★ ZV5 端口那一条把**端口号**写出来（不然用户不知道去关谁）", titleAt(3));
        // 然后是提醒：重复的键、一份备份都没有。
        check(all.value(4).severity == ServerCheck::Severity::Warning
                  && all.value(4).title.contains(QStringLiteral("motd")),
              "★ ZV5 第五条是「同一个键写了两遍」（提醒），并且点名是哪个键", titleAt(4));
        check(all.value(5).severity == ServerCheck::Severity::Warning
                  && all.value(5).action == ServerCheck::Action::OpenBackups,
              "★ ZV5 第六条是「这个世界还没有备份」，出路是去备份那一页", titleAt(5));
        // 最后是说明：世界在盘上、装了模组、有不认识的键。
        check(all.value(6).severity == ServerCheck::Severity::Info,
              "ZV5 第七条是「世界在盘上」（说明）", titleAt(6));
        check(all.value(7).severity == ServerCheck::Severity::Info
                  && all.value(7).title.contains(QStringLiteral("3")),
              "★ ZV5 第八条是「装了几个模组」，把那个数写出来", titleAt(7));
        check(all.value(8).severity == ServerCheck::Severity::Info
                  && all.value(8).detail.contains(QStringLiteral("foo")),
              "★ ZV5 第九条是「有不认识的键」，并且**举个例**（大字里只有个数，名字在小字里）",
              titleAt(8));

        // `summaryText()` 的三档拼法（没有的那几档不出现）。
        check(ServerCheck::summaryText(all) == QStringLiteral("4 项要处理 / 2 项提醒 / 3 项说明"),
              "★★ ZV5 总括那句是三档拼出来的、次序固定", ServerCheck::summaryText(all));
        ServerCheck::Facts onlyInfo = good;
        onlyInfo.modCount = 1;  // 加上"世界在盘上"那一条 ⇒ 两条说明
        const QList<ServerCheck::Finding> twoInfo = ServerCheck::inspect(onlyInfo);
        check(twoInfo.size() == 2 && ServerCheck::summaryText(twoInfo) == QStringLiteral("2 项说明"),
              "★ ZV5 只有说明那一档时，前面两档一个字都不出现",
              ServerCheck::summaryText(twoInfo));

        // 在跑的时候端口那一条**不报**：它自己就占着那个端口，报"被人占了"是一句吓人的废话。
        ServerCheck::Facts running = worst;
        running.running = true;
        running.launchable = true;
        QStringList runningTitles;
        for (const ServerCheck::Finding &finding : ServerCheck::inspect(running)) {
            runningTitles.append(finding.title);
        }
        bool mentionsPort = false;
        for (const QString &title : runningTitles) {
            if (title.contains(QStringLiteral("端口"))) {
                mentionsPort = true;
            }
        }
        check(!mentionsPort, "★★ ZV5 服务端正在跑时**不报**「端口被占」（那正是它自己占着的）");

        // 没装好那一条：不再往下报 EULA / 世界 / 备份那几条 —— 那些本来就没到那一步。
        ServerCheck::Facts fresh;
        fresh.installed = false;
        fresh.installedNote = QStringLiteral("入口文件不在这个目录里。");
        const QList<ServerCheck::Finding> one = ServerCheck::inspect(fresh);
        check(one.size() == 1 && one.first().action == ServerCheck::Action::Reinstall,
              "★★ ZV5 没装好 ⇒ **只报一条**（「还没装好」），并指向「修复」",
              QString::number(one.size()));
        check(one.first().detail.contains(QStringLiteral("入口文件不在这个目录里")),
              "★ ZV5 那一条把 `Entry::note` 原话带上（「缺什么」是那一层说的）", one.first().detail);

        // 世界还没生成（新装的、没跑过）：说一声就好，不是错。
        ServerCheck::Facts virgin = good;
        virgin.worldExists = false;
        virgin.worldBytes = 0;
        virgin.worldBackupCount = 0;
        const QList<ServerCheck::Finding> first = ServerCheck::inspect(virgin);
        check(first.size() == 1 && first.first().severity == ServerCheck::Severity::Info
                  && first.first().title.contains(QStringLiteral("尚未生成")),
              "★★ ZV5 第一次开服之前：只说一声「世界还没生成」（而且**不**报「还没备份」—— 没东西可备）",
              QString::number(first.size()));

        // 三档的名字和六个记号（★ `ui/` 看不见那个枚举，只认记号）。
        check(ServerCheck::severityName(ServerCheck::Severity::Problem) == QStringLiteral("要处理")
                  && ServerCheck::severityName(ServerCheck::Severity::Warning) == QStringLiteral("提醒")
                  && ServerCheck::severityName(ServerCheck::Severity::Info) == QStringLiteral("说明"),
              "★ ZV5 三档那三个词的唯一出处");
        check(ServerCheck::actionToken(ServerCheck::Action::None).isEmpty(),
              "★★ ZV5 `None` 的记号是**空串**（界面上按它决定「这一条不摆按钮」）");
        check(ServerCheck::actionToken(ServerCheck::Action::Reinstall) == QStringLiteral("reinstall")
                  && ServerCheck::actionToken(ServerCheck::Action::AcceptEula)
                         == QStringLiteral("accept-eula")
                  && ServerCheck::actionToken(ServerCheck::Action::FixValues)
                         == QStringLiteral("fix-values")
                  && ServerCheck::actionToken(ServerCheck::Action::OpenSettings)
                         == QStringLiteral("open-settings")
                  && ServerCheck::actionToken(ServerCheck::Action::OpenBackups)
                         == QStringLiteral("open-backups"),
              "★★ ZV5 五个出路的记号（`MainWindow::runServerCheckAction()` 按它分岔 —— "
              "记号改了那一边会静默什么都不做）");
    }

    // ---------- ZV6. 端口：真占一次，再放开 ----------
    {
        // 这一格要**真听一次**才知道（Windows 上没有不需要权限的"谁占着这个端口"接口 ——
        // 见 `ServerPortCheck.hpp` 的文件头）。全程 127.0.0.1，不联网。
        check(ServerPortCheck::stateOf(0) == ServerPortCheck::State::Unknown
                  && ServerPortCheck::stateOf(70000) == ServerPortCheck::State::Unknown
                  && ServerPortCheck::stateOf(-1) == ServerPortCheck::State::Unknown,
              "★★ ZV6 端口不在 1~65535 里 ⇒ `Unknown`（「没查」），**不当成「被占」**");

        QTcpServer holder;
        const bool bound = holder.listen(QHostAddress::Any, 0);
        check(bound, "ZV6 前置：本机能听上一个空闲端口", holder.errorString());
        const int taken = bound ? holder.serverPort() : 0;
        if (bound) {
            check(ServerPortCheck::stateOf(taken) == ServerPortCheck::State::InUse,
                  "★★ ZV6 别人占着这个端口 ⇒ 报 `InUse`", QString::number(taken));
            holder.close();
            check(ServerPortCheck::stateOf(taken) == ServerPortCheck::State::Free,
                  "★★ ZV6 放开之后同一个端口 ⇒ 报 `Free`（一次 listen 之后立刻放掉）",
                  QString::number(taken));
        }
        // 三档那三句话的唯一出处。
        check(ServerPortCheck::stateName(ServerPortCheck::State::Free) == QStringLiteral("没被占用")
                  && ServerPortCheck::stateName(ServerPortCheck::State::InUse)
                         == QStringLiteral("被占用了")
                  && ServerPortCheck::stateName(ServerPortCheck::State::Unknown)
                         == QStringLiteral("没能查出来"),
              "★ ZV6 三档那三句话的唯一出处");
    }
}

