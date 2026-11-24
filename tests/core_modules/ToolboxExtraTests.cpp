#include "Support.hpp"

#include "tools/BannerDesign.hpp"
#include "tools/GameVersion.hpp"

void runToolboxExtraTests() {
    section("ZAI. 工具箱：信标计算 / 酿造助手");

    // ---------- ZAI0. 信标：层、累计、范围 ----------
    {
        check(Beacon::minLevel() == 1 && Beacon::maxLevel() == 4,
              "ZAI0 信标只有 1–4 级");
        check(Beacon::layerBlocks(1) == 9 && Beacon::layerBlocks(2) == 25
                  && Beacon::layerBlocks(3) == 49 && Beacon::layerBlocks(4) == 81,
              "★★ ZAI0 每一层是 (2n+1)²（3×3 / 5×5 / 7×7 / 9×9）",
              QStringLiteral("%1 / %2 / %3 / %4")
                  .arg(Beacon::layerBlocks(1))
                  .arg(Beacon::layerBlocks(2))
                  .arg(Beacon::layerBlocks(3))
                  .arg(Beacon::layerBlocks(4)));
        check(Beacon::totalBlocks(1) == 9 && Beacon::totalBlocks(2) == 34
                  && Beacon::totalBlocks(3) == 83 && Beacon::totalBlocks(4) == 164,
              "★★ ZAI0 从零搭起的累计块数是 9 / 34 / 83 / 164（每层往上叠，不是只剩最上面那层）",
              QStringLiteral("%1 / %2 / %3 / %4")
                  .arg(Beacon::totalBlocks(1))
                  .arg(Beacon::totalBlocks(2))
                  .arg(Beacon::totalBlocks(3))
                  .arg(Beacon::totalBlocks(4)));
        check(Beacon::blocksToAdd(0, 4) == 164 && Beacon::blocksToAdd(3, 4) == 81
                  && Beacon::blocksToAdd(2, 4) == 81 + 49 && Beacon::blocksToAdd(4, 4) == 0
                  && Beacon::blocksToAdd(3, 2) == 0,
              "★★ ZAI0 「已有 0 / 2 / 3 / 4 级」到 4 级还要补多少块（顺序反了回 0）",
              QStringLiteral("0→4=%1 3→4=%2 2→4=%3")
                  .arg(Beacon::blocksToAdd(0, 4))
                  .arg(Beacon::blocksToAdd(3, 4))
                  .arg(Beacon::blocksToAdd(2, 4)));
        check(Beacon::effectRange(1) == 20 && Beacon::effectRange(2) == 30
                  && Beacon::effectRange(3) == 40 && Beacon::effectRange(4) == 50,
              "★ ZAI0 效果范围 = 10n+10（20 / 30 / 40 / 50 格）");

        check(Beacon::layerBlocks(0) == 0 && Beacon::layerBlocks(5) == 0
                  && Beacon::totalBlocks(9) == 0 && Beacon::effectRange(-1) == 0,
              "ZAI0 越界等级回 0（不编一个整数出来）");
        check(!Beacon::isValidLevel(0) && Beacon::isValidLevel(1) && Beacon::isValidLevel(4)
                  && !Beacon::isValidLevel(5) && Beacon::isValidFromLevel(0)
                  && Beacon::isValidFromLevel(3) && !Beacon::isValidFromLevel(4),
              "ZAI0 「目标等级」是 1–4，「已有等级」是 0–3（0 = 还没搭）");
    }

    // ---------- ZAI1. 信标：效果按级解锁 ----------
    {
        check(Beacon::effectNames().size() == 6,
              "ZAI1 一共六种效果", QString::number(Beacon::effectNames().size()));
        const auto same = [](const QStringList &a, const QStringList &b) { return a == b; };
        check(same(Beacon::effectsForLevel(1),
                   {QStringLiteral("迅捷"), QStringLiteral("急迫")}),
              "★ ZAI1 1 级：速度 / 急迫",
              Beacon::effectsForLevel(1).join(QStringLiteral("、")));
        check(same(Beacon::effectsForLevel(2),
                   {QStringLiteral("迅捷"), QStringLiteral("急迫"),
                    QStringLiteral("抗性提升"), QStringLiteral("跳跃提升")}),
              "★ ZAI1 2 级再加抗性提升 / 跳跃提升",
              Beacon::effectsForLevel(2).join(QStringLiteral("、")));
        check(same(Beacon::effectsForLevel(3),
                   {QStringLiteral("迅捷"), QStringLiteral("急迫"),
                    QStringLiteral("抗性提升"), QStringLiteral("跳跃提升"),
                    QStringLiteral("力量")}),
              "★ ZAI1 3 级再加力量", Beacon::effectsForLevel(3).join(QStringLiteral("、")));
        check(same(Beacon::effectsForLevel(4),
                   {QStringLiteral("迅捷"), QStringLiteral("急迫"),
                    QStringLiteral("抗性提升"), QStringLiteral("跳跃提升"),
                    QStringLiteral("力量"), QStringLiteral("生命恢复")}),
              "★ ZAI1 4 级再加再生（六种全开）",
              Beacon::effectsForLevel(4).join(QStringLiteral("、")));
        check(Beacon::effectsForLevel(0).isEmpty() && Beacon::effectsForLevel(5).isEmpty(),
              "ZAI1 越界等级回空表");

        const QString detail = Beacon::detailText(2, 4);
        check(detail.contains(QStringLiteral("9 × 9 = 81"))
                  && detail.contains(QStringLiteral("还需 130 块"))
                  && detail.contains(QStringLiteral("50 格"))
                  && detail.contains(QStringLiteral("力量")),
              "★★ ZAI1 明细里写清最上面那层、还差多少块、范围与效果",
              detail.left(80));
    }

    // ---------- ZAI1b. 信标：矿物折算与多颗共用底座----------
    {
        check(Beacon::materialNames().size() == 5
                  && Beacon::materialNames().first() == QStringLiteral("铁块")
                  && Beacon::materialNames().last() == QStringLiteral("下界合金块"),
              "★ ZAI1b 五种底座方块，次序 = 下拉框次序",
              Beacon::materialNames().join(QStringLiteral("、")));
        check(Beacon::isValidMaterial(0) && Beacon::isValidMaterial(4)
                  && !Beacon::isValidMaterial(-1) && !Beacon::isValidMaterial(5),
              "ZAI1b 材质下标 0–4");
        check(Beacon::blockName(2) == QStringLiteral("钻石块")
                  && Beacon::unitName(2) == QStringLiteral("钻石")
                  && Beacon::unitName(4) == QStringLiteral("下界合金锭"),
              "ZAI1b 方块名与锭/宝石名对得上");

        check(Beacon::unitsPerBlock() == 9 && Beacon::unitsForBlocks(0, 164) == 1476,
              "★★ ZAI1b 164 块铁块 = 1476 个铁锭（每块 9 个）",
              QString::number(Beacon::unitsForBlocks(0, 164)));
        check(Beacon::unitsForBlocks(-1, 164) == 0 && Beacon::unitsForBlocks(0, 0) == 0,
              "ZAI1b 材质越界 / 块数为 0 ⇒ 0（不编数）");

        const QString material = Beacon::materialText(1, 164);
        check(material.contains(QStringLiteral("164 块金块"))
                  && material.contains(QStringLiteral("1476 个金锭")),
              "★★ ZAI1b 折算那一句把方块与锭都写出来", material);
        check(Beacon::materialText(9, 164).contains(QStringLiteral("无效的信标底座方块")),
              "ZAI1b 材质下标越界时说「认不出」，不装作知道");

        check(Beacon::maxBeaconCount() == 9,
              "★ ZAI1b 这个计算器最多算 9 颗（紧凑摆法只在 3 × 3 的脚印里搜）");
        check(Beacon::sharedBaseBlocks(4, 1) == 164,
              "★ ZAI1b 1 颗 ⇒ 就是一座 4 级金字塔的 164 块");
        // `BeaconBlockEntity.updateBase()` 是一颗一颗看的：每颗都要自己脚下一整套，
        // 挨着摆时这些方格重叠 → 只算**并集**（不是"一人一座 164"）。
        check(Beacon::sharedBaseBlocks(4, 2) == 188 && Beacon::sharedBaseBlocks(4, 3) == 212
                  && Beacon::sharedBaseBlocks(4, 4) == 216 && Beacon::sharedBaseBlocks(4, 9) == 276,
              "★★ ZAI1b 挨着摆共用的是并集：4 级 2 / 3 / 4 / 9 颗 = 188 / 212 / 216 / 276 块"
              "（各盖一座是 328 / 492 / 656 / 1476）",
              QStringLiteral("%1 / %2 / %3 / %4")
                  .arg(Beacon::sharedBaseBlocks(4, 2))
                  .arg(Beacon::sharedBaseBlocks(4, 3))
                  .arg(Beacon::sharedBaseBlocks(4, 4))
                  .arg(Beacon::sharedBaseBlocks(4, 9)));
        check(Beacon::sharedBaseBlocks(1, 2) == 12 && Beacon::sharedBaseBlocks(1, 4) == 16
                  && Beacon::sharedBaseBlocks(2, 9) == 74,
              "★★ ZAI1b 并集那笔账一层层算（1 级 2 颗 = 4 × 3 = 12；4 颗 2 × 2 = 16）",
              QStringLiteral("%1 / %2 / %3")
                  .arg(Beacon::sharedBaseBlocks(1, 2))
                  .arg(Beacon::sharedBaseBlocks(1, 4))
                  .arg(Beacon::sharedBaseBlocks(2, 9)));
        check(Beacon::sharedBaseBlocks(4, 0) == 0 && Beacon::sharedBaseBlocks(4, 10) == 0
                  && Beacon::sharedBaseBlocks(5, 1) == 0,
              "ZAI1b 颗数 / 等级越界 ⇒ 0");
        const QString shared = Beacon::sharedBaseText(4, 3);
        check(shared.contains(QStringLiteral("3 颗"))
                  && shared.contains(QStringLiteral("212 块"))
                  && shared.contains(QStringLiteral("9 块黑曜石"))
                  && shared.contains(QStringLiteral("15 块玻璃")),
              "★★ ZAI1b 共用底座那句把底座与每颗信标自己的材料分开说", shared);
        // 合成表 `GGG / GSG / OOO`：玻璃 5 块，不是 3。
        check(Beacon::sharedBaseText(4, 1).contains(QStringLiteral("5 块玻璃"))
                  && !Beacon::sharedBaseText(4, 1).contains(QStringLiteral("3 块玻璃")),
              "★★ ZAI1b 一颗信标要 5 块玻璃（合成表那一圈），不是 3 块",
              Beacon::sharedBaseText(4, 1));
        check(Beacon::sharedBaseText(4, 10).contains(QStringLiteral("超出范围")),
              "ZAI1b 超过 9 颗时说清楚");
    }

    // ---------- ZAI2. 酿造：表与形态 ----------
    {
        check(Brewing::formCount() == 3
                  && Brewing::formName(Brewing::Form::Normal) == QStringLiteral("普通")
                  && Brewing::formName(Brewing::Form::Splash) == QStringLiteral("喷溅型")
                  && Brewing::formName(Brewing::Form::Lingering) == QStringLiteral("滞留型"),
              "ZAI2 形态三档（下标 = 界面下拉框的下标）");
        check(Brewing::potionCount() > 0
                  && Brewing::potionNames().size() == Brewing::potionCount(),
              "ZAI2 药水表与名字表同长",
              QString::number(Brewing::potionCount()));
        check(Brewing::potionAt(-1).name.isEmpty()
                  && Brewing::potionAt(Brewing::potionCount()).name.isEmpty(),
              "★ ZAI2 越界下标回空的那一项（不猜一种药水出来）");
        const Brewing::PotionInfo swift = Brewing::potionAt(0);
        check(swift.name == QStringLiteral("迅捷药水") && swift.ingredient == QStringLiteral("糖")
                  && swift.parent == -1 && !swift.fromWater && swift.extendable
                  && swift.upgradable,
              "ZAI2 第一种是迅捷（糖，从粗制的药水来，可延时也可 II 级）");
    }

    // ---------- ZAI3. 酿造路线 ----------
    {
        const Brewing::Plan simple = Brewing::plan(0, false, false, Brewing::Form::Normal);
        check(simple.ok && simple.steps.size() == 2
                  && simple.steps.at(0) == QStringLiteral("水瓶 + 下界疣 → 粗制的药水")
                  && simple.steps.at(1) == QStringLiteral("粗制的药水 + 糖 → 迅捷药水")
                  && simple.summary.contains(QStringLiteral("迅捷药水"))
                  && simple.effect == QStringLiteral("迅捷 I · 3:00"),
              "★★ ZAI3 迅捷：从水瓶起步，水瓶 → 粗制的药水 → 加糖（一共两步），不带做瓶装水那一步",
              simple.detailText().left(140));
        check(!simple.detailText().contains(QStringLiteral("玻璃瓶"))
                  && !simple.detailText().contains(QStringLiteral("装水")),
              "★★ ZAI3 明细里不再出现玻璃瓶 / 装水（默认输入就是水瓶）",
              simple.detailText());
        check(simple.materials.contains(QStringLiteral("水瓶 × 1"))
                  && simple.materials.contains(QStringLiteral("下界疣 × 1"))
                  && simple.materials.contains(QStringLiteral("糖 × 1")),
              "★★ ZAI3 材料清单是名称 + 数量（水瓶 / 下界疣 / 糖各 1）",
              simple.materials.join(QStringLiteral("、")));
        check(simple.detailText().contains(QStringLiteral("步骤（共 2 步）："))
                  && simple.detailText().contains(QStringLiteral("效果：迅捷 I · 3:00"))
                  && simple.detailText().contains(QStringLiteral("材料：水瓶 × 1、下界疣 × 1")),
              "★★ ZAI3 明细按「产出 / 效果 / 步骤 / 材料」四段摆", simple.detailText());

        const Brewing::Plan extended = Brewing::plan(0, true, false, Brewing::Form::Normal);
        check(extended.ok
                  && extended.steps.last()
                         == QStringLiteral("迅捷药水 + 红石粉 → 迅捷药水（延长）")
                  && extended.summary.contains(QStringLiteral("（延长）"))
                  && extended.effect == QStringLiteral("迅捷 I · 8:00"),
              "★ ZAI3 延时：最后一步加红石粉（配方写法），名字带「（延长）」，时长 8:00",
              extended.summary + QStringLiteral(" / ") + extended.effect);

        const Brewing::Plan both = Brewing::plan(0, true, true, Brewing::Form::Normal);
        check(!both.ok && both.error.contains(QStringLiteral("两种效果互斥")),
              "★★ ZAI3 红石 + 荧石同时要 ⇒ 直接报错（游戏里后加的会冲掉先加的）",
              both.error);

        const Brewing::Plan noExtend =
            Brewing::plan(2, true, false, Brewing::Form::Normal);  // 治疗（瞬间药水）
        check(!noExtend.ok && noExtend.error.contains(QStringLiteral("红石")),
              "★ ZAI3 治疗没有延长版本 ⇒ 说清楚、不硬塞一步", noExtend.error);
        const Brewing::Plan noLevel2 =
            Brewing::plan(7, false, true, Brewing::Form::Normal);  // 抗火
        check(!noLevel2.ok && noLevel2.error.contains(QStringLiteral("荧石")),
              "★ ZAI3 抗火没有 II 级 ⇒ 说清楚", noLevel2.error);

        const Brewing::Plan splash = Brewing::plan(0, false, false, Brewing::Form::Splash);
        check(splash.ok
                  && splash.steps.last()
                         == QStringLiteral("迅捷药水 + 火药 → 喷溅型迅捷药水")
                  && splash.summary.contains(QStringLiteral("喷溅型"))
                  && splash.effect == simple.effect,
              "★ ZAI3 喷溅型：最后加火药；效果行与普通同格式、时长不变",
              splash.summary + QStringLiteral(" / ") + splash.effect);

        const Brewing::Plan lingering = Brewing::plan(0, false, false, Brewing::Form::Lingering);
        check(lingering.ok && lingering.steps.size() >= 2
                  && lingering.steps.at(lingering.steps.size() - 2).contains(QStringLiteral("火药"))
                  && lingering.steps.last().contains(QStringLiteral("龙息"))
                  && lingering.steps.last().endsWith(QStringLiteral("滞留型迅捷药水"))
                  && lingering.summary.contains(QStringLiteral("滞留型"))
                  && lingering.effect == QStringLiteral("迅捷 I · 0:45"),
              "★★ ZAI3 滞留型：先火药（喷溅）再龙息 —— 龙息不能直接加在普通药水上",
              lingering.steps.join(QStringLiteral(" / ")));
    }

    // ---------- ZAI4. 链式前驱与水瓶直出 ----------
    {
        // 隐身 = 夜视 + 发酵蜘蛛眼；夜视 = 粗制的药水 + 金胡萝卜。
        int invisibility = -1;
        int weakness = -1;
        for (int i = 0; i < Brewing::potionCount(); ++i) {
            if (Brewing::potionAt(i).name == QStringLiteral("隐身药水")) invisibility = i;
            if (Brewing::potionAt(i).name == QStringLiteral("虚弱药水")) weakness = i;
        }
        check(invisibility >= 0 && weakness >= 0, "ZAI4 表里有隐身和虚弱");
        if (invisibility >= 0) {
            const Brewing::Plan plan = Brewing::plan(invisibility, false, false,
                                                     Brewing::Form::Normal);
            check(plan.ok && plan.steps.size() == 3
                      && plan.steps.at(0).contains(QStringLiteral("下界疣"))
                      && plan.steps.at(1).contains(QStringLiteral("金胡萝卜"))
                      && plan.steps.at(2).contains(QStringLiteral("发酵蜘蛛眼")),
                  "★★ ZAI4 隐身要把前驱那条链补全：粗制的药水 → 夜视 → 隐身",
                  plan.steps.join(QStringLiteral(" / ")));
        }
        if (weakness >= 0) {
            const Brewing::Plan plan =
                Brewing::plan(weakness, false, false, Brewing::Form::Normal);
            check(plan.ok && plan.steps.size() == 1
                      && plan.steps.at(0) == QStringLiteral("水瓶 + 发酵蜘蛛眼 → 虚弱药水")
                      && !plan.steps.join(QStringLiteral(" ")).contains(QStringLiteral("下界疣"))
                      && plan.directFromWater,
                  "★★ ZAI4 虚弱从水瓶直接做（不需要下界疣那一步）",
                  plan.steps.join(QStringLiteral(" / ")));
            check(plan.detailText().contains(QStringLiteral("特殊路径"))
                      && plan.effect == QStringLiteral("虚弱 I · 1:30"),
                  "★★ ZAI4 水瓶直出的路线单独标出来，效果行照常写等级与时长",
                  plan.detailText().left(160));
            check(plan.materials.contains(QStringLiteral("水瓶 × 1"))
                      && plan.materials.contains(QStringLiteral("发酵蜘蛛眼 × 1"))
                      && !plan.materials.join(QStringLiteral(" "))
                              .contains(QStringLiteral("下界疣")),
                  "★★ ZAI4 虚弱的材料只有水瓶和发酵蜘蛛眼（不补下界疣）",
                  plan.materials.join(QStringLiteral("、")));
        }

        const Brewing::Plan bad = Brewing::plan(-1, false, false, Brewing::Form::Normal);
        check(!bad.ok && !bad.error.isEmpty(),
              "★ ZAI4 认不出来的下标 ⇒ 报错，不编一份计划出来");
    }

    // ---------- ZAI3b. 酿造：瓶数、材料账与燃料----------
    {
        check(Brewing::maxBottlesPerOperation() == 3 && Brewing::maxBottles() == 64,
              "★ ZAI3b 一次最多 3 瓶；界面最多问 64 瓶");
        check(Brewing::operationsFor(0) == 0 && Brewing::operationsFor(1) == 1
                  && Brewing::operationsFor(3) == 1 && Brewing::operationsFor(4) == 2
                  && Brewing::operationsFor(64) == 22,
              "★★ ZAI3b 酿造次数 = ⌈瓶数 / 3⌉（1–3 瓶一次、4 瓶两次、64 瓶 22 次）",
              QStringLiteral("%1 / %2 / %3")
                  .arg(Brewing::operationsFor(1))
                  .arg(Brewing::operationsFor(4))
                  .arg(Brewing::operationsFor(64)));
        check(Brewing::blazePowderFor(0) == 0 && Brewing::blazePowderFor(1) == 1
                  && Brewing::blazePowderFor(20) == 1 && Brewing::blazePowderFor(21) == 2,
              "★★ ZAI3b 1 个烈焰粉够 20 次（21 次要 2 个）",
              QStringLiteral("%1 / %2")
                  .arg(Brewing::blazePowderFor(20))
                  .arg(Brewing::blazePowderFor(21)));

        const Brewing::Plan six = Brewing::plan(0, false, false, Brewing::Form::Normal, 6);
        check(six.ok && six.bottles == 6 && six.operations == 2,
              "★★ ZAI3b 要 6 瓶 ⇒ 酿 2 次", six.error);
        check(six.materials.contains(QStringLiteral("糖 × 2"))
                  && six.materials.contains(QStringLiteral("下界疣 × 2")),
              "★★ ZAI3b 材料按次数备（糖 × 2、下界疣 × 2）",
              six.materials.join(QStringLiteral("、")));
        check(six.materials.contains(QStringLiteral("水瓶 × 6")),
              "★★ ZAI3b 水瓶按瓶数备（6 瓶 ⇒ 水瓶 × 6）",
              six.materials.join(QStringLiteral("、")));
        check(six.detailText().contains(QStringLiteral("6 瓶（酿 2 次）"))
                  && six.detailText().contains(QStringLiteral("材料：水瓶 × 6"))
                  && six.detailText().contains(
                         QStringLiteral("烈焰粉（燃料） × 1（1 个烈焰粉可酿 20 次）"))
                  && !six.detailText().contains(QStringLiteral("玻璃瓶")),
              "★★ ZAI3b 明细里写清瓶数、酿造次数与燃料用量（不再列玻璃瓶）",
              six.detailText().left(200));
        check(six.summary.contains(QStringLiteral("6 瓶（酿 2 次）")),
              "★ ZAI3b 总述里也带上产物的瓶数", six.summary);

        // 力量药水的材料本身就是烈焰粉：材料一行是配方，燃料另起一行，两笔不能混。
        const int strength = Brewing::potionNames().indexOf(QStringLiteral("力量药水"));
        if (strength >= 0) {
            const Brewing::Plan strengthPlan =
                Brewing::plan(strength, false, false, Brewing::Form::Normal);
            check(strengthPlan.materials.contains(QStringLiteral("烈焰粉 × 1"))
                      && strengthPlan.materials.contains(
                             QStringLiteral("烈焰粉（燃料） × 1（1 个烈焰粉可酿 20 次）")),
                  "★★ ZAI3b 力量药水：配方里的烈焰粉与燃料那笔分开写",
                  strengthPlan.materials.join(QStringLiteral("、")));
        }

        const Brewing::Plan lingeringSix =
            Brewing::plan(0, false, false, Brewing::Form::Lingering, 6);
        check(lingeringSix.ok && lingeringSix.materials.contains(QStringLiteral("火药 × 2"))
                  && lingeringSix.materials.contains(QStringLiteral("龙息 × 2")),
              "★★ ZAI3b 滞留型：火药与龙息也各按次数备料",
              lingeringSix.materials.join(QStringLiteral("、")));

        const Brewing::Plan none = Brewing::plan(0, false, false, Brewing::Form::Normal, 0);
        const Brewing::Plan tooMany =
            Brewing::plan(0, false, false, Brewing::Form::Normal, Brewing::maxBottles() + 1);
        check(!none.ok && none.error.contains(QStringLiteral("瓶数"))
                  && !tooMany.ok && tooMany.error.contains(QStringLiteral("瓶数")),
              "★ ZAI3b 瓶数越界 ⇒ 报错（不悄悄钳到 1 或 64）", none.error + tooMany.error);

        // 表里的两处事实（要求「合法效果/等级/延长」）：再生能延长；迟缓的
        // 荧石粉给的是 IV。
        int regeneration = -1;
        int slowness = -1;
        for (int i = 0; i < Brewing::potionCount(); ++i) {
            if (Brewing::potionAt(i).name == QStringLiteral("再生药水")) regeneration = i;
            if (Brewing::potionAt(i).name == QStringLiteral("迟缓药水")) slowness = i;
        }
        check(regeneration >= 0 && Brewing::potionAt(regeneration).extendable,
              "★★ ZAI3b 再生能加红石延长（表里原来标成不能）");
        check(slowness >= 0 && Brewing::potionAt(slowness).upgradedLevel == 4,
              "★★ ZAI3b 迟缓荧石粉给的是 IV（不是 II）",
              QString::number(slowness >= 0 ? Brewing::potionAt(slowness).upgradedLevel : 0));
        const Brewing::Plan slownessIv =
            Brewing::plan(slowness, false, true, Brewing::Form::Normal);
        check(slownessIv.ok && slownessIv.summary.contains(QStringLiteral("IV"))
                  && slownessIv.steps.last().contains(QStringLiteral("IV")),
              "★★ ZAI3b 迟缓升一级之后名字与步骤都写 IV", slownessIv.summary);
        check(slownessIv.effect == QStringLiteral("迟缓 IV · 0:20"),
              "★★ ZAI3b 迟缓 IV 的效果行写等级与时长（0:20）", slownessIv.effect);
        check(Brewing::levelText(0).isEmpty() && Brewing::levelText(2) == QStringLiteral("II")
                  && Brewing::levelText(4) == QStringLiteral("IV"),
              "ZAI3b 等级写法（0 = 空、2 = II、4 = IV）");
    }

    // ---------- ZAI5. 酿造：1.21 那四瓶与版本边界 ----------
    {
        const auto indexOf = [](const QString &name) {
            return Brewing::potionNames().indexOf(name);
        };
        const int weaving = indexOf(QStringLiteral("盘丝药水"));
        const int wind = indexOf(QStringLiteral("蓄风药水"));
        const int oozing = indexOf(QStringLiteral("渗浆药水"));
        const int infested = indexOf(QStringLiteral("虫蚀药水"));
        check(weaving >= 0 && wind >= 0 && oozing >= 0 && infested >= 0,
              "★★ ZAI5 1.21 加的四瓶都在表里（盘丝 / 蓄风 / 渗浆 / 虫蚀）",
              QStringLiteral("%1 / %2 / %3 / %4")
                  .arg(weaving)
                  .arg(wind)
                  .arg(oozing)
                  .arg(infested));
        check(weaving >= 0 && Brewing::potionAt(weaving).ingredient == QStringLiteral("蜘蛛网")
                  && wind >= 0
                  && Brewing::potionAt(wind).ingredient == QStringLiteral("旋风棒")
                  && oozing >= 0
                  && Brewing::potionAt(oozing).ingredient == QStringLiteral("黏液块")
                  && infested >= 0
                  && Brewing::potionAt(infested).ingredient == QStringLiteral("石头"),
              "★★ ZAI5 材料名照 `zh_cn.json`：蜘蛛网 / 旋风棒 / 黏液块 / 石头");
        check(weaving >= 0 && Brewing::potionAt(weaving).parent == -1
                  && !Brewing::potionAt(weaving).fromWater
                  && !Brewing::potionAt(weaving).extendable
                  && !Brewing::potionAt(weaving).upgradable,
              "★★ ZAI5 从粗制的药水来（不是水瓶直做），也没有延长 / II 级版本");

        if (weaving >= 0) {
            const Brewing::Plan plan =
                Brewing::plan(weaving, false, false, Brewing::Form::Normal, 4);
            check(plan.ok && plan.steps.size() == 2
                      && plan.steps.at(0).contains(QStringLiteral("下界疣"))
                      && plan.steps.at(1).contains(QStringLiteral("蜘蛛网"))
                      && plan.summary.contains(QStringLiteral("盘丝药水"))
                      && plan.effect == QStringLiteral("盘丝 I · 3:00"),
                  "★★ ZAI5 盘丝：水瓶 → 粗制的药水 → 加蜘蛛网（一共两步）",
                  plan.steps.join(QStringLiteral(" / ")));
            check(plan.materials.contains(QStringLiteral("蜘蛛网 × 2"))
                      && plan.materials.contains(QStringLiteral("下界疣 × 2"))
                      && plan.materials.contains(QStringLiteral("水瓶 × 4")),
                  "★★ ZAI5 要 4 瓶 ⇒ 酿 2 次，蜘蛛网与下界疣各备 2 份、水瓶 4 个",
                  plan.materials.join(QStringLiteral("、")));
        }

        check(weaving >= 0 && Brewing::potionSince(weaving) == QStringLiteral("1.21")
                  && wind >= 0 && Brewing::potionSince(wind) == QStringLiteral("1.21")
                  && oozing >= 0 && Brewing::potionSince(oozing) == QStringLiteral("1.21")
                  && infested >= 0 && Brewing::potionSince(infested) == QStringLiteral("1.21"),
              "★★ ZAI5 那四瓶的版本门槛写的是 1.21");
        check(Brewing::potionSince(0).isEmpty() && Brewing::potionSince(-1).isEmpty()
                  && Brewing::potionSince(Brewing::potionCount()).isEmpty(),
              "ZAI5 各版本都有的那几瓶回空串；越界也回空串");
        check(weaving >= 0 && Brewing::available(weaving, QString())
                  && Brewing::available(weaving, QStringLiteral("1.21"))
                  && !Brewing::available(weaving, QStringLiteral("1.20.4"))
                  && Brewing::available(0, QStringLiteral("1.14")) && Brewing::available(0, QString()),
              "★★ ZAI5 目标版本 1.20.4 里还没有盘丝；迅捷各版本都有",
              QStringLiteral("1.20.4=%1 / 1.21=%2")
                  .arg(Brewing::available(weaving, QStringLiteral("1.20.4")) ? 1 : 0)
                  .arg(weaving >= 0 && Brewing::available(weaving, QStringLiteral("1.21")) ? 1 : 0));
        check(!Brewing::available(-1, QString())
                  && !Brewing::available(Brewing::potionCount(), QString()),
              "★ ZAI5 越界下标回 false（认不出来就不能说它有）");

        check(!GameVersion::availableIn(QStringLiteral("1.21"), QStringLiteral("1.20.4"))
                  && GameVersion::availableIn(QStringLiteral("1.21"), QStringLiteral("1.21"))
                  && GameVersion::availableIn(QStringLiteral("1.20"),
                                              QStringLiteral("1.20.4"))
                  && GameVersion::availableIn(QString(), QStringLiteral("1.20.4"))
                  && !GameVersion::availableIn(QStringLiteral("1.21.4"),
                                               QStringLiteral("1.21")),
              "★★ ZAI5 版本比较按段：1.20.4 < 1.21（不是按字面比）");
        check(GameVersion::versions().first().isEmpty()
                  && GameVersion::versions().contains(QStringLiteral("1.20.5"))
                  && GameVersion::versions().contains(QStringLiteral("1.21"))
                  && GameVersion::versions().contains(QStringLiteral("1.21.4"))
                  && GameVersion::versions() == BannerDesign::versions(),
              "★ ZAI5 版本表只有一份（旗帜那一页拿的就是它；1.20.5 是图案写法的分界，"
              "1.21.4 这一档是给树脂质用的）");
    }

    // ---------- ZAI6. 酿造：从水瓶起步，材料 / 步骤 / 产物摘要三者对齐 ----------
    {
        const int swift = 0;
        const int weakness = Brewing::potionNames().indexOf(QStringLiteral("虚弱药水"));
        const auto hasMaterial = [](const Brewing::Plan &plan, const QString &line) {
            return plan.materials.contains(line);
        };

        // 普通路径（迅捷）。
        {
            const Brewing::Plan p = Brewing::plan(swift, false, false, Brewing::Form::Normal, 3);
            check(p.ok
                      && p.steps == QStringList{QStringLiteral("水瓶 + 下界疣 → 粗制的药水"),
                                                QStringLiteral("粗制的药水 + 糖 → 迅捷药水")}
                      && hasMaterial(p, QStringLiteral("水瓶 × 3"))
                      && hasMaterial(p, QStringLiteral("下界疣 × 1"))
                      && hasMaterial(p, QStringLiteral("糖 × 1"))
                      && p.summary == QStringLiteral("产出：迅捷药水 × 3 瓶（酿 1 次）")
                      && p.effect == QStringLiteral("迅捷 I · 3:00")
                      && p.detailText().contains(p.summary)
                      && p.detailText().contains(QStringLiteral("效果：") + p.effect),
                  "★★ ZAI6 普通路径（迅捷）：材料 / 步骤 / 产出与效果一致", p.detailText());
        }

        // 特殊路径（虚弱：水瓶直出）。
        if (weakness >= 0) {
            const Brewing::Plan p = Brewing::plan(weakness, false, false, Brewing::Form::Normal, 3);
            check(p.ok && p.steps == QStringList{QStringLiteral("水瓶 + 发酵蜘蛛眼 → 虚弱药水")}
                      && p.directFromWater && hasMaterial(p, QStringLiteral("水瓶 × 3"))
                      && hasMaterial(p, QStringLiteral("发酵蜘蛛眼 × 1"))
                      && p.summary == QStringLiteral("产出：虚弱药水 × 3 瓶（酿 1 次）")
                      && p.effect == QStringLiteral("虚弱 I · 1:30")
                      && p.detailText().contains(QStringLiteral("特殊路径"))
                      && !p.detailText().contains(QStringLiteral("下界疣")),
                  "★★ ZAI6 特殊路径（虚弱）：直接从水瓶酿，不套粗制的药水那一步", p.detailText());
        }

        // 延长（红石）。
        {
            const Brewing::Plan p = Brewing::plan(swift, true, false, Brewing::Form::Normal, 1);
            check(p.ok
                      && p.steps.last()
                             == QStringLiteral("迅捷药水 + 红石粉 → 迅捷药水（延长）")
                      && hasMaterial(p, QStringLiteral("红石粉 × 1"))
                      && p.summary == QStringLiteral("产出：迅捷药水（延长） × 1 瓶（酿 1 次）")
                      && p.effect == QStringLiteral("迅捷 I · 8:00"),
                  "★★ ZAI6 延长（红石）：材料多一行红石粉，效果时长 8:00", p.detailText());
        }

        // 增强（荧石）。
        {
            const Brewing::Plan p = Brewing::plan(swift, false, true, Brewing::Form::Normal, 1);
            check(p.ok
                      && p.steps.last() == QStringLiteral("迅捷药水 + 荧石粉 → 迅捷药水 II")
                      && hasMaterial(p, QStringLiteral("荧石粉 × 1"))
                      && p.summary == QStringLiteral("产出：迅捷药水 II × 1 瓶（酿 1 次）")
                      && p.effect == QStringLiteral("迅捷 II · 1:30"),
                  "★★ ZAI6 增强（荧石）：等级 II、时长 1:30", p.detailText());
        }

        // 喷溅（火药）。
        {
            const Brewing::Plan p = Brewing::plan(swift, false, false, Brewing::Form::Splash, 1);
            check(p.ok
                      && p.steps.last() == QStringLiteral("迅捷药水 + 火药 → 喷溅型迅捷药水")
                      && hasMaterial(p, QStringLiteral("火药 × 1"))
                      && p.summary == QStringLiteral("产出：喷溅型迅捷药水 × 1 瓶（酿 1 次）")
                      && p.effect == QStringLiteral("迅捷 I · 3:00"),
                  "★★ ZAI6 喷溅（火药）：结果格式与普通一致", p.detailText());
        }

        // 滞留（龙息）。
        {
            const Brewing::Plan p = Brewing::plan(swift, false, false, Brewing::Form::Lingering, 1);
            check(p.ok
                      && p.steps.at(p.steps.size() - 2)
                             == QStringLiteral("迅捷药水 + 火药 → 喷溅型迅捷药水")
                      && p.steps.last()
                             == QStringLiteral("喷溅型迅捷药水 + 龙息 → 滞留型迅捷药水")
                      && hasMaterial(p, QStringLiteral("火药 × 1"))
                      && hasMaterial(p, QStringLiteral("龙息 × 1"))
                      && p.summary == QStringLiteral("产出：滞留型迅捷药水 × 1 瓶（酿 1 次）")
                      && p.effect == QStringLiteral("迅捷 I · 0:45"),
                  "★★ ZAI6 滞留（龙息）：先火药再龙息，效果时长按四分之一（0:45）", p.detailText());
        }

        // 瞬间效果（治疗 / 伤害）：写说明，不写持续时间。
        {
            const int healing = Brewing::potionNames().indexOf(QStringLiteral("治疗药水"));
            const int harming = Brewing::potionNames().indexOf(QStringLiteral("伤害药水"));
            if (healing >= 0) {
                const Brewing::Plan one =
                    Brewing::plan(healing, false, false, Brewing::Form::Normal);
                const Brewing::Plan two =
                    Brewing::plan(healing, false, true, Brewing::Form::Normal);
                check(one.effect == QStringLiteral("治疗 I · 瞬间恢复 4 点生命值")
                          && two.effect == QStringLiteral("治疗 II · 瞬间恢复 8 点生命值")
                          && !one.effect.contains(QLatin1Char(':'))
                          && !two.effect.contains(QLatin1Char(':')),
                      "★★ ZAI6 治疗是瞬间效果：写说明、不写持续时间",
                      one.effect + QStringLiteral(" / ") + two.effect);
            }
            if (harming >= 0) {
                const Brewing::Plan one =
                    Brewing::plan(harming, false, false, Brewing::Form::Normal);
                check(one.effect == QStringLiteral("伤害 I · 瞬间造成 6 点伤害")
                          && !one.effect.contains(QLatin1Char(':')),
                      "★★ ZAI6 伤害是瞬间效果：写说明、不写持续时间", one.effect);
            }
        }
    }

    // ---------- ZAI7. 酿造：滞留型的效果时长按普通型四分之一 ----------
    {
        const int swift = 0;
        const Brewing::Plan base = Brewing::plan(swift, false, false, Brewing::Form::Lingering, 1);
        const Brewing::Plan extended =
            Brewing::plan(swift, true, false, Brewing::Form::Lingering, 1);
        const Brewing::Plan level2 =
            Brewing::plan(swift, false, true, Brewing::Form::Lingering, 1);
        check(base.effect == QStringLiteral("迅捷 I · 0:45")
                  && extended.effect == QStringLiteral("迅捷 I · 2:00")
                  && level2.effect == QStringLiteral("迅捷 II · 0:22"),
              "★★ ZAI7 滞留迅捷：3:00 → 0:45、延长 8:00 → 2:00、II 1:30 → 0:22（向下取整）",
              base.effect + QStringLiteral(" / ") + extended.effect + QStringLiteral(" / ")
                  + level2.effect);
        check(base.detailText().contains(QStringLiteral("效果：迅捷 I · 0:45")),
              "★★ ZAI7 明细里摆的就是这四分之一时长", base.detailText().left(120));

        const int turtle = Brewing::potionNames().indexOf(QStringLiteral("神龟药水"));
        if (turtle >= 0) {
            const Brewing::Plan p =
                Brewing::plan(turtle, false, false, Brewing::Form::Lingering);
            check(p.effect == QStringLiteral("海龟大师 I · 0:05"),
                  "★★ ZAI7 滞留海龟大师：0:20 → 0:05", p.effect);
        }

        const int poison = Brewing::potionNames().indexOf(QStringLiteral("剧毒药水"));
        if (poison >= 0) {
            const Brewing::Plan p =
                Brewing::plan(poison, false, false, Brewing::Form::Lingering);
            check(p.effect == QStringLiteral("中毒 I · 0:11"),
                  "★★ ZAI7 滞留中毒：0:45 → 0:11（11.25 秒向下取整）", p.effect);
        }

        const int healing = Brewing::potionNames().indexOf(QStringLiteral("治疗药水"));
        if (healing >= 0) {
            const Brewing::Plan normal =
                Brewing::plan(healing, false, true, Brewing::Form::Normal);
            const Brewing::Plan splash =
                Brewing::plan(healing, false, true, Brewing::Form::Splash);
            const Brewing::Plan lingering =
                Brewing::plan(healing, false, true, Brewing::Form::Lingering);
            check(normal.effect == QStringLiteral("治疗 II · 瞬间恢复 8 点生命值")
                      && splash.effect == QStringLiteral("治疗 II · 瞬间恢复 8 点生命值")
                      && lingering.effect
                             == QStringLiteral("治疗 II · 瞬间，效果云内周期生效"),
                  "★★ ZAI7 滞留治疗 II：换成「瞬间，效果云内周期生效」，普通 / 喷溅仍是原说明",
                  normal.effect + QStringLiteral(" / ") + splash.effect + QStringLiteral(" / ")
                      + lingering.effect);
        }
    }

    // ---------- ZAJ0. 旗帜：图案表与十六色 ----------
    {
        check(Banner::patternCount() == 45,
              "★ ZAJ0 图案表 45 种（和原版织布机那几十种对得上）",
              QString::number(Banner::patternCount()));
        check(Banner::colorCount() == 16 && Banner::colorName(0) == QStringLiteral("白色")
                  && Banner::colorName(14) == QStringLiteral("红色")
                  && Banner::colorName(15) == QStringLiteral("黑色"),
              "★★ ZAJ0 十六色的次序和游戏里一致（0 = 白 … 14 = 红、15 = 黑）",
              QStringLiteral("%1 / %2 / %3")
                  .arg(Banner::colorName(0), Banner::colorName(14), Banner::colorName(15)));
        check(Banner::dyeName(14) == QStringLiteral("红色染料")
                  && Banner::colorId(14) == QStringLiteral("red"),
              "★ ZAJ0 染料名和 id 都从同一张表里出来",
              Banner::dyeName(14) + QStringLiteral(" / ") + Banner::colorId(14));
        check(Banner::patternIndexById(QStringLiteral("creeper_charge")) >= 0
                  && Banner::patternAt(Banner::patternIndexById(QStringLiteral("creeper_charge")))
                             .item
                         == QStringLiteral("苦力怕头颅"),
              "★★ ZAJ0 少数几种图案用的不是「旗帜图案」那件物品（苦力怕 = 苦力怕头颅）",
              Banner::patternAt(Banner::patternIndexById(QStringLiteral("creeper_charge"))).item);
        check(Banner::patternIndexById(QStringLiteral("no_such_pattern")) == -1
                  && Banner::patternAt(999).id.isEmpty(),
              "★ ZAJ0 认不出来就回 -1 / 空，不编一个图案出来");
        check(Banner::patternIndexById(QStringLiteral("circle")) >= 0
                  && Banner::patternIndexById(QStringLiteral("rhombus")) >= 0
                  && Banner::patternIndexById(QStringLiteral("straight_cross")) >= 0
                  && Banner::patternIndexById(QStringLiteral("curly_border")) >= 0,
              "★ ZAJ0 圆形 / 菱形 / 正十字 / 卷曲边框这四种也在表里（原来漏了）");
        check(Banner::patternSince(Banner::patternIndexById(QStringLiteral("flow")))
                      == QStringLiteral("1.21")
                  && Banner::patternSince(Banner::patternIndexById(QStringLiteral("guster")))
                         == QStringLiteral("1.21")
                  && Banner::patternSince(Banner::patternIndexById(QStringLiteral("cross")))
                         == QStringLiteral("1.14"),
              "★★ ZAJ0 图案从哪一版起有：涡流 / 狂风是 1.21，普通图案回 1.14");
    }

    // ---------- ZAJ1. 旗帜：叠层计划与材料账 ----------
    {
        QList<Banner::Layer> layers;
        layers.append({Banner::patternIndexById(QStringLiteral("cross")), 0});       // 白十字
        layers.append({Banner::patternIndexById(QStringLiteral("field_masoned")), 15});  // 黑砖墙纹
        const Banner::Plan plan = Banner::plan(14, layers);  // 红底
        check(plan.ok, "ZAJ1 两层的计划算得出来", plan.error);
        check(plan.steps.size() == 2 && plan.steps.first().contains(QStringLiteral("红色空白旗帜"))
                  && plan.steps.first().contains(QStringLiteral("白色染料"))
                  && plan.steps.last().contains(QStringLiteral("上一层的旗帜"))
                  && plan.steps.last().contains(QStringLiteral("黑色染料")),
              "★★ ZAJ1 使用已有空白旗帜，按列表次序逐层叠加",
              plan.steps.join(QStringLiteral(" / ")));
        check(plan.materials.size() == 4
                  && plan.materials.contains(QStringLiteral("白色染料 × 1"))
                  && plan.materials.contains(QStringLiteral("黑色染料 × 1"))
                  && plan.materials.contains(QStringLiteral("砖块 × 1"))
                  && plan.materials.contains(QStringLiteral("旗帜图案 × 1")),
              "★★ ZAJ1 材料按种类合并成「× N」，特殊材料用图案表里那件东西",
              plan.materials.join(QStringLiteral(" / ")));
        check(plan.summary.contains(QStringLiteral("底色 红色 · 2/6 层")),
              "★ ZAJ1 总述写清底色和层数", plan.summary);
        check(plan.command.contains(QStringLiteral("minecraft:red_banner"))
                  && plan.command.contains(QStringLiteral("{Pattern:\"cross\",Color:0}"))
                  && plan.command.contains(QStringLiteral("field_masoned")),
              "★ ZAJ1 指令里图案、颜色、底色都对得上", plan.command);
        // 写法按目标版本分两套：1.20.5 起 `/give` 不收 NBT 那串，改用物品组件。
        const QString nbtCommand =
            QStringLiteral("/give @p minecraft:red_banner{BlockEntityTag:{Patterns:"
                           "[{Pattern:\"cross\",Color:0},{Pattern:\"field_masoned\",Color:15}]}}");
        const QString componentCommand =
            QStringLiteral("/give @p minecraft:red_banner[minecraft:banner_patterns="
                           "[{pattern:\"minecraft:cross\",color:\"white\"},"
                           "{pattern:\"minecraft:field_masoned\",color:\"black\"}]]");
        check(plan.command == nbtCommand,
              "★★ ZAJ1 不限版本 / 1.20.4 ⇒ 老写法逐字不变（NBT）", plan.command);
        check(Banner::plan(14, layers, QStringLiteral("1.20.4")).command == nbtCommand,
              "★★ ZAJ1 1.20.4 ⇒ NBT（分界在这一档之后）",
              Banner::plan(14, layers, QStringLiteral("1.20.4")).command);
        check(Banner::plan(14, layers, QStringLiteral("1.20.5")).command == componentCommand
                  && Banner::plan(14, layers, QStringLiteral("1.21")).command
                         == componentCommand,
              "★★ ZAJ1 1.20.5 起 ⇒ 物品组件写法（图案 id 与颜色名都带命名空间规则）",
              Banner::plan(14, layers, QStringLiteral("1.20.5")).command);
        check(Banner::plan(3, {}, QStringLiteral("1.21")).command
                  == QStringLiteral("/give @p minecraft:light_blue_banner"),
              "★ ZAJ1 新写法下没叠层 ⇒ 不拖一对空方括号",
              Banner::plan(3, {}, QStringLiteral("1.21")).command);
        check(plan.detailText().contains(QStringLiteral("材料清单："))
                  && plan.detailText().contains(plan.command),
              "★ ZAJ1 明细把步骤、材料、指令都摆出来");
    }

    // ---------- ZAJ2. 旗帜：层数上限与提醒 ----------
    {
        QList<Banner::Layer> seven;
        for (int i = 0; i < Banner::kMaxLayers + 1; ++i) {
            seven.append({0, 0});
        }
        const Banner::Plan tooMany = Banner::plan(0, seven);
        check(!tooMany.ok && tooMany.error.contains(QStringLiteral("6")),
              "★★ ZAJ2 超过 6 层 ⇒ 报错（原版织布机叠不了第 7 层）", tooMany.error);

        QList<Banner::Layer> same;
        same.append({0, 1});
        same.append({0, 1});
        const Banner::Plan duplicate = Banner::plan(0, same);
        check(duplicate.ok && !duplicate.notes.isEmpty()
                  && duplicate.notes.first().contains(QStringLiteral("显示效果与单层相同")),
              "★ ZAJ2 连续两层完全一样 ⇒ 一条提醒（不是错误 —— 游戏允许这么叠）",
              duplicate.notes.join(QStringLiteral(" / ")));

        const Banner::Plan noLayers = Banner::plan(3, {});
        check(noLayers.ok && noLayers.materials.isEmpty() && noLayers.steps.isEmpty()
                  && noLayers.summary.contains(QStringLiteral("底色 淡蓝色 · 0/6 层"))
                  && noLayers.command == QStringLiteral("/give @p minecraft:light_blue_banner"),
              "★ ZAJ2 纯色旗保留底色和指令，不列任何叠层材料或步骤", noLayers.command);

        QList<Banner::Layer> badPattern;
        badPattern.append({-1, 0});
        const Banner::Plan badP = Banner::plan(0, badPattern);
        check(!badP.ok && badP.error.contains(QStringLiteral("图案")),
              "★ ZAJ2 认不出来的图案下标 ⇒ 报错", badP.error);

        // 版本限制：用了 1.21 才加的图案就提醒一句（同一图案只提醒一次）。
        QList<Banner::Layer> newer;
        newer.append({Banner::patternIndexById(QStringLiteral("flow")), 0});
        newer.append({Banner::patternIndexById(QStringLiteral("flow")), 0});
        const Banner::Plan newerPlan = Banner::plan(0, newer);
        check(newerPlan.ok
                  && newerPlan.notes.join(QStringLiteral(" / ")).contains(QStringLiteral("1.21"))
                  && newerPlan.notes.join(QStringLiteral(" / ")).count(QStringLiteral("1.21")) == 1,
              "★★ ZAJ2 用了 1.21 才有的图案 ⇒ 一条版本提醒（同一图案只提一次）",
              newerPlan.notes.join(QStringLiteral(" / ")));
        const Banner::Plan oldPlan = Banner::plan(0, {{Banner::patternIndexById(
                                                          QStringLiteral("cross")),
                                                      0}});
        check(oldPlan.ok && oldPlan.notes.isEmpty(),
              "★ ZAJ2 老图案不啰嗦（没有版本提醒）", oldPlan.notes.join(QStringLiteral(" / ")));
    }

    // ---------- ZAJ3. 纹饰：表与限制 ----------
    {
        check(ArmorTrim::trimMaterialCount() == 11 && ArmorTrim::patternCount() == 18
                  && ArmorTrim::partCount() == 4 && ArmorTrim::armorMaterialCount() == 7,
              "★★ ZAJ3 十一种材料 / 十八种图案 / 四个部位 / 七种盔甲材质",
              QStringLiteral("%1 / %2 / %3 / %4")
                  .arg(ArmorTrim::trimMaterialCount())
                  .arg(ArmorTrim::patternCount())
                  .arg(ArmorTrim::partCount())
                  .arg(ArmorTrim::armorMaterialCount()));
        const int turtle = ArmorTrim::armorMaterialCount() - 1;
        check(ArmorTrim::armorMaterialFitsPart(turtle, 0)
                  && !ArmorTrim::armorMaterialFitsPart(turtle, 1)
                  && ArmorTrim::armorPieceName(turtle, 0) == QStringLiteral("海龟壳"),
              "★★ ZAJ3 海龟壳只认头盔（别的部位连名字都拼不出来）");
        check(ArmorTrim::armorPieceName(4, 1) == QStringLiteral("钻石胸甲"),
              "★ ZAJ3 盔甲全名 = 材质 + 部位", ArmorTrim::armorPieceName(4, 1));
        check(ArmorTrim::trimMaterialAt(0).item == QStringLiteral("紫水晶碎片")
                  && ArmorTrim::trimMaterialAt(9).item == QStringLiteral("红石粉"),
              "★ ZAJ3 每种纹饰材料在锻造台里摆的那件东西写在表里",
              ArmorTrim::trimMaterialAt(0).item + QStringLiteral(" / ")
                  + ArmorTrim::trimMaterialAt(9).item);
        // 名字以游戏 zh_cn.json 为准：`trim_material.minecraft.*` / `trim_pattern.minecraft.*`
        const QStringList materialNames = ArmorTrim::trimMaterialNames();
        check(materialNames
                  == QStringList{QStringLiteral("紫水晶质"), QStringLiteral("铜质"),
                                 QStringLiteral("钻石质"), QStringLiteral("绿宝石质"),
                                 QStringLiteral("金质"), QStringLiteral("铁质"),
                                 QStringLiteral("青金石质"), QStringLiteral("下界合金质"),
                                 QStringLiteral("石英质"), QStringLiteral("红石质"),
                                 QStringLiteral("树脂质")},
              "★★ ZAJ3 材料名照游戏（金质 / 树脂质都在，不自己起「黄金」这种简称）",
              materialNames.join(QStringLiteral(" / ")));
        check(ArmorTrim::trimMaterialAt(10).id == QStringLiteral("resin")
                  && ArmorTrim::trimMaterialAt(10).item == QStringLiteral("树脂砖")
                  && ArmorTrim::trimMaterialSince(10) == QStringLiteral("1.21.4"),
              "★★ ZAJ3 第 11 种 = 树脂质（1.21.4 的树脂砖）",
              ArmorTrim::trimMaterialAt(10).item + QStringLiteral(" / ")
                  + ArmorTrim::trimMaterialSince(10));
        check(ArmorTrim::trimMaterialAt(4).name == QStringLiteral("金质"),
              "★ ZAJ3 gold 的官方名是「金质」，不是「黄金」");
        // 目标版本边界：与旗帜 / 酿造同一条判据（`GameVersion::availableIn()`）。
        // 空串 = 不限版本；越界下标回 false（认不出来不能说它有）。
        check(ArmorTrim::trimMaterialAvailable(10, QStringLiteral("1.21.4"))
                  && !ArmorTrim::trimMaterialAvailable(10, QStringLiteral("1.21"))
                  && ArmorTrim::trimMaterialAvailable(10, QString())
                  && ArmorTrim::trimMaterialAvailable(0, QStringLiteral("1.20")),
              "★★ ZAJ3 树脂质 1.21.4 才有（1.21 里没有；「不限版本」照旧算有）",
              QStringLiteral("1.21.4=%1 1.21=%2 空=%3")
                  .arg(ArmorTrim::trimMaterialAvailable(10, QStringLiteral("1.21.4")))
                  .arg(ArmorTrim::trimMaterialAvailable(10, QStringLiteral("1.21")))
                  .arg(ArmorTrim::trimMaterialAvailable(10, QString())));
        check(!ArmorTrim::trimMaterialAvailable(11, QString())
                  && !ArmorTrim::trimMaterialAvailable(-1, QString()),
              "★ ZAJ3 越界的材料下标回 false（不是「不限版本 → 什么都有」）");
        check(ArmorTrim::patternSince(16) == QStringLiteral("1.21")
                  && ArmorTrim::patternSince(0) == QStringLiteral("1.20")
                  && ArmorTrim::patternSince(99).isEmpty(),
              "★ ZAJ3 图案的那一档版本从表里问（越界回空串）",
              ArmorTrim::patternSince(16));
        check(ArmorTrim::patternAvailable(16, QStringLiteral("1.21"))
                  && !ArmorTrim::patternAvailable(16, QStringLiteral("1.20.4"))
                  && ArmorTrim::patternAvailable(0, QStringLiteral("1.20"))
                  && !ArmorTrim::patternAvailable(99, QString()),
              "★★ ZAJ3 涡流 / 镶铆是 1.21 的图案（1.20.4 里没有）；海岸从 1.20 起",
              QStringLiteral("涡流@1.21=%1 涡流@1.20.4=%2")
                  .arg(ArmorTrim::patternAvailable(16, QStringLiteral("1.21")))
                  .arg(ArmorTrim::patternAvailable(16, QStringLiteral("1.20.4"))));
        const QStringList patternNames = ArmorTrim::patternNames();
        check(patternNames
                  == QStringList{QStringLiteral("海岸"), QStringLiteral("哨兵"),
                                 QStringLiteral("沙丘"), QStringLiteral("荒野"),
                                 QStringLiteral("监守"), QStringLiteral("眼眸"),
                                 QStringLiteral("恼鬼"), QStringLiteral("潮汐"),
                                 QStringLiteral("猪鼻"), QStringLiteral("肋骨"),
                                 QStringLiteral("尖塔"), QStringLiteral("向导"),
                                 QStringLiteral("塑造"), QStringLiteral("幽静"),
                                 QStringLiteral("牧民"), QStringLiteral("雇主"),
                                 QStringLiteral("涡流"), QStringLiteral("镶铆")},
              "★★ ZAJ3 图案名照游戏（监守 / 尖塔 / 向导 / 幽静 / 牧民 / 雇主 / 镶铆）",
              patternNames.join(QStringLiteral(" / ")));
        check(ArmorTrim::patternSource(7).contains(QStringLiteral("远古守卫者"))
                  && !ArmorTrim::patternSource(7).contains(QStringLiteral("海底神殿（击杀守卫者")),
              "★★ ZAJ3 潮汐是远古守卫者掉的（不是普通守卫者）",
              ArmorTrim::patternSource(7));
        check(ArmorTrim::patternSource(17).contains(QStringLiteral("宝库"))
                  && !ArmorTrim::patternSource(17).contains(QStringLiteral("不祥")),
              "★★ ZAJ3 镶铆在普通宝库，涡流才在不祥宝库",
              ArmorTrim::patternSource(17));
        const auto ranked = ArmorTrimAssets::newestFirst(
            {{QStringLiteral("1.12.2-Forge"), QStringLiteral("C:/mc/versions/1.12.2/1.12.2.jar"), 100},
             {QStringLiteral("26.3"), QStringLiteral("C:/mc/versions/26.3/26.3.jar"), 900},
             {QStringLiteral("1.21.4-Fabric"), QStringLiteral("C:/mc/versions/1.21.4/1.21.4.jar"), 500},
             {QStringLiteral("1.21.4-副本"), QStringLiteral("C:\\MC\\versions\\1.21.4\\1.21.4.jar"), 500}});
        check(ranked.size() == 3 && ranked[0].id == QStringLiteral("26.3")
                  && ranked[1].id == QStringLiteral("1.21.4-Fabric")
                  && ranked[2].id == QStringLiteral("1.12.2-Forge"),
              "★★ ZAJ3 预览资源按发布时间从新到旧挑 jar（同一份 jar 只留一次）",
              QStringLiteral("%1 个候选：%2")
                  .arg(ranked.size())
                  .arg([&] {
                      QStringList ids;
                      for (const auto &candidate : ranked) ids.append(candidate.id);
                      return ids.join(QStringLiteral(" / "));
                  }()));
        // 1.21.2+ 把盔甲/纹饰挪进"装备层"、26.x 又把调色板挪走：路径变了，UV 没变。
        check(ArmorTrimAssets::legacyKey(QStringLiteral("entity/equipment/humanoid/iron"))
                      == QLatin1String("models/armor/iron_layer_1")
                  && ArmorTrimAssets::legacyKey(QStringLiteral("entity/equipment/humanoid_leggings/iron"))
                      == QLatin1String("models/armor/iron_layer_2")
                  && ArmorTrimAssets::legacyKey(QStringLiteral("entity/equipment/humanoid/turtle_scute"))
                      == QLatin1String("models/armor/turtle_layer_1")
                  && ArmorTrimAssets::legacyKey(QStringLiteral("entity/equipment/humanoid/leather_overlay"))
                      == QLatin1String("models/armor/leather_layer_1_overlay")
                  && ArmorTrimAssets::legacyKey(QStringLiteral("trims/entity/humanoid/flow"))
                      == QLatin1String("trims/models/armor/flow")
                  && ArmorTrimAssets::legacyKey(QStringLiteral("trims/entity/humanoid_leggings/flow"))
                      == QLatin1String("trims/models/armor/flow_leggings")
                  && ArmorTrimAssets::legacyKey(QStringLiteral("palettes/trim/resin"))
                      == QLatin1String("trims/color_palettes/resin")
                  && ArmorTrimAssets::legacyKey(QStringLiteral("palettes/trim_base"))
                      == QLatin1String("trims/color_palettes/trim_palette")
                  && ArmorTrimAssets::legacyKey(QStringLiteral("models/armor/iron_layer_1"))
                      == QLatin1String("models/armor/iron_layer_1")
                  && ArmorTrimAssets::legacyKey(QStringLiteral("entity/equipment/humanoid_baby/iron"))
                      == QLatin1String("entity/equipment/humanoid_baby/iron"),
              "★★ ZAJ3 1.21.2+/26.x 装备层与新调色板路径折回旧名（旧路径原样、幼年装备不误认）");
    }

    // ---------- ZAJ4. 纹饰：计划与整套 ----------
    {
        const int coast = 0;  // 表里第一个就是海岸
        const ArmorTrim::Plan one = ArmorTrim::plan(4, 1, coast, 0, 1);
        check(one.ok && one.summary.contains(QStringLiteral("紫水晶"))
                  && one.summary.contains(QStringLiteral("海岸"))
                  && one.summary.contains(QStringLiteral("钻石胸甲")),
              "★★ ZAJ4 成品名 = 材料 · 图案纹饰的盔甲", one.summary);
        check(one.materials.size() == 3
                  && one.materials.contains(QStringLiteral("海岸纹饰模板 × 1"))
                  && one.materials.contains(QStringLiteral("紫水晶碎片 × 1"))
                  && one.materials.contains(QStringLiteral("钻石胸甲 × 1")),
              "★★ ZAJ4 三样东西各一份（模板 / 材料 / 盔甲）",
              one.materials.join(QStringLiteral(" / ")));
        check(one.detailText().contains(QStringLiteral("锻造台")),
              "★ ZAJ4 明细说清在锻造台里怎么摆");

        const ArmorTrim::Plan set = ArmorTrim::plan(4, 1, coast, 0, 4);
        check(set.ok && set.materials.contains(QStringLiteral("海岸纹饰模板 × 4"))
                  && set.notes.join(QStringLiteral(" / ")).contains(QStringLiteral("每打一件消耗一个")),
              "★★ ZAJ4 做 4 件 ⇒ 模板也是 4 份（模板不是一次性解锁）",
              set.notes.join(QStringLiteral(" / ")));

        const int turtle = ArmorTrim::armorMaterialCount() - 1;
        const ArmorTrim::Plan badShell = ArmorTrim::plan(turtle, 1, coast, 0, 1);
        check(!badShell.ok && badShell.error.contains(QStringLiteral("海龟壳")),
              "★★ ZAJ4 海龟壳 + 胸甲 ⇒ 报错，不悄悄换成皮革", badShell.error);

        const ArmorTrim::Plan badPieces = ArmorTrim::plan(0, 0, coast, 0, 5);
        check(!badPieces.ok && badPieces.error.contains(QStringLiteral("1 至 4")),
              "★ ZAJ4 件数越界 ⇒ 报错（最多四件）", badPieces.error);

        const ArmorTrim::Plan badIndex = ArmorTrim::plan(0, 0, -1, 0, 1);
        check(!badIndex.ok && badIndex.error.contains(QStringLiteral("图案")),
              "★ ZAJ4 认不出来的图案下标 ⇒ 报错", badIndex.error);
    }

    // ---------- ZAJ5. 纹饰：模板来源 / 版本 / 整套----------
    {
        const int coast = 0;
        check(ArmorTrim::patternSource(coast).contains(QStringLiteral("沉船")),
              "★★ ZAJ5 海岸模板来自沉船（不是含糊的「地牢里」）",
              ArmorTrim::patternSource(coast));
        check(ArmorTrim::patternSource(-1).isEmpty(),
              "★ ZAJ5 图案下标越界 ⇒ 来源是空串（不编一个地方出来）");
        check(ArmorTrim::patternAt(coast).since == QStringLiteral("1.20"),
              "★ ZAJ5 海岸是 1.20 加入的");

        int flow = -1;
        for (int i = 0; i < ArmorTrim::patternCount(); ++i) {
            if (ArmorTrim::patternAt(i).name == QStringLiteral("涡流")) flow = i;
        }
        check(flow >= 0 && ArmorTrim::patternAt(flow).since == QStringLiteral("1.21")
                  && ArmorTrim::patternSource(flow).contains(QStringLiteral("试炼密室")),
              "★★ ZAJ5 涡流是 1.21 的试炼密室模板",
              ArmorTrim::patternAt(flow).since);

        // 复制材料抄的是游戏 `data/minecraft/recipe/*_armor_trim_smithing_template.json`
        // 里那个 `C` 位；抽三枚不同的（石英那组里挑一枚、海晶石、1.21 的旋风棒）钉住。
        check(ArmorTrim::patternDuplicate(coast) == QStringLiteral("圆石")
                  && ArmorTrim::patternDuplicate(5) == QStringLiteral("末地石")
                  && ArmorTrim::patternDuplicate(ArmorTrim::patternCount() - 1)
                         == QStringLiteral("铜块"),
              "★★ ZAJ5 复制用的方块逐图案抄游戏配方（海岸=圆石、眼眸=末地石、镶铆=铜块）",
              ArmorTrim::patternDuplicate(coast) + QStringLiteral(" / ")
                  + ArmorTrim::patternDuplicate(5) + QStringLiteral(" / ")
                  + ArmorTrim::patternDuplicate(ArmorTrim::patternCount() - 1));
        int missingDuplicate = 0;
        for (int i = 0; i < ArmorTrim::patternCount(); ++i) {
            if (ArmorTrim::patternDuplicate(i).isEmpty()) ++missingDuplicate;
        }
        check(missingDuplicate == 0,
              "★★ ZAJ5 十八种图案**一个不缺**复制材料（不留「以游戏内为准」）",
              QString::number(ArmorTrim::patternCount()) + QStringLiteral(" 种"));
        check(ArmorTrim::patternDuplicate(-1).isEmpty(),
              "★ ZAJ5 图案下标越界 ⇒ 复制材料是空串（不编一个方块出来）");

        const ArmorTrim::Plan single = ArmorTrim::plan(4, 1, coast, 0, 1);
        check(single.notes.join(QStringLiteral(" / ")).contains(QStringLiteral("这枚海岸纹饰模板来自：沉船")),
              "★★ ZAJ5 单件计划里也写清模板从哪儿来",
              single.notes.join(QStringLiteral(" / ")));
        check(single.notes.join(QStringLiteral(" / ")).contains(QStringLiteral("复制用圆石"))
                  && !single.notes.join(QStringLiteral(" / ")).contains(QStringLiteral("以游戏内配方为准")),
              "★★ ZAJ5 来源和复制材料写在同一句里，不再留一句「以游戏内配方为准」",
              single.notes.join(QStringLiteral(" / ")));
        const ArmorTrim::Plan flowPlan = ArmorTrim::plan(4, 1, flow, 0, 1);
        check(flowPlan.ok
                  && flowPlan.notes.join(QStringLiteral(" / ")).contains(QStringLiteral("1.21")),
              "★★ ZAJ5 1.21 的模板会提醒版本（更早的版本里没有）",
              flowPlan.notes.join(QStringLiteral(" / ")));
        const ArmorTrim::Plan resinPlan =
            ArmorTrim::plan(4, 1, coast, ArmorTrim::trimMaterialCount() - 1, 1);
        check(resinPlan.ok
                  && resinPlan.notes.join(QStringLiteral(" / ")).contains(QStringLiteral("树脂质"))
                  && resinPlan.notes.join(QStringLiteral(" / ")).contains(QStringLiteral("1.21.4")),
              "★★ ZAJ5 1.21.4 才有的材料也会提醒版本",
              resinPlan.notes.join(QStringLiteral(" / ")));

        const ArmorTrim::Plan whole = ArmorTrim::plan(4, 0, coast, 0, 1, true);
        check(whole.ok && whole.summary.contains(QStringLiteral("钻石四件套")),
              "★★ ZAJ5 整套 = 四件套（部位那格不参与）", whole.summary);
        check(whole.materials.contains(QStringLiteral("海岸纹饰模板 × 4"))
                  && whole.materials.contains(QStringLiteral("紫水晶碎片 × 4"))
                  && whole.materials.contains(QStringLiteral("钻石头盔 × 1"))
                  && whole.materials.contains(QStringLiteral("钻石胸甲 × 1"))
                  && whole.materials.contains(QStringLiteral("钻石护腿 × 1"))
                  && whole.materials.contains(QStringLiteral("钻石靴子 × 1")),
              "★★ ZAJ5 整套材料：模板与材料各 4 份、四件盔甲各 1 件",
              whole.materials.join(QStringLiteral(" / ")));
        check(whole.steps.size() == 5,
              "★ ZAJ5 整套是四次锻造（总说明 + 四件各一步）",
              QString::number(whole.steps.size()));

        const int turtle = ArmorTrim::armorMaterialCount() - 1;
        const ArmorTrim::Plan turtleSet = ArmorTrim::plan(turtle, 0, coast, 0, 1, true);
        check(!turtleSet.ok && turtleSet.error.contains(QStringLiteral("只能用作头部装备")),
              "★★ ZAJ5 海龟壳没有整套 ⇒ 报错，不悄悄只做头盔", turtleSet.error);
    }

    // ---------- ZAK. 哈希：算完之后文件变过没有 ----------
    //
    // 「文件变化使结果失效」：摘要算的是当时那一刻的字节，文件被重新下载
    // 之后屏幕上那串就不是"它现在的摘要"了。判据是**大小 + 修改时间**（只看这两样，不重读
    // 几百 MB）。这里在一个真的临时文件上钉它。
    {
        QTemporaryDir dir;
        check(dir.isValid(), "ZAK0 临时的窝造出来了");
        const QString path = dir.filePath(QStringLiteral("sample.bin"));
        QFile file(path);
        const bool wrote = file.open(QIODevice::WriteOnly)
                           && file.write(QByteArray(1024, 'a')) == 1024;
        file.close();
        check(wrote, "ZAK0 临时文件写进去了");

        const FileHash::Stamp first = FileHash::stampOf(path);
        check(first.ok && first.bytes == 1024 && first.lastModifiedMs > 0,
              "★★ ZAK0 读得出来大小和修改时间",
              QStringLiteral("%1 / %2").arg(first.bytes).arg(first.lastModifiedMs));
        check(FileHash::sameStamp(first, FileHash::stampOf(path)),
              "★★ ZAK0 同一个文件连着读两次 ⇒ 判据说「没变」");

        QFile append(path);
        const bool appended =
            append.open(QIODevice::Append) && append.write(QByteArray(1, 'b')) == 1;
        append.close();
        const FileHash::Stamp grown = FileHash::stampOf(path);
        check(appended && grown.ok && grown.bytes == 1025
                  && !FileHash::sameStamp(first, grown),
              "★★ ZAK0 追加一个字节 ⇒ 判据说「变过」（大小对不上）",
              QStringLiteral("%1 → %2").arg(first.bytes).arg(grown.bytes));

        const FileHash::Stamp missing = FileHash::stampOf(dir.filePath(QStringLiteral("gone.bin")));
        check(!missing.ok && missing.bytes == -1 && missing.lastModifiedMs == -1
                  && !FileHash::sameStamp(first, missing),
              "★★ ZAK0 文件不在了 ⇒ 读不出来，而且**不能**和任何一份算成「没变」");
    }

    // ---------- ZAL. 世界只读概况（种子 / 玩家位置 / 已生成区域 / 大小） ----------
    //
    // 这一层是"世界地图第一阶段"的**读**那一半：`level.dat` 里那几格、`region/*.mca` 的
    // 8 KiB 头部表。这里分别用一棵手搭的树、一段合成的头部、一个真的临时世界目录钉住，
    // 不依赖任何真实存档。
    {
        // 手搭一棵最小可用的 `level.dat` 树（和游戏写出来的形状一致）。
        const auto makeNode = [](NbtTree::Type type) {
            NbtTree::Node node;
            node.type = type;
            return node;
        };
        const auto putField = [](NbtTree::Node *compound, const QString &name,
                                 const NbtTree::Node &node) {
            compound->fields.append(QPair<QString, NbtTree::Node>(name, node));
        };

        NbtTree::Document document;
        document.root.type = NbtTree::Type::Compound;
        NbtTree::Node data = makeNode(NbtTree::Type::Compound);

        NbtTree::Node worldGen = makeNode(NbtTree::Type::Compound);
        NbtTree::Node seed = makeNode(NbtTree::Type::Long);
        seed.number = 1234567890123LL;
        putField(&worldGen, QStringLiteral("seed"), seed);
        putField(&data, QStringLiteral("WorldGenSettings"), worldGen);

        NbtTree::Node player = makeNode(NbtTree::Type::Compound);
        NbtTree::Node pos = makeNode(NbtTree::Type::List);
        pos.itemType = NbtTree::Type::Double;
        for (const double value : {12.5, 64.0, -30.25}) {
            NbtTree::Node item = makeNode(NbtTree::Type::Double);
            item.real = value;
            pos.items.append(item);
        }
        putField(&player, QStringLiteral("Pos"), pos);
        NbtTree::Node dimension = makeNode(NbtTree::Type::String);
        dimension.text = QStringLiteral("minecraft:the_nether");
        putField(&player, QStringLiteral("Dimension"), dimension);
        putField(&data, QStringLiteral("Player"), player);

        for (const auto &spawn : {QPair<QString, qint64>(QStringLiteral("SpawnX"), 100),
                                  QPair<QString, qint64>(QStringLiteral("SpawnY"), 70),
                                  QPair<QString, qint64>(QStringLiteral("SpawnZ"), -200)}) {
            NbtTree::Node value = makeNode(NbtTree::Type::Int);
            value.number = spawn.second;
            putField(&data, spawn.first, value);
        }
        putField(&document.root, QStringLiteral("Data"), data);

        WorldMap::Snapshot facts;
        WorldMap::readLevelFacts(document, &facts);
        check(facts.hasSeed && facts.seed == 1234567890123LL,
              "★★ ZAL0 1.16+ 的种子在 WorldGenSettings/seed 里",
              QString::number(facts.seed));
        check(facts.hasPlayer && facts.playerX == 12.5 && facts.playerY == 64.0
                  && facts.playerZ == -30.25,
              "★★ ZAL0 玩家位置是 Pos 列表里那三个 Double",
              QStringLiteral("%1 / %2 / %3")
                  .arg(facts.playerX)
                  .arg(facts.playerY)
                  .arg(facts.playerZ));
        check(facts.hasPlayerDimension
                  && facts.playerDimension == QStringLiteral("minecraft:the_nether"),
              "★ ZAL0 玩家维度原样读了回来（不翻成中文 —— 那一层是 controller 的事）",
              facts.playerDimension);
        check(facts.hasSpawn && facts.spawnX == 100 && facts.spawnY == 70
                  && facts.spawnZ == -200,
              "★ ZAL0 出生点三格（SpawnX / SpawnY / SpawnZ）");

        // 老存档：没有 WorldGenSettings、种子在 RandomSeed；`Data/Player` 一件都没有。
        NbtTree::Document oldDocument;
        oldDocument.root.type = NbtTree::Type::Compound;
        NbtTree::Node oldData = makeNode(NbtTree::Type::Compound);
        NbtTree::Node randomSeed = makeNode(NbtTree::Type::Long);
        randomSeed.number = -42;
        putField(&oldData, QStringLiteral("RandomSeed"), randomSeed);
        putField(&oldDocument.root, QStringLiteral("Data"), oldData);

        WorldMap::Snapshot oldFacts;
        WorldMap::readLevelFacts(oldDocument, &oldFacts);
        check(oldFacts.hasSeed && oldFacts.seed == -42,
              "★★ ZAL1 没有 WorldGenSettings 的老存档退回 RandomSeed（连负数也照读）",
              QString::number(oldFacts.seed));
        check(!oldFacts.hasPlayer && !oldFacts.hasPlayerDimension && !oldFacts.hasSpawn,
              "★★ ZAL1 没有 Data/Player 就**不编**玩家位置（从没进过的世界 / 服务端存档）");
        check(oldFacts.playerX == 0.0 && !oldFacts.chunkBounds.ok,
              "★ ZAL1 读不到的那几位留的是 `hasXxx = false`，不是拿 0 当成真值");

        // 老存档的维度是一个 Int：-1 = 下界、0 = 主世界、1 = 末地；别的数字**不编名字**。
        NbtTree::Document legacyDimension;
        legacyDimension.root.type = NbtTree::Type::Compound;
        NbtTree::Node legacyData = makeNode(NbtTree::Type::Compound);
        NbtTree::Node legacyPlayer = makeNode(NbtTree::Type::Compound);
        NbtTree::Node legacyValue = makeNode(NbtTree::Type::Int);
        legacyValue.number = -1;
        putField(&legacyPlayer, QStringLiteral("Dimension"), legacyValue);
        putField(&legacyData, QStringLiteral("Player"), legacyPlayer);
        putField(&legacyDimension.root, QStringLiteral("Data"), legacyData);
        WorldMap::Snapshot legacyFacts;
        WorldMap::readLevelFacts(legacyDimension, &legacyFacts);
        check(legacyFacts.hasPlayerDimension
                  && legacyFacts.playerDimension == QStringLiteral("minecraft:the_nether"),
              "★ ZAL1 老存档的 Dimension 是 Int（-1 ⇒ 下界），照样认",
              legacyFacts.playerDimension);

        // ---------- 区域文件头 ----------
        const auto locationEntry = [](QByteArray *header, int index, quint32 sectorOffset) {
            char *bytes = header->data();
            bytes[index * 4] = static_cast<char>((sectorOffset >> 16) & 0xFF);
            bytes[index * 4 + 1] = static_cast<char>((sectorOffset >> 8) & 0xFF);
            bytes[index * 4 + 2] = static_cast<char>(sectorOffset & 0xFF);
            bytes[index * 4 + 3] = 1;  // 占 1 个扇区
        };

        QByteArray header(8192, '\0');
        locationEntry(&header, 0, 2);        // 本地 (0,0)
        locationEntry(&header, 1023, 5);     // 本地 (31,31)
        int chunks = 0;
        WorldMap::ChunkBounds bounds;
        check(WorldMap::parseRegionHeader(header, 2, -1, &chunks, &bounds),
              "★★ ZAL2 8 KiB 的头部解出来了");
        check(chunks == 2, "★★ ZAL2 非空的位置表项才是一个已生成区块",
              QString::number(chunks));
        check(bounds.ok && bounds.minX == 64 && bounds.maxX == 95 && bounds.minZ == -32
                  && bounds.maxZ == -1,
              "★★ ZAL2 区块坐标 = 区域坐标 × 32 + 本地下标（负区域坐标也要对）",
              QStringLiteral("x %1~%2、z %3~%4")
                  .arg(bounds.minX)
                  .arg(bounds.maxX)
                  .arg(bounds.minZ)
                  .arg(bounds.maxZ));
        check(bounds.minBlockX() == 1024 && bounds.maxBlockX() == 1535
                  && bounds.minBlockZ() == -512 && bounds.maxBlockZ() == -1,
              "★★ ZAL2 方块坐标 = 区块坐标 × 16，右边界是 `(max+1)*16-1`（不是直接乘）",
              QStringLiteral("x %1~%2、z %3~%4")
                  .arg(bounds.minBlockX())
                  .arg(bounds.maxBlockX())
                  .arg(bounds.minBlockZ())
                  .arg(bounds.maxBlockZ()));

        const QByteArray emptyHeader(8192, '\0');
        chunks = -7;
        bounds = WorldMap::ChunkBounds();
        check(WorldMap::parseRegionHeader(emptyHeader, 0, 0, &chunks, &bounds) && chunks == 0
                  && !bounds.ok,
              "★★ ZAL3 全零的头部 = 一个区块都没生成（不是「一个包络 0~0」）");
        check(!WorldMap::parseRegionHeader(QByteArray(100, '\0'), 0, 0, nullptr, nullptr),
              "★★ ZAL3 头部短于 8 KiB ⇒ 如实说解不出来（不把半截表当空的）");

        // ---------- 真的扫一个临时世界 ----------
        QTemporaryDir worldRoot;
        check(worldRoot.isValid(), "ZAL4 临时的窝造出来了");
        const QString worldDir = worldRoot.filePath(QStringLiteral("world"));
        QDir().mkpath(worldDir + QStringLiteral("/region"));
        {
            QByteArray realHeader(8192, '\0');
            locationEntry(&realHeader, 0, 2);
            locationEntry(&realHeader, 33, 3);   // 本地 (1,1)
            locationEntry(&realHeader, 1023, 9); // 本地 (31,31)
            QFile regionFile(worldDir + QStringLiteral("/region/r.0.0.mca"));
            if (regionFile.open(QIODevice::WriteOnly)) {
                regionFile.write(realHeader);
            }
        }
        {
            // 一个头部读不全的 `.mca`：不计入区块数与范围，但 `unreadableRegions` 要记一笔。
            QFile broken(worldDir + QStringLiteral("/region/r.1.0.mca"));
            if (broken.open(QIODevice::WriteOnly)) {
                broken.write(QByteArray(100, '\0'));
            }
        }
        {
            QFile notes(worldDir + QStringLiteral("/notes.txt"));
            if (notes.open(QIODevice::WriteOnly)) {
                notes.write(QByteArray(5, 'x'));
            }
        }

        const WorldMap::Snapshot map = WorldMap::scan(worldDir);
        check(map.ok && map.hasRegionDir, "★★ ZAL4 扫一个真的世界目录成功了");
        check(map.regions.size() == 2 && map.regions.at(0).x == 0 && map.regions.at(0).z == 0
                  && map.regions.at(1).x == 1 && map.regions.at(1).z == 0,
              "★★ ZAL4 两个区域文件都认出来了，而且是按 (x, z) 排的",
              QString::number(map.regions.size()));
        check(map.chunkCount == 3 && map.chunkBounds.ok && map.chunkBounds.minBlockX() == 0
                  && map.chunkBounds.maxBlockX() == 511 && map.chunkBounds.maxBlockZ() == 511,
              "★★ ZAL4 区块数与范围只来自那个读得全的文件",
              QStringLiteral("chunks %1、x %2~%3、z %4~%5")
                  .arg(map.chunkCount)
                  .arg(map.chunkBounds.minBlockX())
                  .arg(map.chunkBounds.maxBlockX())
                  .arg(map.chunkBounds.minBlockZ())
                  .arg(map.chunkBounds.maxBlockZ()));
        check(map.unreadableRegions == 1 && !map.regions.at(1).headerRead,
              "★★ ZAL4 头部读不全的那个 `.mca` 记了一笔（界面要把「可能不完整」说出来）",
              QString::number(map.unreadableRegions));
        check(map.dirBytes == 8192 + 100 + 5 && map.fileCount == 3,
              "★★ ZAL4 大小是**整个目录**递归加起来（区域文件 + 坏的那个 + 别的小文件）",
              QStringLiteral("%1 字节 / %2 个文件").arg(map.dirBytes).arg(map.fileCount));

        // 没有 `region/` 的世界（刚建好、还没存过档）不算错。
        const QString emptyWorld = worldRoot.filePath(QStringLiteral("empty"));
        QDir().mkpath(emptyWorld);
        {
            QFile level(emptyWorld + QStringLiteral("/level.dat"));
            if (level.open(QIODevice::WriteOnly)) {
                level.write(QByteArray(16, '\0'));
            }
        }
        const WorldMap::Snapshot empty = WorldMap::scan(emptyWorld);
        check(empty.ok && !empty.hasRegionDir && empty.regions.isEmpty() && empty.chunkCount == 0
                  && !empty.chunkBounds.ok,
              "★★ ZAL4 没有 region/ 的世界不是错（`region/` 本来就可以不在）");

        const WorldMap::Snapshot gone =
            WorldMap::scan(worldRoot.filePath(QStringLiteral("nowhere")));
        check(!gone.ok && !gone.problem.isEmpty(),
              "★ ZAL4 目录不在了 ⇒ 明说一句（不是悄悄回一份空的）");
    }

    // ---------- ZAM. 运行输出共用模型（游戏日志窗 / 服务端控制台同一份） ----------
    //
    // 这一节钉的是"两页共用"的那一半：认级别、独立记录计数、档位筛选、搜索、两条上限
    // （容量 vs 显示）、三份导出文本。`GameLogWindow` 现在整个转发到它，`ServerConsolePage`
    // 也拿它记账 —— 所以这一节红了就是两页一起红。
    {
        using Level = RuntimeOutputModel::Level;

        // 认级别：两条规则 + 认不出来回 fallback。
        check(RuntimeOutputModel::classifyLine(
                  QStringLiteral("[12:34:56] [main/INFO] [net.minecraft.Foo] hi"), Level::Warn) ==
                  Level::Info &&
                  RuntimeOutputModel::classifyLine(
                      QStringLiteral("[Chunk Render Task 12/WARN] slow"), Level::Info) ==
                      Level::Warn,
              "★★ ZAM0 行首连着的方括号里认级别（原版 / 长线程名都认）");
        check(RuntimeOutputModel::classifyLine(QStringLiteral("12:34:56 ERROR: boom"),
                                               Level::Info) == Level::Error &&
                  RuntimeOutputModel::classifyLine(QStringLiteral("Loaded 3 INFO files"),
                                                   Level::Debug) == Level::Debug,
              "★★ ZAM0 裸词**必须带冒号**才算级别（`Loaded 3 INFO files` 不算；认不出来回 "
              "fallback）");
        check(RuntimeOutputModel::classifyLine(QStringLiteral("[TRACE] x"), Level::Info) ==
                      Level::Debug &&
                  RuntimeOutputModel::classifyLine(QStringLiteral("[SEVERE] x"), Level::Info) ==
                      Level::Error &&
                  RuntimeOutputModel::classifyLine(QStringLiteral("[FATAL] x"), Level::Info) ==
                      Level::Fatal,
              "★ ZAM0 TRACE→Debug、SEVERE→Error、FATAL→Fatal（同一个意思的不同写法）");

        RuntimeOutputModel model(100);
        model.addLine(QStringLiteral("[main/INFO] hello"));
        const Level err = model.addLine(QStringLiteral("[main/ERROR] boom"));
        model.addLine(QStringLiteral("    at net.minecraft.Foo(Bar.java:1)"));
        model.addLine(QString());
        model.addLine(QStringLiteral("plain line"));
        check(err == Level::Error && model.lineCount() == 5 &&
                  model.countFor(Level::Info) == 2 && model.countFor(Level::Error) == 1,
              "★★ ZAM1 一条 ERROR 后面的崩栈是**续行**：跟着染色、不重复计数；空行是分隔，"
              "也不算一条 INFO",
              QStringLiteral("Info=%1 Error=%2 行=%3")
                  .arg(model.countFor(Level::Info))
                  .arg(model.countFor(Level::Error))
                  .arg(model.lineCount()));
        check(model.lines().at(2).level == Level::Error && !model.lines().at(2).counted &&
                  model.lines().at(0).counted,
              "★★ ZAM1 续行的级别跟着上一行走（染色靠它），但 `counted` 是 false");

        // 容量（内存的闸）：超了丢最老的，计数跟着减。
        RuntimeOutputModel capped(3);
        for (int i = 0; i < 5; ++i) {
            capped.addLine(QStringLiteral("[main/INFO] line%1").arg(i));
        }
        check(capped.lineCount() == 3 && capped.countFor(Level::Info) == 3 &&
                  capped.lines().first().text == QStringLiteral("[main/INFO] line2"),
              "★★ ZAM2 容量是**内存**的闸：超了丢最老的，计数跟着减（不是只裁视图）",
              QStringLiteral("%1 行，首行「%2」")
                  .arg(capped.lineCount())
                  .arg(capped.lines().isEmpty() ? QString() : capped.lines().first().text));

        check(capped.receivedLineCount() == 5 && capped.droppedLineCount() == 2 &&
                  capped.droppedLineCountFrom(Level::Info) == 2 &&
                  capped.droppedLineCountFrom(Level::Error) == 0,
              "★★ ZAM2b 挤掉的行要记账：收到的（5）= 留着的（3）+ 挤掉的（2），"
              "而且按每一行自己的级别分开记",
              QStringLiteral("收到=%1 留着=%2 挤掉=%3（Info=%4 Error=%5）")
                  .arg(capped.receivedLineCount())
                  .arg(capped.lineCount())
                  .arg(capped.droppedLineCount())
                  .arg(capped.droppedLineCountFrom(Level::Info))
                  .arg(capped.droppedLineCountFrom(Level::Error)));

        RuntimeOutputModel cappedMix(3);
        cappedMix.addLine(QStringLiteral("[main/ERROR] e1"));
        cappedMix.addLine(QStringLiteral("[main/ERROR] e2"));
        cappedMix.addLine(QStringLiteral("[main/INFO] i1"));
        cappedMix.addLine(QStringLiteral("[main/INFO] i2"));
        check(cappedMix.droppedLineCount() == 1 &&
                  cappedMix.droppedLineCountFrom(Level::Error) == 1 &&
                  cappedMix.droppedLineCountFrom(Level::Warn) == 1 &&
                  cappedMix.droppedLineCountFrom(Level::Fatal) == 0 &&
                  cappedMix.receivedLineCount() == 4,
              "★★ ZAM2c droppedLineCountFrom() 是从那一级往上求和（导出报告问"
              "「错误与致命挤掉了几行」用的就是 Error）",
              QStringLiteral("挤掉=%1 Error起=%2 Warn起=%3 Fatal起=%4")
                  .arg(cappedMix.droppedLineCount())
                  .arg(cappedMix.droppedLineCountFrom(Level::Error))
                  .arg(cappedMix.droppedLineCountFrom(Level::Warn))
                  .arg(cappedMix.droppedLineCountFrom(Level::Fatal)));

        // `setCapacity()` 往下缩也是同一条路（用户在设置里调小上限 / 日志窗改容量）。
        cappedMix.setCapacity(1);
        check(cappedMix.lineCount() == 1 && cappedMix.droppedLineCount() == 3 &&
                  cappedMix.receivedLineCount() == 4 && cappedMix.countFor(Level::Info) == 1,
              "★★ ZAM2d 缩小容量走的是同一条记账路（挤掉的 3 行 = 两条 ERROR + 一条 INFO）",
              QStringLiteral("留着=%1 挤掉=%2 收到=%3")
                  .arg(cappedMix.lineCount())
                  .arg(cappedMix.droppedLineCount())
                  .arg(cappedMix.receivedLineCount()));

        // 「清空」是主动不要这些行了：之后再问"原始共几行"必须从零开始，
        // 不然用户清空之后导出的报告还挂着上一次那一大截。
        capped.clear();
        check(capped.receivedLineCount() == 0 && capped.droppedLineCount() == 0 &&
                  capped.droppedLineCountFrom(Level::Info) == 0,
              "★★ ZAM2e clear() 把挤掉的那一份也归零（收到的行数跟着回 0）",
              QStringLiteral("收到=%1 挤掉=%2")
                  .arg(capped.receivedLineCount())
                  .arg(capped.droppedLineCount()));

        // 档位筛选。
        RuntimeOutputModel filt(100);
        filt.addLine(QStringLiteral("[main/INFO] a"));
        filt.addLine(QStringLiteral("[main/WARN] b"));
        filt.addLine(QStringLiteral("[main/ERROR] c"));
        filt.addLine(QStringLiteral("[main/WARN] d"));
        filt.setLevelVisible(Level::Warn, false);
        check(filt.matchingLineCount() == 2 && filt.visibleLines().size() == 2 &&
                  filt.visibleText() == QStringLiteral("[main/INFO] a\n[main/ERROR] c\n"),
              "★★ ZAM3 关掉一档 ⇒ 视图里就没有那几行（模型和计数一行都没少）",
              filt.visibleText());
        check(filt.countFor(Level::Warn) == 2 && filt.lineCount() == 4,
              "★★ ZAM3 被筛掉的行**照样在模型里、照样计数**（数字不会因为关一档就变小）");

        // 搜索：大小写不分的子串，和档位筛选取交集。
        filt.setLevelVisible(Level::Warn, true);
        filt.setSearchText(QStringLiteral("C"));
        check(filt.visibleLines().size() == 1 &&
                  filt.visibleLines().first().text == QStringLiteral("[main/ERROR] c"),
              "★★ ZAM4 搜索是大小写不分的子串（`c` 命中 `[main/ERROR] c`），只影响视图",
              QStringLiteral("%1 行").arg(filt.visibleLines().size()));
        check(filt.allText().contains(QStringLiteral("[main/INFO] a")) &&
                  filt.textFrom(Level::Error) == QStringLiteral("[main/ERROR] c\n"),
              "★★ ZAM4 导出拿的是**整份**（搜索框不会把导出的内容吃掉）");
        filt.setSearchText(QString());
        check(filt.matchingLineCount() == 4, "★ ZAM4 清掉搜索词 ⇒ 四行都回来了");

        // 两条上限不是一回事：显示上限只留最后 N 行，模型不动。
        filt.setDisplayLimit(2);
        check(filt.visibleLines().size() == 2 &&
                  filt.visibleLines().last().text == QStringLiteral("[main/WARN] d") &&
                  filt.lineCount() == 4,
              "★★ ZAM5 显示上限只留**最后** N 行；模型一行不动（换回大的一档就回来）");
        filt.setDisplayLimit(0);
        check(filt.visibleLines().size() == 4, "★ ZAM5 「全部」= 0 ⇒ 过筛的都摆出来");

        // 导出文本的三份口径。
        RuntimeOutputModel exp(100);
        exp.addLine(QStringLiteral("[main/ERROR] boom"));
        exp.addLine(QStringLiteral("    at Foo"));
        exp.addLine(QStringLiteral("[main/INFO] ok"));
        check(exp.allText() == QStringLiteral("[main/ERROR] boom\n    at Foo\n[main/INFO] ok\n"),
              "★★ ZAM6 `allText()` 一行一句、末尾有换行（末尾那半行由日志窗自己接）");
        check(exp.textFrom(Level::Error) == QStringLiteral("[main/ERROR] boom\n    at Foo\n"),
              "★★ ZAM6 `textFrom(Error)` 连**续行**一起挑（级别是模型记着的，不是再匹配一遍）");

        // 行数那几档（两页共用同一份文案）。
        check(RuntimeOutputModel::lineLimitChoices() ==
                  QVector<int>{500, 1000, 2000, 5000, 10000, 0} &&
                  RuntimeOutputModel::lineLimitText(1000) == QStringLiteral("1000 条") &&
                  RuntimeOutputModel::lineLimitText(0) == QStringLiteral("全部") &&
                  RuntimeOutputModel::lineLimitOfIndex(5) == 0,
              "★★ ZAM7 「显示多少行」六档只有一处出处（1000 写成「1000 条」不是「1k」，"
              "最后一档 0 = 全部）");
    }
}

