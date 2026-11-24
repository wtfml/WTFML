#pragma once
// 设计稿对照截图（--design-shots）：主页三主题、窄窗、宽窗、折叠轨、组件总表。
// 只截图，不登录、不联网、不启动游戏；实例和账号是写在隔离目录里的夹具。
#include "AgentPage.hpp"
#include "Animations.hpp"
#include "Glass.hpp"
#include "GlassPopover.hpp"
#include "ListRowDelegate.hpp"
#include "MainWindow.hpp"
#include "MainWindowUI.hpp"
#include "MainWindowPageLayout.hpp"
#include "OverlayHost.hpp"
#include "TaskProgressBar.hpp"
#include "Theme.hpp"
#include "TitleBar.hpp"
#include "auth/AccountStore.hpp"
#include "ToolboxPage.hpp"
#include "GameLogWindow.hpp"
#include "SchematicInfoDialog.hpp"
#include "ChromeDialog.hpp"
#include "DownloadToolPanel.hpp"
#include "InlineMessage.hpp"
#include <QWizard>
#include "PresetModReview.hpp"
#include "UpgradeHistoryDialog.hpp"
#include "UpgradeReportDialog.hpp"
#include "ToggleSwitch.hpp"
#include "VersionIcons.hpp"
#include "InstanceSettingsDialog.hpp"
#include "manage/ServerListFile.hpp"
#include <QContextMenuEvent>
#include <QSettings>
#include <QApplication>
#include <QVariantAnimation>
#include <QAbstractButton>
#include <QCheckBox>
#include <QEnterEvent>
#include <QFile>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QStyle>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QDir>
#include <QVBoxLayout>
#include <functional>

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <cstdio>
#endif

namespace DesignShots {

/// 边缘样张附带的几何账目（物理像素）：窗口矩形、客户区（换到屏幕坐标）、DWM 实际画出来的可见框。
/// 三者一比就知道左右下各有几像素是系统非客户区条带。
inline QJsonObject frameNumbers(QWidget &window) {
    QJsonObject out;
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(window.winId());
    const auto rect = [](const RECT &r) { return QJsonArray{int(r.left), int(r.top), int(r.right), int(r.bottom)}; };
    RECT wr{}, cr{}, vis{};
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    POINT tl{cr.left, cr.top}, br{cr.right, cr.bottom};
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    out.insert("windowRect", rect(wr));
    out.insert("clientRect", rect(RECT{tl.x, tl.y, br.x, br.y}));
    using GetAttr = HRESULT(WINAPI *)(HWND, DWORD, PVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        if (auto fn = reinterpret_cast<GetAttr>(reinterpret_cast<void *>(GetProcAddress(dwm, "DwmGetWindowAttribute"))))
            if (SUCCEEDED(fn(hwnd, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &vis, sizeof(vis)))) out.insert("visibleFrame", rect(vis));
        FreeLibrary(dwm);
    }
    out.insert("dpiForWindow", int(GetDpiForWindow(hwnd)));
    out.insert("frameGeometry", QJsonArray{window.frameGeometry().x(), window.frameGeometry().y(),
                                           window.frameGeometry().width(), window.frameGeometry().height()});
#else
    Q_UNUSED(window);
#endif
    return out;
}

inline void mute(QWidget *w) {
    QPalette p = w->palette();
    p.setColor(QPalette::WindowText, Theme::tokens().secondary);
    w->setPalette(p);
}

/// 管理页夹具：隔离目录里摆几份"看起来已安装"的实例（<id>/<id>.jar + wtfml.meta），不含真文件。
inline void seedInstances(const QString &versionsDir) {
    const struct { const char *id, *name, *mc, *loader, *loaderVersion; } rows[] = {
        {"spring", "春日生存", "1.21.4", "Fabric", "0.16.9"},
        {"create", "机械动力 Create", "1.20.1", "Forge", "47.3.0"},
        {"sakura", "樱花小屋", "1.21.1", "Fabric", "0.16.9"},
        {"shader", "光影测试", "1.21.1", "OptiFine", "HD U J1"},
        {"skyblock", "空岛生存", "1.21.1", "NeoForge", "21.1.77"},
        {"redstone", "红石实验室", "1.20.1", "", ""},
        {"desert", "沙漠神殿", "1.21.4", "", ""},
    };
    for (const auto &row : rows) {
        const QString dir = versionsDir + QLatin1Char('/') + QString::fromLatin1(row.id);
        QDir().mkpath(dir);
        QFile jar(dir + QLatin1Char('/') + QString::fromLatin1(row.id) + QStringLiteral(".jar"));
        if (jar.open(QIODevice::WriteOnly)) jar.write("stub");
        QJsonObject meta{{"name", QString::fromUtf8(row.name)}, {"realVersion", QString::fromLatin1(row.mc)}};
        if (*row.loader) {
            meta.insert("loaderName", QString::fromLatin1(row.loader));
            meta.insert("loaderVersion", QString::fromLatin1(row.loaderVersion));
        }
        QFile file(dir + QStringLiteral("/wtfml.meta"));
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(meta).toJson());
    }
}

/// 管理页那棵树：悬停第二个实例行（快捷操作只在悬停时出现）。
inline void hoverInstanceRow(MainWindow &window) {
    QTreeWidget *tree = window.ui()->getInstanceList();
    if (tree == nullptr) return;
    int seen = 0;
    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
        if ((*it)->data(0, MainWindowUI::instanceIdRole()).toString().isEmpty() || ++seen < 2) continue;
        const QPoint at = tree->visualItemRect(*it).center();
        QHoverEvent hover(QEvent::HoverMove, at, tree->viewport()->mapToGlobal(at), QPoint(-1, -1));
        QApplication::sendEvent(tree->viewport(), &hover);
        tree->viewport()->repaint();
        return;
    }
}

inline QList<MainWindowUI::HomeQuickItem> fixtureItems() {
    const struct { const char *key, *name, *detail; } rows[] = {
        {"spring", "春日生存", "Minecraft 1.21.4 / Fabric 0.16.9\n上次启动：2 小时前"},
        {"create", "机械动力 Create", "Minecraft 1.20.1 / Forge 47.3.0\n上次启动：昨天"},
        {"sakura", "樱花小屋", "Minecraft 1.21.1 / Fabric 0.16.5\n上次启动：3 天前"},
        {"shader", "光影测试", "Minecraft 1.21.1 / Iris 1.8.1\n上次启动：上周"},
        {"skyblock", "空岛生存", "Minecraft 1.21.1 / NeoForge 21.1.77\n上次启动：上周"},
    };
    QList<MainWindowUI::HomeQuickItem> items;
    for (const auto &row : rows) {
        MainWindowUI::HomeQuickItem item;
        item.key = QString::fromUtf8(row.key);
        item.name = QString::fromUtf8(row.name);
        item.detail = QString::fromUtf8(row.detail);
        item.recent = true;
        items.append(item);
    }
    items.first().current = true;
    items[1].favorite = items[2].favorite = true;
    return items;
}

inline void seedHome(MainWindow &window, bool empty = false) {
    if (empty) {
        // 一个实例都没有时那一张（和 HomeQuickController 灌的空槽同形）：「还没有实例」+「下载游戏」。
        MainWindowUI::HomeQuickItem slot;
        slot.empty = true;
        slot.name = QStringLiteral("还没有实例");
        window.ui()->setHomeQuickRows({slot}, {}, {});
        return;
    }
    const auto items = fixtureItems();
    QList<MainWindowUI::HomeQuickItem> favorites;
    for (const auto &item : items)
        if (item.favorite) favorites.append(item);
    window.ui()->setHomeQuickRows({items.first()}, favorites, items);
}

/// 组件总表：左边折叠后的导航轨，右边 14 种控件 × 6 态。
class Gallery : public QMainWindow {
public:
    QList<QWidget *> focusCells;
    QListWidget *hoverList = nullptr;

    Gallery() {
        setWindowTitle(QStringLiteral("WTFML 组件总表"));
        auto *backdrop = new Glass::Backdrop(this);
        setCentralWidget(backdrop);
        auto *shell = new QHBoxLayout(backdrop);
        shell->setContentsMargins(0, 0, 0, 0);
        shell->setSpacing(0);
        auto *rail = new TitleBar(backdrop);
        rail->setNavItems({QStringLiteral("主页"), QStringLiteral("管理"), QStringLiteral("下载"),
                           QStringLiteral("设置"), QStringLiteral("智能体")});
        rail->setCurrentNav(3);
        rail->setCollapsed(true);
        auto *footer = new QWidget(rail);
        auto *footerLayout = new QVBoxLayout(footer);
        footerLayout->setContentsMargins(0, 0, 0, 0);
        auto *ring = new TaskProgressBar(footer);
        ring->setRing(true);
        ring->setFixedSize(28, 28);
        ring->setRange(0, 0);
        footerLayout->addWidget(ring, 0, Qt::AlignHCenter);
        rail->setFooter(footer);
        shell->addWidget(rail);
        auto *right = new QVBoxLayout;
        right->setContentsMargins(0, 0, 0, 0);
        right->setSpacing(0);
        rail->captionStrip()->setParent(backdrop);
        right->addWidget(rail->captionStrip());
        auto *page = new QWidget(backdrop);
        right->addWidget(page, 1);
        shell->addLayout(right, 1);
        build(page);
    }

private:
    QWidget *cell(QWidget *w, int state) {
        // state：0 常态 1 悬停 2 按下 3 选中 4 禁用 5 焦点
        auto *button = qobject_cast<QAbstractButton *>(w);
        if (state == 1) w->setAttribute(Qt::WA_UnderMouse, true);
        if (state == 1 && qobject_cast<ToggleSwitch *>(w)) {
            QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
            QApplication::sendEvent(w, &enter);
        }
        if (state == 1 && qobject_cast<TitleBarButton *>(w)) {
            QEvent enter(QEvent::HoverEnter);
            QApplication::sendEvent(w, &enter);
        }
        if (state == 2 && button) button->setDown(true);
        if (state == 2) if (auto *slider = qobject_cast<QSlider *>(w)) slider->setSliderDown(true);
        if (state == 3 && button) { button->setCheckable(true); button->setChecked(true); }
        if (state == 4) w->setEnabled(false);
        if (state == 5) focusCells.append(w);
        return w;
    }

    static QLabel *head(const QString &text, QWidget *parent, int px = 13) {
        auto *label = new QLabel(text, parent);
        QFont f = label->font();  // 分组头：系统字（手写体只留四处）
        f.setPixelSize(px);
        f.setWeight(QFont::DemiBold);
        label->setFont(f);
        QPalette p = label->palette();
        p.setColor(QPalette::WindowText, px >= 20 ? Theme::tokens().text : Theme::tokens().secondary);
        label->setPalette(p);
        return label;
    }

    void build(QWidget *page) {
        auto *outer = new QVBoxLayout(page);
        outer->setContentsMargins(36, 0, 40, 24);
        outer->setSpacing(14);
        auto *titleRow = new QHBoxLayout;
        auto *title = head(QStringLiteral("组件总表"), page, 30);
        Theme::markDisplay(title, Theme::textFont(title->font(), Theme::TextRole::Display));
        titleRow->addWidget(title);
        titleRow->addStretch(1);
        auto *note = new QLabel(QStringLiteral("左侧为折叠后的导航轨（只剩图标） · Qt 实拍，焦点列逐格取焦后拼合"), page);
        mute(note);
        titleRow->addWidget(note);
        outer->addLayout(titleRow);

        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(14);
        grid->setVerticalSpacing(14);
        const QStringList states{QStringLiteral("常态"), QStringLiteral("悬停"), QStringLiteral("按下"),
                                 QStringLiteral("选中 / 开"), QStringLiteral("禁用"), QStringLiteral("焦点（键盘）")};
        for (int c = 0; c < states.size(); ++c) grid->addWidget(head(states.at(c), page), 0, c + 1);
        grid->setColumnMinimumWidth(0, 96);
        for (int c = 1; c <= 6; ++c) grid->setColumnStretch(c, 1);
        int row = 1;
        const auto addRow = [&](const QString &name, const std::function<QWidget *(int)> &make, QList<int> skip = {}) {
            grid->addWidget(head(name, page, 14), row, 0);
            for (int s = 0; s < 6; ++s) {
                if (skip.contains(s)) {
                    auto *dash = new QLabel(QStringLiteral("—"), page);
                    mute(dash);
                    grid->addWidget(dash, row, s + 1, Qt::AlignLeft | Qt::AlignVCenter);
                    continue;
                }
                grid->addWidget(cell(make(s), s), row, s + 1, Qt::AlignLeft | Qt::AlignVCenter);
            }
            ++row;
        };
        const auto pushButton = [page](const QString &text, Theme::ButtonKind kind, const QString &icon = {}) {
            auto *b = new QPushButton(text, page);
            Theme::markButton(b, kind);
            b->setFocusPolicy(Qt::StrongFocus);
            if (!icon.isEmpty()) {
                const QColor ink = kind == Theme::ButtonKind::Primary ? Theme::tokens().onPrimary
                                   : kind == Theme::ButtonKind::Danger ? Theme::tokens().danger
                                   : Theme::tokens().onDanger;
                b->setIcon(QIcon(VersionIcons::line(icon, 14, ink, 2.0)));
                b->setIconSize(QSize(14, 14));
            }
            return b;
        };
        addRow(QStringLiteral("主按钮"), [&](int) { return pushButton(QStringLiteral("启动"), Theme::ButtonKind::Primary, QStringLiteral("play")); }, {3});
        addRow(QStringLiteral("次按钮"), [&](int) { return pushButton(QStringLiteral("导入"), Theme::ButtonKind::Neutral); });
        addRow(QStringLiteral("文字按钮"), [&](int) { return pushButton(QStringLiteral("管理"), Theme::ButtonKind::Ghost); }, {3});
        addRow(QStringLiteral("危险按钮"), [&](int) { return pushButton(QStringLiteral("删除"), Theme::ButtonKind::Danger, QStringLiteral("trash")); }, {3});
        addRow(QStringLiteral("危险实心"), [&](int) { return pushButton(QStringLiteral("删除实例"), Theme::ButtonKind::DangerSolid); }, {3});
        addRow(QStringLiteral("图标按钮"), [&](int s) {
            auto *b = new QPushButton(page);
            Theme::markIconButton(b);
            b->setFixedSize(36, 36);
            b->setFocusPolicy(Qt::StrongFocus);
            const QColor ink = s == 3 ? Theme::tokens().deep : s == 4 ? Theme::tokens().disabled
                               : s == 1 || s == 2 ? Theme::tokens().text : Theme::tokens().secondary;
            b->setIcon(QIcon(VersionIcons::line(QStringLiteral("star"), 18, ink)));
            b->setIconSize(QSize(18, 18));
            return b;
        });
        addRow(QStringLiteral("输入框"), [&](int s) {
            auto *e = new QLineEdit(page);
            e->setFixedSize(150, 36);
            e->setPlaceholderText(QStringLiteral("搜索实例"));
            if (s == 3) { e->setText(QStringLiteral("-Xmx99G")); e->setProperty("wtfmlError", true); }
            if (s == 4) e->setPlaceholderText(QStringLiteral("由实例接管"));
            if (s == 5) e->setText(QStringLiteral("春日生存"));
            return e;
        }, {2});
        addRow(QStringLiteral("下拉"), [&](int) {
            auto *c = new QComboBox(page);
            c->addItem(QStringLiteral("跟随系统"));
            c->setFixedSize(150, 36);
            return c;
        }, {2, 3});
        // 设计稿禁用列画的是「开着但禁用」，焦点列开关也是开着的。
        addRow(QStringLiteral("开关"), [&](int s) {
            auto *t = new ToggleSwitch(page);
            t->setChecked(s >= 4);
            return t;
        });
        addRow(QStringLiteral("复选"), [&](int s) {
            auto *c = new QCheckBox(page);
            c->setFocusPolicy(Qt::StrongFocus);
            c->setChecked(s == 4);
            return c;
        });
        addRow(QStringLiteral("滑块"), [&](int) {
            auto *s = new QSlider(Qt::Horizontal, page);
            s->setRange(0, 100);
            s->setValue(40);
            s->setFixedWidth(150);
            return s;
        }, {3});
        addRow(QStringLiteral("胶囊筛选"), [&](int) {
            auto *b = new QPushButton(QStringLiteral("模组  42"), page);
            Theme::markChipButton(b);
            b->setFocusPolicy(Qt::StrongFocus);
            return b;
        });
        addRow(QStringLiteral("导航项"), [&](int s) {
            auto *b = new TitleBarButton(TitleBarButton::Glyph::Text, Theme::tokens().hover, Theme::tokens().text, page);
            b->setNavIcon(TitleBarButton::NavIcon::Download);
            b->setLabel(QStringLiteral("下载"));
            QFont f = b->font();
            f.setPixelSize(TitleBar::navFontPixelSize());
            f.setWeight(QFont::Medium);
            b->setFont(f);
            b->setFixedSize(150, 42);
            b->setPressColor(Theme::tokens().fillPress);
            b->setInkColor(s == 3 ? Theme::tokens().deep : Theme::tokens().secondary);
            if (s == 3) { b->setBaseColor(Theme::tokens().selected); b->setCurrent(true); }
            return b;
        });
        outer->addLayout(grid);

        // 下半：列表行 + 徽标 / 分段 / 进度。
        auto *lower = new QHBoxLayout;
        lower->setSpacing(36);
        auto *listCol = new QVBoxLayout;
        listCol->addWidget(head(QStringLiteral("列表行"), page, 20));
        auto *list = new QListWidget(page);
        list->setFrameShape(QFrame::NoFrame);
        ListRowDelegate::install(list);
        list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; }"));  // 页面里的列表是透明底
        // 设计稿 BLOCKS 下标：0 草 / 7 铜 / 5 樱花。
        const struct { int block; const char *title, *sub, *meta; } rows[] = {
            {0, "春日生存", "Fabric 0.16.9 · 1.21.4", "2 小时前"},
            {7, "机械动力 Create", "Forge 47.3 · 1.20.1", ""},
            {5, "樱花小屋", "Fabric 0.16.9 · 1.21.1", "昨天"},
        };
        for (const auto &row : rows) {
            auto *item = new QListWidgetItem(QString::fromUtf8(row.title), list);
            item->setData(Qt::DecorationRole, VersionIcons::isoBlock(row.block, 48));
            item->setData(ListRowDelegate::SubtitleRole, QString::fromUtf8(row.sub));
            item->setData(ListRowDelegate::MetaRole, QString::fromUtf8(row.meta));
        }
        list->setCurrentRow(2);
        list->setFixedHeight(3 * ListRowDelegate::rowHeight() + 4);
        hoverList = list;
        listCol->addWidget(list);
        lower->addLayout(listCol, 1);

        auto *misc = new QGridLayout;
        misc->setHorizontalSpacing(18);
        misc->setVerticalSpacing(14);
        misc->addWidget(head(QStringLiteral("徽标 · 分段 · 进度"), page, 20), 0, 0, 1, 2);
        misc->addWidget(head(QStringLiteral("徽标"), page), 1, 0);
        auto *badges = new QHBoxLayout;
        badges->setSpacing(8);
        const QList<QPair<QString, Theme::BadgeTone>> tones{{QStringLiteral("原版"), Theme::BadgeTone::Neutral},
            {QStringLiteral("Fabric"), Theme::BadgeTone::Accent}, {QStringLiteral("● 运行中"), Theme::BadgeTone::Ok},
            {QStringLiteral("可更新"), Theme::BadgeTone::Warn}, {QStringLiteral("文件损坏"), Theme::BadgeTone::Danger}};
        for (const auto &[text, tone] : tones) {
            auto *badge = new QLabel(text, page);
            Theme::markBadge(badge, tone);
            badges->addWidget(badge);
        }
        badges->addStretch(1);
        misc->addLayout(badges, 1, 1);
        misc->addWidget(head(QStringLiteral("分段"), page), 2, 0);
        auto *seg = new QWidget(page);
        auto *segLayout = new QHBoxLayout(seg);
        segLayout->setContentsMargins(3, 3, 3, 3);
        segLayout->setSpacing(2);
        QList<QPushButton *> segments;
        for (const QString &text : {QStringLiteral("常态"), QStringLiteral("悬停"), QStringLiteral("按下"),
                                    QStringLiteral("选中"), QStringLiteral("禁用")}) {
            auto *b = new QPushButton(text, seg);
            segments.append(b);
            segLayout->addWidget(b);
        }
        Theme::markSegmented(seg, segments);
        segments[1]->setAttribute(Qt::WA_UnderMouse, true);
        segments[2]->setDown(true);
        segments[3]->setChecked(true);
        segments[4]->setEnabled(false);
        auto *segRow = new QHBoxLayout;
        segRow->addWidget(seg);
        segRow->addStretch(1);
        misc->addLayout(segRow, 2, 1);
        misc->addWidget(head(QStringLiteral("进度"), page), 3, 0);
        auto *bars = new QVBoxLayout;
        bars->setSpacing(10);
        for (const auto &[text, value] : {QPair<QString, int>{QStringLiteral("下载 Fabric 1.21.4 · 128 / 342 个文件    37%"), 37},
                                          QPair<QString, int>{QStringLiteral("正在校验…    不定"), -1},
                                          QPair<QString, int>{QStringLiteral("安装完成    100%"), 100}}) {
            auto *label = new QLabel(text, page);
            bars->addWidget(label);
            auto *bar = new TaskProgressBar(page);
            bar->setFixedHeight(4);
            if (value < 0) bar->setRange(0, 0); else { bar->setRange(0, 100); bar->setValue(value); }
            bars->addWidget(bar);
        }
        misc->addLayout(bars, 3, 1);
        misc->setColumnStretch(1, 1);
        lower->addLayout(misc, 1);
        outer->addLayout(lower);
        outer->addStretch(1);
    }
};

/// 焦点那一列：逐格取真焦点（键盘那一档）后从整窗截那一小块，拼回总图。
/// 设计稿里坐标 / 延迟两页填一份样例结果，让结果面板有东西可看（只摆界面，不联网不读文件）。
inline void fillToolboxSample(ToolboxPage &page, const QString &key) {
    if (key == QStringLiteral("coord")) {
        page.setCoordinates(0, 1024, 72, -384);
        if (auto *button = page.findChild<QPushButton *>(QStringLiteral("coordConvert"))) button->click();
    } else if (key == QStringLiteral("server")) {
        page.setServerAddress(QStringLiteral("play.example.net"));
        ToolboxPage::ServerView view;
        view.ok = true; view.address = QStringLiteral("play.example.net");
        view.motd = QStringLiteral("<span style='color:#55ff55'>示例服务器</span> · 生存 1.21");
        view.version = QStringLiteral("Paper 1.21.4"); view.players = QStringLiteral("7 / 20");
        view.latency = QStringLiteral("26 ms");
        page.setServerView(view);
    }
}

/// r2 智能体页对照（设计稿 r2/png/agent-A-*）：直接往页上灌会话 / 消息 / 状态，不经控制器、不发请求。
/// @p state：chat / empty / noapi / sys / menu / inst。
inline void seedAgent(AgentPage &page, const QString &state) {
    const QDateTime now = QDateTime::currentDateTime();
    QList<AgentView::SessionMeta> sessions;
    const auto add = [&sessions](const char *id, const QString &title, const QDateTime &at, bool pinned) {
        AgentView::SessionMeta meta;
        meta.id = QString::fromLatin1(id);
        meta.title = title;
        meta.updatedAt = at;
        meta.pinned = pinned;
        sessions.append(meta);
    };
    add("s1", QStringLiteral("光影包开了就闪退"), now.addSecs(-18 * 60), false);
    add("s2", QStringLiteral("1.20.1 Forge 换 Java 21"), now.addDays(-2), true);
    add("s3", QStringLiteral("整合包启动卡在加载界面"), now.addSecs(-2 * 3600), false);
    add("s4", QStringLiteral("服务器连不上：Connection reset by peer"), now.addSecs(-5 * 3600), false);
    add("s5", QStringLiteral("存档备份放哪里比较好"), now.addDays(-3), false);
    add("s6", QStringLiteral("模组冲突排查"), now.addDays(-6), false);
    page.setSessions(sessions);
    const bool noApi = state == QStringLiteral("noapi");
    page.setConfigured(!noApi, noApi ? QStringLiteral("API 未配置") : QString());
    page.setModelLabel(QStringLiteral("deepseek-chat"));
    if (noApi || state == QStringLiteral("empty")) {
        page.setCurrentSession(QString());
        page.setInstanceChip(QStringLiteral("机械动力 Create"), false);
        page.showEmptyState(true);
        return;
    }
    page.setCurrentSession(QStringLiteral("s1"));
    page.setInstanceChip(QStringLiteral("机械动力 Create"), true);
    AgentView::Message ask;
    ask.id = QStringLiteral("m1");
    ask.user = true;
    ask.content = QStringLiteral("装了 Iris 和 Complementary 光影，一开光影游戏就闪退，怎么查？");
    AgentView::Message answer;
    answer.id = QStringLiteral("m2");
    answer.content = QStringLiteral(
        "先看最近一次的崩溃报告，它在实例目录的 `crash-reports` 里。常见原因有三个：\n\n"
        "1. **Iris 与 Sodium 版本不配套**：两者要对应同一个 Minecraft 版本；\n"
        "2. **显卡驱动过旧**：光影需要 OpenGL 4.x；\n"
        "3. **内存不足**：光影包至少给 4 GB。\n\n"
        "要我读一下最新的崩溃报告吗？");
    AgentView::Message follow;
    follow.id = QStringLiteral("m3");
    follow.user = true;
    follow.content = QStringLiteral("好，读一下。");
    page.setMessages({ask, answer, follow});
    page.showEmptyState(false);
    if (state == QStringLiteral("sys")) page.showNotice(QStringLiteral("本会话已关联实例：机械动力 Create"));
    else if (state == QStringLiteral("menu")) page.openMoreMenu();
    else if (state == QStringLiteral("inst"))
        page.showInstanceMenu({{QStringLiteral("create"), QStringLiteral("机械动力 Create")},
                               {QStringLiteral("spring"), QStringLiteral("春日生存")},
                               {QStringLiteral("vanilla"), QStringLiteral("原版 1.21.4")}},
                              QStringLiteral("create"));
}

inline QImage grabGallery(Gallery &gallery) {
    if (QListWidget *list = gallery.hoverList) {  // 第二行走一遍真的悬停（快捷操作只在悬停时出现）
        const QPoint at = list->visualItemRect(list->item(1)).center();
        QHoverEvent hover(QEvent::HoverMove, at, list->viewport()->mapToGlobal(at), QPoint(-1, -1));
        QApplication::sendEvent(list->viewport(), &hover);
        list->viewport()->repaint();
    }
    QImage image = gallery.grab().toImage();
    QPainter painter(&image);
    const qreal dpr = image.devicePixelRatio();
    for (QWidget *w : std::as_const(gallery.focusCells)) {
        w->setFocus(Qt::TabFocusReason);
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(w, &key);  // 「键盘在用」那一档只认真按键
        w->setFocus(Qt::TabFocusReason);
        w->setProperty("wtfmlKeyboardFocus", true);
        w->style()->unpolish(w);
        w->style()->polish(w);
        w->update();
        QApplication::processEvents();
        const QRect area = QRect(w->mapTo(&gallery, QPoint(0, 0)), w->size()).adjusted(-4, -4, 4, 4);
        const QImage piece = gallery.grab(area).toImage();
        painter.drawImage(QRectF(QPointF(area.topLeft()), QSizeF(area.size())), piece);
        Q_UNUSED(dpr);
    }
    return image;
}

/// 依次截：三主题主页 1280×800、暗色最小窗口 / 1600×1000、折叠轨、组件总表。
inline void run(QApplication &app, MainWindow &window, const QString &root) {
    struct Shot { QString file; QString theme; QSize size; bool collapsed; int tab = MainWindowUI::homeTabIndex(); QString page = {}; };
    const int manage = MainWindowUI::instancesTabIndex();
    const int download = MainWindowUI::downloadTabIndex();
    const int agent = MainWindowUI::agentTabIndex();
    const int home = MainWindowUI::homeTabIndex();
    static const QList<Shot> shots = [&]() { QList<Shot> r66; QList<Shot> list{
        {QStringLiteral("home-dark.png"), QStringLiteral("dark"), {1280, 800}, false},
        {QStringLiteral("home-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false},
        {QStringLiteral("home-ocean.png"), QStringLiteral("ocean"), {1280, 800}, false},
        {QStringLiteral("home-dark-%1x%2.png").arg(MainWindowPageLayout::kMainWindowMinWidth).arg(MainWindowPageLayout::kMainWindowMinHeight), QStringLiteral("dark"), QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight), false},
        {QStringLiteral("home-dark-1600x1000.png"), QStringLiteral("dark"), {1600, 1000}, false},
        {QStringLiteral("rail-collapsed-dark.png"), QStringLiteral("dark"), {1280, 800}, true},
        {QStringLiteral("manage-dark.png"), QStringLiteral("dark"), {1280, 800}, false, manage},
        {QStringLiteral("manage-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false, manage},
        {QStringLiteral("manage-ocean.png"), QStringLiteral("ocean"), {1280, 800}, false, manage},
        {QStringLiteral("download-dark.png"), QStringLiteral("dark"), {1280, 800}, false, download},
        {QStringLiteral("download-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false, download},
        {QStringLiteral("download-ocean.png"), QStringLiteral("ocean"), {1280, 800}, false, download},
        // 实例二级页（设计稿 instance-*）：「机械动力 Create」那一份夹具，按页开。
        {QStringLiteral("instance-overview-dark.png"), QStringLiteral("dark"), {1280, 800}, false, manage, QStringLiteral("overview")},
        {QStringLiteral("instance-overview-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false, manage, QStringLiteral("overview")},
        {QStringLiteral("instance-overview-ocean.png"), QStringLiteral("ocean"), {1280, 800}, false, manage, QStringLiteral("overview")},
        {QStringLiteral("instance-launch-dark.png"), QStringLiteral("dark"), {1280, 800}, false, manage, QStringLiteral("launch")},
        {QStringLiteral("instance-mods-dark.png"), QStringLiteral("dark"), {1280, 800}, false, manage, QStringLiteral("mod")},
        // 智能体页、工具箱（设计稿 agent / toolbox）：页键 toolbox = 目录页，toolbox:<键> = 进那一件工具。
        {QStringLiteral("agent-dark.png"), QStringLiteral("dark"), {1280, 800}, false, agent},
        {QStringLiteral("agent-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false, agent},
        {QStringLiteral("agent-ocean.png"), QStringLiteral("ocean"), {1280, 800}, false, agent},
        {QStringLiteral("toolbox-dark.png"), QStringLiteral("dark"), {1280, 800}, false, home, QStringLiteral("toolbox")},
        {QStringLiteral("toolbox-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false, home, QStringLiteral("toolbox")},
        {QStringLiteral("toolbox-ocean.png"), QStringLiteral("ocean"), {1280, 800}, false, home, QStringLiteral("toolbox")},
        // 游戏日志窗（独立顶层窗，自绘标题条）：页键 gamelog = 另开一只日志窗、抓它自己。
        {QStringLiteral("gamelog-dark.png"), QStringLiteral("dark"), {1280, 800}, false, home, QStringLiteral("gamelog")},
        {QStringLiteral("gamelog-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false, home, QStringLiteral("gamelog")},
        {QStringLiteral("gamelog-ocean.png"), QStringLiteral("ocean"), {1280, 800}, false, home, QStringLiteral("gamelog")},
        // 其余独立对话框（ChromeDialog 自绘窄标题条）：页键 dialog:<种类> = 另开一只、抓它自己。
        {QStringLiteral("dialog-report-dark.png"), QStringLiteral("dark"), {1280, 800}, false, home, QStringLiteral("dialog:report")},
        {QStringLiteral("dialog-report-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false, home, QStringLiteral("dialog:report")},
        {QStringLiteral("dialog-history-dark.png"), QStringLiteral("dark"), {1280, 800}, false, home, QStringLiteral("dialog:history")},
        {QStringLiteral("dialog-history-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false, home, QStringLiteral("dialog:history")},
        {QStringLiteral("dialog-schematic-dark.png"), QStringLiteral("dark"), {1280, 800}, false, home, QStringLiteral("dialog:schematic")},
        {QStringLiteral("dialog-schematic-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false, home, QStringLiteral("dialog:schematic")},
        {QStringLiteral("toolbox-download-dark.png"), QStringLiteral("dark"), {1280, 800}, false, home, QStringLiteral("toolbox:download")},
        {QStringLiteral("wizard-step1-dark.png"), QStringLiteral("dark"), {1280, 800}, false, manage, QStringLiteral("wizard:0")},
        {QStringLiteral("wizard-step2-dark.png"), QStringLiteral("dark"), {1280, 800}, false, manage, QStringLiteral("wizard:1")},
        {QStringLiteral("wizard-step3-dark.png"), QStringLiteral("dark"), {1280, 800}, false, manage, QStringLiteral("wizard:2")},
        {QStringLiteral("wizard-step4-dark.png"), QStringLiteral("dark"), {1280, 800}, false, manage, QStringLiteral("wizard:3")},
        {QStringLiteral("dialog-modreview-dark.png"), QStringLiteral("dark"), {1280, 800}, false, home, QStringLiteral("dialog:modreview")},
        {QStringLiteral("dialog-startup-dark.png"), QStringLiteral("dark"), {1280, 800}, false, home, QStringLiteral("dialog:startup")},
        {QStringLiteral("toolbox-page-dark.png"), QStringLiteral("dark"), {1280, 800}, false, home, QStringLiteral("toolbox:anvil")},
        {QStringLiteral("toolbox-page-emerald.png"), QStringLiteral("emerald"), {1280, 800}, false, home, QStringLiteral("toolbox:anvil")},
    };
        // 精修轮逐页自检：启动设置三主题、工具箱其余九页三主题。
        for (const QString &theme : {QStringLiteral("emerald"), QStringLiteral("ocean")})
            list.append({QStringLiteral("instance-launch-%1.png").arg(theme), theme, {1280, 800}, false, manage, QStringLiteral("launch")});
        for (const char *key : {"anvil", "text", "coord", "server", "beacon", "brewing", "banner", "trim", "download"})
            for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("emerald"), QStringLiteral("ocean")})
                list.append({QStringLiteral("tool-%1-%2.png").arg(QLatin1String(key), theme), theme, {1280, 800}, false, home,
                             QStringLiteral("toolbox:") + QLatin1String(key)});
        // 窗口最外沿样张：真屏抓（带 DWM 圆角与窗外一圈桌面），860 宽让竖屏上四条边都在屏内；WTFML_WINDOW_EDGE=0 再跑一遍出系统默认边对照。
        list.append({QStringLiteral("edge-home-dark.png"), QStringLiteral("dark"), {860, 640}, false, home, QStringLiteral("edge")});
        list.append({QStringLiteral("edge-home-light.png"), QStringLiteral("emerald"), {860, 640}, false, home, QStringLiteral("edge")});
        list.append({QStringLiteral("edge-manage-dark.png"), QStringLiteral("dark"), {860, 640}, false, manage, QStringLiteral("edge")});
        list.append({QStringLiteral("edge-max-dark.png"), QStringLiteral("dark"), {860, 640}, false, home, QStringLiteral("edge:max")});
        list.append({QStringLiteral("edge-home-ocean.png"), QStringLiteral("ocean"), {860, 640}, false, home, QStringLiteral("edge")});
        // 最大化 → 还原 → 拖到屏幕另一处再抓（几何账目在还原后最容易差几像素）。
        list.append({QStringLiteral("edge-restore-dark.png"), QStringLiteral("dark"), {860, 640}, false, home, QStringLiteral("edge:restore")});
        list.append({QStringLiteral("edge-restore-emerald.png"), QStringLiteral("emerald"), {860, 640}, false, home, QStringLiteral("edge:restore")});
        // 主页右上账号胶囊：账号弹层打开的样子（收起的样子看 home-*.png 右上角）。
        for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("emerald"), QStringLiteral("ocean")})
            list.append({QStringLiteral("account-menu-%1.png").arg(theme), theme, {1000, 700}, false, home, QStringLiteral("account")});
        // r2 主页方案 A 对照（设计稿 r2/png/home-A-* / home-A-switch-* / home-empty-* / home-acct-rail-*）：
        // 有实例、实例切换弹层打开、没有实例、账号弹层打开（锚点现在是主页右上那颗胶囊）× 暗 / 冰川碧湖 × 两档尺寸。
        for (const char *state : {"home-A", "home-A-switch", "home-empty", "home-acct-rail"})
            for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("emerald")})
                for (const QSize size : {QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight), QSize(1280, 800)})
                    list.append({QStringLiteral("r2-%1-%2-%3x%4.png").arg(QLatin1String(state), theme).arg(size.width()).arg(size.height()),
                                 theme, size, false, home, QStringLiteral("r2:") + QLatin1String(state)});
        // r2 智能体页方案 A 对照（设计稿 r2/png/agent-A-*）：对话、空、未配置 API、系统提示、⋯ 菜单、实例下拉。
        for (const char *state : {"chat", "empty", "noapi", "sys", "menu", "inst"})
            for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("emerald")})
                for (const QSize size : {QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight), QSize(1280, 800)})
                    list.append({QStringLiteral("r2-agent-%1-%2-%3x%4.png").arg(QLatin1String(state), theme).arg(size.width()).arg(size.height()),
                                 theme, size, false, agent, QStringLiteral("r2:agent-") + QLatin1String(state)});
        // r66 账号胶囊回主页右上：已登录 / 已登录弹层开 / 未登录 / 未登录弹层开 × 三主题 × 两档尺寸。
        // 未登录那两组排在**整张单子最后**（WTFML_DESIGN_SHOT_FILTER=r66- 只出这一组）：头一张会真的
        // 点弹层里的「退出登录」，之后的样张都是未登录。
        for (const char *state : {"in", "in-open", "out", "out-open"})
            for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("emerald"), QStringLiteral("ocean")})
                for (const QSize size : {QSize(1280, 800), QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight)})
                    r66.append({QStringLiteral("r66-%1-%2-%3x%4.png").arg(QLatin1String(state), theme).arg(size.width()).arg(size.height()),
                                theme, size, false, home, QStringLiteral("r66:") + QLatin1String(state)});
        // r68 主页风景 ↔ 磨砂淡变：主页 → 出主页中途 → 设置 → 回主页中途 → 主页 × 四主题 × 两档尺寸
        // （FILTER=r68-）。中途帧 = 淡变停在一半（kHeroFadeMs / 2）的那一刻。
        for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("emerald"), QStringLiteral("ocean"), QStringLiteral("system")})
            for (const QSize size : {QSize(MainWindowPageLayout::kMainWindowMinWidth, MainWindowPageLayout::kMainWindowMinHeight), QSize(1280, 800)})
                for (const auto &[step, tab] : {std::pair{"1-home", home}, std::pair{"2-out-mid", home},
                                                std::pair{"3-settings", MainWindowUI::settingsTabIndex()},
                                                std::pair{"4-in-mid", MainWindowUI::settingsTabIndex()}, std::pair{"5-home-back", home}})
                    list.append({QStringLiteral("r68-%1-%2x%3-%4.png").arg(theme).arg(size.width()).arg(size.height()).arg(QLatin1String(step)),
                                 theme, size, false, tab, QStringLiteral("r68:") + QLatin1String(step)});
        // 设置页三主题（去硬描边那轮逐页自检用）。
        for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("emerald"), QStringLiteral("ocean")})
            list.append({QStringLiteral("settings-%1.png").arg(theme), theme, {1280, 800}, false, MainWindowUI::settingsTabIndex()});
        // r2 字阶 / 手写体收窄对照：五页 × 暗 / 青瓷 × 1280×800（WTFML_DESIGN_SHOT_FILTER=type- 只出这一组）。
        for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("emerald")})
            for (const auto &[page, tab] : {std::pair{"home", home}, std::pair{"settings", MainWindowUI::settingsTabIndex()},
                                            std::pair{"agent", agent}, std::pair{"manage", manage}, std::pair{"download", download}})
                list.append({QStringLiteral("type-%1-%2-1280x800.png").arg(QLatin1String(page), theme), theme, {1280, 800}, false, tab});
        // r2 收尾对照（去竖条 / 去小箭头 / 工具箱字阶）：六页 + 下载树展开 + 实例设置导航 × 暗 / 青瓷（FILTER=final-）。
        for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("emerald")}) {
            for (const auto &[page, tab] : {std::pair{"home", home}, std::pair{"manage", manage}, std::pair{"download", download},
                                            std::pair{"settings", MainWindowUI::settingsTabIndex()}, std::pair{"agent", agent}})
                list.append({QStringLiteral("final-%1-%2-1280x800.png").arg(QLatin1String(page), theme), theme, {1280, 800}, false, tab});
            list.append({QStringLiteral("final-download-expanded-%1-1280x800.png").arg(theme), theme, {1280, 800}, false, download,
                         QStringLiteral("final:expand")});
            list.append({QStringLiteral("final-toolbox-%1-1280x800.png").arg(theme), theme, {1280, 800}, false, home, QStringLiteral("toolbox:server")});
            list.append({QStringLiteral("final-toolbox-text-%1-1280x800.png").arg(theme), theme, {1280, 800}, false, home, QStringLiteral("toolbox:text")});
            list.append({QStringLiteral("final-instance-launch-%1-1280x800.png").arg(theme), theme, {1280, 800}, false, manage, QStringLiteral("launch")});
        }
        // 批量操作（#54）：结构 / 存档页多选之后的样子，最小窗口与拉宽两档。
        for (const char *page : {"structure", "save"})
            for (const QSize &size : {QSize(900, 590), QSize(1400, 800)})
                list.append({QStringLiteral("batch-%1-multi-dark-%2x%3.png").arg(QLatin1String(page)).arg(size.width()).arg(size.height()),
                             QStringLiteral("dark"), size, false, manage, QStringLiteral("multi:%1").arg(QLatin1String(page))});
        // 服务器地址（#68）：默认原文 / 打开「隐藏服务器地址」 / 右键菜单，浅色（青瓷）与深色各一张。
        for (const QString &theme : {QStringLiteral("emerald"), QStringLiteral("dark")})
            for (const char *kind : {"plain", "hidden", "menu"})
                list.append({QStringLiteral("server-address-%1-%2-1280x800.png").arg(QLatin1String(kind), theme), theme, {1280, 800}, false, manage,
                             QStringLiteral("srv:") + QLatin1String(kind)});
        // 只出名字含这段字的几张（逐页精修时不必每次跑满 56 张）。
        const QString only = qEnvironmentVariable("WTFML_DESIGN_SHOT_FILTER");
        if (!only.isEmpty())
            list.removeIf([&only](const Shot &shot) { return !shot.file.contains(only); });
        list += r66;  // 未登录那组会真的退出登录：放最后
        return list;
    }();
    static int index = 0;
    static QJsonArray report;
    static std::function<void()> step;
    step = [&app, &window, root]() {
        if (index >= shots.size()) {
            Theme::setSpec(Theme::presetSpec(QStringLiteral("dark")));
            Theme::apply(app);
            auto *gallery = new Gallery;
            gallery->resize(1280, 1100);
            gallery->show();
            gallery->activateWindow();
            QTimer::singleShot(300, gallery, [gallery]() {
                gallery->resize(1280, 1100);
                // 全局 QSS polish 会把 setFixedHeight 钉的最小高度改回去（输入框 23、胶囊 25），这里按钉的那一档再钉一次。
                for (QWidget *w : gallery->findChildren<QWidget *>())
                    if (w->maximumHeight() < QWIDGETSIZE_MAX && w->minimumHeight() < w->maximumHeight())
                        w->setMinimumHeight(w->maximumHeight());
            });
            QTimer::singleShot(900, gallery, [&app, gallery, root]() {
                const bool saved = grabGallery(*gallery).save(root + QStringLiteral("/components-dark-1280x1100.png"));
                QJsonArray cells;
                for (QWidget *w : std::as_const(gallery->focusCells))
                    cells.append(QStringLiteral("%1 %2x%3 min%4 max%5").arg(QString::fromLatin1(w->metaObject()->className()))
                                     .arg(w->width()).arg(w->height()).arg(w->minimumHeight()).arg(w->maximumHeight()));
                report.append(QJsonObject{{"cells", cells}});
                report.append(QJsonObject{{"file", "components-dark-1280x1100.png"}, {"saved", saved},
                                          {"width", gallery->width()}, {"height", gallery->height()}});
                QFile out(root + QStringLiteral("/design-shots.json"));
                const bool ok = out.open(QIODevice::WriteOnly) &&
                                out.write(QJsonDocument(QJsonObject{{"devicePixelRatio", gallery->devicePixelRatioF()},
                                                                    {"shots", report}}).toJson()) > 0;
                app.exit(ok && saved ? 0 : 1);
            });
            return;
        }
        const Shot shot = shots.at(index++);
        GlassPopover::closeAllIn(&window);  // 上一张开着的弹层（再点一次锚点就成了"收起"）
        // r68 中途帧把风景淡变停在了半路：这一张起手先把它走完（落到终值、撤掉「正在淡」标记）。
        if (auto *fade = window.findChild<QVariantAnimation *>(QStringLiteral("glassHeroFade"));
            fade != nullptr && fade->state() != QAbstractAnimation::Stopped)
            fade->setCurrentTime(fade->duration());
        Theme::setSpec(Theme::presetSpec(shot.theme));
        Theme::apply(app);
        window.showNormal();
        window.resize(shot.size);
        window.ui()->getTabWidget()->setCurrentIndex(shot.tab);
        if (auto *bar = window.findChild<TitleBar *>()) bar->setCollapsed(shot.collapsed);
        const bool emptyHome = shot.page == QStringLiteral("r2:home-empty");
        seedHome(window, emptyHome);
        // 竖屏 / 小屏上 Windows 首次会把超出屏宽的窗口夹回屏宽，排完版再要一次尺寸。
        QTimer::singleShot(300, &window, [&window, shot]() { window.resize(shot.size); });
        // 启动后头一张时主窗口自己还会刷一次主页（在线版本列表回来后按隔离目录里的空实例重排），
        // 抓图前再灌一次夹具，再让布局走一拍。
        QTimer::singleShot(700, &window, [&window, emptyHome]() { seedHome(window, emptyHome); });
        QTimer::singleShot(850, &window, [&window, shot]() {
            if (shot.page.startsWith(QStringLiteral("edge"))) {
                // 整窗（连窗外一圈）都得在屏上：竖屏 / 小屏上 1280 宽会伸出屏外。
                if (shot.page == QStringLiteral("edge:max")) window.showMaximized();
                else if (shot.page == QStringLiteral("edge:restore")) {
                    window.showMaximized();
                    QTimer::singleShot(300, &window, [&window] {
                        window.showNormal();
                        window.move(window.screen()->availableGeometry().topLeft() + QPoint(33, 417));
                    });
                } else window.move(window.screen()->availableGeometry().topLeft() + QPoint(60, 60));
                window.raise();
                window.activateWindow();
            } else if (shot.page == QStringLiteral("gamelog")) {
                auto *log = new GameLogWindow(QStringLiteral("春日生存"), nullptr);
                log->setObjectName(QStringLiteral("designShotGameLog"));
                log->appendText(QStringLiteral(
                    "[12:00:01] [main/INFO]: Loading Minecraft 1.21.4 with Fabric Loader 0.16.9\n"
                    "[12:00:02] [main/INFO]: Loading 42 mods\n"
                    "[12:00:03] [main/WARN]: Mod sodium uses a deprecated mixin config\n"
                    "[12:00:05] [Render thread/INFO]: Backend library: LWJGL version 3.3.3\n"
                    "[12:00:07] [Render thread/ERROR]: Failed to load texture minecraft:textures/misc/unknown.png\n"
                    "[12:00:09] [Server thread/INFO]: Preparing spawn area: 84%\n"));
                log->resize(960, 600);
                log->show();
            } else if (shot.page.startsWith(QStringLiteral("dialog:"))) {
                const QString kind = shot.page.section(QLatin1Char(':'), 1);
                QDialog *dialog = nullptr;
                if (kind == QStringLiteral("report")) {
                    auto *report = new UpgradeReportDialog(nullptr);
                    UpgradeReport::Row sodium{QStringLiteral("Sodium"), QStringLiteral("目标版本没有对应构建"),
                                              UpgradeReport::Bucket::Failed, true};
                    UpgradeReport::Row iris{QStringLiteral("Iris Shaders"), QStringLiteral("只找到测试版"),
                                            UpgradeReport::Bucket::Unknown, false};
                    report->setRows({sodium, iris});
                    dialog = report;
                } else if (kind == QStringLiteral("history")) {
                    dialog = new UpgradeHistoryDialog(nullptr);
                } else if (kind == QStringLiteral("modreview")) {
                    Presets::ModInstallPlan plan;
                    Presets::ModInstallRow sodium;
                    sodium.key = QStringLiteral("modrinth:sodium");
                    sodium.title = QStringLiteral("Sodium");
                    sodium.status = Presets::ModInstallStatus::Download;
                    sodium.version.id = QStringLiteral("abc123");
                    sodium.version.versionNumber = QStringLiteral("0.6.0");
                    sodium.file.filename = QStringLiteral("sodium-fabric-0.6.0.jar");
                    sodium.file.size = 1200000;
                    Presets::ModInstallRow iris = sodium;
                    iris.key = QStringLiteral("modrinth:iris");
                    iris.title = QStringLiteral("Iris Shaders");
                    iris.status = Presets::ModInstallStatus::Unknown;
                    iris.file = {};
                    iris.version = {};
                    plan.rows = {sodium, iris};
                    plan.download = 1;
                    plan.unknown = 1;
                    dialog = new PresetModReview(plan, nullptr);
                } else if (kind == QStringLiteral("startup")) {
                    // 照 main.cpp 那扇「启动器已在运行或目录不可用」搭一扇；提示卡下一拍再摆（它是阻塞的），
                    // 抓完图窗一关就把它收掉，嵌套循环随即退出，后面的截图照常走。
                    auto *host = new ChromeDialog(nullptr);
                    host->setWindowTitle(QStringLiteral("启动器已在运行或目录不可用"));
                    host->resize(OverlayHost::promptWidth() + 80, 320 + ChromeDialog::stripHeight());
                    auto *overlay = new OverlayHost(host->body(), host->body());
                    QObject::connect(host, &QDialog::finished, overlay, [overlay] { overlay->dismiss(); });
                    QTimer::singleShot(0, overlay, [overlay] {
                        InlineMessage::information(overlay, QStringLiteral("启动器已在运行或目录不可用"),
                                                   QStringLiteral("另一个启动器正在使用这个游戏目录。"));
                    });
                    dialog = host;
                } else {
                    dialog = new SchematicInfoDialog(QStringLiteral("C:/不存在/城堡.litematic"), nullptr);
                }
                dialog->setObjectName(QStringLiteral("designShotGameLog"));
                dialog->show();
            } else if (shot.page.startsWith(QStringLiteral("wizard:"))) {
                // 实例更新向导（兜底那扇独立窗）：开「机械动力 Create」的「版本升级」页，拿页里那只真向导翻到第 n 步。
                // 翻页、显示放到下面抓图那一拍里同步做（见那边）。
                QMetaObject::invokeMethod(&window, "openInstanceSettings", Q_ARG(QString, QStringLiteral("create")),
                                          Q_ARG(QString, QStringLiteral("upgrade")));
            } else if (shot.page.startsWith(QStringLiteral("toolbox"))) {
                if (auto *open = window.findChild<QAbstractButton *>(MainWindowUI::toolboxButtonName())) open->click();
                const QString key = shot.page.section(QLatin1Char(':'), 1);
                bool ok = false;
                const ToolboxPage::Tool tool = ToolboxPage::toolFromKey(key, &ok);
                if (ok) {
                    if (auto *button = window.findChild<QPushButton *>(ToolboxPage::toolCellName(tool))) button->click();
                    if (auto *page = window.findChild<ToolboxPage *>()) fillToolboxSample(*page, key);
                }
            } else if (shot.page == QStringLiteral("r2:home-A-switch")) {
                if (auto *picker = window.findChild<QAbstractButton *>(MainWindowUI::homeCurrentPickerName())) picker->click();
            } else if (shot.page == QStringLiteral("r2:home-acct-rail")) {
                window.ui()->getAccountSkinButton()->click();
            } else if (shot.page.startsWith(QStringLiteral("r2:agent-"))) {
                if (auto *page = window.findChild<AgentPage *>()) seedAgent(*page, shot.page.section(QLatin1Char('-'), 1));
            } else if (shot.page.startsWith(QStringLiteral("r2:"))) {
            } else if (shot.page == QStringLiteral("final:expand")) {
                // 展开态：版本树头一个有孩子的频道展开、选中它下面头一条（看「收起」字与选中淡底）。
                QTreeWidget *tree = window.ui()->getVersionTree();
                for (int i = 0; tree != nullptr && i < tree->topLevelItemCount(); ++i)
                    if (QTreeWidgetItem *top = tree->topLevelItem(i); top->childCount() > 0) {
                        top->setExpanded(true);
                        tree->setCurrentItem(top->child(0));
                        break;
                    }
            } else if (shot.page.startsWith(QStringLiteral("r68:"))) {
                if (shot.page.endsWith(QStringLiteral("-mid"))) {
                    // 开着动效换页，把风景淡变停在一半，再关回动效（换页幕布照常 250 ms 收掉，抓图时已不在）。
                    Anim::setEnabled(true);
                    window.ui()->getTabWidget()->setCurrentIndex(shot.page.contains(QStringLiteral("out"))
                                                                     ? MainWindowUI::settingsTabIndex()
                                                                     : MainWindowUI::homeTabIndex());
                    if (auto *fade = window.findChild<QVariantAnimation *>(QStringLiteral("glassHeroFade"))) {
                        fade->pause();
                        fade->setCurrentTime(Glass::Backdrop::kHeroFadeMs / 2);
                    }
                    Anim::setEnabled(false);
                }
            } else if (shot.page.startsWith(QStringLiteral("r66:"))) {
                QPushButton *pill = window.ui()->getAccountSkinButton();
                const bool open = shot.page.endsWith(QStringLiteral("-open"));
                if (shot.page.startsWith(QStringLiteral("r66:out")) && !AccountStore::activeUuid().isEmpty()) {
                    // 走真路径退出：点胶囊 → 弹层里「退出登录」那一行（动作排到下一轮，见 AccountPopover）。
                    pill->click();
                    if (auto *pop = window.findChild<QWidget *>(MainWindowUI::accountPopoverName()))
                        for (QAbstractButton *row : pop->findChildren<QAbstractButton *>())
                            if (row->accessibleName() == MainWindowUI::accountLogoutText()) row->click();
                    if (open) QTimer::singleShot(300, pill, [pill]() { pill->click(); });
                } else if (open) {
                    pill->click();
                }
            } else if (shot.page == QStringLiteral("account")) {
                // 面板按屏幕可用区夹位置：窗口伸出屏外时它会被夹回来、不再贴着胶囊右沿。
                window.move(window.screen()->availableGeometry().topLeft() + QPoint(60, 60));
                window.ui()->getAccountSkinButton()->click();
            } else if (shot.page.startsWith(QStringLiteral("srv:"))) {
                // 「机械动力 Create」的 servers.dat：一条长域名带端口、一条 IP、一条 IPv6、一条没端口。
                const QString dat = ServerListFile::pathFor(QStringLiteral("create"));
                QFile::remove(dat);
                for (const QPair<QString, QString> &server : {
                         qMakePair(QStringLiteral("长地址服务器"), QStringLiteral("survival.very-long-community-network.example.com:25566")),
                         qMakePair(QStringLiteral("朋友的服"), QStringLiteral("123.45.67.89")),
                         qMakePair(QStringLiteral("IPv6 服务器"), QStringLiteral("[2001:db8::1]:25566")),
                         qMakePair(QStringLiteral("Hypixel"), QStringLiteral("mc.hypixel.net"))})
                    ServerListFile::addEntry(dat, server.first, server.second, nullptr);
                QSettings().setValue(QStringLiteral("Look/HideServerAddress"), shot.page.endsWith(QStringLiteral("hidden")));
                QMetaObject::invokeMethod(&window, "openInstanceSettings", Q_ARG(QString, QStringLiteral("create")), Q_ARG(QString, QStringLiteral("server")));
            } else if (shot.page.startsWith(QStringLiteral("multi:"))) {
                // 「机械动力 Create」里摆四个世界、世界里摆四个结构文件，再开对应页。
                const QString saves = AppPaths::versionDir(QStringLiteral("create")) + QStringLiteral("/saves");
                for (const QString world : {QStringLiteral("生存档"), QStringLiteral("Creative Test"), QStringLiteral("红石实验场"), QStringLiteral("Skyblock-2")}) {
                    const QString structures = saves + QLatin1Char('/') + world + QStringLiteral("/generated/minecraft/structures");
                    QDir().mkpath(structures);
                    QFile level(saves + QLatin1Char('/') + world + QStringLiteral("/level.dat"));
                    if (level.open(QIODevice::WriteOnly)) level.write("stub");
                    if (world != QStringLiteral("生存档")) continue;
                    for (const QString name : {QStringLiteral("house"), QStringLiteral("farm-tower"), QStringLiteral("village-hall"), QStringLiteral("bridge")}) {
                        QFile file(structures + QLatin1Char('/') + name + QStringLiteral(".nbt"));
                        if (file.open(QIODevice::WriteOnly)) file.write("stub");
                    }
                }
                QMetaObject::invokeMethod(&window, "openInstanceSettings", Q_ARG(QString, QStringLiteral("create")),
                                          Q_ARG(QString, shot.page.section(QLatin1Char(':'), 1)));
            } else if (!shot.page.isEmpty()) QMetaObject::invokeMethod(&window, "openInstanceSettings", Q_ARG(QString, QStringLiteral("create")), Q_ARG(QString, shot.page));
            else if (shot.tab == MainWindowUI::instancesTabIndex()) hoverInstanceRow(window);
        });
        QTimer::singleShot(shot.page.isEmpty() ? 900 : 1600, &window, [&window, root, shot]() {
            QWidget *log = nullptr;  // 另开的那只独立窗（日志窗 / 对话框）
            for (QWidget *top : QApplication::topLevelWidgets())
                if (top->objectName() == QStringLiteral("designShotGameLog") && top->isVisible()) log = top;
            QWizard *wizard = nullptr;  // 实例更新向导是设置页的孩子：抓完只藏起来，不删（页里的回调还拿着它）
            if (shot.page.startsWith(QStringLiteral("wizard:"))) {
                wizard = window.findChild<QWizard *>(QStringLiteral("instanceUpdateWizard"));
                if (wizard != nullptr) {
                    wizard->restart();
                    for (int i = 0, n = shot.page.section(QLatin1Char(':'), 1).toInt(); i < n; ++i) wizard->next();
                    wizard->show();
                    for (int i = 0; i < 5; ++i) QApplication::processEvents();
                }
            }
            if (shot.page == QStringLiteral("srv:menu")) {
                // 右键第 1 行（长地址那条）弹出菜单；菜单是 Qt::Popup，下面和别的浮层一样合成进图。
                // 版本支持快速加入时下方多一颗「加入服务器」，菜单里的「加入」跟着亮。
                if (auto *dialog = window.findChild<InstanceSettingsDialog *>())
                    dialog->setServerJoinText(InstanceSettingsDialog::serverJoinText(), InstanceSettingsDialog::serverJoinTip());
                if (auto *tree = window.findChild<QTreeWidget *>(InstanceSettingsDialog::serverTreeName())) {
                    const QRect cell = tree->visualRect(tree->model()->index(0, 0));
                    const QPoint at = cell.center() - QPoint(cell.width() / 4, 0);
                    QContextMenuEvent event(QContextMenuEvent::Mouse, at, tree->viewport()->mapToGlobal(at));
                    QApplication::sendEvent(tree->viewport(), &event);
                    for (int i = 0; i < 5; ++i) QApplication::processEvents();
                }
            }
            if (shot.page.startsWith(QStringLiteral("multi:"))) {
                // 点「全选」。
                const QString key = shot.page.section(QLatin1Char(':'), 1);
                if (auto *all = window.findChild<QPushButton *>(QStringLiteral("instanceLifecycleBatchAll_%1").arg(key))) all->click();
                for (int i = 0; i < 5; ++i) QApplication::processEvents();
            }
            QPixmap shotImage = wizard != nullptr ? wizard->grab() : log != nullptr ? log->grab() : window.grab();
            if (wizard != nullptr) wizard->hide();
            if (shot.page.startsWith(QStringLiteral("edge"))) {
                // 真屏抓：widget.grab() 只有客户区的方角，看不到 DWM 圆角、系统边和窗外的衔接。
                QScreen *screen = window.screen();
                // 最大化只留上面 800 行：要看的是贴屏幕边那一圈有没有线。
                const QRect area = window.isMaximized() ? QRect(screen->geometry().topLeft(), QSize(screen->geometry().width(), 800))
                                                        : window.frameGeometry().adjusted(-24, -24, 24, 24);
                shotImage = screen->grabWindow(0, area.x() - screen->geometry().x(), area.y() - screen->geometry().y(),
                                               area.width(), area.height());
                QJsonObject numbers = frameNumbers(window);
                numbers.insert("file", shot.file);
                numbers.insert("grabArea", QJsonArray{area.x(), area.y(), area.width(), area.height()});
                std::fprintf(stdout, "EDGE %s\n", QJsonDocument(numbers).toJson(QJsonDocument::Compact).constData());
                std::fflush(stdout);
            }
            if (log != nullptr) { log->close(); log->deleteLater(); }
            // 账号下拉面板同理（也是 Qt::Popup）。
            if (auto *panel = window.findChild<QWidget *>(MainWindowUI::bottomBarPanelName()); panel != nullptr && panel->isVisible()) {
                QPainter painter(&shotImage);
                painter.drawPixmap(window.mapFromGlobal(panel->mapToGlobal(QPoint(0, 0))), panel->grab());
                panel->close();
            }
            // 智能体页的 ⋯ 菜单 / 实例下拉同理（Qt::Popup）。
            for (const char *name : {"agentMoreMenu", "agentInstanceMenu", "serverMenu"}) {
                if (auto *menu = window.findChild<QWidget *>(QLatin1String(name)); menu != nullptr && menu->isVisible()) {
                    QPainter painter(&shotImage);
                    painter.drawPixmap(window.mapFromGlobal(menu->mapToGlobal(QPoint(0, 0))), menu->grab());
                    menu->close();
                }
            }
            const bool saved = shotImage.save(root + QLatin1Char('/') + shot.file);
            report.append(QJsonObject{{"file", shot.file}, {"saved", saved}, {"width", window.width()},
                                      {"height", window.height()}, {"dpr", window.devicePixelRatioF()},
                                      {"maxWidth", window.maximumWidth()}, {"screenWidth", window.screen()->availableGeometry().width()},
                                      {"state", int(window.windowState())}});
            if (!shot.page.isEmpty()) window.ui()->overlay()->closeOverlay();
            if (shot.page.startsWith(QStringLiteral("toolbox"))) window.ui()->getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
            QTimer::singleShot(shot.page.isEmpty() ? 0 : 300, &window, step);
        });
    };
    QTimer::singleShot(1200, &window, step);
}

}  // namespace DesignShots
