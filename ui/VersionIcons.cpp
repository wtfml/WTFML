#include "VersionIcons.hpp"
#include "Theme.hpp"

#include <QFileInfo>
#include <QApplication>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QtMath>

#include <functional>

namespace {

/// 实例图标**源图**的尺寸上限（见 `VersionIcons::fileMark()`）：那张图是盘上的
/// `icon.png`（用户自己挑的，也可能是从别处拷来 / 包里带进来的），先问头里声明的
/// 尺寸再解 —— 几十字节的头声称 6 万乘 6 万，照它分配就是几个 GB。
/// 8192 / 64 MP 是"一张高像素手机照片"那一档，再大的一律当读不出来。
constexpr int kMaxIconSourceSide = 8192;
constexpr qint64 kMaxIconSourcePixels = 64LL * 1024 * 1024;

class ActionIconEngine final : public QIconEngine {
public:
    explicit ActionIconEngine(VersionIcons::Action action) : m_action(action) {}
    QIconEngine *clone() const override { return new ActionIconEngine(m_action); }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State) override {
        const auto tokens = Theme::tokens();
        const QColor ink = mode == QIcon::Disabled ? tokens.disabled
            : m_action == VersionIcons::Action::Delete ? Theme::dangerColor(QApplication::palette())
            : m_action == VersionIcons::Action::QuietDelete
                ? (mode == QIcon::Active ? Theme::dangerColor(QApplication::palette()) : tokens.secondary)
            : (Theme::isDark() ? Theme::accentShade(Theme::AccentShade::Pure) : tokens.primary);
        const int side = qMin(size.width(), size.height());
        switch (m_action) {
        case VersionIcons::Action::Refresh: return VersionIcons::refresh(side, ink);
        case VersionIcons::Action::Settings: return VersionIcons::gear(side, ink);
        case VersionIcons::Action::Edit: return VersionIcons::pen(side, ink);
        case VersionIcons::Action::Folder: return VersionIcons::folderMark(side, ink);
        case VersionIcons::Action::Details: return VersionIcons::infoMark(side, ink);
        case VersionIcons::Action::Delete:
        case VersionIcons::Action::QuietDelete: return VersionIcons::trash(side, ink);
        }
        return {};
    }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal ratio = painter->device()->devicePixelRatioF();
        const QPixmap image = pixmap(rect.size() * ratio, mode, state);
        painter->drawPixmap(rect, image);
    }
private:
    VersionIcons::Action m_action;
};

// ---------------------------------------------------------------------------
// 草方块：16×16 的像素画，照 MC 那块贴图的侧面（上面一层绿、底下一层土，
// 草的下沿是锯齿状的）。颜色是挑的近似色，不是从游戏里抠出来的。
//
// 快照版用的是同一张底图、只把上面那层换成白色（覆雪草块），远古版一点草都不加
// （整块土）—— 三张共用 blockImage()。
// ---------------------------------------------------------------------------
const QColor kGrassLight(0x8c, 0xc8, 0x5a);
const QColor kGrassDark(0x5d, 0x9c, 0x3f);
const QColor kGrassEdge(0x4a, 0x82, 0x30);
const QColor kDirtLight(0x9c, 0x70, 0x4a);
const QColor kDirtMid(0x84, 0x5f, 0x3d);
const QColor kDirtDark(0x6e, 0x4c, 0x30);

/// 覆雪草块上面那三层（雪白 / 阴面 / 下沿）。照着草方块那三层的位置放，只是换成白的。
const QColor kSnowLight(0xf4, 0xf9, 0xfc);
const QColor kSnowDark(0xdd, 0xe9, 0xf2);
const QColor kSnowEdge(0xbc, 0xcf, 0xdc);

/// 幸运方块那几档金（底 / 深一档的斑 / 边），外加问号那个奶油色。
const QColor kLuckyLight(0xe9, 0xc2, 0x4c);
const QColor kLuckyDark(0xd0, 0xa3, 0x2a);
const QColor kLuckyEdge(0xa5, 0x7a, 0x16);
const QColor kLuckyInk(0xf8, 0xf2, 0xd2);

/// 幸运方块上那个问号：5 格宽 × 7 格高，`#` 是要画成奶油色的格子（最后那一行是底下那一点，
/// 中间空的那一行是它和上面那钩子之间的距离）。**写死**，不画字体 —— 字号一缩放就不是这个
/// 形状了，而且这一格要能对着像素断言。
const char *const kQuestionRows[7] = {
    ".###.",
    "#...#",
    "....#",
    "...#.",
    "..#..",
    ".....",
    "..#..",
};

const int kGrassDepth[16] = {3, 4, 4, 3, 5, 4, 3, 3, 4, 5, 4, 3, 4, 4, 3, 5};

/// 方块上那些深浅斑点（土上、金底上都是它）：看着不规整就行，不是随机数
/// （同样是"每次画出来必须一样"）。
int blockShade(int x, int y) { return (x * 7 + y * 13 + (x * y) % 5) % 3; }

/// 土那部分某一个格子什么颜色（三档斑）。草方块、覆雪草块、土块下面那截都是它。
QColor dirtPixel(int x, int y) {
    switch (blockShade(x, y)) {
        case 0:
            return kDirtLight;
        case 1:
            return kDirtMid;
        default:
            return kDirtDark;
    }
}

/// 一块 16×16 的方块底图。@p withTop 为假 = 上面一点草都没有（土块）；为真时上面那一层
/// 照 kGrassDepth 分层（顶色 → 阴面色 → 下沿色），底下全是土。
QImage blockImage(const QColor &topLight, const QColor &topDark, const QColor &topEdge,
                  bool withTop) {
    QImage img(16, 16, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    for (int x = 0; x < 16; ++x) {
        const int depth = kGrassDepth[x];
        for (int y = 0; y < 16; ++y) {
            QColor color = dirtPixel(x, y);
            if (withTop) {
                if (y <= depth - 3) {
                    color = topLight;
                } else if (y <= depth - 1) {
                    color = topDark;
                } else if (y == depth) {
                    color = topEdge;
                }
            }
            img.setPixelColor(x, y, color);
        }
    }
    return img;
}

/// 像素画放大到目标边长：最近邻，放大之后还得是方块，不能糊成一团。
QPixmap scaledBlock(const QImage &img, int size) {
    return QPixmap::fromImage(
        img.scaled(size, size, Qt::IgnoreAspectRatio, Qt::FastTransformation));
}

// ---------------------------------------------------------------------------
// 四个加载器的标志 —— **真图**。
//
// 四张 64×64 的成品 PNG（底色和圆角都烤在里面了），放在 assets/loader/ 下、由
// assets/icons.qrc 编进来 —— 哪张图是从哪儿取的、怎么拼的，见 loader/README.md。
//
// 顺序就是 MainWindowUI::modLoaderOptionNames() 那个顺序
// （0 Fabric / 1 Forge / 2 NeoForge / 3 OptiFine / 4 Quilt）。前四个与 iconCandidateKeys()
// 后四个键同序（实例图标候选表保持八项不变）。
// ---------------------------------------------------------------------------
const char *const kLoaderLogoFiles[5] = {
    ":/assets/loader/fabric.png",
    ":/assets/loader/forge.png",
    ":/assets/loader/neoforge.png",
    ":/assets/loader/optifine.png",
    ":/assets/loader/quilt.png",
};

/// 一张加载器标志缩到 @p size 见方。
/// @note 资源没编进来时（漏了 assets/icons.qrc）给**空图**，
/// 由调用方退回草方块 —— 那种"少了一张图"的失误要能在界面上看出来，
/// 不能悄悄画个空白。
QPixmap loaderLogoPixmap(int index, int size) {
    const QPixmap source(QString::fromLatin1(kLoaderLogoFiles[index]));
    if (source.isNull()) {
        return QPixmap();
    }
    // 四张成品都是 64 px。放大（高 DPI 屏、以后要更大的格子）走最近邻，像素画才不会被
    // 糊掉；缩小走平滑，18 px 那一档才不至于跳。
    return source.scaled(size, size, Qt::IgnoreAspectRatio,
                         size > source.width() ? Qt::FastTransformation
                                               : Qt::SmoothTransformation);
}

// Forge 那张是**只有形状**的两透明底之一：铁砧本身是深藏青 `#1E2D43`，压在深色主题
// 的底上（页面明度 20 上下）分不出边。这一档把它按主题的文字色重描一遍 —— 只换颜色，
// 形状和 alpha 一个像素都不动。另外三张自带浅色内容 / 自带底色，深色底上都还在。
constexpr int kForgeIndex = 1;

/// 把一张**只有形状**的图按 @p ink 重描：alpha 原样搬过来。
/// @note 走 `QImage`（ARGB32_Premultiplied）而不是 `QPixmap`：`QPixmap(size)` 在本机是
/// **不带 alpha 通道**的那一档，`DestinationIn` 画完再转回来，透明的地方会变成不透明的黑。
QPixmap tintedSilhouette(const QPixmap &source, const QColor &ink) {
    QImage canvas(source.size(), QImage::Format_ARGB32_Premultiplied);
    canvas.setDevicePixelRatio(source.devicePixelRatio());
    canvas.fill(ink);
    QPainter painter(&canvas);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(0, 0, source.toImage());
    painter.end();
    return QPixmap::fromImage(canvas);
}

/// 画一次就够了：这几张图在一棵树上是**每一行都要用**的（900 多条 × 每次重绘），
/// 每次都现画一遍太浪费。缓存按"要画的东西"键住（颜色那几个组合是有限的）。
QPixmap cached(const QString &key, const std::function<QPixmap()> &make) {
    static QHash<QString, QPixmap> cache;
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return it.value();
    }
    const QPixmap pixmap = make();
    cache.insert(key, pixmap);
    return pixmap;
}

}  // namespace

QPixmap VersionIcons::grassBlock(int size) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("grass/%1").arg(size), [size]() {
        return scaledBlock(blockImage(kGrassLight, kGrassDark, kGrassEdge, true), size);
    });
}

QPixmap VersionIcons::snowyGrassBlock(int size) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("snowy/%1").arg(size), [size]() {
        return scaledBlock(blockImage(kSnowLight, kSnowDark, kSnowEdge, true), size);
    });
}

QPixmap VersionIcons::dirtBlock(int size) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("dirt/%1").arg(size), [size]() {
        // 上面那三个颜色传土自己那三档：withTop 给的是假，它们一格都用不上 ——
        // 这么传是有意的，将来谁把那个假改成真，画出来还是土，不会突然冒出一层草。
        return scaledBlock(blockImage(kDirtLight, kDirtMid, kDirtDark, false), size);
    });
}

QPixmap VersionIcons::luckyBlock(int size) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("lucky/%1").arg(size), [size]() {
        QImage img(16, 16, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        for (int x = 0; x < 16; ++x) {
            for (int y = 0; y < 16; ++y) {
                // 最外一圈深一档：这个方块得看着是"一块"，不能糊成一片金。
                const bool frame = (x == 0 || y == 0 || x == 15 || y == 15);
                if (frame) {
                    img.setPixelColor(x, y, kLuckyEdge);
                } else {
                    img.setPixelColor(x, y,
                                      blockShade(x, y) == 0 ? kLuckyLight : kLuckyDark);
                }
            }
        }
        // 中间那个问号：5×7 的位图摆在 (5,4)，**写死**（见头文件里那句"不是画的字"）。
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (kQuestionRows[row][col] == '#') {
                    img.setPixelColor(5 + col, 4 + row, kLuckyInk);
                }
            }
        }
        return scaledBlock(img, size);
    });
}

QStringList VersionIcons::iconCandidateKeys() {
    // 前四个走 versionChannelMark() 那个顺序（正式版 / 快照版 / 远古版 / 愚人节版），
    // 后四个走 loaderMark() 那个顺序（Fabric / Forge / NeoForge / OptiFine）。
    // 两张表各自的唯一出处都在它们自己的注释里，这里只是把两组键拼起来。
    return {QStringLiteral("grass"),    QStringLiteral("snowy"), QStringLiteral("dirt"),
            QStringLiteral("lucky"),    QStringLiteral("fabric"), QStringLiteral("forge"),
            QStringLiteral("neoforge"), QStringLiteral("optifine")};
}

QPixmap VersionIcons::iconCandidateMark(int index, int size) {
    if (index < 0 || index >= iconCandidateKeys().size()) {
        return grassBlock(size);  // 越界 = 认不出来 = 当原版（和 loaderMark 一个规矩）
    }
    if (index < 4) {
        return versionChannelMark(index, size);
    }
    return loaderMark(index - 4, size);
}

QPixmap VersionIcons::iconMarkForKey(const QString &key, int size) {
    const int index = iconCandidateKeys().indexOf(key);
    if (index < 0) {
        return QPixmap();  // 认不出来：空图，让调用方自己决定退回什么
    }
    return iconCandidateMark(index, size);
}

QString VersionIcons::iconKeyFromValue(const QString &value) {
    const QString prefix = QStringLiteral("builtin:");
    if (!value.startsWith(prefix)) {
        return QString();
    }
    return value.mid(prefix.size());
}

QString VersionIcons::iconValueForKey(const QString &key) {
    return QStringLiteral("builtin:") + key;
}

QPixmap VersionIcons::fileMark(const QString &path, int size) {
    if (path.isEmpty() || size <= 0) {
        return QPixmap();
    }
    const QFileInfo info(path);
    if (!info.exists()) {
        return QPixmap();  // 文件没了：空图（调用方退回默认那张）
    }
    const QString key = QStringLiteral("file/%1/%2/%3")
                            .arg(path)
                            .arg(info.lastModified().toMSecsSinceEpoch())
                            .arg(size);
    return cached(key, [path, size]() {
        // 先问尺寸再解：`icon.png` 是从盘上读的，当外部输入看（见上面那两个常量）。
        QImageReader reader(path);
        const QSize pixels = reader.size();
        if (!pixels.isValid() || pixels.width() <= 0 || pixels.height() <= 0
            || pixels.width() > kMaxIconSourceSide || pixels.height() > kMaxIconSourceSide
            || qint64(pixels.width()) * pixels.height() > kMaxIconSourcePixels) {
            // 认不出来的格式（用户挑了个 .txt）、半截文件，或者一张"头里写着几个 G"
            // 的图：**空图**，不是一张黑图。调用方退回默认那张，用户看到的是
            // "它没生效"，而不是"我的图变成黑的了"。
            return QPixmap();
        }
        // 比目标格子大的先按格子尺寸解（有些插件仍会解全图，真正的闸是上面那个检查）。
        if (pixels.width() > size || pixels.height() > size) {
            reader.setScaledSize(pixels.scaled(size, size, Qt::KeepAspectRatio));
        }
        const QImage decoded = reader.read();
        if (decoded.isNull()) {
            return QPixmap();
        }
        const QPixmap source = QPixmap::fromImage(decoded);
        if (source.isNull()) {
            return QPixmap();
        }
        // 先按长宽比缩，再补成方的：树上那一格是方的，直接把一张扁图塞进去会被拉变形。
        const QPixmap scaled =
            source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmap canvas(size, size);
        canvas.fill(Qt::transparent);
        QPainter p(&canvas);
        p.drawPixmap((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
        return canvas;
    });
}

QPixmap VersionIcons::versionChannelMark(int channelIndex, int size) {
    if (size <= 0) {
        return QPixmap();
    }
    switch (channelIndex) {
        case 0:
            return grassBlock(size);  // 正式版：原版那个草方块，不变
        case 1:
            return snowyGrassBlock(size);
        case 2:
            return dirtBlock(size);
        case 3:
            return luckyBlock(size);
        default:
            return grassBlock(size);  // 越界 = 认不出来 = 当原版（和 loaderMark 一个规矩）
    }
}

QPixmap VersionIcons::loaderMark(int index, int size) {
    if (size <= 0) {
        return QPixmap();
    }
    // 「不装（纯原版）」和越界的下标都是草方块 —— 那两条路装出来的就是原版。
    if (index < 0 || index > 4) {
        return grassBlock(size);
    }
    // 深色主题下 Forge 那把深色铁砧按文字色重描（见上面 kForgeIndex 那段）。颜色进缓存键：
    // 用户改过文字色之后拿到的应该是新那一支。
    const QColor ink = index == kForgeIndex && Theme::isDark() ? Theme::tokens().text : QColor();
    const QString inkKey = ink.isValid() ? ink.name(QColor::HexArgb) : QStringLiteral("base");
    return cached(QStringLiteral("mark/%1/%2/%3").arg(index).arg(size).arg(inkKey),
                  [index, size, ink]() {
        const QPixmap logo = loaderLogoPixmap(index, size);
        // 图读不出来（资源没编进来）也总得画点什么：退回草方块，和"认不出下标"一条规矩。
        if (logo.isNull()) return grassBlock(size);
        return ink.isValid() ? tintedSilhouette(logo, ink) : logo;
    });
}

namespace {

/// 箭头三个朝向。**笔宽、端头这些只有一份** —— 版本树那两行和底栏那两颗按钮上画的
/// 必须是同一支笔画出来的箭头，不然同一个界面里的箭头会粗细不一。
enum class ChevronDir { Right, Down, Up };

/// 那两笔的坐标（都是边长的比例）。
QPolygonF chevronShape(ChevronDir dir, int size) {
    const auto at = [size](qreal v) { return v * size; };
    switch (dir) {
    case ChevronDir::Up:
        // 展开方向朝上 = 朝下那个 V 翻过来（底栏那两颗按钮点开是**向上**弹的）
        return QPolygonF({QPointF(at(0.3), at(0.6)), QPointF(at(0.5), at(0.35)),
                          QPointF(at(0.7), at(0.6))});
    case ChevronDir::Down:
        return QPolygonF({QPointF(at(0.3), at(0.4)), QPointF(at(0.5), at(0.65)),
                          QPointF(at(0.7), at(0.4))});
    case ChevronDir::Right:
        break;
    }
    return QPolygonF({QPointF(at(0.4), at(0.3)), QPointF(at(0.65), at(0.5)),
                      QPointF(at(0.4), at(0.7))});
}

/// 按朝向画一张、带缓存（缓存键里带朝向，三个朝向各存各的）。
QPixmap chevronPixmap(ChevronDir dir, int size, const QColor &color) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("chevron/%1/%2/%3").arg(int(dir)).arg(size).arg(color.name()),
                  [dir, size, color]() {
                      QPixmap pixmap(size, size);
                      pixmap.fill(Qt::transparent);
                      QPainter p(&pixmap);
                      p.setRenderHint(QPainter::Antialiasing, true);
                      QPen pen(color);
                      // 细了看不见、粗了糊成一个三角：按边长给个比例，别用一个写死的像素数。
                      pen.setWidthF(qMax(1.6, size / 7.0));
                      pen.setCapStyle(Qt::RoundCap);
                      pen.setJoinStyle(Qt::RoundJoin);
                      p.setPen(pen);
                      p.drawPolyline(chevronShape(dir, size));
                      return pixmap;
                  });
}


}  // namespace

QPixmap VersionIcons::chevron(bool expanded, int size, const QColor &color) {
    return chevronPixmap(expanded ? ChevronDir::Down : ChevronDir::Right, size, color);
}

QPixmap VersionIcons::chevronUp(int size, const QColor &color) {
    return chevronPixmap(ChevronDir::Up, size, color);
}

namespace {

/// ↑ / ↓ 这一对箭头的形状（带杆的实心箭头）—— **只有这一份**，两个方向由 `up` 挑。
///
/// 为什么合成一个函数：这两颗在**同一格里轮流出现**
/// （不在置顶组 → ↑「置顶」，在置顶组 → ↓「取消置顶」），比例必须逐像素对称 ——
/// 谁照着一份手抄一遍，改了一边比例之后另一边就不动了，而屏幕上表现成"两颗箭头
/// 长得不太一样"（很难认出是哪一处没跟上）。所以上下两个方向由**同一组比例**翻过来：
/// 上箭头里"离上边 0.15 的那个"就是下箭头里"离下边 0.15 的那个"。
QPixmap arrowPixmap(bool up, int size, const QColor &color) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("arrow/%1/%2/%3").arg(up ? 1 : 0).arg(size).arg(color.name()),
                  [up, size, color]() {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        // 三角和杆都是**实心填充**（和 triangleUp/gear 一个路数，笔宽在 16 px 下描边会糊）：
        // 一颗 16 px 的按钮里，"一根杆 + 一个箭头"得是两块实心的面才看得清。
        p.setPen(Qt::NoPen);
        p.setBrush(color);

        // 一维尺寸全按边长给比例（别写死像素）：这个字形在实例行里是 16 px，别处要用
        // 也就是同一个函数换个 size。
        //
        // 这几档是**量出来的**，两个约束一起定死它们：
        // ① 最外一圈不留色 ⇒ 三角形的左右两个底角、尖端、杆底，四个方向离画布边
        // 都得有 2 px 以上（16 px 那一档：左右各到 2.6 / 13.4，上下到 2.4 / 13.6）；
        // ② 箭头得比杆宽出一截才读得出"这是个箭头" ⇒ 半底 0.34 对杆半宽 0.10，
        // 差了 3.4 倍（16 px 下箭头宽 10.9、杆宽 3.2）。
        // 杆**不画满整格**：从箭头底边一直伸到 0.85，那一头留出同样的余量。
        const qreal centerX = size * 0.5;
        const qreal headHalf = size * 0.34;  // 箭头底边的半宽
        const qreal shaftHalf = size * 0.10; // 杆的半宽
        // 尖端 / 底边 / 杆末：下箭头就是整条竖直翻过来（`1 - x`），**不是**另给一组数。
        const qreal tip = up ? size * 0.15 : size * 0.85;
        const qreal headBase = up ? size * 0.52 : size * 0.48;
        const qreal shaftEnd = up ? size * 0.85 : size * 0.15;

        // 先画箭头、再画杆：杆的那条边和箭头底边**重合在同一条线**上，同色叠上去之后
        // 接缝处不会留下一条比别处浅的线（抗锯齿只在整幅的外轮廓上走）。
        QPolygonF head({QPointF(centerX, tip), QPointF(centerX + headHalf, headBase),
                        QPointF(centerX - headHalf, headBase)});
        p.drawPolygon(head);
        p.drawRect(QRectF(centerX - shaftHalf, qMin(headBase, shaftEnd), shaftHalf * 2,
                          qAbs(shaftEnd - headBase)));
        return pixmap;
    });
}

}  // namespace

QPixmap VersionIcons::arrowUp(int size, const QColor &color) {
    return arrowPixmap(true, size, color);
}

QPixmap VersionIcons::arrowDown(int size, const QColor &color) {
    return arrowPixmap(false, size, color);
}

namespace {

/// 上移/下移那两颗箭头的形状：一个**实心三角**（▲ / ▼）。
///
/// 为什么不用现成的那两支 chevron（^ / V）：组行行尾本来就有一个 chevron（展开/收起的
/// 那个），再摆两个同一支笔画的 V，一行里三个 V，用户分不出哪个是"展开"、哪个是"挪"。
/// 实心三角是另一族的形状（滚动条/数字框那两个小箭头就是它），一眼就分得开。
///
/// 尺寸取边长的 64% × 44%（宽一点、扁一点，和旁边 12 px 的 chevron 摆在一起才配得上），
/// 上下各留 28% 的边距 —— 贴到边上会看着比同一个格里的箭头大一号。
///
/// NOTE: 44% / 28% 是从下面那四行里**量出来**的：0.28~0.72 这一档才是屏幕上
/// 这个东西，照着"50% / 上下各 25%"去调形状调不出它。
QPolygonF triangleShape(bool up, int size) {
    const qreal s = size;
    const qreal left = s * 0.18;
    const qreal right = s * 0.82;
    const qreal near = s * 0.28;   // 底边（朝下那个的上沿）
    const qreal apex = s * 0.72;   // 尖端
    if (up) {
        return QPolygonF({QPointF(s * 0.5, near), QPointF(right, apex), QPointF(left, apex)});
    }
    return QPolygonF({QPointF(left, near), QPointF(right, near), QPointF(s * 0.5, apex)});
}

/// 画一个实心三角，带缓存（两个朝向各存各的）。
QPixmap trianglePixmap(bool up, int size, const QColor &color) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("triangle/%1/%2/%3").arg(up ? 1 : 0).arg(size).arg(color.name()),
                  [up, size, color]() {
                      QPixmap pixmap(size, size);
                      pixmap.fill(Qt::transparent);
                      QPainter p(&pixmap);
                      p.setRenderHint(QPainter::Antialiasing, true);
                      p.setPen(Qt::NoPen);
                      p.setBrush(color);
                      p.drawPolygon(triangleShape(up, size));
                      return pixmap;
                  });
}

}  // namespace

QPixmap VersionIcons::triangleUp(int size, const QColor &color) {
    return trianglePixmap(true, size, color);
}

QPixmap VersionIcons::triangleDown(int size, const QColor &color) {
    return trianglePixmap(false, size, color);
}

QPixmap VersionIcons::cross(int size, const QColor &color) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("cross/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color);
        // 和 chevron 同一个比例：细了看不见、粗了糊成一个点。
        pen.setWidthF(qMax(1.6, size / 7.0));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        // 两条对角线。留 22% 的边距：贴到边上会看着比同一个格里的箭头大一号。
        const qreal a = size * 0.22;
        const qreal b = size * 0.78;
        p.drawLine(QPointF(a, a), QPointF(b, b));
        p.drawLine(QPointF(b, a), QPointF(a, b));
        return pixmap;
    });
}

QPixmap VersionIcons::windowMinimize(int size, const QColor &color) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("winMin/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color);
        // 和 cross 一支笔：同样的 size/7 + 圆头。两颗按钮就挨着，粗细不一样看得出来。
        pen.setWidthF(qMax(1.6, size / 7.0));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        // 左右各留 22%（cross 也是 22%）：只画中间那一段横杠，不顶到格子边上。
        const qreal y = size * 0.5;
        p.drawLine(QPointF(size * 0.22, y), QPointF(size * 0.78, y));
        return pixmap;
    });
}

QIcon VersionIcons::actionIcon(Action action) { return QIcon(new ActionIconEngine(action)); }

QPixmap VersionIcons::refresh(int size, const QColor &color) {
    if (size <= 0) return {};
    return cached(QStringLiteral("refresh-line/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 24.0, size / 24.0);
        painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        // A restrained circular arrow, with an open head rather than an oversized filled wedge.
        painter.drawArc(QRectF(4.5, 4.5, 15, 15), 45 * 16, 290 * 16);
        QPainterPath head;
        head.moveTo(13.2, 6.7);
        head.lineTo(17.3, 6.7);
        head.lineTo(17.3, 2.8);
        painter.drawPath(head);
        return image;
    });
}

QPixmap VersionIcons::eye(int size, const QColor &color, bool crossed) {
    if (size <= 0) return {};
    return cached(QStringLiteral("eye/%1/%2/%3").arg(size).arg(color.name(QColor::HexArgb)).arg(int(crossed)),
                  [size, color, crossed]() {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 24.0, size / 24.0);
        painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPainterPath lid;
        lid.moveTo(2.5, 12);
        lid.quadTo(12, 3.2, 21.5, 12);
        lid.quadTo(12, 20.8, 2.5, 12);
        painter.drawPath(lid);
        painter.drawEllipse(QPointF(12, 12), 3.1, 3.1);
        if (crossed) painter.drawLine(QPointF(4.5, 19.5), QPointF(19.5, 4.5));
        return image;
    });
}

QPixmap VersionIcons::power(int size, const QColor &color) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("power/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);

        // 笔宽和 cross / chevron 同一个比例（size/7 ≈ 0.14）：同一行里几个图标得看着是
        // 同一支笔画出来的，细一档就会显得"这颗按钮小一号"。
        QPen pen(color);
        pen.setWidthF(qMax(1.4, size / 7.0));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        // 环心**略低于**正中：上面那一竖要从缺口里穿出去，整幅的重心落在格子里才正。
        const QPointF center(size / 2.0, size * 0.54);
        const qreal radius = size * 0.30;
        const QRectF box(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius);
        // 缺口开在**正上方、左右对称**（50° 起、顺时针扫 280° → 收在 130°，空的是
        // 50°~130° 那 80°）：机箱上那个开关就是这个长相，缺口偏一边会读成"转圈箭头"。
        // 角度按 Qt 那套（0° 在正右、逆时针为正、90° 是正上），见 refresh()。
        p.drawArc(box, 50 * 16, -280 * 16);

        // 那一竖：从环的上沿之上一点点，穿进缺口、落到接近环心。
        // NOTE: 起点**不能贴着格子上边** —— RoundCap 两头各撑出半个笔宽，贴边那一头会
        // 顶得比整个环还显眼（这一竖看着比环粗，就是这个）。
        p.drawLine(QPointF(center.x(), center.y() - radius - size * 0.07),
                   QPointF(center.x(), center.y() - size * 0.04));
        return pixmap;
    });
}

QPixmap VersionIcons::gear(int size, const QColor &color) {
    if (size <= 0) return {};
    return cached(QStringLiteral("gear-line/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 24.0, size / 24.0);
        painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPainterPath outline;
        constexpr qreal pitch = 60.0;
        for (int tooth = 0; tooth < 6; ++tooth) {
            for (int corner = 0; corner < 4; ++corner) {
                const qreal offsets[] = {0.06, 0.24, 0.76, 0.94};
                const qreal radius = (corner == 0 || corner == 3) ? 7.0 : 9.3;
                const qreal angle = qDegreesToRadians((tooth + offsets[corner]) * pitch);
                const QPointF point(12 + radius * qCos(angle), 12 + radius * qSin(angle));
                if (tooth == 0 && corner == 0) outline.moveTo(point);
                else outline.lineTo(point);
            }
        }
        outline.closeSubpath();
        painter.drawPath(outline);
        painter.drawEllipse(QPointF(12, 12), 3.0, 3.0);
        return image;
    });
}

QPixmap VersionIcons::pen(int size, const QColor &color) {
    if (size <= 0) return {};
    return cached(QStringLiteral("pen-line/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 24.0, size / 24.0);
        painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(QPolygonF({QPointF(4,16), QPointF(16,4), QPointF(20,8), QPointF(8,20), QPointF(4,20)}));
        painter.drawLine(QPointF(14,6), QPointF(18,10));
        return image;
    });
}

QPixmap VersionIcons::folderMark(int size, const QColor &color) {
    if (size <= 0) return {};
    return cached(QStringLiteral("folderMark-line/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 24.0, size / 24.0);
        painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPainterPath folder;
        folder.moveTo(3,7); folder.lineTo(9,7); folder.lineTo(11,10); folder.lineTo(21,10);
        folder.lineTo(19,20); folder.lineTo(3,20); folder.closeSubpath();
        painter.drawPath(folder);
        painter.drawLine(QPointF(3,11), QPointF(20.8,11));
        return image;
    });
}

QPixmap VersionIcons::trash(int size, const QColor &color) {
    if (size <= 0) return {};
    return cached(QStringLiteral("trash-line/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 24.0, size / 24.0);
        painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(4,7), QPointF(20,7));
        painter.drawLine(QPointF(9,4), QPointF(15,4));
        painter.drawLine(QPointF(9,4), QPointF(8,7));
        painter.drawLine(QPointF(15,4), QPointF(16,7));
        QPainterPath body;
        body.moveTo(6,7); body.lineTo(7,20); body.lineTo(17,20); body.lineTo(18,7);
        painter.drawPath(body);
        painter.drawLine(QPointF(10,11), QPointF(10.5,16));
        painter.drawLine(QPointF(14,11), QPointF(13.5,16));
        return image;
    });
}

QPixmap VersionIcons::infoMark(int size, const QColor &color) {
    if (size <= 0) return {};
    return cached(QStringLiteral("infoMark-line/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 24.0, size / 24.0);
        painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(12,12), 8.3, 8.3);
        painter.drawPoint(QPointF(12,8));
        painter.drawLine(QPointF(12,11.5), QPointF(12,16));
        return image;
    });
}

QPixmap VersionIcons::puzzleMark(int size, const QColor &color) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("puzzle/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(color);

        // 一块方片，右边凸一个圆钮（"插进别人"的那一头）、上边挖掉一个同样大的圆
        // （"让别人插进来"的那个坑）—— 这两笔就是"拼图"和"一块方片"的全部区别。
        // 右边那个圆钮是最外圈的东西，所以右边界只到 0.76，剩下的留给它。
        const qreal left = size * 0.12;
        const qreal right = size * 0.76;
        const qreal top = size * 0.20;
        const qreal bottom = size * 0.84;
        const qreal knob = (bottom - top) * 0.20;

        QPainterPath body;
        body.addRect(QRectF(left, top, right - left, bottom - top));
        QPainterPath bump;
        bump.addEllipse(QPointF(right, (top + bottom) / 2.0), knob, knob);
        body = body.united(bump);
        // 坑是**挖掉**的（和文件夹那道缝、垃圾桶那两条缝同一个做法）：图标底下是这一行的
        // 底色，画一个"底色的圆"在浅色/深色主题里各错一次。
        QPainterPath notch;
        notch.addEllipse(QPointF((left + right) / 2.0, top), knob, knob);
        p.drawPath(body.subtracted(notch));
        return pixmap;
    });
}

QPixmap VersionIcons::runStateMark(int size, const QColor &color) {
    if (size <= 0) {
        return QPixmap();
    }
    return cached(QStringLiteral("stateMark/%1/%2").arg(size).arg(color.name()), [size, color]() {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        // 设计稿的状态点是圆点；测试量的是正中那一像素，圆心处是满色。
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QRectF(0, 0, size, size));
        return pixmap;
    });
}

QPixmap VersionIcons::isoBlock(int kind, int size) {
    if (size <= 0) {
        return QPixmap();
    }
    // 顶 / 左 / 右三面，照抄设计稿 shell.js BLOCKS。
    static const QRgb kFaces[kIsoBlockKinds][3] = {
        {0x7cb862, 0x8a5a3b, 0x6b4129}, {0x7fe3dc, 0x45b3ad, 0x2f8a86}, {0xe25b4f, 0xb23a32, 0x86281f},
        {0xb79be8, 0x8d6fc8, 0x6a4fa3}, {0xe8d9a4, 0xcdbb82, 0xad9c63}, {0xf1b8c9, 0xd891a8, 0xb56f86},
        {0xa9adb5, 0x8a8f98, 0x6c7079}, {0xe39a6e, 0xc27a52, 0x9a5d3e},
    };
    kind = ((kind % kIsoBlockKinds) + kIsoBlockKinds) % kIsoBlockKinds;
    return cached(QStringLiteral("isoBlock/%1/%2").arg(kind).arg(size), [kind, size]() {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        // 设计稿 viewBox 28×32，按高缩放、水平居中。
        const qreal k = size / 32.0;
        p.translate((size - 28 * k) / 2, 0);
        p.scale(k, k);
        const QPolygonF top{{14, 1}, {27, 8.5}, {14, 16}, {1, 8.5}};
        p.setBrush(QColor(kFaces[kind][0]));
        p.drawPolygon(top);
        p.setBrush(QColor(kFaces[kind][1]));
        p.drawPolygon(QPolygonF{{1, 8.5}, {14, 16}, {14, 31}, {1, 23.5}});
        p.setBrush(QColor(kFaces[kind][2]));
        p.drawPolygon(QPolygonF{{27, 8.5}, {14, 16}, {14, 31}, {27, 23.5}});
        p.setBrush(QColor(255, 255, 255, 31));  // 顶面 .12 白
        p.drawPolygon(top);
        return pixmap;
    });
}

int VersionIcons::isoBlockKindFor(const QString &name) {
    quint32 hash = 2166136261u;  // FNV-1a
    for (const QChar c : name) {
        hash = (hash ^ c.unicode()) * 16777619u;
    }
    return int(hash % kIsoBlockKinds);
}

namespace {

/// SVG path 数据的一个小解析器：只认设计稿图标里出现的 M L H V C S A Z（大小写都认）。
QPainterPath parseSvgPath(const QString &data) {
    QPainterPath path;
    int i = 0;
    QChar command;
    QPointF current;
    QPointF start;
    QPointF lastControl;
    const auto skip = [&]() {
        while (i < data.size() && (data.at(i).isSpace() || data.at(i) == QLatin1Char(','))) ++i;
    };
    const auto number = [&]() {
        skip();
        int j = i;
        if (j < data.size() && (data.at(j) == QLatin1Char('-') || data.at(j) == QLatin1Char('+'))) ++j;
        bool dot = false;
        while (j < data.size() && (data.at(j).isDigit() || (data.at(j) == QLatin1Char('.') && !dot))) {
            if (data.at(j) == QLatin1Char('.')) dot = true;
            ++j;
        }
        const qreal v = QStringView(data).mid(i, j - i).toDouble();
        i = j;
        return v;
    };
    // 圆弧标志位可以紧挨着写，一次只吃一位。
    const auto flag = [&]() {
        skip();
        const bool v = i < data.size() && data.at(i) == QLatin1Char('1');
        ++i;
        return v;
    };
    const auto arcTo = [&path](QPointF p0, qreal rx, qreal ry, bool large, bool sweep, QPointF p1) {
        // SVG 规范 F.6.5（设计稿的弧都不旋转）：端点式换成中心式，再交给 arcTo。
        if (rx == 0 || ry == 0) { path.lineTo(p1); return; }
        const qreal x1 = (p0.x() - p1.x()) / 2, y1 = (p0.y() - p1.y()) / 2;
        rx = qAbs(rx); ry = qAbs(ry);
        const qreal lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lambda > 1) { rx *= qSqrt(lambda); ry *= qSqrt(lambda); }
        const qreal num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        const qreal den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        qreal k = den == 0 ? 0 : qSqrt(qMax<qreal>(0, num / den));
        if (large == sweep) k = -k;
        const qreal cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
        const QPointF center(cx1 + (p0.x() + p1.x()) / 2, cy1 + (p0.y() + p1.y()) / 2);
        const auto ang = [](qreal ux, qreal uy, qreal vx, qreal vy) {
            return qAtan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const qreal theta = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
        qreal delta = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
        if (!sweep && delta > 0) delta -= 2 * M_PI;
        if (sweep && delta < 0) delta += 2 * M_PI;
        // Qt 的角度向上为正，SVG 的 y 轴向下，所以取负号。
        path.arcTo(QRectF(center.x() - rx, center.y() - ry, 2 * rx, 2 * ry),
                   -qRadiansToDegrees(theta), -qRadiansToDegrees(delta));
    };
    while (true) {
        skip();
        if (i >= data.size()) break;
        if (data.at(i).isLetter()) command = data.at(i++);
        const bool rel = command.isLower();
        const QPointF base = rel ? current : QPointF();
        const char upper = char(command.toUpper().unicode());
        if (upper != 'C' && upper != 'S') lastControl = QPointF();
        switch (upper) {
        case 'M': {
            const qreal x = number(), y = number();
            current = base + QPointF(x, y);
            path.moveTo(current);
            start = current;
            command = rel ? QLatin1Char('l') : QLatin1Char('L');  // M 后面跟的坐标按 L 处理
            break;
        }
        case 'L': { const qreal x = number(), y = number(); current = base + QPointF(x, y); path.lineTo(current); break; }
        case 'H': { const qreal x = number(); current.setX(rel ? current.x() + x : x); path.lineTo(current); break; }
        case 'V': { const qreal y = number(); current.setY(rel ? current.y() + y : y); path.lineTo(current); break; }
        case 'C': {
            const qreal a = number(), b = number(), c = number(), d = number(), e = number(), f = number();
            const QPointF c1 = base + QPointF(a, b), c2 = base + QPointF(c, d), end = base + QPointF(e, f);
            path.cubicTo(c1, c2, end);
            lastControl = c2;
            current = end;
            break;
        }
        case 'S': {
            const qreal c = number(), d = number(), e = number(), f = number();
            const QPointF c1 = lastControl.isNull() ? current : 2 * current - lastControl;
            const QPointF c2 = base + QPointF(c, d), end = base + QPointF(e, f);
            path.cubicTo(c1, c2, end);
            lastControl = c2;
            current = end;
            break;
        }
        case 'A': {
            const qreal rx = number(), ry = number();
            number();  // x-axis-rotation：设计稿全是 0
            const bool large = flag(), sweep = flag();
            const qreal x = number(), y = number();
            const QPointF end = base + QPointF(x, y);
            arcTo(current, rx, ry, large, sweep, end);
            current = end;
            break;
        }
        case 'Z':
            path.closeSubpath();
            current = start;
            break;
        default:
            ++i;  // 不认识的命令：跳过，别死循环
            break;
        }
    }
    return path;
}

const QHash<QString, QString> &linePaths() {
    static const QHash<QString, QString> paths{
        {QStringLiteral("home"), QStringLiteral("M3.5 10.5 12 3.8l8.5 6.7M5.8 8.8V20h12.4V8.8M10 20v-5.2h4V20")},
        {QStringLiteral("manage"), QStringLiteral("M4 4h6.5v6.5H4zM13.5 4H20v6.5h-6.5zM4 13.5h6.5V20H4zM13.5 13.5H20V20h-6.5z")},
        {QStringLiteral("download"), QStringLiteral("M12 3.5v11M7.5 10.5 12 15l4.5-4.5M4 15.5V20h16v-4.5")},
        {QStringLiteral("settings"), QStringLiteral("M4 7h9M17 7h3M4 17h3M11 17h9M15 4.8v4.4M9 14.8v4.4M4 12h5M13 12h7")},
        {QStringLiteral("agent"), QStringLiteral("M11 3.5c.6 4.4 2.6 6.4 7 7-4.4.6-6.4 2.6-7 7-.6-4.4-2.6-6.4-7-7 4.4-.6 6.4-2.6 7-7zM18.5 15.5v5M16 18h5")},
        {QStringLiteral("toolbox"), QStringLiteral("M3.5 9h17v10.5h-17zM8.5 9V6.2c0-.9.7-1.7 1.7-1.7h3.6c.9 0 1.7.8 1.7 1.7V9M3.5 13.5h17M10.5 12v3M13.5 12v3")},
        {QStringLiteral("fold"), QStringLiteral("M4 5h16v14H4zM9.5 5v14M15.5 10l-2 2 2 2")},
        {QStringLiteral("unfold"), QStringLiteral("M4 5h16v14H4zM9.5 5v14M13.5 10l2 2-2 2")},
        {QStringLiteral("min"), QStringLiteral("M6.5 12h11")},
        {QStringLiteral("max"), QStringLiteral("M7 7h10v10H7z")},
        {QStringLiteral("restore"), QStringLiteral("M7 9h8v8H7zM9 9V7h8v8h-2")},
        {QStringLiteral("close"), QStringLiteral("M7 7l10 10M17 7 7 17")},
        {QStringLiteral("play"), QStringLiteral("M8 5.5v13l10.5-6.5z")},
        {QStringLiteral("swap"), QStringLiteral("M5 8.5h13l-3.5-3.5M19 15.5H6l3.5 3.5")},
        {QStringLiteral("gear"), QStringLiteral("M12 9a3 3 0 1 1 0 6 3 3 0 0 1 0-6zM12 3.5l1.6 2.2 2.7-.5.6 2.7 2.4 1.3-1.1 2.5 1.1 2.5-2.4 1.3-.6 2.7-2.7-.5L12 20.5l-1.6-2.2-2.7.5-.6-2.7-2.4-1.3L5.8 12 4.7 9.5l2.4-1.3.6-2.7 2.7.5z")},
        {QStringLiteral("chevR"), QStringLiteral("m10 7 5 5-5 5")},
        {QStringLiteral("chevD"), QStringLiteral("m7 10 5 5 5-5")},
        {QStringLiteral("chevU"), QStringLiteral("m7 14 5-5 5 5")},
        {QStringLiteral("plus"), QStringLiteral("M12 5v14M5 12h14")},
        {QStringLiteral("user"), QStringLiteral("M12 4.5a3.5 3.5 0 1 1 0 7 3.5 3.5 0 0 1 0-7zM5 19.5c.8-3.5 3.6-5.5 7-5.5s6.2 2 7 5.5")},
        {QStringLiteral("logout"), QStringLiteral("M9 20H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2h3M15 16l4-4-4-4M19 12H9")},
        {QStringLiteral("star"), QStringLiteral("M12 4.2l2.3 4.8 5.2.7-3.8 3.6.9 5.2-4.6-2.5-4.6 2.5.9-5.2-3.8-3.6 5.2-.7z")},
        {QStringLiteral("trash"), QStringLiteral("M4.5 7h15M9.5 7V4.5h5V7M6.5 7l.9 12.5h9.2l.9-12.5M10 11v5M14 11v5")},
        {QStringLiteral("folder"), QStringLiteral("M3.5 6.5c0-.8.7-1.5 1.5-1.5h4.3l2 2.2H19c.8 0 1.5.7 1.5 1.5v9.3c0 .8-.7 1.5-1.5 1.5H5c-.8 0-1.5-.7-1.5-1.5z")},
        {QStringLiteral("more"), QStringLiteral("M6 12h.01M12 12h.01M18 12h.01")},
        {QStringLiteral("list"), QStringLiteral("M9 7h11M9 12h11M9 17h11M4.5 7h.01M4.5 12h.01M4.5 17h.01")},
        {QStringLiteral("puzzle"), QStringLiteral("M10 4h4v3a2 2 0 1 0 4 0h2v4h-3a2 2 0 1 0 0 4h3v5H4v-5h3a2 2 0 1 0 0-4H4V7h6z")},
        // 7c：任务详情 / 对话框 / 日志窗 / 账号页用的几枚（照抄设计稿 shell.js、shell-extra.js）。
        {QStringLiteral("chevL"), QStringLiteral("m14 7-5 5 5 5")},
        {QStringLiteral("check"), QStringLiteral("m5.5 12.5 4 4 9-9")},
        {QStringLiteral("clock"), QStringLiteral("M12 4a8 8 0 1 1 0 16 8 8 0 0 1 0-16zM12 8v4.5l3 2")},
        {QStringLiteral("copy"), QStringLiteral("M9 9h10.5v10.5H9zM15 9V4.5H4.5V15H9")},
        {QStringLiteral("refresh"), QStringLiteral("M19.5 12a7.5 7.5 0 1 1-2.2-5.3M19.5 4.5v4h-4")},
        {QStringLiteral("info"), QStringLiteral("M12 4a8 8 0 1 1 0 16 8 8 0 0 1 0-16zM12 11v5M12 8h.01")},
        {QStringLiteral("warn"), QStringLiteral("M12 4.5 20.5 19h-17zM12 10v4M12 16.8h.01")},
        {QStringLiteral("open"), QStringLiteral("M14 4.5h5.5V10M19.5 4.5 11 13M18 14v5.5H4.5V6H10")},
        {QStringLiteral("log"), QStringLiteral("M5 4.5h14v15H5zM8.5 9h7M8.5 12.5h7M8.5 16h4")},
        {QStringLiteral("java"), QStringLiteral("M9 4.5c2 1.6-2 3 0 4.8M13 4c2.5 2-2.5 3.6 0 5.8M6 13h11v1.5a4 4 0 0 1-4 4H10a4 4 0 0 1-4-4zM17 14h1.5a2 2 0 0 1 0 4H16.5M5 21h13")},
        {QStringLiteral("pause"), QStringLiteral("M9 6v12M15 6v12")},
        {QStringLiteral("okc"), QStringLiteral("M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM8 12.2l2.7 2.7L16 9.6")},
        {QStringLiteral("xc"), QStringLiteral("M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM9 9l6 6M15 9l-6 6")},
        {QStringLiteral("dots"), QStringLiteral("M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM8 12h.01M12 12h.01M16 12h.01")},
        {QStringLiteral("up"), QStringLiteral("M7 14.5l5-5 5 5")},
        {QStringLiteral("down"), QStringLiteral("M7 9.5l5 5 5-5")},
        {QStringLiteral("runc"), QStringLiteral("M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM8.5 12h.01M12 12h.01M15.5 12h.01")},
        {QStringLiteral("grip"), QStringLiteral("M9 6h.01M15 6h.01M9 12h.01M15 12h.01M9 18h.01M15 18h.01")},
        {QStringLiteral("upload"), QStringLiteral("M12 15.5V4.5M7.5 8.5 12 4l4.5 4.5M4 15.5V20h16v-4.5")},
        {QStringLiteral("export"), QStringLiteral("M12 4v11M7.5 8.5 12 4l4.5 4.5M5 13v6.5h14V13")},
        {QStringLiteral("eraser"), QStringLiteral("M8 20h12M5.5 14.5l8-8a2 2 0 0 1 2.8 0l2.2 2.2a2 2 0 0 1 0 2.8L11 19H8z")},
        {QStringLiteral("search"), QStringLiteral("M10.5 4a6.5 6.5 0 1 1 0 13 6.5 6.5 0 0 1 0-13zM15.5 15.5 20 20")},
        {QStringLiteral("edit"), QStringLiteral("M4.5 19.5h4l10-10-4-4-10 10zM13 7l4 4")},
        {QStringLiteral("lock"), QStringLiteral("M5.5 11h13v9h-13zM8.5 11V8l1.5-2.5h4L15.5 8v3")},
        {QStringLiteral("send"), QStringLiteral("M12 19V5.5M6.5 11 12 5.5l5.5 5.5")},
        {QStringLiteral("stop"), QStringLiteral("M7.5 7.5h9v9h-9z")},
        {QStringLiteral("ms"), QStringLiteral("M4 4h7.5v7.5H4zM12.5 4H20v7.5h-7.5zM4 12.5h7.5V20H4zM12.5 12.5H20V20h-7.5z")},
    };
    return paths;
}

}  // namespace

namespace VersionIcons {

void paintLine(QPainter &painter, const QString &name, const QRectF &box, const QColor &color, qreal stroke) {
    const auto it = linePaths().constFind(name);
    if (it == linePaths().constEnd()) return;
    static QHash<QString, QPainterPath> cache;
    auto cached = cache.find(name);
    if (cached == cache.end()) cached = cache.insert(name, parseSvgPath(*it));
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(box.topLeft());
    painter.scale(box.width() / 24.0, box.height() / 24.0);
    // 线宽按 24 网格给（设计稿 1.6），和 SVG 一样随图标大小缩放。
    painter.setPen(QPen(color, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(name == QLatin1String("play") ? QBrush(color) : QBrush(Qt::NoBrush));
    painter.drawPath(*cached);
    painter.restore();
}

QPixmap line(const QString &name, int size, const QColor &color, qreal stroke) {
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    paintLine(p, name, QRectF(0, 0, size, size), color, stroke);
    return pixmap;
}

}  // namespace VersionIcons
