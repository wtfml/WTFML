#include "Support.hpp"

void runCoordConvertTests() {
    section("ZAB. 坐标换算：8:1 / 区块 / 区域文件 / F3+C 那条命令（工具箱第 3 件）");

    using CoordConvert::Dimension;

    // ---------- ZAB1. 维度 id ----------
    {
        check(CoordConvert::dimensionFromId(QStringLiteral("minecraft:overworld")) == Dimension::Overworld
                  && CoordConvert::dimensionFromId(QStringLiteral("overworld")) == Dimension::Overworld
                  && CoordConvert::dimensionFromId(QStringLiteral("the_nether")) == Dimension::Nether
                  && CoordConvert::dimensionFromId(QStringLiteral("minecraft:the_nether")) == Dimension::Nether
                  && CoordConvert::dimensionFromId(QStringLiteral("nether")) == Dimension::Nether
                  && CoordConvert::dimensionFromId(QStringLiteral("minecraft:the_end")) == Dimension::End,
              "ZAB1 带不带 `minecraft:`、以及 `nether` 这种简写都认（大小写也不计较）");
        check(CoordConvert::dimensionFromId(QStringLiteral("")) == Dimension::Unknown
                  && CoordConvert::dimensionFromId(QStringLiteral("minecraft:the_moon")) == Dimension::Unknown,
              "★ ZAB1 认不出来回 **Unknown**（`minecraft:overworld` 和一个拼错的 id 不能同等对待）");
        check(CoordConvert::dimensionId(Dimension::Overworld) == QStringLiteral("minecraft:overworld")
                  && CoordConvert::dimensionId(Dimension::Nether) == QStringLiteral("minecraft:the_nether")
                  && CoordConvert::dimensionId(Dimension::End) == QStringLiteral("minecraft:the_end"),
              "ZAB1 反过来的那三个 id 就是游戏里写的那三个");
        check(CoordConvert::dimensionText(Dimension::Unknown) == QStringLiteral("未知维度"),
              "ZAB1 `Unknown` 在界面上写「未知维度」");
        check(CoordConvert::convertible(Dimension::Overworld, Dimension::Nether)
                  && CoordConvert::convertible(Dimension::Nether, Dimension::Overworld)
                  && CoordConvert::convertible(Dimension::Overworld, Dimension::Overworld),
              "ZAB1 主世界 ↔ 下界能换算、同维度能「换算」（等于不动）");
        check(!CoordConvert::convertible(Dimension::Overworld, Dimension::End)
                  && !CoordConvert::convertible(Dimension::End, Dimension::Nether),
              "★ ZAB1 末地 ↔ 别处**不在**这个关系里（末地坐标不与主世界成比例）");
        check(CoordConvert::convertible(Dimension::End, Dimension::End)
                  && !CoordConvert::convertible(Dimension::Unknown, Dimension::Overworld),
              "ZAB1 同维度那一档对末地也算（「不用换算」），但**认不出来的维度什么都算不上**");
    }

    // ---------- ZAB2. 向下取整（负数那一侧才是要紧的）----------
    {
        check(CoordConvert::floorDiv(-1, 16) == -1 && CoordConvert::floorDiv(-16, 16) == -1
                  && CoordConvert::floorDiv(-17, 16) == -2 && CoordConvert::floorDiv(-15, 16) == -1,
              "★★ ZAB2 `-1 / 16` 必须**向下**取整成 -1（C++ 的 `/` 会给 0，`>>` 给 -1 —— 两者在负数那侧差一格）",
              QStringLiteral("floorDiv(-1,16)=%1").arg(CoordConvert::floorDiv(-1, 16)));
        check(CoordConvert::floorDiv(15, 16) == 0 && CoordConvert::floorDiv(16, 16) == 1
                  && CoordConvert::floorDiv(31, 16) == 1,
              "ZAB2 正数那一侧和普通除法一样（15 ⇒ 0、16 ⇒ 1）");
        check(CoordConvert::floorToInt(-0.3) == -1 && CoordConvert::floorToInt(-1.0) == -1
                  && CoordConvert::floorToInt(0.9) == 0 && CoordConvert::floorToInt(-16.5) == -17,
              "ZAB2 浮点坐标按**向下**取整（-0.3 ⇒ -1，不是 0）",
              QString::number(CoordConvert::floorToInt(-0.3)));
        check(CoordConvert::blockToChunk(-1) == -1 && CoordConvert::blockToChunk(-16) == -1
                  && CoordConvert::blockToChunk(0) == 0 && CoordConvert::blockToChunk(16) == 1,
              "ZAB2 方块 → 区块：-1 那个方块在区块 -1 里（方块 -16…-1）");
        check(CoordConvert::chunkToRegion(-1) == -1 && CoordConvert::chunkToRegion(31) == 0
                  && CoordConvert::chunkToRegion(32) == 1,
              "ZAB2 区块 → 区域文件（32 个区块一份）");
        check(CoordConvert::regionFileName(-2, 3) == QStringLiteral("r.-2.3.mca"),
              "ZAB2 区域文件那个名字就是 `r.<x>.<z>.mca`（负数带那个减号）",
              CoordConvert::regionFileName(-2, 3));
        check(CoordConvert::blockRangeText(-1) == QStringLiteral("-16…-1")
                  && CoordConvert::blockRangeText(0) == QStringLiteral("0…15"),
              "ZAB2 一个区块盖住哪一片方块：-1 ⇒ `-16…-1`",
              CoordConvert::blockRangeText(-1));
    }

    // ---------- ZAB3. 8:1 换算 ----------
    {
        const CoordConvert::Position overworld{80.5, 63.0, -76.5};
        const CoordConvert::Position nether =
            CoordConvert::convert(Dimension::Overworld, Dimension::Nether, overworld);
        check(qFuzzyCompare(nether.x, 10.0625) && qFuzzyCompare(nether.z, -9.5625)
                  && qFuzzyCompare(nether.y, 63.0),
              "ZAB3 主世界 → 下界：x、z 各除以 8，**y 一个字节都不动**",
              CoordConvert::positionText(nether));
        const CoordConvert::Position back =
            CoordConvert::convert(Dimension::Nether, Dimension::Overworld, nether);
        check(qFuzzyCompare(back.x, overworld.x) && qFuzzyCompare(back.z, overworld.z),
              "ZAB3 下界 → 主世界：×8，和上面那一趟能对上（80.5 / -76.5 回来）",
              CoordConvert::positionText(back));

        const CoordConvert::Position same =
            CoordConvert::convert(Dimension::Nether, Dimension::Nether, nether);
        check(qFuzzyCompare(same.x, nether.x) && qFuzzyCompare(same.z, nether.z),
              "ZAB3 同维度 = 原样回");
        const CoordConvert::Position end =
            CoordConvert::convert(Dimension::Overworld, Dimension::End, overworld);
        check(qFuzzyCompare(end.x, overworld.x) && qFuzzyCompare(end.z, overworld.z),
              "★ ZAB3 末地在任一头 ⇒ **原样回**（`convertible()` 先说是 false，界面不许显示结果）",
              CoordConvert::positionText(end));
    }

    // ---------- ZAB4. F3+C 复制出来的那条命令 ----------
    {
        // 游戏里按 F3+C 往剪贴板里放的就是这一整条（维度 + 位置 + 两个朝向角）。
        const CoordConvert::ParsedTp f3c = CoordConvert::parseTeleportCommand(
            QStringLiteral("/execute in minecraft:overworld run tp @s 80.500000 63.000000 -76.500000 "
                           "0.000000 0.000000"));
        check(f3c.ok && f3c.hasDimension && f3c.dimension == Dimension::Overworld
                  && qFuzzyCompare(f3c.position.x, 80.5) && qFuzzyCompare(f3c.position.y, 63.0)
                  && qFuzzyCompare(f3c.position.z, -76.5),
              "★★ ZAB4 F3+C 那条整命令读得出**位置**和**维度**（这是这个工具的输入）",
              CoordConvert::positionText(f3c.position));
        check(f3c.hasRotation && qFuzzyCompare(f3c.yaw, 0.0) && qFuzzyCompare(f3c.pitch, 0.0),
              "ZAB4 最后那两个数是朝向角（有就给，**不影响位置**）");

        const CoordConvert::ParsedTp withRotation = CoordConvert::parseTeleportCommand(
            QStringLiteral("/tp Steve 1 2 3 90 0"));
        check(withRotation.ok && withRotation.hasRotation && qFuzzyCompare(withRotation.yaw, 90.0)
                  && !withRotation.hasDimension && withRotation.dimension == Dimension::Unknown,
              "ZAB4 `/tp Steve 1 2 3 90 0`：选择器那一格有就跳过；没说维度 ⇒ Unknown（**不是**主世界）");

        const CoordConvert::ParsedTp bare =
            CoordConvert::parseTeleportCommand(QStringLiteral("tp 1 2 3"));
        check(bare.ok && qFuzzyCompare(bare.position.y, 2.0) && !bare.hasRotation,
              "ZAB4 `tp 1 2 3`（连选择器都没有、前面也没 `/`）也认");

        const CoordConvert::ParsedTp nether = CoordConvert::parseTeleportCommand(
            QStringLiteral("/execute in the_nether run tp @s -12.5 40 -300.25"));
        check(nether.ok && nether.dimension == Dimension::Nether
                  && qFuzzyCompare(nether.position.z, -300.25),
              "ZAB4 维度 id 不带 `minecraft:` 前缀也认（游戏两个版本都出现过）",
              CoordConvert::positionText(nether.position));

        const CoordConvert::ParsedTp relative =
            CoordConvert::parseTeleportCommand(QStringLiteral("/tp @s ~ ~1 ~"));
        check(!relative.ok && relative.relative && !relative.error.isEmpty(),
              "★★ ZAB4 `~` / `^` 那种相对坐标：**ok 一定是 false**、标成 relative、并给一句人话",
              relative.error);
        const CoordConvert::ParsedTp caret =
            CoordConvert::parseTeleportCommand(QStringLiteral("/tp @s ^ ^ ^1"));
        check(!caret.ok && caret.relative, "ZAB4 `^` 那套局部坐标同理");

        const CoordConvert::ParsedTp empty = CoordConvert::parseTeleportCommand(QString());
        check(!empty.ok && !empty.error.isEmpty(), "ZAB4 空格子里读不出东西 ⇒ ok=false + 一句人话",
              empty.error);
        const CoordConvert::ParsedTp noTp =
            CoordConvert::parseTeleportCommand(QStringLiteral("/kill @s"));
        check(!noTp.ok && noTp.error.contains(QStringLiteral("tp")),
              "ZAB4 不是传送命令 ⇒ 明说只认 tp（错误那句话里要点到 tp）", noTp.error);
        const CoordConvert::ParsedTp badExecute =
            CoordConvert::parseTeleportCommand(QStringLiteral("/execute run tp @s 1 2 3"));
        check(!badExecute.ok && badExecute.error.contains(QStringLiteral("execute")),
              "ZAB4 `execute` 后面缺 `in` ⇒ 明说只认 `execute in <维度> run` 这一种",
              badExecute.error);
        const CoordConvert::ParsedTp shortCoords =
            CoordConvert::parseTeleportCommand(QStringLiteral("/tp @s 1 2"));
        check(!shortCoords.ok, "ZAB4 三个坐标不全 ⇒ 不算读出来", shortCoords.error);
        const CoordConvert::ParsedTp notNumber =
            CoordConvert::parseTeleportCommand(QStringLiteral("/tp @s 1 abc 3"));
        check(!notNumber.ok && !notNumber.error.isEmpty(),
              "ZAB4 中间那个不是数 ⇒ 报出来（**不当 0**）", notNumber.error);
    }

    // ---------- ZAB5. 拼回去 ----------
    {
        const CoordConvert::Position position{123.45, 64.0, -78.9};
        check(CoordConvert::positionText(position) == QStringLiteral("123.45 64.00 -78.90"),
              "ZAB5 坐标显示两位小数", CoordConvert::positionText(position));
        check(CoordConvert::toTeleportCommand(position, Dimension::Unknown)
                  == QStringLiteral("/tp @s 123.45 64.00 -78.90"),
              "ZAB5 没维度时就是一条朴素 `/tp`",
              CoordConvert::toTeleportCommand(position, Dimension::Unknown));
        const QString withDimension = CoordConvert::toTeleportCommand(position, Dimension::Nether);
        check(withDimension
                  == QStringLiteral("/execute in minecraft:the_nether run tp @s 123.45 64.00 -78.90"),
              "ZAB5 说了维度就带上 `execute in … run`（粘回聊天栏能直接跑）", withDimension);

        const CoordConvert::ParsedTp roundTrip = CoordConvert::parseTeleportCommand(withDimension);
        check(roundTrip.ok && roundTrip.dimension == Dimension::Nether
                  && qFuzzyCompare(roundTrip.position.x, position.x)
                  && qFuzzyCompare(roundTrip.position.z, position.z),
              "★ ZAB5 拼出去的那条能**原样读回来**（两边的写法是同一套）",
              CoordConvert::positionText(roundTrip.position));
    }

    // ---------- ZAB6. 指定目标维度：先换算，再拼命令（#133） ----------
    {
        const CoordConvert::Position overworld{80.5, 63.0, -76.5};
        const QString toNether =
            CoordConvert::convertedTeleportCommand(Dimension::Overworld, Dimension::Nether, overworld);
        check(toNether
                  == QStringLiteral(
                         "/execute in minecraft:the_nether run tp @s 10.06 63.00 -9.56"),
              "ZAB6 主世界 → 下界：命令里的坐标是**除以 8 之后**的，维度也写成下界", toNether);
        const QString toOverworld =
            CoordConvert::convertedTeleportCommand(Dimension::Nether, Dimension::Overworld,
                                                   CoordConvert::Position{-9.5, 70.0, 12.25});
        check(toOverworld
                  == QStringLiteral(
                         "/execute in minecraft:overworld run tp @s -76.00 70.00 98.00"),
              "ZAB6 下界 → 主世界：x、z 各乘 8", toOverworld);
        const QString toSame =
            CoordConvert::convertedTeleportCommand(Dimension::Overworld, Dimension::Overworld,
                                                   overworld);
        check(toSame == QStringLiteral("/execute in minecraft:overworld run tp @s 80.50 63.00 -76.50"),
              "ZAB6 同维度不缩放，但命令里仍写明维度", toSame);
        const QString toEnd = CoordConvert::convertedTeleportCommand(
            Dimension::Overworld, Dimension::End, overworld);
        check(toEnd == QStringLiteral("/execute in minecraft:the_end run tp @s 80.50 63.00 -76.50"),
              "ZAB6 末地不参与 8:1：坐标原样，维度换成末地", toEnd);
    }
}

