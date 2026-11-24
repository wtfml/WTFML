/**
 * @file MainWindowHomePopovers.cpp
 * @brief 主页上的两块毛玻璃弹层：实例切换（盖在启动区实例名上展开）与账号（从右上账号胶囊下面弹出）。
 *
 * 两块都是 GlassPopover（窗口里的子卡片，圆角 14、150 ms 淡入、点外面 / Esc 收起），
 * 设计稿见 r2 home.html 的 .swpop / .acpop。数据由 MainWindow 每次打开时现给，这里只画和回报。
 */
#include "MainWindowUI.hpp"

#include "ElidedLabel.hpp"
#include "GlassPopover.hpp"
#include "InlineMessage.hpp"
#include "MainWindowPageLayout.hpp"
#include "OverlayHost.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

using namespace MainWindowPageLayout;

namespace {

/// 锚点"弹层开着"那一档的底色（启动区实例名那一块 / 轨上账号那一格都认 wtfmlOpen）。
void markAnchorOpen(QWidget *anchor, bool open) {
    if (anchor == nullptr) return;
    anchor->setProperty("wtfmlOpen", open);
    anchor->update();
}

/// 弹层里的一行字：系统字 @p px，颜色取 token。
QLabel *popLabel(const QString &text, int px, QFont::Weight weight, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    QFont f = label->font();
    f.setPixelSize(px);
    f.setWeight(weight);
    label->setFont(f);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    return label;
}

void setInk(QWidget *w, const QColor &ink) {
    QPalette p = w->palette();
    p.setColor(QPalette::WindowText, ink);
    p.setColor(QPalette::ButtonText, ink);
    w->setPalette(p);
}

/// 面板里一整行就是一颗按钮（设计稿 .pop .it / .acpop .mi）：平时无底，悬停 fill-press，
/// 选中（当前那一行）弱强调底。里面的字和图由调用方摆进 layout。
class PopRow final : public QAbstractButton {
public:
    explicit PopRow(QWidget *parent) : QAbstractButton(parent) {
        setAttribute(Qt::WA_Hover, true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }
    void setOn(bool on) { m_on = on; }
    QSize sizeHint() const override { return layout() != nullptr ? layout()->sizeHint() : QSize(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme::Tokens t = Theme::tokens();
        const QColor base = m_on ? t.selected : (isDown() || underMouse() || hasFocus()) ? t.fillPress : QColor(Qt::transparent);
        if (base.alpha() > 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(base);
            p.drawRoundedRect(QRectF(rect()), 8, 8);
        }
    }
    void enterEvent(QEnterEvent *e) override { QAbstractButton::enterEvent(e); update(); }
    void leaveEvent(QEvent *e) override { QAbstractButton::leaveEvent(e); update(); }
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            click();
            return;
        }
        QAbstractButton::keyPressEvent(e);
    }

private:
    bool m_on = false;
};

/// 圆角方块头像（账号弹层 32 / 48）：没有图时画淡底 + 人形线条。
QPixmap roundedFace(const QPixmap &face, int side, int radius) {
    const qreal dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
    QPixmap out(QSize(side, side) * dpr);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, side, side), radius, radius);
    const Theme::Tokens t = Theme::tokens();
    if (face.isNull()) {
        p.fillPath(clip, t.subtle);
        const qreal s = side * 0.56;
        VersionIcons::paintLine(p, QStringLiteral("user"), QRectF((side - s) / 2, (side - s) / 2, s, s), t.secondary);
    } else {
        p.setClipPath(clip);
        p.drawPixmap(QRect(0, 0, side, side), face);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 实例切换弹层（设计稿 .swpop）
// ---------------------------------------------------------------------------

constexpr int kPickerWidth = 320;
constexpr int kPickerRowHeight = 34;
constexpr int kPickerHeadHeight = 28;
constexpr int kPickerRecentLimit = 5;
constexpr int kDownloadPickerListHeight = 8 * kPickerRowHeight;
constexpr int kRoleId = Qt::UserRole;
constexpr int kRoleFolder = Qt::UserRole + 1;
constexpr int kRoleKind = Qt::UserRole + 2;    ///< 0 = 小节标题，1 = 实例
constexpr int kRoleCurrent = Qt::UserRole + 3;
constexpr int kRoleSub = Qt::UserRole + 4;

/// 列表里一行（设计稿 .pop .it / .mh）：小节标题是 12 号次要字；实例行 = 26 封面 + 名字 + 右边版本。
class PickerDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override {
        return QSize(0, index.data(kRoleKind).toInt() == 0 ? kPickerHeadHeight : kPickerRowHeight);
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const Theme::Tokens t = Theme::tokens();
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const QRect r = option.rect;
        QFont f = option.font;
        if (index.data(kRoleKind).toInt() == 0) {
            f.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
            f.setWeight(QFont::DemiBold);
            p->setFont(f);
            p->setPen(t.secondary);
            p->drawText(r.adjusted(12, 10, -12, -4), Qt::AlignLeft | Qt::AlignBottom, index.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }
        const bool on = index.data(kRoleCurrent).toBool();
        const bool hot = (option.state & QStyle::State_MouseOver) || (option.state & QStyle::State_Selected);
        if (on || hot) {
            p->setPen(Qt::NoPen);
            p->setBrush(on ? t.selected : t.fillPress);
            p->drawRoundedRect(QRectF(r), 8, 8);
        }
        const QColor ink = on ? Theme::textTone(t.accentInk, t.deep) : t.text;
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        const QRect cover(r.left() + 12, r.center().y() - 13 + 1, 26, 26);
        if (!icon.isNull()) {
            icon.paint(p, cover);
        } else {
            p->drawPixmap(cover, VersionIcons::isoBlock(VersionIcons::isoBlockKindFor(index.data(Qt::DisplayRole).toString()), 26));
        }
        f.setPixelSize(14);
        f.setWeight(QFont::Normal);
        QFont small = f;
        small.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
        const QString sub = index.data(kRoleSub).toString();
        const int subWidth = sub.isEmpty() ? 0 : QFontMetrics(small).horizontalAdvance(sub);
        const int textLeft = cover.right() + 11;
        const int right = r.right() - 12;
        p->setFont(f);
        p->setPen(ink);
        const QRect nameBox(textLeft, r.top(), qMax(0, right - textLeft - (subWidth > 0 ? subWidth + 12 : 0)), r.height());
        p->drawText(nameBox, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(f).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, nameBox.width()));
        if (!sub.isEmpty()) {
            QColor subInk = on ? ink : t.secondary;
            if (on) subInk.setAlphaF(0.8);
            p->setFont(small);
            p->setPen(subInk);
            p->drawText(QRect(right - subWidth, r.top(), subWidth, r.height()), Qt::AlignRight | Qt::AlignVCenter, sub);
        }
        p->restore();
    }
};

class InstancePickerList final : public QWidget {
public:
    InstancePickerList(const QList<MainWindowUI::HomePickerInstance> &instances, QWidget *parent,
                       bool confirmSelection = false)
        : QWidget(parent), m_instances(instances), m_confirmSelection(confirmSelection) {
        auto *box = new QVBoxLayout(this);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(4);
        m_search = new QLineEdit(this);
        m_search->setObjectName(QStringLiteral("homePickerSearch"));
        m_search->setPlaceholderText(QCoreApplication::translate("MainWindowUI", "搜索实例"));
        m_search->setClearButtonEnabled(true);
        m_search->setFixedHeight(34);
        addSearchIcon(m_search);
        box->addWidget(m_search);
        m_list = new QListWidget(this);
        m_list->setObjectName(QStringLiteral("homePickerInstanceList"));
        m_list->setFrameShape(QFrame::NoFrame);
        m_list->setMouseTracking(true);
        m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_list->setItemDelegate(new PickerDelegate(m_list));
        m_list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; outline: none; }"
                                             "QListWidget::item { background: transparent; border: none; }"));
        m_list->viewport()->setAutoFillBackground(false);
        box->addWidget(m_list, 1);
        m_empty = popLabel(QCoreApplication::translate("MainWindowUI", "没有符合的实例"), 13, QFont::Normal, this);
        m_empty->setObjectName(QStringLiteral("homePickerEmpty"));
        m_empty->setAlignment(Qt::AlignCenter);
        m_empty->setMinimumHeight(64);
        setInk(m_empty, Theme::tokens().secondary);
        m_empty->hide();
        box->addWidget(m_empty, 1);

        if (m_confirmSelection) {
            for (const auto &instance : m_instances) {
                if (instance.current) {
                    m_selectedId = instance.id;
                    break;
                }
            }
            QObject::connect(m_list, &QListWidget::currentItemChanged, this,
                             [this](QListWidgetItem *item) {
                m_selectedId = item == nullptr ? QString() : item->data(kRoleId).toString();
                if (onSelectionChanged) onSelectionChanged();
            });
        } else {
            QObject::connect(m_list, &QListWidget::itemClicked, this,
                             [this](QListWidgetItem *item) { activate(item); });
        }
        QObject::connect(m_search, &QLineEdit::textChanged, this, [this]() { rebuild(); });
        QObject::connect(m_search, &QLineEdit::returnPressed, this, [this]() {
            if (m_confirmSelection) {
                activate(m_list->currentItem());
            } else {
                activate(firstInstance());
            }
        });
        QObject::connect(m_list, &QListWidget::itemActivated, this,
                         [this](QListWidgetItem *item) { activate(item); });
        m_search->installEventFilter(this);
        rebuild();
    }

    void focusSearch() { m_search->setFocus(Qt::PopupFocusReason); }
    QString selectedId() const { return m_selectedId; }
    int contentHeight() const {
        int height = m_search->height() + layout()->spacing();
        if (m_list->isHidden()) return height + m_empty->minimumHeight();
        for (int row = 0; row < m_list->count(); ++row) height += m_list->sizeHintForRow(row);
        return height;
    }

    MainWindowUI::HomePickerInstanceHandler onActivate;
    std::function<void()> onSelectionChanged;
    std::function<void()> onRebuilt;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == m_search && event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent *>(event)->key() == Qt::Key_Down) {
            if (auto *item = m_list->currentItem() != nullptr ? m_list->currentItem() : firstInstance()) {
                m_list->setCurrentItem(item);
                m_list->setFocus();
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    QListWidgetItem *firstInstance() const {
        for (int row = 0; row < m_list->count(); ++row) {
            if (m_list->item(row)->data(kRoleKind).toInt() == 1) return m_list->item(row);
        }
        return nullptr;
    }

    bool matches(const MainWindowUI::HomePickerInstance &instance, const QStringList &terms) const {
        QString haystack = instance.title + QLatin1Char(' ') + instance.subtitle + QLatin1Char(' ') + instance.id
                           + QLatin1Char(' ') + instance.loaderKey + QLatin1Char(' ') + instance.folderPath
                           + QLatin1Char(' ') + instance.searchKeywords;
        if (instance.loaderKey.isEmpty()) haystack += QStringLiteral(" 原版 纯净 vanilla 无加载器");
        else haystack += QStringLiteral(" 模组 modded");
        if (instance.favorite) haystack += QStringLiteral(" 收藏 置顶 favorite starred");
        if (instance.recent) haystack += QStringLiteral(" 最近 上次游玩 recent played");
        return std::all_of(terms.cbegin(), terms.cend(),
                           [&haystack](const QString &term) { return haystack.contains(term, Qt::CaseInsensitive); });
    }
    void addHead(const QString &text) {
        auto *item = new QListWidgetItem(text, m_list);
        item->setFlags(Qt::NoItemFlags);
        item->setData(kRoleKind, 0);
    }
    void addRow(const MainWindowUI::HomePickerInstance &instance) {
        auto *item = new QListWidgetItem(instance.icon, instance.title, m_list);
        item->setData(kRoleKind, 1);
        item->setData(kRoleId, instance.id);
        item->setData(kRoleFolder, instance.folderPath);
        item->setData(kRoleCurrent, instance.current && !m_confirmSelection);
        item->setData(kRoleSub, instance.subtitle);
        item->setToolTip(instance.subtitle.isEmpty() ? instance.title
            : instance.title + QLatin1Char('\n') + instance.subtitle);
    }
    void rebuild();
    void activate(QListWidgetItem *item) {
        if (!isVisible() || item == nullptr || item->data(kRoleKind).toInt() != 1 || !onActivate) return;
        onActivate(item->data(kRoleFolder).toString(), item->data(kRoleId).toString());
    }

    QList<MainWindowUI::HomePickerInstance> m_instances;
    bool m_confirmSelection;
    QString m_selectedId;
    QLineEdit *m_search = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_empty = nullptr;
};

class HomeInstancePicker final : public GlassPopover {
public:
    HomeInstancePicker(QWidget *anchor, const QList<MainWindowUI::HomePickerInstance> &instances,
                       MainWindowUI::HomePickerInstanceHandler onPick,
                       std::function<void()> onNew, std::function<void()> onManage)
        : GlassPopover(anchor) {
        setObjectName(MainWindowUI::homeInstancePickerName());
        auto *box = new QVBoxLayout(this);
        box->setContentsMargins(6, 6, 6, 6);
        box->setSpacing(0);
        m_content = new InstancePickerList(instances, this);
        box->addWidget(m_content, 1);
        m_content->onActivate = [this, onPick = std::move(onPick)](const QString &folder, const QString &id) {
            close();
            if (onPick) onPick(folder, id);
        };
        m_content->onRebuilt = [this]() { if (isVisible()) place(); };
        // 底部一行：上面一道细线（设计稿 .foot box-shadow 0 -1px 0 line）。
        auto *line = new QWidget(this);
        line->setFixedHeight(1);
        line->setAutoFillBackground(true);
        {
            QPalette p = line->palette();
            p.setColor(QPalette::Window, Theme::tokens().divider);
            line->setPalette(p);
        }
        box->addSpacing(6);
        box->addWidget(line);
        auto *foot = new QHBoxLayout;
        foot->setContentsMargins(2, 6, 2, 0);
        foot->setSpacing(4);
        const auto footButton = [this, foot](const QString &name, const QString &text, const QString &icon,
                                             std::function<void()> fn) {
            auto *b = new QPushButton(text, this);
            b->setObjectName(name);
            b->setCursor(Qt::PointingHandCursor);
            Theme::markButton(b, Theme::ButtonKind::Ghost);
            QFont f = b->font();
            f.setPixelSize(Theme::textSize(Theme::TextRole::Body));
            b->setFont(f);
            if (!icon.isEmpty()) {
                b->setIcon(QIcon(VersionIcons::line(icon, 16, Theme::tokens().secondary, 1.8)));
                b->setIconSize(QSize(16, 16));
            }
            QObject::connect(b, &QPushButton::clicked, this, [this, fn = std::move(fn)]() {
                close();
                if (fn) fn();
            });
            foot->addWidget(b);
            return b;
        };
        footButton(QStringLiteral("homePickerNew"), QCoreApplication::translate("MainWindowUI", "新建实例"), QStringLiteral("plus"), std::move(onNew));
        foot->addStretch(1);
        footButton(QStringLiteral("homePickerManage"), QCoreApplication::translate("MainWindowUI", "管理全部实例"), QString(), std::move(onManage));
        box->addLayout(foot);

        markAnchorOpen(anchor, true);
    }

protected:
    void closeEvent(QCloseEvent *e) override {
        markAnchorOpen(m_anchor, false);
        GlassPopover::closeEvent(e);
    }
    void place() override {
        QWidget *host = parentWidget();
        if (host == nullptr || m_anchor == nullptr) return;
        const QPoint a = m_anchor->mapTo(host, QPoint(0, 0));
        const int width = qMax(kPickerWidth, m_anchor->width() + 12);
        const int top = qMax(48, a.y() - 6);
        const int x = qBound(8, a.x() - 6, qMax(8, host->width() - width - 8));
        // 想要的高 = 搜索 + 列表全部行 + 底栏；下面放不下就收到窗底留 16。
        const QMargins m = layout()->contentsMargins();
        const int chrome = m.top() + m.bottom() + 6 + 1 + 6 + 32;
        const int wanted = chrome + m_content->contentHeight() + 2;
        const int height = qMax(160, qMin(wanted, host->height() - top - 16));
        setGeometry(x, top, width, height);
        raise();
    }
    void showEvent(QShowEvent *e) override {
        GlassPopover::showEvent(e);
        QTimer::singleShot(0, this, [this]() { m_content->focusSearch(); });
    }

private:
    InstancePickerList *m_content = nullptr;
};

void InstancePickerList::rebuild() {
    const QString previousId = m_selectedId;
    {
        const QSignalBlocker blocker(m_list);
        m_list->clear();
        const QStringList terms =
            m_search->text().trimmed().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (!terms.isEmpty()) {
            // 搜的时候只列结果（不分段）。
            for (const auto &instance : m_instances)
                if (matches(instance, terms)) addRow(instance);
        } else {
            QList<MainWindowUI::HomePickerInstance> recent;
            for (const auto &instance : m_instances)
                if (instance.recentRank >= 0) recent.append(instance);
            std::stable_sort(recent.begin(), recent.end(),
                             [](const auto &a, const auto &b) { return a.recentRank < b.recentRank; });
            if (recent.size() > kPickerRecentLimit) recent = recent.mid(0, kPickerRecentLimit);
            if (!recent.isEmpty()) {
                addHead(QCoreApplication::translate("MainWindowUI", "最近"));
                for (const auto &instance : recent) addRow(instance);
            }
            addHead(QCoreApplication::translate("MainWindowUI", "全部 · %1").arg(m_instances.size()));
            for (const auto &instance : m_instances) addRow(instance);
        }
        m_selectedId.clear();
        if (m_confirmSelection && !previousId.isEmpty()) {
            for (int row = 0; row < m_list->count(); ++row) {
                auto *item = m_list->item(row);
                if (item->data(kRoleId).toString() == previousId) {
                    m_list->setCurrentItem(item);
                    m_selectedId = previousId;
                    break;
                }
            }
        }
        const bool any = firstInstance() != nullptr;
        m_empty->setVisible(!any);
        m_list->setVisible(any);
    }
    if (onSelectionChanged) onSelectionChanged();
    if (onRebuilt) onRebuilt();
}

// ---------------------------------------------------------------------------
// 账号弹层（设计稿 .acpop）
// ---------------------------------------------------------------------------

/// 已登录：账号一行一个（头像 32 + 名字 / 类型，当前那个打勾），一道细线，「添加账号」「管理账号与皮肤」。
/// 一个账号都没有：居中一块占位头像 48 +「未登录」+ 一颗「登录」主按钮。
/// 从锚点（主页右上那颗账号胶囊）下面 8 弹出，右边和它对齐（窄窗时夹在窗口里）。
class AccountPopover final : public GlassPopover {
public:
    AccountPopover(QWidget *anchor, const QList<MainWindowUI::PanelRow> &rows,
                   std::function<void(const QString &, int)> onAction)
        : GlassPopover(anchor), m_onAction(std::move(onAction)) {
        setObjectName(MainWindowUI::accountPopoverName());
        setFixedWidth(300);
        auto *box = new QVBoxLayout(this);
        box->setSpacing(0);
        const Theme::Tokens t = Theme::tokens();
        const bool signedOut = !rows.isEmpty() && rows.first().placeholder;
        if (signedOut) {
            box->setContentsMargins(18, 18, 18, 16);
            box->setSpacing(10);
            auto *face = new QLabel(this);
            face->setPixmap(roundedFace(QPixmap(), 48, 14));
            face->setFixedSize(48, 48);
            box->addWidget(face, 0, Qt::AlignHCenter);
            auto *state = popLabel(rows.first().title, 14, QFont::Normal, this);
            state->setObjectName(QStringLiteral("accountPopoverState"));
            setInk(state, t.secondary);
            box->addWidget(state, 0, Qt::AlignHCenter);
            for (int i = 1; i < rows.size(); ++i) {
                auto *login = new QPushButton(rows.at(i).title, this);
                login->setObjectName(QStringLiteral("accountPopoverRow%1").arg(i));
                login->setCursor(Qt::PointingHandCursor);
                Theme::markButton(login, Theme::ButtonKind::Primary);
                connectAction(login, rows.at(i).key);
                box->addSpacing(4);
                box->addWidget(login);
            }
            markAnchorOpen(anchor, true);
            return;
        }
        box->setContentsMargins(6, 6, 6, 6);
        bool dividerPlaced = false;
        for (int i = 0; i < rows.size(); ++i) {
            const MainWindowUI::PanelRow &row = rows.at(i);
            if (row.menuItem && !dividerPlaced) {
                dividerPlaced = true;
                auto *divider = new QWidget(this);
                divider->setFixedHeight(1);
                divider->setAutoFillBackground(true);
                QPalette p = divider->palette();
                p.setColor(QPalette::Window, t.divider);
                divider->setPalette(p);
                box->addSpacing(6);
                auto *pad = new QHBoxLayout;
                pad->setContentsMargins(10, 0, 10, 0);
                pad->addWidget(divider);
                box->addLayout(pad);
                box->addSpacing(6);
            }
            auto *button = new PopRow(this);
            button->setObjectName(QStringLiteral("accountPopoverRow%1").arg(i));
            button->setAccessibleName(row.title);
            auto *line = new QHBoxLayout(button);
            line->setContentsMargins(12, 0, 12, 0);
            line->setSpacing(12);
            if (row.menuItem) {
                button->setFixedHeight(36);
                auto *icon = new QLabel(button);
                icon->setFixedSize(18, 18);
                icon->setAttribute(Qt::WA_TransparentForMouseEvents);
                icon->setPixmap(row.icon.isNull() ? QPixmap() : row.icon);
                line->addWidget(icon);
                auto *text = popLabel(row.title, 14, QFont::Normal, button);
                text->setObjectName(QStringLiteral("accountPopoverRowTitle%1").arg(i));
                setInk(text, t.text);
                line->addWidget(text, 1);
            } else {
                button->setFixedHeight(52);
                button->setOn(row.checked);
                auto *face = new QLabel(button);
                face->setFixedSize(32, 32);
                face->setAttribute(Qt::WA_TransparentForMouseEvents);
                face->setObjectName(QStringLiteral("accountPopoverRowFace%1").arg(i));
                face->setPixmap(roundedFace(row.icon, 32, 9));
                face->setProperty("wtfmlFaceSource", row.icon);  // 圆角前那张（验证工程比"是不是同一张脸"）
                line->addWidget(face);
                auto *text = new QVBoxLayout;
                text->setSpacing(2);
                text->setContentsMargins(0, 0, 0, 0);
                auto *name = new ElidedLabel(button);
                name->setObjectName(QStringLiteral("accountPopoverRowTitle%1").arg(i));
                name->setText(row.title);
                QFont nf = name->font();
                nf.setPixelSize(14);
                nf.setWeight(QFont::Medium);
                name->setFont(nf);
                name->setMinimumWidth(1);
                name->setAttribute(Qt::WA_TransparentForMouseEvents);
                const QColor accent = Theme::textTone(t.accentInk, t.deep);
                setInk(name, row.checked ? accent : t.text);
                text->addWidget(name);
                auto *kind = popLabel(row.subtitle, Theme::textSize(Theme::TextRole::Caption), QFont::Normal, button);
                kind->setObjectName(QStringLiteral("accountPopoverRowSub%1").arg(i));
                QColor kindInk = row.checked ? accent : t.secondary;
                if (row.checked) kindInk.setAlphaF(0.8);
                setInk(kind, kindInk);
                text->addWidget(kind);
                line->addLayout(text, 1);
                if (row.checked) {
                    auto *check = new QLabel(button);
                    check->setObjectName(QStringLiteral("accountPopoverCheck"));
                    check->setFixedSize(18, 18);
                    check->setAttribute(Qt::WA_TransparentForMouseEvents);
                    check->setPixmap(VersionIcons::line(QStringLiteral("check"), 18, accent, 2.0));
                    line->addWidget(check);
                }
            }
            connectAction(button, row.key);
            box->addWidget(button);
        }
        markAnchorOpen(anchor, true);
    }

protected:
    void closeEvent(QCloseEvent *e) override {
        markAnchorOpen(m_anchor, false);
        GlassPopover::closeEvent(e);
    }
    void place() override {
        QWidget *host = parentWidget();
        if (host == nullptr || m_anchor == nullptr) return;
        adjustSize();
        const QPoint a = m_anchor->mapTo(host, QPoint(0, 0));
        const int x = qBound(8, a.x() + m_anchor->width() - width(), qMax(8, host->width() - width() - 8));
        move(x, a.y() + m_anchor->height() + 8);
        raise();
    }

private:
    void connectAction(QAbstractButton *button, const QString &key) {
        QObject::connect(button, &QAbstractButton::clicked, this, [this, key]() {
            auto fn = m_onAction;
            QWidget *context = m_anchor != nullptr ? m_anchor.data() : parentWidget();
            close();
            // 排到下一轮：这些动作里有的要开整页覆盖层，弹层先收干净再开。
            if (fn) QTimer::singleShot(0, context, [fn, key]() { fn(key, 0); });
        });
    }
    std::function<void(const QString &, int)> m_onAction;
};

template <typename T>
T *openPopover(QWidget *anchor, const QString &name) {
    if (anchor == nullptr) return nullptr;
    // 两个弹层类没写 Q_OBJECT（不加信号），findChild 只能按 QWidget 找、再 dynamic_cast。
    auto *open = dynamic_cast<T *>(anchor->window()->findChild<QWidget *>(name));
    if (open != nullptr && open->isVisible()) return open;
    return nullptr;
}

}  // namespace

QString MainWindowUI::chooseResourceDownloadInstance(const QList<HomePickerInstance> &instances,
                                                      bool *otherFolder, bool importing) {
    const QString title = importing ? QCoreApplication::translate("MainWindowUI", "装到哪个实例里")
                                    : QStringLiteral("下载到哪个实例");
    const QString action = importing ? QCoreApplication::translate("MainWindowUI", "安装")
                                     : QStringLiteral("下载");
    InlineMessageBox box(MessageKind::Question, title,
                         QCoreApplication::translate("MainWindowUI", "请选择目标实例或其他文件夹。"),
                         {action, QCoreApplication::translate("MainWindowUI", "其他文件夹…"),
                          QStringLiteral("取消")}, -1, -1);
    box.setObjectName(resourceDownloadInstancePickerName());
    auto *content = new InstancePickerList(instances, &box, true);
    content->setFixedHeight(34 + 4 + kDownloadPickerListHeight);
    box.setContent(content);
    auto *download = box.buttonAt(0);
    content->onSelectionChanged = [content, download]() {
        download->setEnabled(!content->selectedId().isEmpty());
    };
    content->onSelectionChanged();
    content->onActivate = [download](const QString &, const QString &) { download->click(); };
    overlay()->execPrompt(&box);
    *otherFolder = box.clickedIndex() == 1;
    return box.clickedIndex() == 0 ? content->selectedId() : QString();
}

void MainWindowUI::openHomeInstancePicker(const QList<HomePickerFolder> &folders,
                                          const QList<HomePickerInstance> &instances,
                                          const HomePickerFolderHandler &folderHandler,
                                          const HomePickerInstanceHandler &instanceHandler) {
    Q_UNUSED(folders);
    Q_UNUSED(folderHandler);  // 文件夹由选中实例的回调一并切换（实例自己带着 folderPath）
    QWidget *anchor = homeQuickCurrentSection == nullptr
                          ? nullptr
                          : homeQuickCurrentSection->findChild<QWidget *>(homeCurrentPickerName());
    if (anchor == nullptr) return;
    if (homeInstancePickerOpen()) {
        closeHomeInstancePicker();
        return;
    }
    auto *picker = new HomeInstancePicker(
        anchor, instances, instanceHandler,
        [this]() { tabWidget->setCurrentIndex(downloadTabIndex()); },
        [this]() { tabWidget->setCurrentIndex(instancesTabIndex()); });
    picker->popUp();
}

bool MainWindowUI::homeInstancePickerOpen() const {
    return tabWidget != nullptr && openPopover<HomeInstancePicker>(tabWidget, homeInstancePickerName()) != nullptr;
}

void MainWindowUI::closeHomeInstancePicker() {
    if (auto *open = tabWidget == nullptr ? nullptr : openPopover<HomeInstancePicker>(tabWidget, homeInstancePickerName()))
        open->close();
}

void MainWindowUI::toggleAccountsPanel() {
    if (accountSkinButton == nullptr) return;
    if (auto *open = openPopover<AccountPopover>(accountSkinButton, accountPopoverName())) {
        open->close();
        return;
    }
    closeHomeInstancePicker();
    // 底栏那块运行中面板开着就先收（同一时间只挂一块弹层）。
    if (bottomBar != nullptr) {
        if (QWidget *panel = bottomBar->findChild<QWidget *>(bottomBarPanelName()); panel != nullptr && panel->isVisible()) {
            panel->close();
        }
    }
    const QList<PanelRow> rows = m_panelRowsProvider ? m_panelRowsProvider(true) : QList<PanelRow>();
    auto *popover = new AccountPopover(accountSkinButton, rows, [this](const QString &key, int action) {
        if (m_panelActionHandler) m_panelActionHandler(true, key, action);
    });
    popover->popUp();
}
