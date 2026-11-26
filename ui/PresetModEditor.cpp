#include "PresetModEditor.hpp"
#include <QCoreApplication>
#include "InputSizing.hpp"
#include "FlowLayout.hpp"
#include "Theme.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QSet>
#include <QRegularExpression>
#include <QLineEdit>
#include <QUrl>

namespace {
bool normalizeProject(QString *platform, QString *id) {
    *id = id->trimmed();
    if (!id->contains(QLatin1String("://"))) return true;
    const QUrl url(*id);
    const QString host = url.host().toLower();
    const auto parts = url.path().split('/', Qt::SkipEmptyParts);
    if ((url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http")) &&
        (host == QStringLiteral("modrinth.com") || host == QStringLiteral("www.modrinth.com")) &&
        parts.size() >= 2 && parts.first() == QStringLiteral("mod")) {
        *platform = QStringLiteral("modrinth");
        *id = parts.at(1);
        return true;
    }
    // CurseForge's public slug is not a stable numeric project ID. Never guess it.
    return false;
}
}

PresetModEditor::PresetModEditor(const QList<Entry> &entries, QWidget *parent) : QDialog(parent) {
    setWindowTitle(QCoreApplication::translate("PresetModEditor", "编辑模组预设"));
    resize(760, 440);
    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(QCoreApplication::translate("PresetModEditor", "版本和加载器留空时跟随实例；标为必要的模组失败会中止安装。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *quickRow = new QHBoxLayout;
    auto *link = new QLineEdit(this);
    link->setObjectName(QStringLiteral("presetModQuickAdd"));
    InputSizing::longValue(link, "Modrinth 链接：网址长度不定");  // i18n-ignore
    link->setPlaceholderText(QCoreApplication::translate("PresetModEditor", "Modrinth 链接或项目 ID，例如 https://modrinth.com/mod/sodium"));
    link->setClearButtonEnabled(true);
    auto *quickAdd = new QPushButton(QCoreApplication::translate("PresetModEditor", "添加"), this);
    quickAdd->setObjectName(QStringLiteral("presetModQuickAddButton"));
    quickAdd->setAutoDefault(false);
    Theme::markButton(quickAdd, Theme::ButtonKind::Primary);
    quickRow->addWidget(link, 1); quickRow->addWidget(quickAdd);
    layout->addLayout(quickRow);
    m_table = new QTableWidget(0, 6, this);
    m_table->setObjectName(QStringLiteral("presetModEditorTable"));
    m_table->setHorizontalHeaderLabels({
        QCoreApplication::translate("PresetModEditor", "平台"),
        QCoreApplication::translate("PresetModEditor", "项目 ID"),
        QCoreApplication::translate("PresetModEditor", "显示名称"),
        QCoreApplication::translate("PresetModEditor", "必要"),
        QCoreApplication::translate("PresetModEditor", "限定游戏版本"),
        QCoreApplication::translate("PresetModEditor", "限定加载器"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(false);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);  // 末列收余宽，英文不出横滚条
    m_table->verticalHeader()->hide();  // 行号没有用处（顺序不影响安装），和预设选项表一样不显示
    layout->addWidget(m_table, 1);
    for (const auto &entry : entries) append(entry);
    auto *actions = new FlowLayout(6, 6);  // 嵌在预设编辑器里时右栏窄，要能折行
    auto *add = new QPushButton(QCoreApplication::translate("PresetModEditor", "添加模组"), this);
    auto *remove = new QPushButton(QCoreApplication::translate("PresetModEditor", "移除选中"), this);
    add->setAutoDefault(false); remove->setAutoDefault(false);
    Theme::markButton(add, Theme::ButtonKind::Neutral);
    Theme::markButton(remove, Theme::ButtonKind::Neutral);
    // 提取实例时两个平台都没认出来的那些行：平台那格停在「未知来源（请选择）」。这两件
    // 是给人工收拾它们用的 —— 先把它们挑出来看，再逐条补或者整批删掉。
    auto *onlyUnknown = new QCheckBox(QCoreApplication::translate("PresetModEditor", "仅显示未识别项"), this);
    onlyUnknown->setObjectName(QStringLiteral("presetModOnlyUnknown"));
    onlyUnknown->setToolTip(QCoreApplication::translate("PresetModEditor", "仅显示平台为「未知来源（请选择）」的条目。取消筛选可显示全部条目。"));
    m_onlyUnknown = onlyUnknown;
    auto *removeUnknown = new QPushButton(QCoreApplication::translate("PresetModEditor", "移除未识别项"), this);
    removeUnknown->setObjectName(QStringLiteral("presetModRemoveUnknown"));
    removeUnknown->setAutoDefault(false);
    removeUnknown->setToolTip(QCoreApplication::translate("PresetModEditor", "移除平台为「未知来源（请选择）」的所有条目。选择「取消」可撤回本次编辑。"));
    Theme::markButton(removeUnknown, Theme::ButtonKind::Neutral);
    actions->addWidget(add); actions->addWidget(remove); actions->addStretch();
    actions->addWidget(onlyUnknown); actions->addWidget(removeUnknown);
    layout->addLayout(actions);
    applyUnknownFilter();
    connect(onlyUnknown, &QCheckBox::toggled, this, [this] { applyUnknownFilter(); });
    connect(removeUnknown, &QPushButton::clicked, this, [this] {
        int removed = 0;
        for (int row = m_table->rowCount() - 1; row >= 0; --row) {
            if (!unrecognizedRow(row)) continue;
            m_table->removeRow(row);
            ++removed;
        }
        applyUnknownFilter();
        m_problem->setText(removed > 0
                               ? QCoreApplication::translate("PresetModEditor", "已移除 %1 条来源未知的记录。选择「取消」可撤回本次编辑。")
                                     .arg(removed)
                               : QCoreApplication::translate("PresetModEditor", "无未识别的模组。"));
    });
    connect(add, &QPushButton::clicked, this, [this] {
        append({QStringLiteral("modrinth"), {}, {}, {}, {}, true});
        applyUnknownFilter();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        for (int row = m_table->rowCount() - 1; row >= 0; --row)
            if (m_table->selectionModel()->isRowSelected(row, QModelIndex())) m_table->removeRow(row);
        applyUnknownFilter();
    });
    m_problem = new QLabel(this); m_problem->setObjectName(QStringLiteral("presetModProblem")); m_problem->setWordWrap(true); layout->addWidget(m_problem);
    const auto addLink = [this, link] {
        QString platform = QStringLiteral("modrinth"), id = link->text().trimmed();
        if (!normalizeProject(&platform, &id) ||
            !QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]+$")).match(id).hasMatch()) {
            m_problem->setText(QCoreApplication::translate("PresetModEditor", "请输入 Modrinth 链接或项目 ID；CurseForge 只能填数字 ID。"));
            link->setFocus(); return;
        }
        for (int row = 0; row < m_table->rowCount(); ++row) {
            const auto *source = qobject_cast<QComboBox *>(m_table->cellWidget(row, 0));
            if (source->currentData().toString() == platform && m_table->item(row, 1)->text().trimmed() == id) {
                // 这一行可能正被「只看没认出来的」藏着 —— 先放开筛选，才真能定位到它。
                if (m_onlyUnknown->isChecked()) m_onlyUnknown->setChecked(false);
                m_table->selectRow(row); m_table->scrollToItem(m_table->item(row, 1));
                m_problem->setText(QCoreApplication::translate("PresetModEditor", "此模组已在清单中，已定位到该行。")); return;
            }
        }
        append({platform, id, id, {}, {}, true});
        applyUnknownFilter();
        m_table->selectRow(m_table->rowCount() - 1);
        m_table->scrollToBottom(); link->clear(); link->setFocus(); m_problem->clear();
    };
    connect(quickAdd, &QPushButton::clicked, this, addLink);
    connect(link, &QLineEdit::returnPressed, this, addLink);
    // 不用标准钮（QDialogButtonBox::Save/Cancel/…）：它的字在 LanguageChange 时会被 Qt 刷回自带的英文，
    // 装没装 Qt 中文翻译都一样不稳。自己 addButton(字, 角色)，accepted/rejected 照常发。
    auto *buttons = new QDialogButtonBox(this);
    QPushButton *saveButton = buttons->addButton(
        QCoreApplication::translate("PresetModEditor", "保存"), QDialogButtonBox::AcceptRole);
    QPushButton *cancelButton = buttons->addButton(
        QCoreApplication::translate("PresetModEditor", "取消"), QDialogButtonBox::RejectRole);
    // 「保存」是这个框里唯一的主动作：回车 = 保存（列表上那几颗仍是 autoDefault(false)）。
    saveButton->setAutoDefault(false);
    saveButton->setDefault(true);
    cancelButton->setAutoDefault(false);
    Theme::markButton(saveButton, Theme::ButtonKind::Primary);
    Theme::markButton(cancelButton, Theme::ButtonKind::Neutral);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QSet<QString> seen;
        int row = 0;
        for (const auto &entry : this->entries()) {
            const QString key = entry.platform + QLatin1Char(':') + entry.projectId;
            const auto pattern = entry.platform == QStringLiteral("curseforge") ? QStringLiteral("^[0-9]+$") : QStringLiteral("^[A-Za-z0-9_-]+$");
            if ((entry.platform != QStringLiteral("modrinth") && entry.platform != QStringLiteral("curseforge")) ||
                !QRegularExpression(pattern).match(entry.projectId).hasMatch() || seen.contains(key)) {
                m_problem->setText(QCoreApplication::translate("PresetModEditor", "第 %1 行：项目 ID 为空、格式错误或重复。").arg(row + 1));
                m_table->setCurrentCell(row, 1); m_table->scrollToItem(m_table->item(row, 1)); return;
            }
            seen.insert(key);
            ++row;
        }
        accept();
    });
}
void PresetModEditor::append(const Entry &entry) {
    const int row = m_table->rowCount(); m_table->insertRow(row);
    auto *platform = new QComboBox(m_table);
    platform->addItem(QStringLiteral("Modrinth"), QStringLiteral("modrinth"));
    platform->addItem(QStringLiteral("CurseForge"), QStringLiteral("curseforge"));
    int index = platform->findData(entry.platform);
    if (index < 0) {
        platform->addItem(QCoreApplication::translate("PresetModEditor", "未知来源（请选择）"), entry.platform);
        index = platform->count() - 1;
    }
    platform->setCurrentIndex(index);
    m_table->setCellWidget(row, 0, platform);
    for (const auto &pair : {qMakePair(1, entry.projectId), qMakePair(2, entry.name), qMakePair(4, entry.gameVersion), qMakePair(5, entry.loader)})
        m_table->setItem(row, pair.first, new QTableWidgetItem(pair.second));
    auto *required = new QTableWidgetItem;
    required->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    required->setCheckState(entry.required ? Qt::Checked : Qt::Unchecked);
    m_table->setItem(row, 3, required);
}
QList<PresetModEditor::Entry> PresetModEditor::entries() const {
    QList<Entry> result;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto text = [&](int column) { return m_table->item(row, column)->text().trimmed(); };
        auto *platform = qobject_cast<QComboBox *>(m_table->cellWidget(row, 0));
        QString source = platform->currentData().toString(), id = text(1);
        normalizeProject(&source, &id);
        result.append({source, id, text(2), text(4), text(5).toLower(), m_table->item(row, 3)->checkState() == Qt::Checked});
    }
    return result;
}
bool PresetModEditor::unrecognizedRow(int row) const {
    const auto *source = qobject_cast<QComboBox *>(m_table->cellWidget(row, 0));
    const QString platform = source ? source->currentData().toString() : QString();
    // 「未知来源（请选择）」那一项的数据就是原来那串认不出的平台（多半是空串），
    // 所以不是这两家就是没认出来 —— 别拿"下拉框里有没有这一项"判。
    return platform != QStringLiteral("modrinth") && platform != QStringLiteral("curseforge");
}
void PresetModEditor::applyUnknownFilter() {
    const bool only = m_onlyUnknown && m_onlyUnknown->isChecked();
    for (int row = 0; row < m_table->rowCount(); ++row)
        m_table->setRowHidden(row, only && !unrecognizedRow(row));
}

