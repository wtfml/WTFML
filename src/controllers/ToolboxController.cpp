/**
 * @file ToolboxController.cpp
 * @brief ToolboxController 的实现（为什么这么分、哪几件事不归它管，见头文件那一段）。
 */

#include "controllers/ToolboxController.hpp"
#include <QCoreApplication>
#include "controllers/ArmorTrimPreviewController.hpp"
#include "controllers/DirectDownloadController.hpp"
#include "controllers/ToolboxInputState.hpp"

#include "InlineMessage.hpp"
#include "MotdText.hpp"
#include "OverlayHost.hpp"
#include "ToolboxPage.hpp"
#include "tools/AnvilPlanner.hpp"
#include "tools/ArmorTrim.hpp"
#include "tools/Banner.hpp"
#include "tools/Beacon.hpp"
#include "tools/BeaconBeam.hpp"
#include "tools/BannerRender.hpp"
#include "tools/BannerDesign.hpp"
#include "tools/BannerTextures.hpp"
#include "tools/Brewing.hpp"
#include "tools/CoordConvert.hpp"
#include "tools/GameVersion.hpp"
#include "tools/TextFormat.hpp"
#include "util/ImageDecode.hpp"
#include "util/ServerPing.hpp"

#include <QBoxLayout>
#include <QColor>
#include <QFileDialog>
#include <QMetaObject>
#include <QPalette>
#include <QThread>

#include <algorithm>

namespace {

/// 一段附魔的显示名（`锋利 IV` —— 等级用罗马数字，和游戏里的写法和铁砧界面对得上）。
QString enchantmentText(const QString &id, int level) {
    static const char *kRoman[] = {"", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
    const QString roman = (level >= 1 && level <= 10) ? QString::fromLatin1(kRoman[level])
                                                      : QString::number(level);
    return QStringLiteral("%1 %2").arg(Anvil::enchantmentName(id), roman);
}

/// 一件东西的显示名（`锋利 III 的书` / `锋利 IV + 耐久 III 的装备`）。
QString unitText(const QMap<QString, int> &enchantments, bool isBook) {
    QStringList parts;
    for (auto it = enchantments.constBegin(); it != enchantments.constEnd(); ++it) {
        parts.append(enchantmentText(it.key(), it.value()));
    }
    const QString body = parts.isEmpty() ? QCoreApplication::translate("ToolboxController", "（没有附魔）") : parts.join(QStringLiteral(" + "));
    return isBook ? QCoreApplication::translate("ToolboxController", "%1 的书").arg(body) : QCoreApplication::translate("ToolboxController", "%1 的装备").arg(body);
}

/// 服务器 MOTD 那一行的 HTML（`§` 码 → 带色的 span；两行封顶，和游戏里一样）。
QString motdHtml(const QString &raw, const QColor &background) {
    const QVector<MotdPiece> pieces = MotdText::parse(raw, 2);
    QString html;
    int line = 0;
    for (const MotdPiece &piece : pieces) {
        while (line < piece.line) {
            html.append(QStringLiteral("<br>"));
            ++line;
        }
        const QColor color = MotdText::readableOn(MotdText::pieceColor(piece), background);
        QStringList styles;
        styles.append(QStringLiteral("color:%1").arg(color.name()));
        if (piece.bold) {
            styles.append(QStringLiteral("font-weight:bold"));
        }
        if (piece.italic) {
            styles.append(QStringLiteral("font-style:italic"));
        }
        QStringList decoration;
        if (piece.underline) {
            decoration.append(QStringLiteral("underline"));
        }
        if (piece.strike) {
            decoration.append(QStringLiteral("line-through"));
        }
        if (!decoration.isEmpty()) {
            styles.append(QStringLiteral("text-decoration:%1").arg(decoration.join(QChar(' '))));
        }
        html.append(QStringLiteral("<span style='%1'>%2</span>")
                        .arg(styles.join(QChar(';')), piece.text.toHtmlEscaped()));
    }
    return html;
}

}  // namespace

ToolboxController::ToolboxController(OverlayHost *overlay, QWidget *pageHost, Host host,
                                    QObject *parent)
    : QObject(parent), m_overlay(overlay), m_pageHost(pageHost), m_host(std::move(host)) {
    // 下载恢复和任务操作不依赖工具页，必须在用户打开工具箱之前可用。
    m_directDownload = new DirectDownloadController(m_host.activity, overlay, this);
}

void ToolboxController::ensurePage() {
    if (m_page != nullptr) return;
    m_page = new ToolboxPage(m_pageHost);
    if (auto *hostLayout = qobject_cast<QBoxLayout *>(m_pageHost->layout())) hostLayout->addWidget(m_page);
    // 工具页第一次进才搭（见 ToolboxPage 文件头）：下拉表、恢复输入、初值都等那一片搭出来再做。
    connect(m_page, &ToolboxPage::toolBuilt, this, [this](ToolboxPage::Tool tool) { setupTool(tool); });
    // 纹饰那一片一露出来就去读本地预览资源，高速下载那一片一露出来就统计缓存 ——
    // 入口有两条（目录格、面包屑切换浮层），都走 `showTool()`，所以只接这一个信号。
    connect(m_page, &ToolboxPage::toolShown, this, [this](ToolboxPage::Tool tool) {
        if (tool == ToolboxPage::Tool::Trim) m_trimPreview->ensureLocal();
        if (tool == ToolboxPage::Tool::Download) m_directDownload->panelShown();
    });
    connect(m_page, &ToolboxPage::beamRequested, this, [this](const QList<int> &glass) {
        const auto result = BeaconBeam::calculate(glass);
        QStringList rows;
        for (int i = 0; i < result.segments.size(); ++i)
            rows.append(tr("第 %1 层 / %2玻璃 → %3").arg(i + 1).arg(Banner::colorName(glass[i]), result.segments[i].name()));
        const QColor final = result.finalColor;
        rows.append(tr("最终颜色：%1 / RGB(%2, %3, %4)").arg(final.name()).arg(final.red()).arg(final.green()).arg(final.blue()));
        QStringList materials;
        for (int color = 0; color < Banner::colorCount(); ++color) {
            const int count = glass.count(color);
            if (count > 0) materials.append(tr("%1玻璃 × %2").arg(Banner::colorName(color)).arg(count));
        }
        if (!materials.isEmpty()) rows.append(tr("材料：%1").arg(materials.join(QCoreApplication::translate("ToolboxController", "、"))));
        if (glass.isEmpty()) rows.prepend(tr("未放置染色玻璃：白色光柱。"));
        m_page->setBeamResult(result.segments, result.ok ? rows.join(QLatin1Char('\n')) : result.error);
    });
    connect(m_page, &ToolboxPage::bannerTexturesRequested, this, [this] {
        if (m_bannerTextureLoading) return;
        const QString archive = QFileDialog::getOpenFileName(m_page, tr("选择原版游戏 JAR 或旗帜资源包"), {}, tr("游戏资源 (*.jar *.zip)"));
        if (archive.isEmpty()) return;
        m_bannerTextureLoading = true;
        m_page->setBannerPreview({}, tr("正在后台读取本地旗帜贴图…"));
        auto result = QSharedPointer<BannerTextures::Result>::create();
        auto *worker = QThread::create([result, archive] { *result = BannerTextures::load(archive); });
        connect(worker, &QThread::finished, this, [this, result] {
            m_bannerTextureLoading = false;
            if (!result->error.isEmpty()) { m_page->setBannerPreview({}, result->error); return; }
            m_bannerTextures = result->images;
            QList<QIcon> icons;
            for (int i = 0; i < Banner::patternCount(); ++i)
                icons.append(QIcon(QPixmap::fromImage(BannerRender::compose(0, {{i, 15}}, m_bannerTextures))));
            m_page->setBannerPatternIcons(icons);
            refreshBanner(m_page->bannerBaseColor(), m_page->bannerLayers());
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    });

    connect(m_page, &ToolboxPage::bannerSaveRequested, this, [this] {
        const QString path = QFileDialog::getSaveFileName(m_page, tr("保存旗帜设计"), QStringLiteral("banner.wtfml-banner.json"), tr("旗帜设计 (*.json)"));
        if (path.isEmpty()) return;
        QString error;
        if (!BannerDesign::save(path, {m_page->bannerBaseColor(), m_page->bannerVersion(), m_page->bannerLayers()}, &error))
            InlineMessage::information(m_overlay, tr("保存失败"), error);
    });
    connect(m_page, &ToolboxPage::bannerLoadRequested, this, [this] {
        const QString path = QFileDialog::getOpenFileName(m_page, tr("载入旗帜设计"), {}, tr("旗帜设计 (*.json)"));
        if (path.isEmpty()) return;
        BannerDesign::Design design; QString error;
        if (!BannerDesign::load(path, &design, &error)) {
            InlineMessage::information(m_overlay, tr("载入失败，当前设计未改变"), error); return;
        }
        m_page->setBannerDesign(design.base, design.layers, design.version);
        refreshBanner(design.base, design.layers);
    });

    // ---------- 工具箱里那几件的信号 ----------
    connect(m_page, &ToolboxPage::anvilPlanRequested, this, [this]() { runAnvilPlan(); });
    connect(m_page, &ToolboxPage::textSourceChanged, this, [this]() { refreshText(); });
    connect(m_page, &ToolboxPage::textPreviewEdited, this, &ToolboxController::refreshTextFromPreview);
    connect(m_page, &ToolboxPage::textJsonRequested, this,
            [this](const QString &json) { loadTextFromJson(json); });
    connect(m_page, &ToolboxPage::textHexColorRequested, this, [this](const QString &hex) {
        quint32 rgb = 0;
        if (!TextFormat::parseHexColor(hex, &rgb)) {
            // 不猜：三位缩写、写错位数都直接说清楚要什么形状。
            m_page->setTextNotice(tr("请输入六位十六进制颜色值，如 `#ff8800`。游戏不支持 `#f80` 这样的缩写。"));
            return;
        }
        m_page->applyTextPreviewColor(QColor::fromRgb(rgb), TextFormat::legacyHexColor(rgb));
        m_page->setTextNotice(QString());
    });
    connect(m_page, &ToolboxPage::coordinateRequested, this,
            [this](int dimension, double x, double y, double z) {
                convertCoordinates(dimension, x, y, z);
            });
    connect(m_page, &ToolboxPage::teleportParseRequested, this,
            [this](const QString &text) { parseTeleport(text); });
    connect(m_page, &ToolboxPage::teleportCommandRequested, this,
            [this](int dimension, double x, double y, double z) {
                makeTeleportCommand(dimension, x, y, z);
            });
    connect(m_page, &ToolboxPage::serverCheckRequested, this,
            [this](const QString &address) { checkServer(address); });
    connect(m_page, &ToolboxPage::serverCheckCancelRequested, this,
            [this]() { cancelServerCheck(); });
    connect(m_page, &ToolboxPage::serverToolExited, this, &ToolboxController::cancelServerCheck);
    connect(m_page, &ToolboxPage::beaconRequested, this,
            [this](int fromLevel, int level, int materialIndex, int beaconCount) {
                refreshBeacon(fromLevel, level, materialIndex, beaconCount);
            });
    connect(m_page, &ToolboxPage::brewingRequested, this,
            [this](int potion, bool extended, bool level2, int formIndex, int bottles) {
                refreshBrewing(potion, extended, level2, formIndex, bottles);
            });
    connect(m_page, &ToolboxPage::bannerRequested, this,
            [this](int baseColor, const QStringList &layers) { refreshBanner(baseColor, layers); });
    // 这几个信号还留着（隐藏的迁移适配器与验证工程用），但结果只看四列现在摆的装备。
    connect(m_page, &ToolboxPage::trimRequested, this, [this] { refreshTrim(); });
    connect(m_page, &ToolboxPage::trimEquipmentChanged, this, [this] { refreshTrim(); });

    ToolboxInputState::attach(m_page);
}

void ToolboxController::setupTool(ToolboxPage::Tool tool) {
    using Tool = ToolboxPage::Tool;
    // 次序和原先一次全搭时一样：先灌下拉表（恢复输入要按下标落到表里），再摆回记住的输入，最后算初值。
    switch (tool) {
    case Tool::Anvil: {
        QList<QPair<QString, QString>> enchantments;
        for (const Anvil::EnchantInfo &info : Anvil::enchantmentTable()) {
            enchantments.append({info.id, QCoreApplication::translate("ToolboxController", "%1（%2，上限 %3 级）")
                                            .arg(info.name, info.english)
                                            .arg(info.maxLevel)});
        }
        m_page->setEnchantmentChoices(enchantments);
        break;
    }
    case Tool::Text: {
        QList<ToolboxPage::PaletteEntry> palette;
        for (char code = '0'; code <= '9'; ++code) {
            const QChar ch(code);
            const TextFormat::Color color = TextFormat::colorFromCode(ch);
            palette.append({QString(ch), TextFormat::colorText(color),
                            QColor(TextFormat::colorRgb(color)).name(), true});
        }
        for (char code = 'a'; code <= 'f'; ++code) {
            const QChar ch(code);
            const TextFormat::Color color = TextFormat::colorFromCode(ch);
            palette.append({QString(ch), TextFormat::colorText(color),
                            QColor(TextFormat::colorRgb(color)).name(), true});
        }
        // 五个格式码 + 复位（`k` 乱码、`l` 粗体、`m` 删除线、`n` 下划线、`o` 斜体、`r` 复位）。
        for (QChar ch : {QChar('k'), QChar('l'), QChar('m'), QChar('n'), QChar('o'), QChar('r')}) {
            palette.append({QString(ch), TextFormat::formatText(ch), QString(), false});
        }
        m_page->setTextPalette(palette);
        break;
    }
    case Tool::Coord:
        m_page->setDimensionChoices(dimensionNames());
        break;
    case Tool::Server:
        break;
    case Tool::Beacon: {
        QStringList beamNames;
        for (int i = 0; i < Banner::colorCount(); ++i) beamNames.append(Banner::colorName(i));
        m_page->setBeamColors(beamNames);
        m_page->beamRequested({});
        // 底座材质表的出处在 `Beacon` 里，这一层只翻成字符串。
        m_page->setBeaconMaterials(Beacon::materialNames());
        break;
    }
    case Tool::Brewing: {
        m_page->setBrewingPotions(Brewing::potionNames());
        QStringList brewForms;
        for (int index = 0; index < Brewing::formCount(); ++index) {
            brewForms.append(Brewing::formName(Brewing::Form(index)));
        }
        m_page->setBrewingForms(brewForms);
        // 版本表和旗帜 / 纹饰共用一份（`GameVersion` 是唯一出处）。
        m_page->setBrewingVersions(GameVersion::versions());
        break;
    }
    case Tool::Banner: {
        QStringList bannerColors;
        QList<QColor> bannerSwatches;
        for (int index = 0; index < Banner::colorCount(); ++index) {
            bannerColors.append(Banner::colorName(index));
            bannerSwatches.append(BeaconBeam::dyeColor(index));
        }
        m_page->setBannerColors(bannerColors, bannerSwatches);
        m_page->setBannerPatterns(Banner::patternNames());
        m_page->setBannerVersions(BannerDesign::versions());
        break;
    }
    case Tool::Trim: {
        m_trimPreview = new ArmorTrimPreviewController(m_page, m_overlay, this);
        // 「显示披风」勾选只改预览那一张图，不碰资源、不写盘 —— 直接重绘即可。
        connect(m_page, &ToolboxPage::trimCapeVisibilityChanged, this,
                [this](bool) { m_trimPreview->refresh(); });
        QStringList trimArmor;
        for (int index = 0; index < ArmorTrim::armorMaterialCount(); ++index) {
            trimArmor.append(ArmorTrim::armorMaterialName(index));
        }
        m_page->setTrimArmorMaterials(trimArmor);
        QStringList trimParts;
        for (int index = 0; index < ArmorTrim::partCount(); ++index) {
            trimParts.append(ArmorTrim::partName(index));
        }
        m_page->setTrimParts(trimParts);
        m_page->setTrimPatterns(ArmorTrim::patternNames());
        m_page->setTrimMaterials(ArmorTrim::trimMaterialNames());
        // 纹饰与旗帜 / 酿造同一套版本表（`GameVersion` 是唯一出处）。
        m_page->setTrimVersions(GameVersion::versions());
        break;
    }
    case Tool::Download:
        m_directDownload->attach(m_page->downloadPanel());
        break;
    }

    ToolboxInputState::restoreTool(m_page, tool);

    // 开页之前先把结果那几格算出来，别摆在"空的"那一档上（空预览框看着像没搭好）。
    switch (tool) {
    case Tool::Text:
        refreshText();
        break;
    case Tool::Beacon:
        refreshBeacon(m_page->beaconFromLevel(), m_page->beaconTargetLevel(),
                      m_page->beaconMaterialIndex(), m_page->beaconCount());
        break;
    case Tool::Brewing:
        refreshBrewing(m_page->brewingPotionIndex(), m_page->brewingExtended(),
                       m_page->brewingLevel2(), m_page->brewingFormIndex(), m_page->brewingBottles());
        break;
    case Tool::Banner:
        refreshBanner(m_page->bannerBaseColor(), m_page->bannerLayers());
        break;
    case Tool::Trim:
        refreshTrim();
        if (m_hasPlayerSkin) m_trimPreview->setPlayerSkin(m_playerSkin, m_playerCape, m_playerSlim);
        break;
    default:
        break;
    }
}

ToolboxController::~ToolboxController() { shutdown(); }
void ToolboxController::setPlayerSkin(const QImage &skin, const QImage &cape, bool slim) {
    m_playerSkin = skin;
    m_playerCape = cape;
    m_playerSlim = slim;
    m_hasPlayerSkin = true;
    if (m_trimPreview != nullptr) m_trimPreview->setPlayerSkin(skin, cape, slim);
}
bool ToolboxController::ownsDownloadTask(quint64 id) const { return m_directDownload->owns(id); }
bool ToolboxController::canCancelDownloadTask(quint64 id) const { return m_directDownload->canCancel(id); }
bool ToolboxController::cancelDownloadTask(quint64 id) { return m_directDownload->cancel(id); }
bool ToolboxController::pauseDownloadTask(quint64 id) { return m_directDownload->pause(id); }
bool ToolboxController::resumeDownloadTask(quint64 id) { return m_directDownload->resume(id); }

void ToolboxController::shutdown() {
    if (m_trimPreview != nullptr) m_trimPreview->shutdown();
    m_directDownload->shutdown();
    // 旗帜贴图那条后台线程跑着的时候析构 QObject 会把子对象连同仍在跑的线程一起删掉
    // ⇒ qFatal。按子对象兜一遍底，保证收手时它们都已经退出。
    for (QThread *thread : findChildren<QThread *>()) {
        thread->wait();
    }
    if (m_pinger != nullptr) {
        m_pinger->abort();
    }
}

QStringList ToolboxController::dimensionNames() const {
    // 能按比例换算的那三个（末地不在 8:1 那个关系里，但"从这一头看它在哪"这件事仍然
    // 要能问，所以它也列出来 —— 只是结果那一段会写明"末地不按比例算"）。
    QStringList names;
    for (CoordConvert::Dimension dimension :
         {CoordConvert::Dimension::Overworld, CoordConvert::Dimension::Nether,
          CoordConvert::Dimension::End}) {
        names.append(CoordConvert::dimensionText(dimension));
    }
    return names;
}

int ToolboxController::dimensionAt(int index) const {
    switch (index) {
    case 0:
        return int(CoordConvert::Dimension::Overworld);
    case 1:
        return int(CoordConvert::Dimension::Nether);
    case 2:
        return int(CoordConvert::Dimension::End);
    default:
        return int(CoordConvert::Dimension::Unknown);
    }
}

void ToolboxController::open(const QString &toolKey) {
    if (m_pageHost == nullptr) {
        return;
    }
    const bool catalog = toolKey == QStringLiteral("catalog");
    bool known = false;
    ToolboxPage::Tool tool = ToolboxPage::Tool::Anvil;
    if (!toolKey.isEmpty() && !catalog) {
        tool = ToolboxPage::toolFromKey(toolKey, &known);
        if (!known) {
            return;  // 认不出来的键：不该发生（键就是 ToolboxPage 那张表）
        }
    }
    ensurePage();
    if (catalog) {
        m_page->showCatalog();
    } else if (known) {
        m_page->showTool(tool);
    }
}

// ---------------------------------------------------------------------------
// 一、附魔顺序助手
// ---------------------------------------------------------------------------

void ToolboxController::runAnvilPlan() {
    const QList<ToolboxPage::AnvilRow> rows = m_page->anvilRows();
    if (rows.isEmpty()) {
        InlineMessage::information(m_overlay, tr("没有可合并的内容"),
                                   tr("先在表里加一两行：选一条附魔、写上等级。"));
        return;
    }
    QList<Anvil::Unit> units;
    for (const ToolboxPage::AnvilRow &row : rows) {
        const QString id = Anvil::normalizeId(row.id);
        // 勾了「和上一行同一件」的，并进上一行那一件：一本书上能带好几条附魔，一件装备
        // 本来也可能已经有好几条 —— 它们不是"几件东西"，拆开算会把前科算重。
        if (row.sameAsPrevious && !units.isEmpty()) {
            Anvil::Unit &unit = units.last();
            // 同一件上写了两遍同一条附魔（手滑）：取高的那一档，别让低的那条把它盖回去。
            unit.enchantments.insert(id, qMax(unit.enchantments.value(id, 0), row.level));
            unit.label = unitText(unit.enchantments, unit.isBook);
            continue;
        }
        Anvil::Unit unit;
        unit.enchantments.insert(id, row.level);
        unit.penalty = Anvil::penaltyForUses(row.uses);
        unit.isBook = row.isBook;
        unit.label = unitText(unit.enchantments, unit.isBook);
        units.append(unit);
    }

    const Anvil::Plan plan = Anvil::plan(units, m_page->anvilRename());
    ToolboxPage::AnvilView view;
    view.ok = plan.ok;
    view.error = plan.error;
    if (!plan.ok) {
        m_page->setAnvilView(view);
        return;
    }
    // 每一步一行：`第 1 步：锋利 III 的书 + 锋利 III 的书 → 锋利 IV 的书（代价 3 级）`。
    const auto nodeText = [&units, &plan](const Anvil::Step &step, bool target) {
        const int index = target ? step.targetIndex : step.sacrificeIndex;
        if (index >= 0 && index < units.size()) {
            return units.at(index).label;
        }
        const int node = target ? step.targetNode : step.sacrificeNode;
        if (node >= 0 && node - units.size() < plan.steps.size()) {
            const Anvil::Step &produced = plan.steps.at(node - units.size());
            return QCoreApplication::translate("ToolboxController", "第 %1 步的合并产物（%2）")
                .arg(node - units.size() + 1)
                .arg(unitText(produced.result, produced.resultIsBook));
        }
        return QCoreApplication::translate("ToolboxController", "（无法识别的一件）");
    };
    int stepNumber = 0;
    for (const Anvil::Step &step : plan.steps) {
        ++stepNumber;
        QString line = QCoreApplication::translate("ToolboxController", "第 %1 步：左槽 %2 + 右槽 %3 → %4（代价 %5 级")
                           .arg(stepNumber)
                           .arg(nodeText(step, true), nodeText(step, false),
                                unitText(step.result, step.resultIsBook))
                           .arg(step.cost.total);
        if (step.cost.penalties > 0) {
            line.append(QCoreApplication::translate("ToolboxController", "，其中先前工作惩罚 %1").arg(step.cost.penalties));
        }
        if (step.cost.incompatible > 0) {
            line.append(QCoreApplication::translate("ToolboxController", "，含不兼容 +%1").arg(step.cost.incompatible));
        }
        line.append(QChar(')'));
        view.steps.append(line);
        // 互斥顶掉的那几条**点名说清楚**：不解释的话，用户会以为那一条悄悄丢了
        // （代价里那 +1 是照旧的，所以要说明"合不上来但还是要多花一级"）。
        for (auto it = step.cost.dropped.constBegin(); it != step.cost.dropped.constEnd(); ++it) {
            view.notes.append(tr("第 %1 步：「%2」与目标已有的附魔互斥，这一步无法合入；目标保留原有附魔（铁砧仍按每条互斥 +1 级）。若要保留这一条，请把它所在的一件放到左槽先合。")
                                  .arg(stepNumber)
                                  .arg(enchantmentText(it.key(), it.value())));
        }
    }
    const QString finalText = unitText(plan.finalEnchantments, plan.finalIsBook)
                                  .replace(QCoreApplication::translate("ToolboxController", " 的书"), QString())
                                  .replace(QCoreApplication::translate("ToolboxController", " 的装备"), QString());
    view.summary = plan.steps.isEmpty()
                       ? (plan.renameCost > 0
                              ? tr("只有一件，无需合并；改名需要 %1 级。").arg(plan.renameCost)
                              : tr("只有一件，无需合并。"))
                       : tr("合计 %1 级%2，合并后为 %3。")
                             .arg(plan.totalCost)
                             .arg(plan.withinSurvival ? tr("（每一步都在 39 级以内）") : QString(),
                                  finalText);
    view.anyTooExpensive = plan.anyTooExpensive;
    if (plan.approximate) {
        // 搜索额度到顶：这一条仍然照常给，但不能说它是最省的那条。
        view.notes.append(tr("材料组合较多，当前结果为近似最优。减少材料数量可计算精确最优解。"));
    }
    if (plan.anyTooExpensive) {
        view.notes.append(tr("★ 计划中有一步的代价超过 39 级；生存模式下铁砧会拒绝该步骤（创造模式不受限制）。请调整顺序，或先降低部分材料的先前工作惩罚。"));
    }
    if (!plan.unused.isEmpty()) {
        QStringList leftover;
        for (int index : plan.unused) {
            if (index >= 0 && index < units.size()) {
                leftover.append(units.at(index).label);
            }
        }
        view.notes.append(tr("这些材料用不上：%1（加入后也无法提升，材料不必全部使用）。")
                              .arg(leftover.join(QCoreApplication::translate("ToolboxController", "、"))));
    }
    if (plan.finalPenalty > 0) {
        view.notes.append(tr("合并后该件的先前工作惩罚为 %1（相当于在铁砧上使用过 %2 次）。")
                              .arg(plan.finalPenalty)
                              .arg(Anvil::usesForPenalty(plan.finalPenalty)));
    }
    // 改名那 1 级只花在**一次**操作上：某一步顺手改，或者合完单独改一次。
    if (plan.renameCost > 0) {
        if (plan.renameStep >= 0 && plan.renameStep < plan.steps.size()) {
            view.notes.append(tr("第 %1 步合并时会同时改名，总代价多 1 级（这一步本身按 %2 级计；一条计划只改一次名）。")
                                  .arg(plan.renameStep + 1)
                                  .arg(plan.steps.at(plan.renameStep).cost.total));
        } else {
            view.notes.append(tr("合并完成后再单独改名：%1 级（单独改名 = 该件先前工作惩罚 + 1，上限 39；单独改名不会再提高该惩罚）。")
                                  .arg(plan.renameCost));
        }
    }
    m_page->setAnvilView(view);
    if (m_host.log) {
        m_host.log(tr("附魔顺序助手：%1 件，合计 %2 级%3")
                       .arg(units.size())
                       .arg(plan.totalCost)
                       .arg(plan.anyTooExpensive ? tr("（其中一步超过 39 级）") : QString()));
    }
}

// ---------------------------------------------------------------------------
// 二、文本生成器
// ---------------------------------------------------------------------------

void ToolboxController::refreshText() {
    const QString raw = m_page->textSource();
    const QList<TextFormat::Segment> segments = TextFormat::parseLegacy(raw);
    const QColor background = m_page->palette().color(QPalette::Base);
    QList<ToolboxPage::TextRun> runs;
    runs.reserve(segments.size());
    for (const auto &segment : segments) {
        const auto &style = segment.style;
        QString code;
        if (style.hasCustomColor) code = TextFormat::legacyHexColor(style.customRgb);
        else if (const QChar named = TextFormat::codeForColor(style.color); !named.isNull()) code = QStringLiteral("§%1").arg(named);
        const QColor color = code.isEmpty() ? QColor() :
            MotdText::readableOn(QColor::fromRgb(TextFormat::styleRgb(style)), background);
        runs.append({segment.text, code, color, style.bold, style.italic, style.underline,
                     style.strikethrough, style.obfuscated});
    }
    m_page->setTextPreviewRuns(runs);
    m_page->setTextOutputs(QString(), TextFormat::toJsonText(segments));
    m_page->setTextMotd(TextFormat::toMotd(segments));
    m_page->setTextNotice(QString());
}

void ToolboxController::refreshTextFromPreview() {
    QList<TextFormat::Segment> segments;
    for (const auto &run : m_page->textPreviewRuns()) {
        TextFormat::Style style;
        if (run.colorCode.startsWith(QStringLiteral("§x"))) {
            QString hex = run.colorCode.mid(2);
            hex.remove(QChar(u'§'));
            style.hasCustomColor = TextFormat::parseHexColor(hex, &style.customRgb);
        } else if (run.colorCode.size() == 2) {
            style.color = TextFormat::colorFromCode(run.colorCode.at(1));
        }
        style.bold = run.bold;
        style.italic = run.italic;
        style.underline = run.underline;
        style.strikethrough = run.strike;
        style.obfuscated = run.obfuscated;
        segments.append({run.text, style});
    }
    const QString legacy = TextFormat::toLegacy(segments);
    m_page->setTextOutputs(legacy.isNull() ? QStringLiteral("") : legacy, TextFormat::toJsonText(segments));
    m_page->setTextMotd(TextFormat::toMotd(segments));
    m_page->setTextNotice(QString());
}

void ToolboxController::loadTextFromJson(const QString &json) {
    if (json.trimmed().isEmpty()) {
        m_page->setTextNotice(tr("「JSON 组件」为空，请先粘贴一段 JSON。"));
        return;
    }
    const TextFormat::JsonParseResult parsed = TextFormat::parseJson(json);
    if (!parsed.segments.isEmpty()) {
        m_page->setTextSource(TextFormat::toLegacy(parsed.segments));
    }
    if (parsed.unsupported.isEmpty()) {
        m_page->setTextNotice(tr("已读入：%1 段。").arg(parsed.segments.size()));
        return;
    }
    // 认不出来的那几种组件**不猜**（`translate` 要查语言文件、`score` 要游戏在跑、
    // `selector` 要挑实体 —— 都不是这一层能算的）。把名字列出来，用户自己知道少读了什么。
    m_page->setTextNotice(tr("以下组件未解析：%1。它们需要游戏内的服务器或语言文件才能确定，此处不做推测。")
                              .arg(parsed.unsupported.join(QCoreApplication::translate("ToolboxController", "、"))));
}

// ---------------------------------------------------------------------------
// 三、坐标换算
// ---------------------------------------------------------------------------

void ToolboxController::convertCoordinates(int dimensionIndex, double x, double y, double z) {
    const auto from = CoordConvert::Dimension(dimensionAt(dimensionIndex));
    const CoordConvert::Position position{x, y, z};
    QStringList rows;
    const auto show = [&](CoordConvert::Dimension to) {
        const CoordConvert::Position converted = CoordConvert::convert(from, to, position);
        rows.append(QCoreApplication::translate("ToolboxController", "%1：%2")
                        .arg(CoordConvert::dimensionText(to),
                             CoordConvert::positionText(converted)));
    };
    rows.append(QCoreApplication::translate("ToolboxController", "%1 中的字段：%2")
                    .arg(CoordConvert::dimensionText(from), CoordConvert::positionText(position)));
    for (CoordConvert::Dimension to :
         {CoordConvert::Dimension::Overworld, CoordConvert::Dimension::Nether,
          CoordConvert::Dimension::End}) {
        if (to == from) {
            continue;
        }
        if (CoordConvert::convertible(from, to)) {
            show(to);
        } else {
            rows.append(QCoreApplication::translate("ToolboxController", "%1：末地不按比例算（两边坐标之间没有 8:1 那种关系）。")
                            .arg(CoordConvert::dimensionText(to)));
        }
    }
    // 区块与区域文件：只有主世界/下界这两套坐标能这么算（末地同样没有"比例"这回事，
    // 但区块是**每个维度各自**的 —— 这里给的是"手上这组坐标在它自己那个维度里的格子"）。
    const int chunkX = CoordConvert::blockToChunk(CoordConvert::floorToInt(position.x));
    const int chunkZ = CoordConvert::blockToChunk(CoordConvert::floorToInt(position.z));
    rows.append(QCoreApplication::translate("ToolboxController", "区块：(x %1, z %2)（x %3、z %4）")
                    .arg(chunkX)
                    .arg(chunkZ)
                    .arg(CoordConvert::blockRangeText(chunkX), CoordConvert::blockRangeText(chunkZ)));
    const int regionX = CoordConvert::chunkToRegion(chunkX);
    const int regionZ = CoordConvert::chunkToRegion(chunkZ);
    rows.append(QCoreApplication::translate("ToolboxController", "区域文件：%1")
                    .arg(CoordConvert::regionFileName(regionX, regionZ)));
    m_page->setCoordinateRows(rows);
}

void ToolboxController::parseTeleport(const QString &text) {
    const CoordConvert::ParsedTp parsed = CoordConvert::parseTeleportCommand(text);
    if (!parsed.ok) {
        m_page->setTeleportParse(false, parsed.error);
        return;
    }
    const auto dimension = parsed.dimension;
    // 认出来的是"哪一档维度"：说出来过就按下拉框里那一项选上（没说出来就**不动**那个框 ——
    // 用户自己挑的那个维度不该被悄悄改掉）。
    int index = -1;
    for (int candidate = 0; candidate < 3; ++candidate) {
        if (CoordConvert::Dimension(dimensionAt(candidate)) == dimension) {
            index = candidate;
        }
    }
    if (index >= 0) {
        m_page->setCoordinates(index, parsed.position.x, parsed.position.y, parsed.position.z);
    } else {
        // 没说维度的 `/tp @s 1 2 3`：坐标照填，维度那一格留在用户原来选的那一项上。
        m_page->setCoordinates(m_page->dimensionIndex(), parsed.position.x, parsed.position.y,
                               parsed.position.z);
    }
    QString message = tr("识别结果：%1")
                          .arg(CoordConvert::positionText(parsed.position));
    if (parsed.hasDimension) {
        message.append(tr("，维度 %1").arg(CoordConvert::dimensionText(parsed.dimension)));
    } else {
        message.append(tr("（这条命令里没写维度）"));
    }
    if (parsed.hasRotation) {
        message.append(tr("，朝向 yaw %1 / pitch %2")
                           .arg(parsed.yaw, 0, 'f', 2)
                           .arg(parsed.pitch, 0, 'f', 2));
    }
    m_page->setTeleportParse(true, message);
    convertCoordinates(m_page->dimensionIndex(), parsed.position.x, parsed.position.y,
                       parsed.position.z);
}

void ToolboxController::makeTeleportCommand(int dimensionIndex, double x, double y, double z) {
    const auto dimension = CoordConvert::Dimension(dimensionAt(dimensionIndex));
    const QString command =
        CoordConvert::toTeleportCommand(CoordConvert::Position{x, y, z}, dimension);
    m_page->setTeleportCommand(command);
}

// ---------------------------------------------------------------------------
// 四、服务器在线检测
// ---------------------------------------------------------------------------

void ToolboxController::checkServer(const QString &address) {
    if (address.isEmpty()) {
        InlineMessage::information(m_overlay, tr("先写个地址"),
                                   tr("比如 `mc.hypixel.net` 或者 `127.0.0.1:25565`。"));
        return;
    }
    if (m_pinger == nullptr) {
        m_pinger = new ServerPing::Pinger(this);
        connect(m_pinger, &ServerPing::Pinger::oneFinished, this,
                [this](int index, const ServerPing::Status &status) {
                    if (index != 0 || m_serverAddress.isEmpty()) {
                        return;  // 一次只问一台
                    }
                    ToolboxPage::ServerView view;
                    view.ok = status.ok;
                    view.timedOut = status.timedOut;
                    // 分类那句由这里翻成文字：`ui/` 不认识 `src/` 的枚举。
                    view.kind = ServerPing::kindName(status.kind);
                    view.error = status.ok ? QString() : status.error;
                    view.address = m_serverAddress;
                    const auto address = ServerPing::splitAddress(m_serverAddress);
                    if (address.valid) {
                        view.addressKey = address.host.toCaseFolded() + QLatin1Char(':') +
                            (address.portExplicit ? QString::number(address.port) : QStringLiteral("auto"));
                    }
                    if (status.ok) {
                        const QColor background = m_page->palette().color(QPalette::Base);
                        view.motd = motdHtml(status.motdRaw, background);
                        if (view.motd.isEmpty()) {
                            view.motd = status.motd.toHtmlEscaped();
                        }
                        view.version = status.versionName;
                        view.players = ToolboxPage::serverPlayerCountText(
                            status.playersOnline, status.playersMax);
                        view.latency = tr("%1 ms").arg(status.latencyMs);
                        view.latencyMs = status.latencyMs;
                        if (!status.iconPng.isEmpty()) {
                            // 服务器给的字节走 ImageDecode：那一层有尺寸上限（见
                            // `ImageDecode::Limits`），挡解压炸弹。
                            const QImage icon = ImageDecode::decode(status.iconPng);
                            if (!icon.isNull()) {
                                view.icon = QPixmap::fromImage(icon);
                            }
                        }
                    }
                    m_page->setServerView(view);
                });
        connect(m_pinger, &ServerPing::Pinger::allFinished, this, [this]() {
            m_page->setServerBusy(false);
        });
    }
    m_serverAddress = address;
    m_page->setServerBusy(true);
    m_pinger->start({address});
}

void ToolboxController::cancelServerCheck() {
    m_serverAddress.clear();
    if (m_pinger != nullptr) {
        m_pinger->abort();  // 手上那一台会问完（同步的 socket 掐不断），后面的不再问
    }
    m_page->setServerBusy(false);
}

// ---------------------------------------------------------------------------
// 五、信标计算 / 六、酿造助手
// ---------------------------------------------------------------------------

void ToolboxController::refreshBeacon(int fromLevel, int level, int materialIndex,
                                     int beaconCount) {
    if (m_page == nullptr) {
        return;
    }
    // 两格都是转盘（0–4 / 1–4），越不了界；真给了怪值也由 `Beacon` 回一句人话。
    //
    // 后面两行是两块：矿物折算（方块 → 锭/宝石）与多颗信标共用
    // 底座。`detailText()` 那段本身不碰材质 —— 材质只影响折算，效果和层数跟它无关。
    QString text = Beacon::detailText(fromLevel, level);
    const int baseBlocks = Beacon::sharedBaseBlocks(level, beaconCount);
    text += QStringLiteral("\n\n") + Beacon::materialText(materialIndex, baseBlocks);
    text += QStringLiteral("\n") + Beacon::sharedBaseText(level, beaconCount);
    m_page->setBeaconResult(text);
}

void ToolboxController::refreshBrewing(int potionIndex, bool extended, bool level2,
                                       int formIndex, int bottles) {
    if (m_page == nullptr) {
        return;
    }
    Brewing::Form form = Brewing::Form::Normal;
    if (formIndex >= 0 && formIndex < Brewing::formCount()) {
        form = Brewing::Form(formIndex);
    }
    const Brewing::Plan plan = Brewing::plan(potionIndex, extended, level2, form, bottles);
    // 目标版本里还没有这一瓶时，先摆一句人话，路线照旧列在下面 —— 与旗帜那一页同一套
    // 判据（`GameVersion::availableIn()`），不悄悄把选中的药水换掉。
    QString prefix;
    const QString version = m_page->brewingVersion();
    if (potionIndex >= 0 && potionIndex < Brewing::potionCount()
        && !Brewing::available(potionIndex, version)) {
        prefix = tr("目标版本 %1 不含%2。此药水自 Java 版 %3 起可用。\n\n")
                     .arg(version,
                          Brewing::potionAt(potionIndex).name,
                          Brewing::potionSince(potionIndex));
    }
    m_page->setBrewingResult(prefix + plan.detailText());
}

void ToolboxController::refreshBanner(int baseColor, const QStringList &layers) {
    if (m_page == nullptr) {
        return;
    }
    // 页面上每层是 `图案下标:颜色下标`；认不出来的那一段原样变成 -1，由 `Banner` 回一句人话。
    QList<Banner::Layer> parsed;
    for (const QString &entry : layers) {
        const QStringList parts = entry.split(QChar(':'));
        Banner::Layer layer;
        layer.patternIndex = parts.value(0).toInt();
        layer.colorIndex = parts.value(1).toInt();
        parsed.append(layer);
    }
    // 目标版本也进判据层：1.20.5 起 `/give` 的图案写法换成物品组件（见 `Banner::plan`）。
    const QString version = m_page->bannerVersion();
    const Banner::Plan plan = Banner::plan(baseColor, parsed, version);
    QList<bool> availability;
    for (int i = 0; i < Banner::patternCount(); ++i) availability.append(BannerDesign::available(i, version));
    m_page->setBannerPatternAvailability(availability);
    QStringList unsupported;
    for (const auto &layer : parsed) {
        if (!BannerDesign::available(layer.patternIndex, version)) unsupported.append(Banner::patternAt(layer.patternIndex).name);
    }
    m_page->setBannerResult((unsupported.isEmpty() ? QString() : tr("目标版本 %1 不支持：%2。已保留图层，请调整设计或目标版本。\n\n").arg(version, unsupported.join(QCoreApplication::translate("ToolboxController", "、")))) + plan.detailText());
    m_page->setBannerCommand(plan.ok && unsupported.isEmpty() ? plan.command : QString());
    QString notice;
    const auto image = plan.ok ? BannerRender::compose(baseColor, parsed, m_bannerTextures, &notice) : QImage();
    if (!plan.ok) notice = plan.error;
    // 一句就够：下面的「加载贴图…」按钮紧跟在这句之下（走查 j05 之后），重新加载怎么办不用再写一遍。
    // 文案短到一行放得下 —— 四十个字挤在卡里会折三行，断句还落在词中间。
    if (m_bannerTextures.isEmpty() && parsed.isEmpty()) notice = tr("加载本地游戏贴图后可预览图案。");
    m_page->setBannerPreview(image, notice);
}

void ToolboxController::refreshTrim() {
    if (m_page == nullptr) {
        return;
    }
    QStringList notes;
    QStringList unsupported;
    const QString version = m_page->trimVersion();
    QString error;
    // 每件选了什么都写在上面那三排下拉框里；结果区只留提示，不列材料账（UI 问题 #18）。
    int equippedCount = 0;
    const auto equipment = m_page->trimEquipment();
    for (int slot = 0; slot < equipment.size(); ++slot) {
        const auto selection = equipment[slot];
        if (selection.armor < 0) {
            continue;
        }
        const auto armor = ArmorTrim::armorPieceName(selection.armor, slot);
        if (armor.isEmpty() || !ArmorTrim::armorMaterialFitsPart(selection.armor, slot)) {
            continue;
        }
        ++equippedCount;
        if (selection.pattern < 0) {
            continue;
        }
        const auto pattern = ArmorTrim::patternAt(selection.pattern);
        const auto material = ArmorTrim::trimMaterialAt(selection.material);
        // 目标版本边界：判据在 `ArmorTrim` 里（与旗帜 / 酿造同一套 `GameVersion`）。
        // 已经选好的搭配**不改** —— 只在结果最上面提一句，让人自己换或换目标版本。
        if (!pattern.name.isEmpty() && !ArmorTrim::patternAvailable(selection.pattern, version)) {
            const QString name = tr("%1纹饰").arg(pattern.name);
            if (!unsupported.contains(name)) unsupported.append(name);
        }
        if (!material.name.isEmpty() &&
            !ArmorTrim::trimMaterialAvailable(selection.material, version)) {
            const QString name = tr("%1纹饰材料").arg(material.name);
            if (!unsupported.contains(name)) unsupported.append(name);
        }
        // 提示（这枚模板从哪来 / 哪个版本才有 / 模板可以复制）只从判据层拿：
        // 每件各算一条单件计划，只取它那几条 notes，材料账仍按四列合起来算。
        const auto plan = ArmorTrim::plan(selection.armor, slot, selection.pattern,
                                          selection.material, 1);
        if (!plan.ok) {
            error = plan.error;
            break;
        }
        for (const QString &note : plan.notes) {
            if (!notes.contains(note)) {
                notes.append(note);
            }
        }
    }
    if (!error.isEmpty()) {
        m_page->setTrimResult(error);
        m_trimPreview->refresh();
        return;
    }
    if (equippedCount == 0) {
        m_page->setTrimResult(tr("尚未选择装备"));
        m_trimPreview->refresh();
        return;
    }
    if (notes.isEmpty() && unsupported.isEmpty()) {
        m_page->setTrimResult(tr("尚未选择纹饰"));
        m_trimPreview->refresh();
        return;
    }
    QString text;
    if (!unsupported.isEmpty()) {
        text = tr("目标版本 %1 不支持以下内容：%2。当前搭配已保留，可调整纹饰或材料，也可更换目标版本。")
                   .arg(version, unsupported.join(QCoreApplication::translate("ToolboxController", "、")));
    }
    if (!notes.isEmpty()) {
        if (!text.isEmpty()) text += QStringLiteral("\n\n");
        text += tr("提示：\n");
        for (const QString &note : notes) {
            text += tr("• %1\n").arg(note);
        }
        text.chop(1);
    }
    m_page->setTrimResult(text);
    m_trimPreview->refresh();
}
