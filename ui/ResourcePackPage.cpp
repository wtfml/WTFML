#include "ResourcePackPage.hpp"
#include "Theme.hpp"
#include <QAbstractItemModel>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

ResourcePackPage::ResourcePackPage(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *top = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("resourcePackSearch"));
    m_search->setPlaceholderText(tr("搜索可用资源包名称或描述"));
    m_search->setClearButtonEnabled(true);
    top->addWidget(m_search, 1);
    auto *refresh = new QPushButton(tr("刷新"), this);
    refresh->setObjectName(QStringLiteral("resourcePackRefresh"));
    Theme::markButton(refresh, Theme::ButtonKind::Neutral);
    top->addWidget(refresh);
    layout->addLayout(top);
    // 上下两段同在一个滚动区里：两张列表随内容撑高、不自己滚，整页只有这一条竖滑条（#136）。
    m_scroll = new Theme::GutterScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("resourcePackScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget(m_scroll);
    auto *sections = new QVBoxLayout(body);
    sections->setContentsMargins(0, 0, 0, 0);
    const auto addSection = [this, body, sections](const QString &title, const QString &name, QHBoxLayout *&header) {
        header = new QHBoxLayout;
        auto *label = new QLabel(title, body);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        header->addWidget(label, 1);
        sections->addLayout(header);
        auto *list = new QListWidget(body);
        list->setObjectName(name);
        list->setFrameShape(QFrame::NoFrame);
        list->setSpacing(2);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        sections->addWidget(list);
        // 键盘换行时把当前那一行滚进外层视野（列表自己不滚）。
        connect(list, &QListWidget::currentItemChanged, this, [this, list](QListWidgetItem *item) {
            if (item == nullptr || item->isHidden()) return;
            const QRect rect = list->visualItemRect(item);
            const QPoint center = list->viewport()->mapTo(m_scroll->widget(), rect.center());
            m_scroll->ensureVisible(center.x(), center.y(), 0, rect.height() / 2 + list->spacing());
        });
        return list;
    };
    QHBoxLayout *selectedHeader = nullptr;
    QHBoxLayout *availableHeader = nullptr;
    m_selected = addSection(tr("已启用（优先级从高到低）"), QStringLiteral("resourcePackSelected"), selectedHeader);
    sections->addSpacing(8);
    m_available = addSection(tr("可用资源包"), QStringLiteral("resourcePackAvailable"), availableHeader);
    sections->addStretch(1);
    m_scroll->setWidget(body);
    m_selected->setDragDropMode(QAbstractItemView::InternalMove);
    m_selected->setDefaultDropAction(Qt::MoveAction);
    m_selected->setDragDropOverwriteMode(false);
    layout->addWidget(m_scroll, 1);
    const auto button = [this](QHBoxLayout *row, const QString &text, const QString &name) {
        auto *value = new QPushButton(text, this);
        value->setObjectName(name);
        Theme::markButton(value, Theme::ButtonKind::Neutral);
        row->addWidget(value);
        return value;
    };
    m_up = button(selectedHeader, tr("提高优先级"), QStringLiteral("resourcePackUp"));
    m_down = button(selectedHeader, tr("降低优先级"), QStringLiteral("resourcePackDown"));
    m_disable = button(selectedHeader, tr("停用"), QStringLiteral("resourcePackDisable"));
    m_enable = button(availableHeader, tr("启用"), QStringLiteral("resourcePackEnable"));
    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("resourcePackNote"));
    m_note->setWordWrap(true);
    layout->addWidget(m_note);
    connect(refresh, &QPushButton::clicked, this, &ResourcePackPage::refreshRequested);
    connect(m_search, &QLineEdit::textChanged, this, [this] { filter(); });
    connect(m_available, &QListWidget::itemSelectionChanged, this, &ResourcePackPage::updateButtons);
    connect(m_selected, &QListWidget::itemSelectionChanged, this, &ResourcePackPage::updateButtons);
    // 回车 / 双击可用的一项 = 启用它。
    connect(m_available, &QListWidget::itemActivated, m_enable, &QPushButton::click);
    connect(m_enable, &QPushButton::clicked, this, [this] {
        if (!m_editable || !m_available->currentItem()) return;
        m_loading = true;
        auto *item = m_available->takeItem(m_available->currentRow());
        item->setHidden(false);
        m_selected->insertItem(0, item);
        m_selected->setCurrentItem(item);
        m_loading = false;
        submit();
    });
    connect(m_disable, &QPushButton::clicked, this, [this] {
        auto *item = m_selected->currentItem();
        if (!m_editable || !item || !item->data(Qt::UserRole).toString().startsWith(QStringLiteral("file/"))) return;
        m_loading = true;
        item = m_selected->takeItem(m_selected->currentRow());
        // 缺失的文件可从已启用名单移除，但不应成为可启用的文件。
        if (item->data(Qt::UserRole + 1).toBool()) m_available->addItem(item);
        else delete item;
        m_loading = false;
        submit();
    });
    connect(m_up, &QPushButton::clicked, this, [this] { moveCurrent(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { moveCurrent(1); });
    connect(m_selected->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        if (m_loading || m_pending) return;
        m_pending = true;
        // 等 Qt 完成 drop 后再提交，避免在 model 正在移动行时清空重建它。
        QTimer::singleShot(0, this, [this] { m_pending = false; submit(); });
    });
    updateButtons();
}

void ResourcePackPage::setPacks(const QList<Pack> &packs, const QStringList &selected,
                              const QByteArray &options, bool exists, const QString &problem) {
    const QString focused = m_selected->currentItem() ? m_selected->currentItem()->data(Qt::UserRole).toString() : QString();
    m_loading = true;
    m_expected = options;
    m_expectedExists = exists;
    m_editable = problem.isEmpty();
    m_available->clear();
    m_selected->clear();
    const QSet<QString> enabled(selected.cbegin(), selected.cend());
    const auto append = [](QListWidget *list, const QString &id, const QString &title,
                           const QString &detail, bool present) {
        auto *item = new QListWidgetItem(title, list);
        item->setSizeHint(QSize(0, 36));
        item->setData(Qt::UserRole, id);
        item->setData(Qt::UserRole + 1, present);
        item->setToolTip(detail);
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
        if (!id.startsWith(QStringLiteral("file/"))) item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
        return item;
    };
    for (const auto &pack : packs)
        if (!enabled.contains(pack.id)) append(m_available, pack.id, pack.title, pack.detail, true);
    for (auto it = selected.crbegin(); it != selected.crend(); ++it) {
        const auto found = std::find_if(packs.cbegin(), packs.cend(), [&it](const Pack &pack) { return pack.id == *it; });
        auto *item = found == packs.cend()
            ? append(m_selected, *it, it->startsWith(QStringLiteral("file/")) ? it->mid(5) + tr("（文件缺失）") : *it + tr("（内置 / 模组提供）"), *it, false)
            : append(m_selected, *it, found->title, found->detail, true);
        if (*it == focused) m_selected->setCurrentItem(item);
    }
    m_selected->setDragEnabled(m_editable);
    m_selected->setAcceptDrops(m_editable);
    m_note->setText(problem.isEmpty()
        ? tr("自动保存生效，不修改源文件。内置项不可移除。")
        : tr("只读：%1").arg(problem));
    m_loading = false;
    filter();
    updateButtons();
}
QStringList ResourcePackPage::selectedIds() const {
    QStringList ids;
    for (int row = m_selected->count() - 1; row >= 0; --row) ids.append(m_selected->item(row)->data(Qt::UserRole).toString());
    return ids;
}
void ResourcePackPage::submit() {
    if (!m_loading && m_editable) emit selectionRequested(selectedIds(), m_expected, m_expectedExists);
    updateButtons();
    filter();
}
void ResourcePackPage::moveCurrent(int delta) {
    const int row = m_selected->currentRow();
    if (!m_editable || row < 0 || row + delta < 0 || row + delta >= m_selected->count()) return;
    if (!m_selected->item(row)->data(Qt::UserRole).toString().startsWith(QStringLiteral("file/"))) return;
    m_loading = true;
    auto *item = m_selected->takeItem(row);
    m_selected->insertItem(row + delta, item);
    m_selected->setCurrentItem(item);
    m_loading = false;
    submit();
}
void ResourcePackPage::updateButtons() {
    if (m_loading) return;
    auto *available = m_available->currentItem();
    auto *selected = m_selected->currentItem();
    const bool local = selected && selected->data(Qt::UserRole).toString().startsWith(QStringLiteral("file/"));
    m_enable->setEnabled(m_editable && available && !available->isHidden());
    m_disable->setEnabled(m_editable && local);
    m_up->setEnabled(m_editable && local && m_selected->currentRow() > 0);
    m_down->setEnabled(m_editable && local && m_selected->currentRow() + 1 < m_selected->count());
}
void ResourcePackPage::filter() {
    const QStringList terms = m_search->text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int row = 0; row < m_available->count(); ++row) {
        auto *item = m_available->item(row);
        const QString haystack = item->text() + QLatin1Char(' ') + item->toolTip();
        item->setHidden(!std::all_of(terms.cbegin(), terms.cend(), [&haystack](const QString &term) {
            return haystack.contains(term, Qt::CaseInsensitive);
        }));
    }
    fitLists();
    updateButtons();
}
void ResourcePackPage::fitLists() {
    // 列表高度按排好版的最后一行现算（滑条交给外层）；空的一段留一行高，免得标题下面贴着下一段。
    for (QListWidget *list : {m_selected, m_available}) {
        list->ensurePolished();  // 还没显示时样式表的字体还没落上，量出来的行高偏矮
        list->doItemsLayout();
        list->scrollToTop();
        const int spacing = list->spacing();
        int bottom = 0;
        for (int row = 0; row < list->count(); ++row)
            if (!list->item(row)->isHidden()) bottom = qMax(bottom, list->visualItemRect(list->item(row)).bottom() + 1);
        if (bottom == 0) bottom = (list->sizeHintForRow(0) > 0 ? list->sizeHintForRow(0) : list->fontMetrics().height() * 2) + spacing;
        list->setFixedHeight(bottom + spacing + 2 * list->frameWidth());
    }
}
