// 工具箱九片的样例数据（toolbox-layout 与 review-shots 共用）。
#pragma once

#include "ToolboxPage.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QLatin1String>

/// 当前进程是否已装英文界面翻译（用于切换演示夹具的语言）。
inline bool isEnglishDemo() {
    return QCoreApplication::translate("MainWindowUI", "主页") != QStringLiteral("主页");
}

/// 按当前界面语言挑中/英演示文案（仅用于测试夹具中的假数据）。
inline QString demoText(const char *zh, const char *en) {
    return QString::fromUtf8(isEnglishDemo() ? en : zh);
}

/// 平时由 ToolboxController 灌的几张表；这里灌一份样例，版式才接近真实界面。
/// 和 controller 一样按片灌：哪一片搭出来（`toolBuilt`）才灌哪一片。
inline void fillToolboxSamples(ToolboxPage &page, ToolboxPage::Tool tool) {
    using Tool = ToolboxPage::Tool;
    switch (tool) {
    case Tool::Anvil: {
        page.setEnchantmentChoices({{QStringLiteral("protection"), demoText("保护（Protection，上限 4 级）", "Protection (Max IV)")},
                                    {QStringLiteral("unbreaking"), demoText("耐久（Unbreaking，上限 3 级）", "Unbreaking (Max III)")},
                                    {QStringLiteral("mending"), demoText("经验修补（Mending，上限 1 级）", "Mending (Max I)")},
                                    {QStringLiteral("thorns"), demoText("荆棘（Thorns，上限 3 级）", "Thorns (Max III)")}});
        page.setAnvilRows({{QStringLiteral("protection"), 4, false, 1, false},
                           {QStringLiteral("unbreaking"), 3, true, 1, true},
                           {QStringLiteral("mending"), 1, true, 0, false},
                           {QStringLiteral("thorns"), 3, true, 0, false}});
        ToolboxPage::AnvilView view;
        view.ok = true;
        view.summary = demoText("合计 14 级（每一步都在 39 级以内），合并后为 保护 IV、耐久 III。",
                                "Total 14 levels (all steps within 39 levels); result has Protection IV, Unbreaking III.");
        view.steps = {demoText("第 1 步：左槽 经验修补的书 + 右槽 荆棘 III 的书 → 经验修补、荆棘 III 的书（代价 6 级）",
                               "Step 1: Book (Mending) + Book (Thorns III) → Book (Mending, Thorns III) (cost 6 levels)"),
                      demoText("第 2 步：左槽 胸甲 + 右槽 第 1 步的合并产物 → 胸甲（代价 8 级）",
                               "Step 2: Chestplate + Step 1 result → Chestplate (cost 8 levels)")};
        page.setAnvilView(view);
        break;
    }
    case Tool::Text: {
        QList<ToolboxPage::PaletteEntry> palette;
        const QStringList colors{QStringLiteral("#000000"), QStringLiteral("#0000aa"), QStringLiteral("#00aa00"),
                                 QStringLiteral("#00aaaa"), QStringLiteral("#aa0000"), QStringLiteral("#aa00aa"),
                                 QStringLiteral("#ffaa00"), QStringLiteral("#aaaaaa"), QStringLiteral("#555555"),
                                 QStringLiteral("#5555ff"), QStringLiteral("#55ff55"), QStringLiteral("#55ffff"),
                                 QStringLiteral("#ff5555"), QStringLiteral("#ff55ff"), QStringLiteral("#ffff55"),
                                 QStringLiteral("#ffffff")};
        const QString codes = QStringLiteral("0123456789abcdef");
        for (int i = 0; i < 16; ++i) palette.append({codes.mid(i, 1), demoText("颜色", "Color"), colors.at(i), true});
        for (const auto &pair : {qMakePair(QStringLiteral("k"), demoText("乱码", "Obfuscated")),
                                 qMakePair(QStringLiteral("l"), demoText("粗体", "Bold")),
                                 qMakePair(QStringLiteral("m"), demoText("删除线", "Strikethrough")),
                                 qMakePair(QStringLiteral("n"), demoText("下划线", "Underline")),
                                 qMakePair(QStringLiteral("o"), demoText("斜体", "Italic")),
                                 qMakePair(QStringLiteral("r"), demoText("复位", "Reset"))})
            palette.append({pair.first, pair.second, QString(), false});
        page.setTextPalette(palette);
        page.setTextSource(demoText("欢迎来到 §b服务器§r！", "Welcome to the §bServer§r!"));
        page.setTextPreview(demoText("欢迎来到 <span style=\"color:#55ffff\">服务器</span>！",
                                     "Welcome to the <span style=\"color:#55ffff\">Server</span>!"));
        page.setTextOutputs(
            demoText("欢迎来到 §b服务器§r！", "Welcome to the §bServer§r!"),
            demoText("{\"text\":\"欢迎来到 \",\"extra\":[{\"text\":\"服务器\",\"color\":\"aqua\"},{\"text\":\"！\",\"color\":\"white\"}]}",
                     "{\"text\":\"Welcome to the \",\"extra\":[{\"text\":\"Server\",\"color\":\"aqua\"},{\"text\":\"!\",\"color\":\"white\"}]}"));
        break;
    }
    case Tool::Coord:
        page.setDimensionChoices({QCoreApplication::translate("CoordConvert", "主世界"),
                                  QCoreApplication::translate("CoordConvert", "下界"),
                                  QCoreApplication::translate("CoordConvert", "末地")});
        page.setCoordinateRows({demoText("下界：x 10.00 y 64.00 z -9.50", "Nether: x 10.00 y 64.00 z -9.50"),
                                demoText("区块 (0, -1) 一带", "Chunk (0, -1)"),
                                demoText("region 文件 r.0.-1.mca", "Region file r.0.-1.mca")});
        break;
    case Tool::Server: {
        ToolboxPage::ServerView server;
        server.ok = true;
        server.address = QStringLiteral("mc.example.com:25565");
        server.motd = demoText("欢迎来到服务器", "Welcome to the server");
        server.version = QStringLiteral("Paper 1.21.4");
        server.players = QStringLiteral("128 / 500");
        server.latency = QStringLiteral("42 ms");
        page.setServerAddress(server.address);
        page.setServerView(server);
        break;
    }
    case Tool::Beacon:
        page.setBeaconMaterials({demoText("铁块", "Block of Iron"), demoText("金块", "Block of Gold"),
                                 demoText("钻石块", "Block of Diamond"), demoText("绿宝石块", "Block of Emerald"),
                                 demoText("下界合金块", "Block of Netherite")});
        page.setBeamColors({demoText("白色", "White"), demoText("橙色", "Orange"), demoText("蓝色", "Blue")});
        page.setBeaconResult(demoText(
            "4 级信标：范围 50 格，底座共 164 块。\n• 第 4 层：9 × 9 = 81 块（最上面那层固定 3 × 3）\n"
            "• 效果：速度、急迫、抗性提升、跳跃提升、力量、再生\n• 信标放在金字塔正中间，上方留出天空（可用玻璃）\n\n"
            "164 块铁块 = 1476 个铁锭（每块 9 个）\n每颗信标另需 3 块黑曜石 + 1 颗下界之星 + 5 块玻璃（合成表：底下 3 块黑曜石、"
            "中间一颗下界之星、四周 5 块玻璃）。",
            "Tier 4 Beacon: 50-block range, 164 base blocks total.\n• Layer 4: 9 × 9 = 81 blocks (top layer is always 3 × 3)\n"
            "• Effects: Speed, Haste, Resistance, Jump Boost, Strength, Regeneration\n"
            "• Place the beacon at the center of the pyramid with sky access above (glass is allowed)\n\n"
            "164 Blocks of Iron = 1,476 Iron Ingots (9 per block)\nEach beacon also requires 3 Obsidian + 1 Nether Star + 5 Glass."));
        page.setBeamLayers({0, 1, 2, 1, 0});
        page.setBeamResult({QColor(255, 255, 255), QColor(255, 140, 0), QColor(0, 0, 255), QColor(255, 140, 0), QColor(255, 255, 255)},
                           demoText("第 1 层 / 白色玻璃 → 白\n第 2 层 / 橙色玻璃 → 橙\n第 3 层 / 蓝色玻璃 → 蓝\n"
                                    "第 4 层 / 橙色玻璃 → 橙\n第 5 层 / 白色玻璃 → 白\n最终颜色：#c9c9c9 / RGB(201, 201, 201)\n"
                                    "材料：白色玻璃 × 2、橙色玻璃 × 2、蓝色玻璃 × 1",
                                    "Layer 1 / White Stained Glass → White\nLayer 2 / Orange Stained Glass → Orange\n"
                                    "Layer 3 / Blue Stained Glass → Blue\nLayer 4 / Orange Stained Glass → Orange\n"
                                    "Layer 5 / White Stained Glass → White\nFinal color: #c9c9c9 / RGB(201, 201, 201)\n"
                                    "Materials: White Stained Glass × 2, Orange Stained Glass × 2, Blue Stained Glass × 1"));
        break;
    case Tool::Brewing:
        page.setBrewingPotions({demoText("迅捷", "Swiftness"), demoText("力量", "Strength"), demoText("治疗", "Healing")});
        page.setBrewingForms({demoText("普通", "Normal"), demoText("喷溅型", "Splash"), demoText("滞留型", "Lingering")});
        page.setBrewingVersions({QString(), QStringLiteral("1.21"), QStringLiteral("1.20")});
        page.setBrewingResult(demoText(
            "产出：迅捷药水 × 1 瓶（酿造 1 次）\n效果：迅捷 I · 3:00\n"
            "步骤（共 2 步）：\n1. 水瓶 + 下界疣 → 粗制的药水\n2. 粗制的药水 + 糖 → 迅捷药水\n"
            "材料：水瓶 × 1、下界疣 × 1、糖 × 1、烈焰粉（燃料） × 1（1 个烈焰粉可酿造 20 次）",
            "Output: Potion of Swiftness × 1 (1 brew)\nEffect: Speed I · 3:00\n"
            "Steps (2 total):\n1. Water Bottle + Nether Wart → Awkward Potion\n2. Awkward Potion + Sugar → Potion of Swiftness\n"
            "Ingredients: Water Bottle × 1, Nether Wart × 1, Sugar × 1, Blaze Powder (fuel) × 1 (1 Blaze Powder fuels 20 brews)"));
        break;
    case Tool::Banner:
        page.setBannerColors({demoText("白色", "White"), demoText("橙色", "Orange"),
                              demoText("品红色", "Magenta"), demoText("黑色", "Black")},
                             {QColor(0xf9fffe), QColor(0xf9801d), QColor(0xc74ebd), QColor(0x1d1d21)});
        page.setBannerPatterns({demoText("条纹", "Stripe"), demoText("十字", "Cross"), demoText("边框", "Bordure")});
        page.setBannerVersions({QString(), QStringLiteral("1.21")});
        // 真机上空贴图时 controller 给的就是这一句（`ToolboxController::refreshBanner`），此处照抄，
        // 验收图里说明句的宽度与折行才和产品一致。
        page.setBannerPreview(QImage(), QCoreApplication::translate("ToolboxController", "加载本地游戏贴图后可预览图案。"));
        page.setBannerDesign(0, {QStringLiteral("0:1"), QStringLiteral("1:2"), QStringLiteral("2:3"), QStringLiteral("0:0"),
                                 QStringLiteral("1:1"), QStringLiteral("2:2")}, QString());
        page.setBannerResult(demoText(
            "底色 白色 · 6/6 层\n\n一步一步：\n• 先做一面白色旗帜：6 块白色羊毛 + 1 根木棍\n• 第 1 层：条纹（橙色）\n• 第 2 层：十字（品红色）\n"
            "• 第 3 层：边框（黑色）\n• 第 4 层：条纹（白色）\n• 第 5 层：十字（橙色）\n• 第 6 层：边框（品红色）\n\n"
            "材料清单：\n• 白色羊毛 × 6\n• 木棍 × 1\n• 橙色染料 × 2\n• 品红色染料 × 2\n• 黑色染料 × 1\n\n"
            "指令（1.13–1.20.4 的 NBT 写法；1.20.5 起换成了组件写法，这一版不出）：\n"
            "/give @p white_banner{BlockEntityTag:{Patterns:[{Pattern:ss,Color:2},{Pattern:cr,Color:6},{Pattern:bo,Color:15}]}}",
            "Base White · 6/6 layers\n\nSteps:\n• Craft a White Banner: 6 White Wool + 1 Stick\n• Layer 1: Stripe (Orange)\n• Layer 2: Cross (Magenta)\n"
            "• Layer 3: Bordure (Black)\n• Layer 4: Stripe (White)\n• Layer 5: Cross (Orange)\n• Layer 6: Bordure (Magenta)\n\n"
            "Materials:\n• White Wool × 6\n• Stick × 1\n• Orange Dye × 2\n• Magenta Dye × 2\n• Black Dye × 1\n\n"
            "Command (NBT syntax for 1.13–1.20.4; 1.20.5+ uses item components):\n"
            "/give @p white_banner{BlockEntityTag:{Patterns:[{Pattern:ss,Color:2},{Pattern:cr,Color:6},{Pattern:bo,Color:15}]}}"));
        break;
    case Tool::Trim:
        page.setTrimArmorMaterials({demoText("下界合金", "Netherite"), demoText("钻石", "Diamond"), demoText("铁", "Iron")});
        page.setTrimParts({demoText("头盔", "Helmet"), demoText("胸甲", "Chestplate"),
                           demoText("护腿", "Leggings"), demoText("靴子", "Boots")});
        page.setTrimPatterns({demoText("海岸", "Coast"), demoText("哨兵", "Sentry"), demoText("尖塔", "Spire")});
        page.setTrimMaterials({demoText("紫水晶", "Amethyst"), demoText("金", "Gold"), demoText("红石", "Redstone")});
        page.setTrimVersions({QString(), QStringLiteral("1.21")});
        page.setTrimResult(demoText("要准备：海岸锻造模板 × 2、紫水晶 × 4",
                                    "Required: Coast Armor Trim Smithing Template × 2, Amethyst Shard × 4"));
        break;
    case Tool::Download:
        break;
    }
}

/// 页一搭出哪一片就灌哪一片样例（与 ToolboxController 同一时机）。
inline void wireToolboxSamples(ToolboxPage &page) {
    QObject::connect(&page, &ToolboxPage::toolBuilt, &page, [&page](ToolboxPage::Tool tool) { fillToolboxSamples(page, tool); });
}
