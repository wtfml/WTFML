/**
 * @file PresetInstallPicker.cpp
 * @brief 安装页那张「预设」弹窗的正文（头文件里写了它为什么长这样）。
 */

#include "PresetInstallPicker.hpp"
#include "ChromeDialog.hpp"
#include "FlowLayout.hpp"

#include "LoadingState.hpp"
#include "MainWindowPageLayout.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {
constexpr int kStatusRole = Qt::UserRole + 1;
constexpr int kCompositeRole = Qt::UserRole + 2;
constexpr int kCountRole = Qt::UserRole + 3;
constexpr int kProblemRole = Qt::UserRole + 4;
constexpr int kTypeRole = Qt::UserRole + 5;
constexpr int kChosenRole = Qt::UserRole + 6;  // 左栏：这一类本次有选（计数旁画小点）

QString css(const QColor &color) { return color.name(QColor::HexArgb); }

/// 悬停 / 选中那块底（设计稿 .op / .ix：悬停 fill-hover、选中 accent-soft-2，不画竖条）。
void paintRowBlock(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index, qreal radius) {
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver) && index.flags().testFlag(Qt::ItemIsEnabled);
    if (!selected && !hovered) return;
    const Theme::Tokens t = Theme::tokens();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(selected ? t.accentSoft2 : t.hover);
    painter->drawRoundedRect(QRectF(option.rect).adjusted(0, 1, 0, -1), radius, radius);
    painter->restore();
}

/// 右栏候选行（设计稿 .op）：32 图标块 + 名字「/ N 项」+ 类型小字，行尾一句状态；行间 1px 极淡线。
class PresetChoiceDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return QSize(180, 54); }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const Theme::Tokens t = Theme::tokens();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        painter->save();
        painter->setClipRect(option.rect);
        if (!index.flags().testFlag(Qt::ItemIsEnabled)) painter->setOpacity(0.55);
        paintRowBlock(painter, option, index, 10);
        const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
        const QModelIndex above = index.sibling(index.row() - 1, 0);
        const bool aboveSelected = view != nullptr && view->selectionModel() != nullptr && above.isValid() &&
                                   view->selectionModel()->isSelected(above);
        if (index.row() > 0 && !selected && !hovered && !aboveSelected) {
            painter->fillRect(QRect(option.rect.left() + 12, option.rect.top(), option.rect.width() - 24, 1), t.divider);
        }
        const bool none = index.data(Qt::UserRole).toString().isEmpty();
        const QRectF tile(option.rect.left() + 12, option.rect.center().y() - 15.5, 32, 32);
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (none) {
            VersionIcons::paintLine(*painter, QStringLiteral("xc"), tile.adjusted(7, 7, -7, -7), t.disabled);
        } else {
            painter->setPen(Qt::NoPen);
            painter->setBrush(t.subtle);
            painter->drawRoundedRect(tile, 9, 9);
            const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            if (!icon.isNull()) icon.paint(painter, tile.adjusted(7, 7, -7, -7).toRect());
            else VersionIcons::paintLine(*painter, QStringLiteral("list"), tile.adjusted(7, 7, -7, -7), t.secondary);
        }
        const int textLeft = option.rect.left() + 56;
        int right = option.rect.right() - 12;
        const QFont caption = Theme::textFont(option.font, Theme::TextRole::Caption);
        const QString status = index.data(kStatusRole).toString();
        const int statusMax = int(option.rect.width() * 0.42);
        const int checkingW = status.isEmpty() ? -1 : LoadingState::paintRowStatus(
            painter, QRect(right + 1 - statusMax, option.rect.top(), statusMax, option.rect.height()), index, caption, status);
        if (checkingW >= 0) {
            right -= checkingW + 12;
        } else if (!status.isEmpty()) {
            const QFontMetrics fm(caption);
            const int width = qMin(fm.horizontalAdvance(status), int(option.rect.width() * 0.42));
            painter->setFont(caption);
            painter->setPen(index.data(kProblemRole).toBool() ? Theme::textTone(t.warnInk, t.warn) : t.secondary);
            painter->drawText(QRect(right - width, option.rect.top(), width, option.rect.height()),
                              Qt::AlignVCenter | Qt::AlignRight, fm.elidedText(status, Qt::ElideRight, width));
            right -= width + 12;
        }
        const int width = qMax(0, right - textLeft);
        QFont name = Theme::textFont(option.font, Theme::TextRole::Body);
        name.setWeight(QFont::Medium);
        const QFontMetrics nameMetrics(name), captionMetrics(caption);
        const int count = index.data(kCountRole).toInt();
        const QString countText = !none && count >= 0 ? QObject::tr(" / %1 项").arg(count) : QString();
        const int countWidth = qMin(captionMetrics.horizontalAdvance(countText), width / 2);
        const int top = none ? option.rect.center().y() - 10 : option.rect.center().y() - 19;
        const QString shown = nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                                                     width - countWidth);
        painter->setFont(name);
        painter->setPen(t.text);
        painter->drawText(QRect(textLeft, top, width, 20), Qt::AlignVCenter | Qt::AlignLeft, shown);
        if (!countText.isEmpty()) {
            painter->setFont(caption);
            painter->setPen(t.secondary);
            painter->drawText(QRect(textLeft + nameMetrics.horizontalAdvance(shown), top, countWidth, 20),
                              Qt::AlignVCenter | Qt::AlignLeft, countText);
        }
        if (!none) {
            painter->setFont(caption);
            painter->setPen(t.secondary);
            painter->drawText(QRect(textLeft, top + 21, width, 17), Qt::AlignVCenter | Qt::AlignLeft,
                              captionMetrics.elidedText(index.data(kTypeRole).toString(), Qt::ElideRight, width));
        }
        painter->restore();
    }
};

/// 左栏（设计稿 .nav-l .ix）：36 高侧栏行，右边计数；这一类本次有选时计数前一颗强调色小点。
class PresetNavDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return QSize(110, 36); }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const Theme::Tokens t = Theme::tokens();
        painter->save();
        paintRowBlock(painter, option, index, Theme::controlRadius());
        const QRect inner = option.rect.adjusted(12, 0, -12, 0);
        QFont font = Theme::textFont(option.font, Theme::TextRole::Body);
        const QString count = QString::number(index.data(kCountRole).toInt());
        const QFontMetrics fm(font);
        const int countWidth = fm.horizontalAdvance(count);
        painter->setFont(font);
        painter->setPen(t.secondary);
        painter->drawText(inner, Qt::AlignVCenter | Qt::AlignRight, count);
        int textRight = inner.right() - countWidth - 8;
        if (index.data(kChosenRole).toBool()) {
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(t.primary);
            painter->drawEllipse(QRectF(inner.right() - countWidth - 12, inner.center().y() - 2.5, 6, 6));
            textRight -= 12;
        }
        painter->setPen(t.text);
        painter->drawText(QRect(inner.left(), inner.top(), textRight - inner.left(), inner.height()),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, textRight - inner.left()));
        painter->restore();
    }
};

/// 每页顶上一行：手写体标题 + 右边搜索框（没候选时不给搜索框）。
QLineEdit *addPageTop(QVBoxLayout *layout, QWidget *page, const QString &title, bool searchable) {
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    auto *label = new QLabel(title, page);
    QFont font = Theme::handFont(label->font(), Theme::HandFont::KaiLight);
    font.setPixelSize(18);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
    label->setFont(font);
    row->addWidget(label, 1);
    QLineEdit *search = nullptr;
    if (searchable) {
        search = new QLineEdit(page);
        search->setPlaceholderText(QObject::tr("搜索预设"));
        search->setFixedWidth(220);
        MainWindowPageLayout::addSearchIcon(search);
        row->addWidget(search);
    }
    layout->addLayout(row);
    return search;
}

/// 页底那一行（设计稿 .src / .parts）：上面一根细线隔开。
QFrame *addPageFoot(QVBoxLayout *layout, QWidget *page, QHBoxLayout **rowOut) {
    auto *foot = new QFrame(page);
    foot->setObjectName(QStringLiteral("presetInstallPageFoot"));
    const auto paint = [foot] {
        foot->setStyleSheet(QStringLiteral("QFrame#presetInstallPageFoot { border: none; border-top: 1px solid %1; }")
                                .arg(css(Theme::tokens().divider)));
    };
    paint();
    Theme::follow(foot, paint);
    auto *row = new QHBoxLayout(foot);
    row->setContentsMargins(0, 10, 0, 0);
    row->setSpacing(10);
    layout->addSpacing(8);
    layout->addWidget(foot);
    *rowOut = row;
    return foot;
}

/// 空态（设计稿 .empty）：图标 + 一句话 + 可选说明 +「管理预设」次按钮，整块居中。
QPushButton *addEmptyState(QVBoxLayout *layout, QWidget *page, int index, const QString &text, const QString &hint) {
    auto *box = new QWidget(page);
    auto *column = new QVBoxLayout(box);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(10);
    column->addStretch(1);
    auto *icon = new QLabel(box);
    const auto paintIcon = [icon] { icon->setPixmap(VersionIcons::line(QStringLiteral("list"), 28, Theme::tokens().disabled)); };
    paintIcon();
    Theme::follow(icon, paintIcon);
    column->addWidget(icon, 0, Qt::AlignHCenter);
    auto *label = new QLabel(text, box);
    label->setObjectName(PresetInstallPicker::emptyLabelName(index));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    column->addWidget(label);
    if (!hint.isEmpty()) {
        auto *note = new QLabel(hint, box);
        note->setAlignment(Qt::AlignCenter);
        note->setWordWrap(true);
        // 带 AlignHCenter 的布局项不走 heightForWidth，换行标签只拿到一行高：定宽后把高度按这个宽算好给死。
        note->setFont(Theme::textFont(note->font(), Theme::TextRole::Caption));
        note->setFixedWidth(300);
        note->setMinimumHeight(note->heightForWidth(300));
        const auto tone = [note] {
            QPalette palette = note->palette();
            palette.setColor(QPalette::WindowText, Theme::tokens().secondary);
            note->setPalette(palette);
        };
        tone();
        Theme::follow(note, tone);
        column->addWidget(note, 0, Qt::AlignHCenter);
    }
    auto *manage = new QPushButton(PresetInstallPicker::manageText(), box);
    manage->setObjectName(PresetInstallPicker::manageButtonName(index));
    Theme::markButton(manage, Theme::ButtonKind::Accent);
    column->addSpacing(4);
    column->addWidget(manage, 0, Qt::AlignHCenter);
    column->addStretch(1);
    layout->addWidget(box, 1);
    return manage;
}

QListWidget *makeChoiceList(QWidget *page) {
    auto *list = new QListWidget(page);
    list->setItemDelegate(new PresetChoiceDelegate(list));
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setMouseTracking(true);
    list->setMinimumHeight(100);
    return list;
}
}  // namespace

PresetInstallPicker::PresetInstallPicker(QWidget *parent) : QWidget(parent) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);
    // 替换确认（设计稿 .pend）：警示淡底一条，左警示图标 + 一句「替换X：A → B」，右「取消」「替换」。
    auto *pendingFrame = new QFrame(this);
    m_pendingBar = pendingFrame;
    m_pendingBar->setObjectName(QStringLiteral("presetReplacementPrompt"));
    auto *pending = new QHBoxLayout(m_pendingBar);
    pending->setContentsMargins(14, 8, 10, 8);
    pending->setSpacing(10);
    auto *warnIcon = new QLabel(m_pendingBar);
    pending->addWidget(warnIcon);
    const auto paintPending = [pendingFrame, warnIcon] {
        const Theme::Tokens t = Theme::tokens();
        pendingFrame->setStyleSheet(QStringLiteral("QFrame#presetReplacementPrompt { background: %1; border-radius: 12px; }")
                                        .arg(css(t.warnSoft)));
        warnIcon->setPixmap(VersionIcons::line(QStringLiteral("warn"), 18, t.warn));
    };
    paintPending();
    Theme::follow(pendingFrame, paintPending);
    m_pendingLabel = new QLabel(m_pendingBar);
    m_pendingLabel->setTextFormat(Qt::PlainText);
    m_pendingLabel->setWordWrap(true);
    pending->addWidget(m_pendingLabel, 1);
    auto *replace = new QPushButton(tr("替换"), m_pendingBar);
    replace->setObjectName(QStringLiteral("presetReplaceConfirm"));
    auto *cancel = new QPushButton(tr("取消"), m_pendingBar);
    cancel->setObjectName(QStringLiteral("presetReplaceCancel"));
    Theme::markButton(replace, Theme::ButtonKind::Primary);
    Theme::markButton(cancel, Theme::ButtonKind::Accent);
    pending->addWidget(cancel); pending->addWidget(replace);
    connect(replace, &QPushButton::clicked, this, [this] {
        auto action = std::move(m_pendingSelection);
        m_pendingBar->hide(); emit pendingChanged(false);
        if (action) action();
    });
    connect(cancel, &QPushButton::clicked, this, [this] {
        m_pendingSelection = {}; m_pendingBar->hide(); emit pendingChanged(false);
    });
    m_pendingBar->hide(); root->addWidget(m_pendingBar);
    auto *body = new QHBoxLayout;
    body->setSpacing(24);
    m_navigation = new QListWidget(this);
    m_navigation->setObjectName(tabBarName());
    m_navigation->setProperty("wtfmlSidebarRail", true);
    m_navigation->setItemDelegate(new PresetNavDelegate(m_navigation));
    m_navigation->setMouseTracking(true);
    m_navigation->setFixedWidth(150);
    m_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    body->addWidget(m_navigation);
    m_stack = new QStackedWidget(this);  // 换页动效由 DesignDefaults 默认接上（pageAppear）
    m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    body->addWidget(m_stack, 1);
    root->addLayout(body, 1);
    // 「本次应用」汇总：装进 InlineMessageBox 后被挪到按钮行左边（见 InlineMessageBox::setContent）。
    m_summaryTitle = new QLabel(this);
    m_summaryTitle->setObjectName(QStringLiteral("presetInstallSummary"));
    m_summaryTitle->setTextFormat(Qt::RichText);
    m_summaryTitle->setProperty("wtfmlPromptActionsLead", true);
    m_summary = m_summaryTitle;
    root->addWidget(m_summary);
    Theme::follow(m_summaryTitle, [this] { updateSummary(); });
    connect(m_navigation, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    setMinimumSize(0, 330);
}

void PresetInstallPicker::setContext(const QString &gameVersion, const QString &loader) {
    // 上下文只供无障碍与检查使用，不重复安装页已有的信息。
    setAccessibleDescription(tr("Minecraft %1 / %2").arg(gameVersion, loader.isEmpty() ? tr("原版") : loader));
    const QString kind = loader.trimmed().toLower();
    m_modsAllowed = kind == QStringLiteral("fabric") || kind == QStringLiteral("forge") ||
        kind == QStringLiteral("neoforge") || kind == QStringLiteral("quilt");
    const int mods = indexOfKey(QStringLiteral("mods"));
    if (mods >= 0 && !m_modsAllowed) selectId(QStringLiteral("mods"), QString());
    if (mods >= 0 && m_pages[mods].list) {
        auto *list = m_pages[mods].list;
        for (int i = 1; i < list->count(); ++i) {
            auto flags = list->item(i)->flags();
            flags.setFlag(Qt::ItemIsEnabled, m_modsAllowed);
            list->item(i)->setFlags(flags);
            if (!m_modsAllowed) list->item(i)->setToolTip(tr("需要模组加载器"));
            else if (list->item(i)->data(kStatusRole).toString() == tr("需要模组加载器")) {
                list->item(i)->setData(kStatusRole, tr("未检查")); list->item(i)->setToolTip(QString());
            }
        }
    }
    updateCompositeChecks();
}

QWidget *PresetInstallPicker::buildPage(int index, const Category &category) {
    auto *page = new QWidget(m_stack);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    if (category.options.isEmpty()) {
        addPageTop(layout, page, category.title, false);
        auto *manage = addEmptyState(layout, page, index, tr("暂无可用的%1预设").arg(category.title), QString());
        connect(manage, &QPushButton::clicked, this, &PresetInstallPicker::manageRequested);
        return page;
    }
    QLineEdit *search = addPageTop(layout, page, category.title, true);
    auto *list = makeChoiceList(page);
    m_pages[index].list = list;
    list->setObjectName(listName(index));
    list->setAccessibleName(category.title);
    auto *none = new QListWidgetItem(noneText(), list);
    none->setData(Qt::UserRole, QString()); none->setData(kCountRole, -1);
    int selected = 0;
    bool hasComposite = false;
    for (const auto &option : category.options) {
        auto *item = new QListWidgetItem(option.name.isEmpty() ? option.id : option.name, list);
        item->setData(Qt::UserRole, option.id);
        item->setData(kCompositeRole, option.composite);
        item->setData(kCountRole, option.itemCount);
        item->setIcon(option.icon);
        item->setData(kTypeRole, option.composite ? tr("综合预设") : category.title + tr("预设"));
        item->setData(kStatusRole, option.available ? tr("未检查") : tr("读取失败"));
        if (category.key == QStringLiteral("mods") && !m_modsAllowed) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setToolTip(tr("需要模组加载器"));
            item->setData(kStatusRole, tr("需要模组加载器"));
        }
        hasComposite |= option.composite;
        if (option.id == category.selectedId) selected = list->count() - 1;
    }
    list->setCurrentRow(selected);
    layout->addWidget(list, 1);
    // 页底：「已选：xx」+「兼容性详情」，右边是这一类自己的「显示综合预设」开关（有综合预设时才有）。
    QHBoxLayout *foot = nullptr;
    QFrame *footFrame = addPageFoot(layout, page, &foot);
    auto *source = new QLabel(footFrame);
    source->setTextFormat(Qt::RichText);
    source->setObjectName(QStringLiteral("presetSource_") + category.key);
    source->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_pages[index].source = source;
    foot->addWidget(source, 1);
    auto *details = new QPushButton(reasonsText(), footFrame);
    details->setObjectName(reasonsButtonName(index));
    Theme::markButton(details, Theme::ButtonKind::Ghost);
    foot->addWidget(details);
    m_pages[index].details = details;
    details->setEnabled(!category.selectedId.isEmpty());
    connect(details, &QPushButton::clicked, this, [this, index] { openDetails(index); });
    QCheckBox *show = nullptr;
    if (hasComposite) {
        foot->addSpacing(8);
        foot->addWidget(new QLabel(tr("显示综合预设"), footFrame));
        show = new QCheckBox(tr("显示综合预设"), footFrame);
        show->setObjectName(QStringLiteral("presetShowComposite%1").arg(index));
        show->setChecked(true);
        Theme::markSwitch(show);
        foot->addWidget(show);
    }
    const auto filter = [list, show, search] {
        for (int i = 1; i < list->count(); ++i) {
            auto *item = list->item(i);
            const bool excluded = (show != nullptr && !show->isChecked() && item->data(kCompositeRole).toBool()) ||
                !item->text().contains(search->text().trimmed(), Qt::CaseInsensitive);
            item->setHidden(excluded);
        }
    };
    if (show != nullptr) connect(show, &QCheckBox::toggled, list, [filter](bool) { filter(); });
    connect(search, &QLineEdit::textChanged, list, [filter](const QString &) { filter(); });
    connect(list, &QListWidget::currentRowChanged, this, [this, index, list](int row) {
        if (row < 0) return;
        const QString id = list->item(row)->data(Qt::UserRole).toString();
        const QString old = m_categories[index].selectedId;
        // 先回到已经提交的选择，确认替换后才改变最终来源。
        { const QSignalBlocker blocker(list);
          for (int i = 0; i < list->count(); ++i)
              if (list->item(i)->data(Qt::UserRole).toString() == old) list->setCurrentRow(i); }
        requestSelection(index, id);
    });
    connect(list, &QListWidget::itemClicked, this, [this, index](QListWidgetItem *item) {
        if (item->data(Qt::UserRole).toString() == m_categories[index].selectedId)
            m_userTouched.insert(m_categories[index].key);
    });
    return page;
}

void PresetInstallPicker::setCategories(const QList<Category> &categories) {
    LoadingState::finish(m_stack);
    m_pendingSelection = {}; m_pendingBar->hide(); emit pendingChanged(false);
    m_compositePage = nullptr; m_compositeList = nullptr; m_compositeChecks.clear();
    m_selectedComposite.clear(); m_composites.clear();
    while (m_stack->count()) { QWidget *page = m_stack->widget(0); m_stack->removeWidget(page); delete page; }
    { const QSignalBlocker blocker(m_navigation); m_navigation->clear(); }
    m_categories = categories;
    if (!m_modsAllowed) for (auto &category : m_categories)
        if (category.key == QStringLiteral("mods")) category.selectedId.clear();
    m_pages.clear(); m_userTouched.clear();
    m_pages.resize(m_categories.size());
    for (int i = 0; i < m_categories.size(); ++i) {
        const auto &category = m_categories[i];
        auto *nav = new QListWidgetItem(category.title, m_navigation);
        nav->setData(kCountRole, int(category.options.size()));
        m_pages[i].nav = nav;
        m_stack->addWidget(buildPage(i, category));
        setStatus(category.key, category.status, category.reasons, category.problem);
    }
    if (!m_categories.isEmpty()) m_navigation->setCurrentRow(0);
    updateSummary();
}

void PresetInstallPicker::setChecking(const QString &key, bool checking) {
    const int i = indexOfKey(key);
    if (i < 0 || !m_pages[i].list) return;
    QListWidget *list = m_pages[i].list;
    QListWidgetItem *current = checking ? list->currentItem() : nullptr;
    for (int n = 0; n < list->count(); ++n)
        if (list->item(n) != current) LoadingState::finishRowStatus(list, list->model()->index(n, 0));
    if (current != nullptr) LoadingState::beginRowStatus(list, list->model()->index(list->row(current), 0));
}

void PresetInstallPicker::beginLoading() { LoadingState::begin(m_stack, tr("正在读取预设…")); }
void PresetInstallPicker::failLoading(const QString &reason, std::function<void()> retry) {
    LoadingState::fail(m_stack, reason, std::move(retry));
}

int PresetInstallPicker::indexOfKey(const QString &key) const {
    for (int i = 0; i < m_categories.size(); ++i) if (m_categories[i].key == key) return i;
    return -1;
}
QString PresetInstallPicker::selectedId(const QString &key) const {
    const int i = indexOfKey(key); return i < 0 ? QString() : m_categories[i].selectedId;
}
QString PresetInstallPicker::nameFor(const QString &key, const QString &id) const {
    if (id.isEmpty()) return tr("未选择");
    const int i = indexOfKey(key);
    if (i >= 0) for (const auto &option : m_categories[i].options) if (option.id == id) return option.name;
    return id;
}
void PresetInstallPicker::confirmReplacement(const QString &text, std::function<void()> action) {
    m_pendingLabel->setText(text); m_pendingSelection = std::move(action);
    m_pendingBar->show(); emit pendingChanged(true);
}
void PresetInstallPicker::requestSelection(int index, const QString &id) {
    const auto &category = m_categories[index];
    if (category.key == QStringLiteral("mods") && !m_modsAllowed && !id.isEmpty()) return;
    if (!id.isEmpty() && !category.selectedId.isEmpty() && id != category.selectedId) {
        const QString key = category.key;
        confirmReplacement(tr("替换%1：%2 → %3").arg(category.title, nameFor(key, category.selectedId), nameFor(key, id)),
                           [this, key, id] { selectId(key, id); });
    } else selectId(category.key, id);
}
void PresetInstallPicker::selectId(const QString &key, const QString &id) {
    const int i = indexOfKey(key);
    if (key == QStringLiteral("mods") && !m_modsAllowed && !id.isEmpty()) return;
    if (i < 0 || !m_pages[i].list) return;
    QListWidget *list = m_pages[i].list;
    int row = -1;
    for (int n = 0; n < list->count(); ++n) if (list->item(n)->data(Qt::UserRole).toString() == id) { row = n; break; }
    if (row < 0) return;
    m_userTouched.insert(key);
    { const QSignalBlocker blocker(list); list->setCurrentRow(row); }
    const bool changed = m_categories[i].selectedId != id;
    m_categories[i].selectedId = id;
    if (changed) {
        m_pages[i].missing = m_pages[i].unknown = -1;
        setStatus(key, id.isEmpty() ? QString() : tr("未检查"), {}, false);
        emit selectionChanged(key, id);
    }
    m_pages[i].details->setEnabled(!id.isEmpty());
    updateSummary(); updateCompositeChecks();
}
void PresetInstallPicker::setStatus(const QString &key, const QString &status, const QStringList &reasons,
                                  bool problem, int itemCount, int missing, int unknown) {
    const int i = indexOfKey(key); if (i < 0) return;
    Page &page = m_pages[i]; page.status = status; page.reasons = reasons;
    if (missing >= 0) page.missing = missing;
    if (unknown >= 0) page.unknown = unknown;
    if (page.list) {
        // 换了一行：上一行行尾那句检查中的点跟着撤（检查归当前选着的那一份）。
        for (int n = 0; n < page.list->count(); ++n)
            if (page.list->item(n) != page.list->currentItem())
                LoadingState::finishRowStatus(page.list, page.list->model()->index(n, 0));
    }
    if (page.list && page.list->currentItem()) {
        auto *item = page.list->currentItem();
        item->setData(kStatusRole, status);
        item->setData(kProblemRole, problem);
        if (itemCount >= 0) item->setData(kCountRole, itemCount);
        item->setData(Qt::AccessibleDescriptionRole, status);
        item->setToolTip(reasons.join(QLatin1Char('\n')));
    }
    updateSummary(); if (m_detailsIndex == i) updateDetails();
}
void PresetInstallPicker::updateSummary() {
    const Theme::Tokens t = Theme::tokens();
    const QString ink = css(t.text), muted = css(t.secondary), faint = css(t.disabled);
    const QString warn = css(Theme::textTone(t.warnInk, t.warn));
    int missing = 0, unknown = 0;
    for (int i = 0; i < m_categories.size(); ++i) {
        const auto &category = m_categories[i];
        const bool chosen = !category.selectedId.isEmpty();
        if (m_pages[i].nav) m_pages[i].nav->setData(kChosenRole, chosen);
        if (m_pages[i].source) {
            const QString name = nameFor(category.key, category.selectedId).toHtmlEscaped();
            m_pages[i].source->setText(QStringLiteral("<span style=\"color:%1\">%2</span><span style=\"color:%3\">%4</span>")
                .arg(muted, tr("已选："), chosen ? ink : muted, name));
            m_pages[i].source->setToolTip(tr("%1：%2").arg(category.title, nameFor(category.key, category.selectedId)));
        }
        if (chosen) { missing += qMax(0, m_pages[i].missing); unknown += qMax(0, m_pages[i].unknown); }
    }
    // 只写真实数据给得出的几段：缺失 / 待确认来自模组兼容检查，没查过就不写。
    const QString sep = QStringLiteral("<span style=\"color:%1\">&nbsp;·&nbsp;</span>").arg(faint);
    QString text = selectedCount() > 0
        ? tr("本次应用 <span style=\"color:%1; font-weight:500\">%2</span> 类").arg(ink).arg(selectedCount())
        : tr("本次不应用预设");
    if (missing) text += sep + QStringLiteral("<span style=\"color:%1\">%2</span>").arg(warn, tr("缺失 %1 项").arg(missing));
    if (unknown) text += sep + tr("待确认 %1 项").arg(unknown);
    m_summaryTitle->setText(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(muted, text));
    m_navigation->viewport()->update();
}
QList<QPair<QString, QString>> PresetInstallPicker::selections() const {
    QList<QPair<QString, QString>> out;
    for (const auto &category : m_categories)
        if (!category.selectedId.isEmpty() || m_userTouched.contains(category.key)) out.append({category.key, category.selectedId});
    return out;
}
int PresetInstallPicker::selectedCount() const {
    int count = 0; for (const auto &category : m_categories) if (!category.selectedId.isEmpty()) ++count; return count;
}
int PresetInstallPicker::currentTab() const { return m_navigation->currentRow(); }
void PresetInstallPicker::setCurrentTab(int index) { m_navigation->setCurrentRow(index); }
void PresetInstallPicker::openDetails(int index) {
    if (index < 0 || index >= m_categories.size() || m_categories[index].selectedId.isEmpty()) return;
    if (m_details) m_details->close();
    auto *dialog = new ChromeDialog(window());
    dialog->setObjectName(QStringLiteral("presetCompatibilityDetails"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(reasonsText()); dialog->resize(600, 400);
    auto *layout = new QVBoxLayout(dialog->body());
    auto *text = new QPlainTextEdit(dialog); text->setReadOnly(true);
    layout->addWidget(text);
    auto *buttons = new QDialogButtonBox(dialog);
    buttons->addButton(tr("关闭"), QDialogButtonBox::RejectRole);  // 不用标准 Close：理由见 SchematicInfoDialog
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    m_details = dialog; m_detailsText = text; m_detailsIndex = index; updateDetails(); dialog->show();
}
void PresetInstallPicker::updateDetails() {
    if (!m_details || m_detailsIndex < 0 || m_detailsIndex >= m_pages.size()) return;
    const auto &category = m_categories[m_detailsIndex];
    const auto &page = m_pages[m_detailsIndex];
    m_detailsText->setPlainText(nameFor(category.key, category.selectedId) + QStringLiteral("\n\n") +
        page.status + QStringLiteral("\n\n") + page.reasons.join(QLatin1Char('\n')));
}

void PresetInstallPicker::setComposites(const QList<Composite> &presets) {
    if (m_compositePage) {
        const int index = m_stack->indexOf(m_compositePage);
        m_stack->removeWidget(m_compositePage); delete m_compositePage;
        delete m_navigation->takeItem(index);
    }
    m_composites = presets; m_compositeChecks.clear(); m_compositeList = nullptr;
    auto *page = new QWidget(m_stack); m_compositePage = page;
    page->setObjectName(QStringLiteral("presetCompositePicker"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    if (presets.isEmpty()) {
        // 空态不留一块空列表：图标 + 一句话 + 说明 +「管理预设」。
        addPageTop(layout, page, tr("综合预设"), false);
        auto *manage = addEmptyState(layout, page, m_categories.size(), tr("暂无可用的综合预设"),
            tr("综合预设将多类预设合并，一次安装完成。可在「预设管理」中新建，或从实例提取。"));
        connect(manage, &QPushButton::clicked, this, &PresetInstallPicker::manageRequested);
    } else {
        QLineEdit *search = addPageTop(layout, page, tr("综合预设"), true);
        auto *list = makeChoiceList(page); m_compositeList = list;
        list->setObjectName(QStringLiteral("presetCompositeList"));
        for (const auto &preset : presets) {
            auto *item = new QListWidgetItem(preset.name, list); item->setData(Qt::UserRole, preset.id);
            item->setData(kCountRole, preset.itemCount); item->setIcon(preset.icon);
            // 第二行写这一份装哪几类、各几项（设计稿「模组 24 · 配置 6」）。
            QStringList parts;
            for (const auto &category : std::as_const(m_categories)) {
                if (!preset.parts.contains(category.key)) continue;
                parts.append(preset.counts.contains(category.key)
                    ? QStringLiteral("%1 %2").arg(category.title).arg(preset.counts.value(category.key)) : category.title);
            }
            item->setData(kTypeRole, parts.isEmpty() ? tr("综合预设") : parts.join(QStringLiteral(" · ")));
        }
        layout->addWidget(list, 1);
        connect(search, &QLineEdit::textChanged, list, [list](const QString &text) {
            for (int i = 0; i < list->count(); ++i)
                list->item(i)->setHidden(!list->item(i)->text().contains(text.trimmed(), Qt::CaseInsensitive));
        });
        // 页底「应用其中」：每类一颗可勾的 chip（这一份不含的那类灰着）。
        QHBoxLayout *foot = nullptr;
        QFrame *footFrame = addPageFoot(layout, page, &foot);
        foot->setSpacing(8);
        auto *chips = new FlowLayout(8, 8);  // 左栏随译文变宽后可能一行排不下
        foot->addLayout(chips, 1);
        chips->addWidget(new QLabel(tr("应用其中："), footFrame));
        for (const auto &category : std::as_const(m_categories)) {
            auto *chip = new QPushButton(category.title, footFrame);
            chip->setObjectName(QStringLiteral("presetCompositePart_") + category.key);
            chip->setCheckable(true);
            Theme::markChipButton(chip);
            chip->setEnabled(false);
            chips->addWidget(chip);
            m_compositeChecks.insert(category.key, chip);
            connect(chip, &QPushButton::toggled, page, [this, key = category.key](bool on) {
                for (const auto &preset : m_composites) if (preset.id == m_selectedComposite) {
                    const QString id = preset.parts.value(key); const int i = indexOfKey(key);
                    if (i >= 0 && !id.isEmpty()) {
                        if (on) requestSelection(i, id);
                        else if (selectedId(key) == id) selectId(key, QString());
                    }
                    updateCompositeChecks(); break;
                }
            });
        }
        connect(list, &QListWidget::currentRowChanged, page, [this, list](int row) {
            if (row < 0) return;
            const Composite preset = m_composites[row];
            const auto apply = [this, preset] {
                m_selectedComposite = preset.id;
                { const QSignalBlocker blocker(m_compositeList);
                  for (int i = 0; i < m_compositeList->count(); ++i)
                      if (m_compositeList->item(i)->data(Qt::UserRole).toString() == preset.id) m_compositeList->setCurrentRow(i); }
                for (auto it = preset.parts.cbegin(); it != preset.parts.cend(); ++it) selectId(it.key(), it.value());
                updateCompositeChecks();
            };
            QStringList replacements;
            for (auto it = preset.parts.cbegin(); it != preset.parts.cend(); ++it) {
                const int i = indexOfKey(it.key());
                if (i >= 0 && !selectedId(it.key()).isEmpty() && selectedId(it.key()) != it.value()) replacements.append(m_categories[i].title);
            }
            if (replacements.isEmpty()) apply();
            else {
                { const QSignalBlocker blocker(list); list->clearSelection(); list->setCurrentRow(-1); }
        confirmReplacement(tr("替换%1，使用「%2」").arg(replacements.join(tr("、")), preset.name), apply);
            }
        });
    }
    m_stack->addWidget(page);
    auto *nav = new QListWidgetItem(tr("综合预设"), m_navigation);
    nav->setData(kCountRole, int(presets.size()));
    // 左栏 150 起，译文更长时放宽：行里除了名字还有两侧 12 边距、圆点与计数（共 56）。
    int railWidth = 150;
    const QFontMetrics railMetrics(Theme::textFont(m_navigation->font(), Theme::TextRole::Body));
    for (int i = 0; i < m_navigation->count(); ++i)
        railWidth = qMax(railWidth, railMetrics.horizontalAdvance(m_navigation->item(i)->text()) + 56);
    m_navigation->setFixedWidth(railWidth);
    updateCompositeChecks();
}
void PresetInstallPicker::updateCompositeChecks() {
    QMap<QString, QString> parts;
    QMap<QString, int> counts;
    for (const auto &preset : m_composites) if (preset.id == m_selectedComposite) { parts = preset.parts; counts = preset.counts; }
    for (auto it = m_compositeChecks.cbegin(); it != m_compositeChecks.cend(); ++it) {
        const QSignalBlocker blocker(it.value()); const QString id = parts.value(it.key());
        const int index = indexOfKey(it.key());
        if (index >= 0) it.value()->setText(m_categories[index].title +
            (counts.contains(it.key()) ? QStringLiteral(" %1").arg(counts.value(it.key())) : QString()));
        const bool allowed = it.key() != QStringLiteral("mods") || m_modsAllowed;
        it.value()->setEnabled(!id.isEmpty() && allowed);
        it.value()->setToolTip(allowed ? QString() : tr("需要模组加载器"));
        it.value()->setChecked(!id.isEmpty() && selectedId(it.key()) == id);
    }
}
