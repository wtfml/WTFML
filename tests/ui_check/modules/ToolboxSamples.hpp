// 工具箱九片的样例数据（toolbox-layout 与 review-shots 共用）。
#pragma once

#include "ToolboxPage.hpp"

#include <QColor>
#include <QImage>

/// 平时由 ToolboxController 灌的几张表；这里灌一份样例，版式才接近真实界面。
/// 和 controller 一样按片灌：哪一片搭出来（`toolBuilt`）才灌哪一片。
inline void fillToolboxSamples(ToolboxPage &page, ToolboxPage::Tool tool) {
    using Tool = ToolboxPage::Tool;
    switch (tool) {
    case Tool::Anvil: {
        page.setEnchantmentChoices({{QStringLiteral("protection"), QStringLiteral("保护（Protection，上限 4 级）")},
                                    {QStringLiteral("unbreaking"), QStringLiteral("耐久（Unbreaking，上限 3 级）")},
                                    {QStringLiteral("mending"), QStringLiteral("经验修补（Mending，上限 1 级）")},
                                    {QStringLiteral("thorns"), QStringLiteral("荆棘（Thorns，上限 3 级）")}});
        page.setAnvilRows({{QStringLiteral("protection"), 4, false, 1, false},
                           {QStringLiteral("unbreaking"), 3, true, 1, true},
                           {QStringLiteral("mending"), 1, true, 0, false},
                           {QStringLiteral("thorns"), 3, true, 0, false}});
        ToolboxPage::AnvilView view;
        view.ok = true;
        view.summary = QStringLiteral("合计 14 级（每一步都在 39 级以内），合并后为 保护 IV、耐久 III。");
        view.steps = {QStringLiteral("第 1 步：左槽 经验修补的书 + 右槽 荆棘 III 的书 → 经验修补、荆棘 III 的书（代价 6 级）"),
                      QStringLiteral("第 2 步：左槽 胸甲 + 右槽 第 1 步的合并产物 → 胸甲（代价 8 级）")};
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
        for (int i = 0; i < 16; ++i) palette.append({codes.mid(i, 1), QStringLiteral("颜色"), colors.at(i), true});
        for (const auto &pair : {qMakePair(QStringLiteral("k"), QStringLiteral("乱码")), qMakePair(QStringLiteral("l"), QStringLiteral("粗体")),
                                 qMakePair(QStringLiteral("m"), QStringLiteral("删除线")), qMakePair(QStringLiteral("n"), QStringLiteral("下划线")),
                                 qMakePair(QStringLiteral("o"), QStringLiteral("斜体")), qMakePair(QStringLiteral("r"), QStringLiteral("复位"))})
            palette.append({pair.first, pair.second, QString(), false});
        page.setTextPalette(palette);
        page.setTextSource(QStringLiteral("欢迎来到 §b服务器§r！"));
        page.setTextPreview(QStringLiteral("欢迎来到 <span style=\"color:#55ffff\">服务器</span>！"));
        page.setTextOutputs(QStringLiteral("欢迎来到 §b服务器§r！"),
                            QStringLiteral("{\"text\":\"欢迎来到 \",\"extra\":[{\"text\":\"服务器\",\"color\":\"aqua\"},{\"text\":\"！\",\"color\":\"white\"}]}"));
        break;
    }
    case Tool::Coord:
        page.setDimensionChoices({QStringLiteral("主世界"), QStringLiteral("下界"), QStringLiteral("末地")});
        page.setCoordinateRows({QStringLiteral("下界：x 10.00 y 64.00 z -9.50"), QStringLiteral("区块 (0, -1) 一带"),
                                QStringLiteral("region 文件 r.0.-1.mca")});
        break;
    case Tool::Server: {
        ToolboxPage::ServerView server;
        server.ok = true;
        server.address = QStringLiteral("mc.example.com:25565");
        server.motd = QStringLiteral("欢迎来到服务器");
        server.version = QStringLiteral("Paper 1.21.4");
        server.players = QStringLiteral("128 / 500");
        server.latency = QStringLiteral("42 ms");
        page.setServerAddress(server.address);
        page.setServerView(server);
        break;
    }
    case Tool::Beacon:
        page.setBeaconMaterials({QStringLiteral("铁块"), QStringLiteral("金块"), QStringLiteral("钻石块"),
                                 QStringLiteral("绿宝石块"), QStringLiteral("下界合金块")});
        page.setBeamColors({QStringLiteral("白色"), QStringLiteral("橙色"), QStringLiteral("蓝色")});
        page.setBeaconResult(QStringLiteral(
            "4 级信标：范围 50 格，底座共 164 块。\n• 第 4 层：9 × 9 = 81 块（最上面那层固定 3 × 3）\n"
            "• 效果：速度、急迫、抗性提升、跳跃提升、力量、再生\n• 信标放在金字塔正中间，上方留出天空（可用玻璃）\n\n"
            "164 块铁块 = 1476 个铁锭（每块 9 个）\n每颗信标另需 3 块黑曜石 + 1 颗下界之星 + 5 块玻璃（合成表：底下 3 块黑曜石、"
            "中间一颗下界之星、四周 5 块玻璃）。"));
        page.setBeamLayers({0, 1, 2, 1, 0});
        page.setBeamResult({QColor(255, 255, 255), QColor(255, 140, 0), QColor(0, 0, 255), QColor(255, 140, 0), QColor(255, 255, 255)},
                           QStringLiteral("第 1 层 / 白色玻璃 → 白\n第 2 层 / 橙色玻璃 → 橙\n第 3 层 / 蓝色玻璃 → 蓝\n"
                                          "第 4 层 / 橙色玻璃 → 橙\n第 5 层 / 白色玻璃 → 白\n最终颜色：#c9c9c9 / RGB(201, 201, 201)\n"
                                          "材料：白色玻璃 × 2、橙色玻璃 × 2、蓝色玻璃 × 1"));
        break;
    case Tool::Brewing:
        page.setBrewingPotions({QStringLiteral("迅捷"), QStringLiteral("力量"), QStringLiteral("治疗")});
        page.setBrewingForms({QStringLiteral("普通"), QStringLiteral("喷溅型"), QStringLiteral("滞留型")});
        page.setBrewingVersions({QString(), QStringLiteral("1.21"), QStringLiteral("1.20")});
        page.setBrewingResult(QStringLiteral(
            "产出：迅捷药水 × 1 瓶（酿 1 次）\n效果：迅捷 I · 3:00\n"
            "步骤（共 2 步）：\n1. 水瓶 + 下界疣 → 粗制的药水\n2. 粗制的药水 + 糖 → 迅捷药水\n"
            "材料：水瓶 × 1、下界疣 × 1、糖 × 1、烈焰粉（燃料） × 1（1 个烈焰粉可酿 20 次）"));
        break;
    case Tool::Banner:
        page.setBannerColors({QStringLiteral("白色"), QStringLiteral("橙色"), QStringLiteral("品红色"), QStringLiteral("黑色")},
                             {QColor(0xf9fffe), QColor(0xf9801d), QColor(0xc74ebd), QColor(0x1d1d21)});
        page.setBannerPatterns({QStringLiteral("条纹"), QStringLiteral("十字"), QStringLiteral("边框")});
        page.setBannerVersions({QString(), QStringLiteral("1.21")});
        // 真机上空贴图时 controller 给的就是这一句（`ToolboxController::refreshBanner`），此处照抄，
        // 验收图里说明句的宽度与折行才和产品一致。
        page.setBannerPreview(QImage(), QStringLiteral("加载本地游戏贴图后可预览图案。"));
        page.setBannerDesign(0, {QStringLiteral("0:1"), QStringLiteral("1:2"), QStringLiteral("2:3"), QStringLiteral("0:0"),
                                 QStringLiteral("1:1"), QStringLiteral("2:2")}, QString());
        page.setBannerResult(QStringLiteral(
            "底色 白色 · 6/6 层\n\n一步一步：\n• 先做一面白色旗帜：6 块白色羊毛 + 1 根木棍\n• 第 1 层：条纹（橙色）\n• 第 2 层：十字（品红色）\n"
            "• 第 3 层：边框（黑色）\n• 第 4 层：条纹（白色）\n• 第 5 层：十字（橙色）\n• 第 6 层：边框（品红色）\n\n"
            "材料清单：\n• 白色羊毛 × 6\n• 木棍 × 1\n• 橙色染料 × 2\n• 品红色染料 × 2\n• 黑色染料 × 1\n\n"
            "指令（1.13–1.20.4 的 NBT 写法；1.20.5 起换成了组件写法，这一版不出）：\n"
            "/give @p white_banner{BlockEntityTag:{Patterns:[{Pattern:ss,Color:2},{Pattern:cr,Color:6},{Pattern:bo,Color:15}]}}"));
        break;
    case Tool::Trim:
        page.setTrimArmorMaterials({QStringLiteral("下界合金"), QStringLiteral("钻石"), QStringLiteral("铁")});
        page.setTrimParts({QStringLiteral("头盔"), QStringLiteral("胸甲"), QStringLiteral("护腿"), QStringLiteral("靴子")});
        page.setTrimPatterns({QStringLiteral("海岸"), QStringLiteral("哨兵"), QStringLiteral("尖塔")});
        page.setTrimMaterials({QStringLiteral("紫水晶"), QStringLiteral("金"), QStringLiteral("红石")});
        page.setTrimVersions({QString(), QStringLiteral("1.21")});
        page.setTrimResult(QStringLiteral("要准备：海岸锻造模板 × 2、紫水晶 × 4"));
        break;
    case Tool::Download:
        break;
    }
}

/// 页一搭出哪一片就灌哪一片样例（与 ToolboxController 同一时机）。
inline void wireToolboxSamples(ToolboxPage &page) {
    QObject::connect(&page, &ToolboxPage::toolBuilt, &page, [&page](ToolboxPage::Tool tool) { fillToolboxSamples(page, tool); });
}
