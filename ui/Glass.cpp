/**
 * @file Glass.cpp
 * @brief 整窗磨砂的实现：四档与回退、着色 alpha 现算、风景底与主页场景的缓存。
 */

#include "Glass.hpp"

#include "Animations.hpp"

#include "SystemBackdrop.hpp"
#include "Theme.hpp"

#include <QApplication>
#include <QEvent>
#include <QOperatingSystemVersion>
#include <QPainter>
#include <QResizeEvent>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QVariantAnimation>
#include <QEasingCurve>

#include <cmath>
#include <optional>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

/// 场景图在 icons.qrc 里；它编在静态库里时没人引用那个初始化函数就会被链接器丢掉。
/// Q_INIT_RESOURCE 不能写在命名空间里。
static void initGlassResources() { Q_INIT_RESOURCE(icons); }

namespace Glass {
namespace {

/// 拖窗口改尺寸时，停手 120 ms 再按新尺寸重算（中间那几帧把旧缓存拉伸着贴）。
constexpr int kDebounceMs = 120;
/// 模糊在 1/8 尺寸上做，放大回来约等于 26 px 高斯（半径 3 时）。
constexpr int kDownscale = 8;
/// 主页场景左沿渐隐进导航轨的宽度（逻辑像素）。

QPointer<Backdrop> &activeBackdrop() {
    static QPointer<Backdrop> backdrop;
    return backdrop;
}

Mode parseMode(const QString &key, Mode fallback) {
    for (Mode m : {Mode::Acrylic, Mode::Mica, Mode::Scene, Mode::Solid}) {
        if (key.compare(modeKey(m), Qt::CaseInsensitive) == 0) return m;
    }
    return fallback;
}

bool highContrast() {
#ifdef Q_OS_WIN
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) != FALSE)
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
#endif
    return false;
}

/// 系统「透明效果」开关（设置 → 个性化 → 颜色）。
bool transparencyOff() {
#ifdef Q_OS_WIN
    const QSettings personalize(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return personalize.value(QStringLiteral("EnableTransparency"), 1).toInt() == 0;
#else
    return false;
#endif
}

bool systemBackdropSupported() {
    return QGuiApplication::platformName() == QLatin1String("windows") &&
           QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows11_22H2;
}

/// 四档判据要读的那几样，**一次读完留着**。
///
/// `enabled()` 在每一次重绘上（`ThemeBackground::paintEvent`、`SkinViewer`、浮层），
/// 而它底下的 `transparencyOff()` 是一次原生 `QSettings` 读注册表、`highContrast()` 是一次
/// `SystemParametersInfoW`：实例页来回切的时候采到过 64 ms 的栈就在这儿。值只会在三个地方
/// 变 —— 本进程写设置（`setMode()`）、主题 / 无障碍设置变化（事件过滤器）、系统个人化
/// 设置变化（切回窗口时 Qt 派的 `ApplicationStateChange` / `ThemeChange`）—— 都在下面
/// 逐条清掉这个缓存。
struct BackdropSettings {
    bool valid = false;
    Mode chosen = Mode::Scene;
    bool highContrast = false;
    bool transparencyOff = false;
    bool systemBackdrop = false;
};

BackdropSettings &settingsCache() {
    static BackdropSettings cache;
    return cache;
}

/// 验证工程把"系统那三位"钉成给定值用；产品里永远是空的（现读系统）。
struct SystemFlagsForTest {
    bool highContrast = false;
    bool transparencyOff = false;
    bool systemBackdrop = false;
};

std::optional<SystemFlagsForTest> &systemFlagsForTest() {
    static std::optional<SystemFlagsForTest> flags;
    return flags;
}

void refreshSettingsCache() {
    BackdropSettings &cache = settingsCache();
    QSettings settings;
    const bool legacyGlassOff =
        settings.contains(QStringLiteral("Theme/Glass")) &&
        !settings.value(QStringLiteral("Theme/Glass")).toBool();
    const Mode fallback = factoryMode(QThread::idealThreadCount(), legacyGlassOff);
    cache.chosen = parseMode(settings.value(settingsKey()).toString(), fallback);
    if (const std::optional<SystemFlagsForTest> &pinned = systemFlagsForTest()) {
        cache.highContrast = pinned->highContrast;
        cache.transparencyOff = pinned->transparencyOff;
        cache.systemBackdrop = pinned->systemBackdrop;
    } else {
        cache.highContrast = highContrast();
        cache.transparencyOff = transparencyOff();
        cache.systemBackdrop = systemBackdropSupported();
    }
    cache.valid = true;
}

const BackdropSettings &cachedSettings() {
    BackdropSettings &cache = settingsCache();
    if (!cache.valid) refreshSettingsCache();
    return cache;
}

/// 主题 / 无障碍 / 系统个人化设置可能变了：丢掉缓存。
///
/// 早先这里只缓存了「用户选的那一档」（`modeCache`），系统那两位每次现读。现在两位也进来
/// 了，所以除了 `setMode()` 之外还要有人在"用户去系统设置里按了开关再切回来"时清一次 ——
/// 那几种变化 Qt 会在应用身上派 `ThemeChange` / `ApplicationPaletteChange` /
/// `ApplicationStateChange`。
class SettingsCacheInvalidator final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        switch (event->type()) {
        case QEvent::ThemeChange:
        case QEvent::ApplicationPaletteChange:
        case QEvent::ApplicationStateChange:
            invalidateSettingsCache();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }
};

/// 当前主题用哪一张风景（「跟着系统」按这一刻的深浅挑）。
QString sceneId() {
    const QString id = Theme::current().presetId;
    if (id == QLatin1String("dark") || id == QLatin1String("emerald") || id == QLatin1String("ocean"))
        return id;
    // 走到这儿只剩「跟着系统」：认不出的 id 在 `Theme::normalize()` 里已经落到出厂那一档
    // （暮色雪山）了，不会带着空 presetId 上来。
    return Theme::isDark() ? QStringLiteral("dark") : QStringLiteral("emerald");
}

bool darkTokens() { return Theme::tokens().page.lightness() < 128; }

double linearChannel(int value) {
    const double c = value / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor &color) {
    return 0.2126 * linearChannel(color.red()) + 0.7152 * linearChannel(color.green()) +
           0.0722 * linearChannel(color.blue());
}

/// 每个像素往 `target` 混 `amount`，再按 `saturation` 拉饱和（1 = 不动）。
void processFrost(QImage &image, const QColor &target, qreal amount, qreal saturation) {
    const int tr = target.red(), tg = target.green(), tb = target.blue();
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = line[x];
            qreal r = qRed(px), g = qGreen(px), b = qBlue(px);
            const qreal grey = 0.299 * r + 0.587 * g + 0.114 * b;
            r = grey + (r - grey) * saturation;
            g = grey + (g - grey) * saturation;
            b = grey + (b - grey) * saturation;
            r += (tr - r) * amount;
            g += (tg - g) * amount;
            b += (tb - b) * amount;
            line[x] = qRgb(qBound(0, qRound(r), 255), qBound(0, qRound(g), 255), qBound(0, qRound(b), 255));
        }
    }
}

/// 一张图里最暗、最亮的两个像素（按相对亮度）。
void extremes(const QImage &image, const QRect &area, QColor *darkest, QColor *brightest) {
    double lo = 2.0, hi = -1.0;
    const QRect r = area.intersected(image.rect());
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x) {
            const QColor c = QColor::fromRgb(line[x]);
            const double l = luminance(c);
            if (l < lo) {
                lo = l;
                if (darkest != nullptr) *darkest = c;
            }
            if (l > hi) {
                hi = l;
                if (brightest != nullptr) *brightest = c;
            }
        }
    }
}

/// `source` 按 Cover 铺满 `size`。
QImage coverScaled(const QImage &source, const QSize &size, const QColor &fill) {
    QImage out(size, QImage::Format_RGB32);
    out.fill(fill);
    if (source.isNull()) return out;
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QSize scaled = source.size().scaled(size, Qt::KeepAspectRatioByExpanding);
    p.drawImage(QRect(QPoint((size.width() - scaled.width()) / 2, (size.height() - scaled.height()) / 2), scaled),
                source);
    return out;
}


}  // namespace

// ---------------------------------------------------------------------------
// 四档与回退
// ---------------------------------------------------------------------------

void invalidateSettingsCache() {
    settingsCache().valid = false;
}

void setSystemFlagsForTest(bool highContrast, bool transparencyOff, bool systemBackdrop) {
    systemFlagsForTest() = SystemFlagsForTest{highContrast, transparencyOff, systemBackdrop};
    invalidateSettingsCache();
}

void clearSystemFlagsForTest() {
    systemFlagsForTest().reset();
    invalidateSettingsCache();
}

QString settingsKey() { return QStringLiteral("Theme/Backdrop"); }

QString modeKey(Mode mode) {
    switch (mode) {
    case Mode::Acrylic: return QStringLiteral("acrylic");
    case Mode::Mica: return QStringLiteral("mica");
    case Mode::Scene: return QStringLiteral("scene");
    case Mode::Solid: return QStringLiteral("solid");
    }
    return QStringLiteral("acrylic");
}

QString modeName(Mode mode) {
    switch (mode) {
    case Mode::Acrylic: return QCoreApplication::translate("Glass", "亚克力");
    case Mode::Mica: return QCoreApplication::translate("Glass", "云母");
    case Mode::Scene: return QCoreApplication::translate("Glass", "风景");
    case Mode::Solid: return QCoreApplication::translate("Glass", "纯色");
    }
    return QString();
}

QString modeShortName(Mode mode) {
    if (mode == Mode::Scene) return QCoreApplication::translate("Glass", "风景");
    return modeName(mode);
}

Mode factoryMode(int threadCount, bool legacyGlassOff) {
    // 核数太少的机器（≤ 2 线程）出厂纯色：那一档的模糊是亏的。旧版「磨砂效果」关着 ⇒ 纯色。
    if (legacyGlassOff) return Mode::Solid;
    return threadCount > 2 ? Mode::Scene : Mode::Solid;
}

Mode mode() {
    const QByteArray env = qgetenv("WTFML_BACKDROP");
    if (!env.isEmpty()) return parseMode(QString::fromLatin1(env), Mode::Scene);
    return cachedSettings().chosen;
}

Mode effectiveMode() {
    const BackdropSettings &settings = cachedSettings();
    const Mode chosen = settings.chosen;
    if (chosen == Mode::Solid || settings.highContrast || settings.transparencyOff) return Mode::Solid;
    if ((chosen == Mode::Acrylic || chosen == Mode::Mica) && !settings.systemBackdrop) return Mode::Scene;
    return chosen;
}

QString fallbackReason() {
    const BackdropSettings &settings = cachedSettings();
    const Mode chosen = settings.chosen;
    if (chosen == Mode::Solid) return QString();
    if (settings.highContrast) {
        return QCoreApplication::translate("Glass", "系统开着高对比度，用纯色。");
    }
    if (settings.transparencyOff) {
        return QCoreApplication::translate("Glass", "系统关了透明效果，用纯色。");
    }
    if ((chosen == Mode::Acrylic || chosen == Mode::Mica) && !settings.systemBackdrop)
        return QCoreApplication::translate("Glass", "需要 Windows 11 22H2 或更新，已改用风景底。");
    return QString();
}

void setMode(Mode m) {
    QSettings().setValue(settingsKey(), modeKey(m));
    // 写设置的**唯一**生产入口就是这里；写完当场丢掉缓存，下一次 `enabled()` 读到的就是
    // 刚落盘这一档（不是等某个事件把缓存清掉）。
    invalidateSettingsCache();
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        Theme::apply(*app);
        for (QWidget *top : QApplication::topLevelWidgets()) top->update();
    }
}

bool enabled() { return effectiveMode() != Mode::Solid; }
bool sceneShown() { return !cachedSettings().highContrast; }

bool systemBackdrop() {
    const Mode m = effectiveMode();
    return m == Mode::Acrylic || m == Mode::Mica;
}

// 出厂 20：三主题 × 亚克力 / 云母 / 风景底里对比度要的最低 alpha 是 0.51（暮色雪山风景底；亚克力 / 云母最低 0.55 冰川碧湖），20 低于它，
// 出厂时每一档都正好落在「正文、次要字 ≥ 4.5:1 的最薄着色」，磨砂透得最多。
int tintStrength() { return qBound(0, QSettings().value(QStringLiteral("Theme/BackdropTint"), kDefaultTintStrength).toInt(), 100); }
int blurStrength() { return qBound(0, QSettings().value(QStringLiteral("Theme/BackdropBlur"), 50).toInt(), 100); }

void setTintStrength(int value) {
    QSettings().setValue(QStringLiteral("Theme/BackdropTint"), qBound(0, value, 100));
    if (Backdrop *b = Backdrop::active()) b->update();
}

void setBlurStrength(int value) {
    QSettings().setValue(QStringLiteral("Theme/BackdropBlur"), qBound(0, value, 100));
    if (Backdrop *b = Backdrop::active()) {
        b->rebuildNow();
        b->update();
    }
}

void install(QApplication &app) {
    initGlassResources();
    // 系统个人化 / 无障碍那两位的缓存靠这个过滤器清（见 SettingsCacheInvalidator）。
    // 产品与验证工程各调一次 `install()`，装过的就不再装第二条。
    if (app.findChild<QObject *>(QStringLiteral("glassSettingsCacheInvalidator")) == nullptr) {
        auto *invalidator = new SettingsCacheInvalidator(&app);
        invalidator->setObjectName(QStringLiteral("glassSettingsCacheInvalidator"));
    }
}

// ---------------------------------------------------------------------------
// 对比度
// ---------------------------------------------------------------------------

double contrast(const QColor &a, const QColor &b) {
    const double la = luminance(a), lb = luminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

QColor over(const QColor &top, const QColor &bottom) {
    const qreal a = top.alphaF();
    const auto mix = [a](int t, int b) { return qBound(0, qRound(t * a + b * (1.0 - a)), 255); };
    return QColor(mix(top.red(), bottom.red()), mix(top.green(), bottom.green()), mix(top.blue(), bottom.blue()));
}

qreal minimumTintAlpha(const QColor &tint, const QList<QPair<QColor, double>> &inks,
                       const QList<QColor> &backgrounds, qreal floor) {
    // 逐 1% 往上试：亮度沿 alpha 不一定单调（三个通道可能往不同方向走），二分不保险。
    for (int step = qBound(0, qCeil(floor * 100), 100); step <= 100; ++step) {
        QColor layer = tint;
        layer.setAlphaF(step / 100.0);
        bool ok = true;
        for (const QColor &bg : backgrounds) {
            const QColor result = over(layer, bg);
            for (const auto &ink : inks) {
                if (contrast(over(ink.first, result), result) < ink.second) ok = false;
            }
        }
        if (ok) return step / 100.0;
    }
    return 1.0;
}

QList<QColor> systemBackdropRange() {
    // 背后是纯黑、纯白和六个纯色时，系统亚克力、云母（激活态，见 ui/SystemBackdrop）合出来的颜色。
    // 浅色亚克力背后纯黑时是 #878787，比旧估计的 #b4 暗得多。
    const bool dark = darkTokens();
    QList<QColor> out;
    for (const SystemBackdrop::Material material : {SystemBackdrop::Material::Acrylic, SystemBackdrop::Material::Mica}) {
        const SystemBackdrop::Recipe recipe = SystemBackdrop::recipe(material, dark);
        for (const unsigned behind : {0x000000u, 0xffffffu, 0xff0000u, 0x00ff00u, 0x0000ffu, 0x00ffffu, 0xff00ffu, 0xffff00u})
            out << QColor::fromRgb(SystemBackdrop::composite(recipe, behind));
    }
    return out;
}

QList<QPair<QColor, double>> bodyInks() {
    const Theme::Tokens t = Theme::tokens();
    return {{t.text, 4.5}, {t.secondary, 4.5}};
}

QImage boxBlur(const QImage &source, int radius) {
    QImage image = source.convertToFormat(QImage::Format_RGB32);
    if (radius <= 0 || image.isNull()) return image;
    const int w = image.width(), h = image.height();
    QList<QRgb> buffer(qMax(w, h));
    const auto pass = [&](bool horizontal) {
        const int lines = horizontal ? h : w;
        const int length = horizontal ? w : h;
        for (int line = 0; line < lines; ++line) {
            const auto at = [&](int i) -> QRgb & {
                const int x = horizontal ? i : line, y = horizontal ? line : i;
                return reinterpret_cast<QRgb *>(image.scanLine(y))[x];
            };
            for (int i = 0; i < length; ++i) buffer[i] = at(i);
            int sr = 0, sg = 0, sb = 0;
            const int window = 2 * radius + 1;
            for (int k = -radius; k <= radius; ++k) {
                const QRgb px = buffer[qBound(0, k, length - 1)];
                sr += qRed(px);
                sg += qGreen(px);
                sb += qBlue(px);
            }
            for (int i = 0; i < length; ++i) {
                at(i) = qRgb(sr / window, sg / window, sb / window);
                const QRgb out = buffer[qBound(0, i - radius, length - 1)];
                const QRgb in = buffer[qBound(0, i + radius + 1, length - 1)];
                sr += qRed(in) - qRed(out);
                sg += qGreen(in) - qGreen(out);
                sb += qBlue(in) - qBlue(out);
            }
        }
    };
    for (int i = 0; i < 3; ++i) {
        pass(true);
        pass(false);
    }
    return image;
}

// ---------------------------------------------------------------------------
// 整窗那一层底
// ---------------------------------------------------------------------------

Backdrop::Backdrop(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("glassBackdrop"));
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, [this] {
        rebuildNow();
        update();
    });
    activeBackdrop() = this;
    // 主页风景淡入 / 淡出：只动风景那一层的不透明度，磨砂和着色层不动（导航轨也不动）。
    m_heroFade = new QVariantAnimation(this);
    m_heroFade->setObjectName(QStringLiteral("glassHeroFade"));
    m_heroFade->setEasingCurve(QEasingCurve::InOutQuad);
    connect(m_heroFade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_heroOpacity = value.toReal();
        update();
        emit heroOpacityChanged(m_heroOpacity);
    });
    connect(m_heroFade, &QAbstractAnimation::finished, this, [this] {
        m_heroOpacity = m_heroFade->endValue().toReal();  // 终值钉死成 0 / 1，不留缓动的尾数
        update();
        emit heroOpacityChanged(m_heroOpacity);
    });
    Theme::follow(this, [this] {
        rebuildNow();
        update();
    });
}

Backdrop::~Backdrop() = default;

Backdrop *Backdrop::of(const QWidget *widget) {
    for (const QWidget *w = widget; w != nullptr; w = w->parentWidget()) {
        if (auto *backdrop = qobject_cast<Backdrop *>(const_cast<QWidget *>(w))) return backdrop;
        if (w->isWindow()) break;
    }
    return nullptr;
}

Backdrop *Backdrop::active() { return activeBackdrop().data(); }

void Backdrop::setHeroWidget(QWidget *widget) {
    if (m_hero != nullptr) m_hero->removeEventFilter(this);
    m_hero = widget;
    if (widget != nullptr) widget->installEventFilter(this);
    update();
}

bool Backdrop::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_hero) {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::Move:
        case QEvent::Resize:
            scheduleRebuild();
            update();
            noteHeroShown();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Backdrop::setHeroCovered(bool covered) {
    if (m_heroCovered == covered) return;
    m_heroCovered = covered;
    update();
    noteHeroShown();
}

void Backdrop::noteHeroShown() {
    const bool shown = heroShown();
    if (shown == m_heroWasShown) return;
    m_heroWasShown = shown;
    fadeHeroTo(shown ? 1.0 : 0.0);
}

void Backdrop::fadeHeroTo(qreal target) {
    m_heroFade->stop();
    // 开程序「雾散开」：首帧之前风景停在 0（只有磨砂），首帧画完再从 0 淡上来（见 paintEvent 末尾）。
    if (!m_painted && m_deferHero) {
        m_heroOpacity = 0.0;
        emit heroOpacityChanged(m_heroOpacity);
        return;
    }
    m_heroFade->setEasingCurve(QEasingCurve::InOutQuad);
    const qreal from = m_heroOpacity;
    if (!Anim::enabled() || !isVisible() || !m_painted || from == target) {
        m_heroOpacity = target;
        update();
        emit heroOpacityChanged(m_heroOpacity);
        return;
    }
    // 半路折返（淡到一半又切回来）从当前值接着走，时长按剩下的路程折算。
    m_heroFade->setStartValue(from);
    m_heroFade->setEndValue(target);
    m_heroFade->setDuration(qMax(1, qRound(kHeroFadeMs * qAbs(target - from))));
    m_heroFade->start();
}

void Backdrop::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    // 窗口真的收起来了（不是最小化）：下次露面先按终值画，不在露面那一刻淡一次。
    if (!event->spontaneous()) m_painted = false;
}

QRect Backdrop::heroRect() const {
    if (m_hero == nullptr || m_heroCovered || !m_hero->isVisible() || m_hero->window() != window()) return QRect();
    // 全出血：铺满整窗，导航轨和右上那条窗口按钮条下面也是清晰风景（设计稿 .window.hero）。
    return rect();
}

QColor Backdrop::tint() const {
    const Theme::Tokens t = Theme::tokens();
    QColor base = t.page;
    const Mode m = effectiveMode();
    if (m == Mode::Solid) return base;
    const qreal floor = tintStrength() / 100.0;
    const QList<QColor> range = m == Mode::Scene ? QList<QColor>{m_darkest, m_brightest} : systemBackdropRange();
    base.setAlphaF(minimumTintAlpha(base, bodyInks(), range, floor));
    return base;
}

QImage Backdrop::sourceImage() {
    const Theme::PartImage custom = Theme::partImage(Theme::Part::PageBackground);
    const QString builtIn = QStringLiteral(":/assets/scenes/%1.jpg").arg(sceneId());
    const QString path = custom.isSet() ? custom.path : builtIn;
    if (path != m_sourceKey || m_source.isNull()) {
        m_sourceKey = path;
        // 解码走 `Theme::cachedImage()`：页面底（`ThemeBackground`）铺的常常就是同一张图，
        // 各解一遍等于同一份文件解两三次。
        m_source = Theme::cachedImage(path);
        // 用户那张读不出来（拔了 U 盘之类）：退回主题自带的风景，不报错。
        if (m_source.isNull() && custom.isSet()) m_source = Theme::cachedImage(builtIn);
    }
    return m_source;
}

void Backdrop::rebuildNow() {
    m_debounce->stop();
    const qreal dpr = devicePixelRatioF();
    const QSize device = (QSizeF(size()) * dpr).toSize();
    if (device.isEmpty()) return;
    const QImage source = sourceImage();
    const Theme::Tokens t = Theme::tokens();
    const bool dark = darkTokens();

    // 风景底：在 1/8 尺寸上铺满、模糊、往令牌色混，贴的时候放大（平滑缩放本身再糊一层）。
    const int blurRadius = 1 + blurStrength() * 5 / 100;
    const QString frostKey = m_sourceKey + QLatin1Char('|') + QString::number(device.width()) + QLatin1Char('x') +
                             QString::number(device.height()) + QLatin1Char('|') + t.page.name() +
                             QLatin1Char('|') + QString::number(blurRadius);
    if (frostKey != m_frostKey || m_frost.isNull()) {
        m_frostKey = frostKey;
        const QSize small(qMax(1, device.width() / kDownscale), qMax(1, device.height() / kDownscale));
        QImage frost = boxBlur(coverScaled(source, small, t.page), blurRadius);
        processFrost(frost, t.page, dark ? 0.35 : 0.3, dark ? 1.05 : 1.15);
        extremes(frost, frost.rect(), &m_darkest, &m_brightest);
        m_frostSmall = frost;
        m_frost = QPixmap::fromImage(frost.scaled(device, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        m_frost.setDevicePixelRatio(dpr);
        m_frostSize = size();
    }
    rebuildHero();
}

void Backdrop::rebuildHero() {
    // 不在主页（风景没露）也备好这一张：导航轨品牌标按「任一页」取色要量它（colorsUnderAnyPage）。
    if (m_hero == nullptr || m_hero->window() != window()) return;
    const QRect hero = rect();
    if (hero.isEmpty()) return;
    const qreal dpr = devicePixelRatioF();
    const QSize device = (QSizeF(hero.size()) * dpr).toSize();
    const Theme::Tokens t = Theme::tokens();
    const QWidget *rail = findChild<QWidget *>(QStringLiteral("titleBar"));
    // 雾只在亮主题有；深色主题不把轨宽放进键里，折叠动画时不必每帧重画整张图。
    const int railWidth = t.railFog > 0 && rail != nullptr && rail->isVisible() ? rail->width() : 0;
    const QString key = m_sourceKey + QLatin1Char('|') + QString::number(device.width()) + QLatin1Char('x') +
                        QString::number(device.height()) + QLatin1Char('|') + t.heroVeil.name() +
                        QLatin1Char('|') + QString::number(t.heroTop) + QLatin1Char('|') +
                        QString::number(t.heroScrim) + QLatin1Char('|') + QString::number(t.railFog) +
                        QLatin1Char('|') + QString::number(railWidth);
    if (key == m_heroKey && !m_heroPixmap.isNull()) {
        m_heroCacheRect = hero;
        return;
    }
    m_heroKey = key;
    QImage sharp = coverScaled(sourceImage(), device, t.page).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    {
        // 设计稿 tokens.md 第 4 节：四层 hero-veil 色渐变压暗角（左侧线性、底部线性、左上径向
        // 压标题、启动区径向），亮主题再在导航轨下加一层雾。逻辑像素按 dpr 换算。
        QPainter p(&sharp);
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal w = device.width();
        const qreal h = device.height();
        const auto veil = [&t](qreal alpha) {
            QColor c = t.heroVeil;
            c.setAlphaF(qBound(0.0, alpha, 1.0));
            return c;
        };
        QLinearGradient left(0, 0, 520 * dpr, 0);
        left.setColorAt(0.0, veil(0.82));
        left.setColorAt(200.0 / 520.0, veil(0.55));
        left.setColorAt(1.0, veil(0));
        p.fillRect(QRectF(0, 0, qMin(w, 520 * dpr), h), left);
        QLinearGradient bottom(0, h, 0, h - 420 * dpr);
        bottom.setColorAt(0.0, veil(0.86));
        bottom.setColorAt(190.0 / 420.0, veil(0.55));
        bottom.setColorAt(1.0, veil(0));
        p.fillRect(QRectF(0, qMax(0.0, h - 420 * dpr), w, qMin(h, 420 * dpr)), bottom);
        // CSS radial-gradient(rx ry at cx cy)：在单位圆上画，再按 rx / ry 拉成椭圆。
        const auto ellipse = [&](qreal cx, qreal cy, qreal rx, qreal ry, qreal alpha, qreal end) {
            p.save();
            p.translate(cx, cy);
            p.scale(rx, ry);
            QRadialGradient g(QPointF(0, 0), 1.0);
            g.setColorAt(0.0, veil(alpha));
            g.setColorAt(end, veil(0));
            g.setColorAt(1.0, veil(0));
            p.fillRect(QRectF(-cx / rx, -cy / ry, w / rx, h / ry), g);
            p.restore();
        };
        ellipse(w * 0.25, h * 0.12, w * 0.70, h * 0.55, t.heroTop, 0.70);
        ellipse(w * 0.30, h * 0.66, w * 0.42, h * 0.30, t.heroScrim, 1.0);
        if (t.railFog > 0 && railWidth > 0) {
            QLinearGradient fog(0, 0, railWidth * dpr, 0);
            fog.setColorAt(0.0, veil(t.railFog));
            fog.setColorAt(0.55, veil(t.railFog));
            fog.setColorAt(1.0, veil(0));
            p.fillRect(QRectF(0, 0, railWidth * dpr, h), fog);
        }
        m_heroSmall = sharp.scaled(qMax(1, device.width() / kDownscale), qMax(1, device.height() / kDownscale),
                                   Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    m_heroPixmap = QPixmap::fromImage(sharp);
    m_heroPixmap.setDevicePixelRatio(dpr);
    m_heroCacheRect = hero;
}

QColor Backdrop::brightestUnder(const QWidget *widget) const {
    const QRect hero = heroRect();
    if (m_heroSmall.isNull() || widget == nullptr || hero.isEmpty()) return QColor(Qt::white);
    const QRect area = QRect(widget->mapTo(this, QPoint()), widget->size()).translated(-hero.topLeft());
    const qreal sx = qreal(m_heroSmall.width()) / hero.width();
    const qreal sy = qreal(m_heroSmall.height()) / hero.height();
    const QRect small(qFloor(area.left() * sx), qFloor(area.top() * sy), qCeil(area.width() * sx) + 1,
                      qCeil(area.height() * sy) + 1);
    QColor brightest(Qt::black);
    extremes(m_heroSmall, small, nullptr, &brightest);
    return brightest;
}

QList<QColor> Backdrop::colorsUnder(const QWidget *widget) const {
    if (effectiveMode() == Mode::Solid || widget == nullptr) return {Theme::tokens().page};
    QList<QColor> out;
    if (m_heroOpacity > 0.0 && !heroRect().isEmpty()) out = sceneColorsUnder(widget);
    if (m_heroOpacity < 1.0 || out.isEmpty()) out << frostColorsUnder(widget, false);
    return out;
}

QList<QColor> Backdrop::colorsUnderAnyPage(const QWidget *widget) const {
    if (effectiveMode() == Mode::Solid || widget == nullptr) return {Theme::tokens().page};
    QList<QColor> out = frostColorsUnder(widget, true);
    if (sceneShown()) out << sceneColorsUnder(widget);
    return out;
}

/// 主页清晰风景（含暗角）在 `widget` 那一块的最暗 / 最亮；还没备好图时为空。
QList<QColor> Backdrop::sceneColorsUnder(const QWidget *widget) const {
    const QRect hero = m_heroCacheRect;
    if (m_heroSmall.isNull() || hero.isEmpty()) return {};
    const QRect area = QRect(widget->mapTo(this, QPoint()), widget->size()).translated(-hero.topLeft());
    const qreal sx = qreal(m_heroSmall.width()) / hero.width();
    const qreal sy = qreal(m_heroSmall.height()) / hero.height();
    QColor darkest(Qt::white), brightest(Qt::black);
    extremes(m_heroSmall,
             QRect(qFloor(area.left() * sx), qFloor(area.top() * sy), qCeil(area.width() * sx) + 1,
                   qCeil(area.height() * sy) + 1),
             &darkest, &brightest);
    return {darkest, brightest};
}

/// 磨砂底盖上着色层之后的两端。`local`：风景底档按 `widget` 那一块实测（否则取整窗两端）；
/// 亚克力 / 云母透的是桌面，量不到，总按配方可能落到的范围。
QList<QColor> Backdrop::frostColorsUnder(const QWidget *widget, bool local) const {
    const Mode m = effectiveMode();
    const QList<QColor> range = m == Mode::Scene ? QList<QColor>{m_darkest, m_brightest} : systemBackdropRange();
    QColor layer = Theme::tokens().page;
    layer.setAlphaF(minimumTintAlpha(layer, bodyInks(), range, tintStrength() / 100.0));
    QList<QColor> under = range;
    if (local && m == Mode::Scene && !m_frostSmall.isNull() && !m_frostSize.isEmpty()) {
        const QRect area(widget->mapTo(this, QPoint()), widget->size());
        const qreal sx = qreal(m_frostSmall.width()) / m_frostSize.width();
        const qreal sy = qreal(m_frostSmall.height()) / m_frostSize.height();
        QColor darkest(Qt::white), brightest(Qt::black);
        extremes(m_frostSmall,
                 QRect(qFloor(area.left() * sx), qFloor(area.top() * sy), qCeil(area.width() * sx) + 1,
                       qCeil(area.height() * sy) + 1),
                 &darkest, &brightest);
        under = {darkest, brightest};
    }
    QList<QColor> out;
    for (const QColor &bg : under) out << over(layer, bg);
    return out;
}

void Backdrop::scheduleRebuild() {
    if (m_frost.isNull()) {
        rebuildNow();
    } else {
        m_debounce->start();
    }
}

void Backdrop::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    activeBackdrop() = this;
    scheduleRebuild();
}

void Backdrop::paintBase(QPainter &painter, const QWidget *target, bool toPixmap, bool withHero) {
    // 按同一扇窗里的位置算：浮层（GlassPopover）是窗口的直接孩子，不在本控件底下。
    const QRect area(target->mapTo(target->window(), QPoint(0, 0)) - mapTo(window(), QPoint(0, 0)), target->size());
    const QRect local(QPoint(0, 0), target->size());
    const Mode m = effectiveMode();
    if (m == Mode::Solid) {
        painter.fillRect(local, Theme::tokens().page);
    } else if ((m == Mode::Acrylic || m == Mode::Mica) && !toPixmap) {
        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(local, tint());
        painter.restore();
    } else {
        if (m_frost.isNull()) rebuildNow();
        const qreal sx = qreal(m_frost.width()) / qMax(1, width());
        const qreal sy = qreal(m_frost.height()) / qMax(1, height());
        const QRectF source(area.x() * sx, area.y() * sy, area.width() * sx, area.height() * sy);
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(QRectF(local), m_frost, source);
        QColor layer = Theme::tokens().page;
        layer.setAlphaF(minimumTintAlpha(layer, bodyInks(), {m_darkest, m_brightest}, tintStrength() / 100.0));
        painter.fillRect(local, layer);
        painter.restore();
    }
    // 和 paintEvent() 末尾那一笔同一张图、同一个不透明度，按 target 那一块裁出来。
    if (withHero && m_heroOpacity > 0.0 && sceneShown()) {
        rebuildHero();
        if (!m_heroPixmap.isNull()) {
            const qreal hx = qreal(m_heroPixmap.width()) / qMax(1, width());
            const qreal hy = qreal(m_heroPixmap.height()) / qMax(1, height());
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.setOpacity(painter.opacity() * m_heroOpacity);
            painter.drawPixmap(QRectF(local), m_heroPixmap,
                               QRectF(area.x() * hx, area.y() * hy, area.width() * hx, area.height() * hy));
            painter.restore();
        }
    }
}

void Backdrop::paintScene(QPainter &painter, const QRect &rect) {
    const Mode m = effectiveMode();
    if (m == Mode::Solid) {
        painter.fillRect(rect, Theme::tokens().page);
        return;
    }
    // 亚克力 / 云母：和主窗口一样，窗口里只写着色层，系统背板（真透桌面）由 DWM 画在底下。
    // 往位图里画（grab / 验收截图）时拍不到 DWM 那一层，才退回风景底。
    if ((m == Mode::Acrylic || m == Mode::Mica) && painter.device()->devType() != QInternal::Pixmap) {
        painter.save();
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect, tint());
        painter.restore();
        return;
    }
    if (m_frost.isNull()) rebuildNow();
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawPixmap(rect, m_frost, m_frost.rect());
    QColor layer = Theme::tokens().page;
    layer.setAlphaF(minimumTintAlpha(layer, bodyInks(), {m_darkest, m_brightest}, tintStrength() / 100.0));
    painter.fillRect(rect, layer);
    painter.restore();
}

void Backdrop::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const Mode m = effectiveMode();
    // `grab()` 往 QPixmap 里画（屏上那一趟重定向到的是 backing store 的 QImage）——
    // DWM 那一层拍不进位图，这一趟按风景底画。
    const QPaintDevice *redirect = redirected(nullptr);
    const bool toPixmap = redirect != nullptr && redirect->devType() == QInternal::Pixmap;
    if (m == Mode::Solid) {
        painter.fillRect(rect(), Theme::tokens().page);
    } else if ((m == Mode::Acrylic || m == Mode::Mica) && !toPixmap) {
        // 系统背板在 DWM 那一层；这里只换掉窗口里的像素（Source：连 alpha 一起写）。
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), tint());
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    } else {
        if (m_frost.isNull()) rebuildNow();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, m_frostSize != size());
        painter.drawPixmap(rect(), m_frost, m_frost.rect());
        QColor layer = Theme::tokens().page;
        layer.setAlphaF(minimumTintAlpha(layer, bodyInks(), {m_darkest, m_brightest}, tintStrength() / 100.0));
        painter.fillRect(rect(), layer);
    }
    // 风景铺满整窗（heroRect() == rect()）。淡出时 heroRect() 已经空了：rebuildHero() 不动缓存，
    // 拿上一张接着淡完。
    if (m_heroOpacity > 0.0 && sceneShown()) {
        rebuildHero();  // 键没变时立刻返回（尺寸、主题、导航轨宽都在键里）
        if (!m_heroPixmap.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, m_heroCacheRect.size() != size());
            painter.setOpacity(m_heroOpacity);
            painter.drawPixmap(rect(), m_heroPixmap, m_heroPixmap.rect());
        }
    }
    if (!m_painted && m_deferHero) {
        m_deferHero = false;
        // 首帧已经画了（磨砂 + 没有风景）：下一轮事件循环起雾散，不占首帧。
        QTimer::singleShot(0, this, [this]() {
            if (!heroShown()) return;
            m_heroFade->stop();
            m_heroFade->setEasingCurve(QEasingCurve::OutQuad);
            m_heroFade->setStartValue(0.0);
            m_heroFade->setEndValue(1.0);
            m_heroFade->setDuration(520);
            m_heroFade->start();
        });
    }
    m_painted = true;
}

}  // namespace Glass

