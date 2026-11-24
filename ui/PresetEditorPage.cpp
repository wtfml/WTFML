#include "PresetEditorPage.hpp"
#include <QCoreApplication>
#include "InputSizing.hpp"
#include "InlineMessage.hpp"
#include "OverlayHost.hpp"
#include "PresetModEditor.hpp"
#include "PresetOptionsEditor.hpp"
#include "Theme.hpp"
#include "VersionTreeDelegate.hpp"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

namespace {
const QStringList parts{"gameSettings", "mods", "resourcePacks", "dataPacks", "shaderPacks"};
QPushButton *button(const QString &text, QWidget *parent, QVBoxLayout *layout,
                    Theme::ButtonKind kind = Theme::ButtonKind::Neutral) {
    auto *b = new QPushButton(text, parent);
    Theme::markButton(b, kind);
    b->setAutoDefault(false);
    layout->addWidget(b);
    return b;
}
QString partFor(const QJsonObject &section) {
    const auto kind = section.value("kind").toString();
    if (kind == "vanillaOptions" || kind == "vanillaKeys" || kind == "serverList")
        return "gameSettings";
    const auto items = section.value("items").toArray();
    if (!items.isEmpty()) {
        const auto path = items.first().toObject().value("path").toString();
        if (path.startsWith("resourcepacks/")) return "resourcePacks";
        if (path.startsWith("shaderpacks/")) return "shaderPacks";
        if (path.contains("/datapacks/"))
            return "dataPacks";
    }
    return {};
}
void embed(QDialog *dialog) {
    dialog->setWindowFlags(Qt::Widget);
    dialog->setMinimumSize(0, 0);
    for (auto *table : dialog->findChildren<QTableWidget *>())
        table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (auto *box : dialog->findChildren<QDialogButtonBox *>())
        box->hide();
    // Explanatory text belongs in tooltips; the editor shows only actionable content.
    for (auto *label : dialog->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly))
        if (!label->text().isEmpty())
            label->hide();
}
} // namespace
PresetEditorPage::PresetEditorPage(const QJsonObject &document, QWidget *parent) : QDialog(parent) {
    setObjectName("presetEditorPage");
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(16);
    auto *left = new QWidget(this);
    left->setFixedWidth(204);
    auto *side = new QVBoxLayout(left);
    side->setContentsMargins(0, 0, 0, 0);
    // 左上角那颗「返回」**不摆了**：这一页由覆盖层整块铺在内容区上，返回箭头在页眉上
    // （和实例设置一模一样）。`backRequested` 那条路仍然通 —— 走 reject()。
    m_navigation = new QListWidget(left);
    m_navigation->setObjectName("presetEditorNavigation");
    m_navigation->setFrameShape(QFrame::NoFrame);
    m_navigation->setProperty("wtfmlNav", "sidebar");
    m_navigation->setItemDelegate(new VersionTreeDelegate(m_navigation, false, 0, true));
    m_navigation->setMouseTracking(true);
    m_navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navigation->setFont(Theme::textFont(m_navigation->font(), Theme::TextRole::Body));
    m_navigation->addItems(
        {
            QCoreApplication::translate("PresetEditorPage", "概览"),
            QCoreApplication::translate("PresetEditorPage", "游戏选项"),
            QCoreApplication::translate("PresetEditorPage", "按键"),
            QCoreApplication::translate("PresetEditorPage", "模组"),
            QCoreApplication::translate("PresetEditorPage", "资源包"),
            QCoreApplication::translate("PresetEditorPage", "数据包"),
        });
    side->addWidget(m_navigation, 1);
    auto *draft = button(QCoreApplication::translate("PresetEditorPage", "保存为草稿"), left, side);
    draft->setObjectName("presetEditorSaveDraft");
    auto *save = button(QCoreApplication::translate("PresetEditorPage", "保存为预设"), left, side, Theme::ButtonKind::Primary);
    save->setObjectName("presetEditorPublish");
    auto *restore = button(QCoreApplication::translate("PresetEditorPage", "恢复到上次"), left, side);
    restore->setObjectName("presetEditorRestore");
    auto *clear = button(QCoreApplication::translate("PresetEditorPage", "全部清空"), left, side, Theme::ButtonKind::Danger);
    clear->setObjectName("presetEditorClear");
    connect(draft, &QPushButton::clicked, this, [this] {
        if (saveRequested)
            saveRequested(true);
    });
    connect(save, &QPushButton::clicked, this, [this] {
        if (saveRequested)
            saveRequested(false);
    });
    connect(restore, &QPushButton::clicked, this, [this] {
        if (!isDirty() || (confirmReset && confirmReset(QCoreApplication::translate("PresetEditorPage", "丢弃未保存的修改？"))))
            setDocument(m_original);
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        if ((!confirmReset || !confirmReset(QCoreApplication::translate("PresetEditorPage", "清空各部分内容？名称和图标保留。"))))
            return;
        auto state = this->document();
        state["sections"] = QJsonArray();
        state["modReferences"] = QJsonArray();
        setDocument(state);
    });
    outer->addWidget(left);
    auto *right = new QVBoxLayout;
    m_pages = new QStackedWidget(this);
    right->addWidget(m_pages, 1);
    m_problem = new QLabel(this);
    m_problem->setObjectName("presetEditorProblem");
    m_problem->setWordWrap(true);
    right->addWidget(m_problem);
    outer->addLayout(right, 1);
    connect(m_navigation, &QListWidget::currentRowChanged, m_pages,
            &QStackedWidget::setCurrentIndex);
    setDocument(document);
    m_original = this->document();
}
void PresetEditorPage::reject() {
    // 一个字都不关：要不要保存是控制器的决定（见 .hpp 那段）。控制器收页走 accept()。
    if (backRequested)
        backRequested();
}

void PresetEditorPage::setDocument(const QJsonObject &document) {
    const int page = qMax(0, m_navigation->currentRow());
    rebuildPages(document);
    m_navigation->setCurrentRow(page);
    m_pages->setCurrentIndex(page);
    showProblem({});
}
void PresetEditorPage::rebuildPages(const QJsonObject &state) {
    while (m_pages->count())
        delete m_pages->widget(0);
    m_enabled.clear();
    m_packs.clear();
    m_base = state;
    auto *overview = new QWidget(m_pages);
    auto *layout = new QVBoxLayout(overview);
    layout->addWidget(new QLabel(QCoreApplication::translate("PresetEditorPage", "名称"), overview));
    m_name = new QLineEdit(state.value("name").toString(), overview);
    m_name->setObjectName("presetEditorName");
    InputSizing::shortValue(m_name, InputSizing::kNameChars, InputSizing::Script::Han);
    layout->addWidget(m_name);
    layout->addWidget(new QLabel(QCoreApplication::translate("PresetEditorPage", "图标"), overview));
    auto *iconRow = new QHBoxLayout;
    m_icon = new QLineEdit(state.value("icon").toString(), overview);
    m_icon->setReadOnly(true);
    m_icon->setObjectName("presetEditorIcon");
    InputSizing::longValue(m_icon, "只读：图标文件路径，旁边是「选择图片」");  // i18n-ignore
    iconRow->addWidget(m_icon, 1);
    auto *choose = new QPushButton(QCoreApplication::translate("PresetEditorPage", "选择图片"), overview);
    Theme::markButton(choose, Theme::ButtonKind::Neutral);
    iconRow->addWidget(choose);
    layout->addLayout(iconRow);
    connect(choose, &QPushButton::clicked, this, [this] {
        auto path =
            QFileDialog::getOpenFileName(this, QCoreApplication::translate("PresetEditorPage", "选择图标"), {}, QCoreApplication::translate("PresetEditorPage", "图片 (*.png *.jpg *.webp)"));
        if (!path.isEmpty())
            m_icon->setText(path);
    });
    layout->addWidget(new QLabel(QCoreApplication::translate("PresetEditorPage", "启用部分"), overview));
    const auto enabled = state.value("enabledParts").toObject();
    const QStringList titles{
        QCoreApplication::translate("PresetEditorPage", "游戏设置"),
        QCoreApplication::translate("PresetEditorPage", "模组"),
        QCoreApplication::translate("PresetEditorPage", "资源包"),
        QCoreApplication::translate("PresetEditorPage", "数据包"),
        QCoreApplication::translate("PresetEditorPage", "光影包"),
    };
    for (int i = 0; i < parts.size(); ++i) {
        auto *check = new QCheckBox(titles[i], overview);
        check->setObjectName("presetEditorEnabled_" + parts[i]);
        const bool legacyShaderDisabled = parts[i] == QStringLiteral("shaderPacks") &&
            !enabled.contains(parts[i]) && enabled.contains(QStringLiteral("resourcePacks")) &&
            !enabled.value(QStringLiteral("resourcePacks")).toBool();
        check->setChecked(!legacyShaderDisabled && (!enabled.contains(parts[i]) || enabled.value(parts[i]).toBool()));
        m_enabled[parts[i]] = check;
        layout->addWidget(check);
    }
    layout->addStretch();
    m_pages->addWidget(overview);
    QList<PresetOptionsEditor::Entry> options, keys;
    for (const auto &value : state.value("sections").toArray()) {
        auto section = value.toObject();
        const auto kind = section.value("kind").toString();
        if (kind != "vanillaOptions" && kind != "vanillaKeys")
            continue;
        for (const auto &value : section.value("items").toArray()) {
            auto item = value.toObject();
            (kind == "vanillaKeys" ? keys : options)
                .append({item.value("key").toString(), item.value("value").toString()});
        }
    }
    m_options = new PresetOptionsEditor(options, m_pages, PresetOptionsEditor::Mode::Options);
    embed(m_options);
    m_pages->addWidget(m_options);
    m_keys = new PresetOptionsEditor(keys, m_pages, PresetOptionsEditor::Mode::Keys);
    embed(m_keys);
    m_pages->addWidget(m_keys);
    QList<PresetModEditor::Entry> refs;
    for (const auto &v : state.value("modReferences").toArray()) {
        auto r = v.toObject();
        refs.append({r.value("platform").toString(), r.value("projectId").toString(),
                     r.value("name").toString(), r.value("gameVersion").toString(),
                     r.value("loader").toString(), r.value("required").toBool(true)});
    }
    m_mods = new PresetModEditor(refs, m_pages);
    embed(m_mods);
    m_pages->addWidget(m_mods);
    for (const auto &part : QStringList{"resourcePacks", "dataPacks", "shaderPacks"}) {
        auto *page = new QWidget(m_pages);
        auto *layout = new QVBoxLayout(page);
        auto *table = new QTableWidget(0, 2, page);
        m_packs[part] = table;
        table->setObjectName("presetEditorFiles_" + part);
        table->setHorizontalHeaderLabels({QCoreApplication::translate("PresetEditorPage", "文件"), QCoreApplication::translate("PresetEditorPage", "启用")});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        layout->addWidget(table, 1);
        QList<QJsonObject> fileItems;
        for (const auto &value : state.value("sections").toArray()) {
            const auto section = value.toObject();
            if (partFor(section) == part)
                for (const auto &v : section.value("items").toArray())
                    fileItems.append(v.toObject());
        }
        if (part == "resourcePacks")
            std::stable_sort(fileItems.begin(), fileItems.end(),
                             [](const QJsonObject &a, const QJsonObject &b) {
                                 return a.value("priority").toInt() > b.value("priority").toInt();
                             });
        if (part != "resourcePacks") table->hideColumn(1);
        table->horizontalHeaderItem(0)->setToolTip(QCoreApplication::translate("PresetEditorPage", "资源包列表上方优先。"));
        for (const auto &item : fileItems) {
            const int row = table->rowCount();
            table->insertRow(row);
            auto *name = new QTableWidgetItem(item.value("path").toString());
            name->setData(Qt::UserRole, item);
            name->setFlags(name->flags() & ~Qt::ItemIsEditable);
            table->setItem(row, 0, name);
            auto *on = new QTableWidgetItem;
            on->setCheckState(!item.contains("enabled")
                                  ? Qt::PartiallyChecked
                                  : (item.value("enabled").toBool() ? Qt::Checked : Qt::Unchecked));
            if (item.value("path").toString().startsWith("shaderpacks/"))
                on->setFlags(Qt::NoItemFlags);
            table->setItem(row, 1, on);
        }
        auto *actions = new QHBoxLayout;
        auto add = [&](const QString &label, auto action) {
            auto *b = new QPushButton(label, page);
            Theme::markButton(b, Theme::ButtonKind::Neutral);
            actions->addWidget(b);
            connect(b, &QPushButton::clicked, this, action);
        };
        add(QCoreApplication::translate("PresetEditorPage", "添加"), [this, part] { addPack(part); });
        add(QCoreApplication::translate("PresetEditorPage", "移除"), [table] {
            for (int i = table->rowCount() - 1; i >= 0; --i)
                if (table->selectionModel()->isRowSelected(i, {}))
                    table->removeRow(i);
        });
        for (int delta : {-1, 1})
            add(delta < 0 ? QCoreApplication::translate("PresetEditorPage", "上移") : QCoreApplication::translate("PresetEditorPage", "下移"), [table, delta] {
                int row = table->currentRow(), next = row + delta;
                if (row < 0 || next < 0 || next >= table->rowCount())
                    return;
                for (int c = 0; c < 2; ++c) {
                    auto *a = table->takeItem(row, c), *b = table->takeItem(next, c);
                    table->setItem(row, c, b);
                    table->setItem(next, c, a);
                }
                table->selectRow(next);
            });
        actions->addStretch();
        layout->addLayout(actions);
        m_pages->addWidget(page);
    }
}
void PresetEditorPage::addPack(const QString &part) {
    QString world;
    if (part == "dataPacks") {
        bool ok = false;
        // 本页是覆盖页：问名字用同一层里的提示卡，不另开带系统标题栏的 QInputDialog。
        world = InlineMessage::getText(
            OverlayHost::forWidget(this), QCoreApplication::translate("PresetEditorPage", "目标存档"),
            QCoreApplication::translate("PresetEditorPage", "存档文件夹名称"),
                                       QString(), &ok)
                    .trimmed();
        if (!ok || world.isEmpty() || world.contains('/') || world.contains('\\') ||
            world == ".." || world == ".")
            return;
    }
    const auto paths =
        QFileDialog::getOpenFileNames(this, QCoreApplication::translate("PresetEditorPage", "添加文件"), {}, QCoreApplication::translate("PresetEditorPage", "压缩包 (*.zip)"));
    auto *table = m_packs.value(part);
    for (const auto &path : paths) {
        const int row = table->rowCount();
        table->insertRow(row);
        QJsonObject item{{"path", (part == "resourcePacks" ? QStringLiteral("resourcepacks/")
                                   : part == "shaderPacks" ? QStringLiteral("shaderpacks/")
                                                           : QStringLiteral("saves/") + world +
                                                                 QStringLiteral("/datapacks/")) +
                                      QFileInfo(path).fileName()},
                         {"_source", path},
                         {"copyOnly", true}};
        if (part == "dataPacks") {
            item["world"] = world;
            item["name"] = QFileInfo(path).fileName();
        }
        auto *name = new QTableWidgetItem(QFileInfo(path).fileName());
        name->setData(Qt::UserRole, item);
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 0, name);
        auto *on = new QTableWidgetItem;
        on->setCheckState(Qt::Checked);
        table->setItem(row, 1, on);
    }
}
QJsonObject PresetEditorPage::document() const {
    auto state = m_base;
    state["name"] = m_name->text().trimmed();
    state["icon"] = m_icon->text();
    QJsonObject enabled;
    for (const auto &part : parts)
        enabled[part] = m_enabled[part]->isChecked();
    state["enabledParts"] = enabled;
    QJsonArray sections;
    // Preserve supported server-list sections and unsupported legacy content without silently
    // applying it.
    for (const auto &v : m_base.value("sections").toArray()) {
        auto s = v.toObject();
        const auto kind = s.value("kind").toString();
        if (kind != "vanillaOptions" && kind != "vanillaKeys" && partFor(s) != "resourcePacks" &&
            partFor(s) != "dataPacks" && partFor(s) != "shaderPacks")
            sections.append(s);
    }
    auto options = [&](PresetOptionsEditor *editor, const QString &kind) {
        QJsonObject section{{"id", kind}, {"kind", kind}, {"mode", "merge"}};
        QHash<QString, QJsonObject> originals;
        bool found = false;
        for (const auto &v : m_base.value("sections").toArray()) {
            const auto original = v.toObject();
            if (original.value("kind").toString() != kind)
                continue;
            if (!found) {
                section = original;
                found = true;
            }
            for (const auto &item : original.value("items").toArray())
                originals[item.toObject().value("key").toString()] = item.toObject();
        }
        QJsonArray items;
        for (const auto &e : editor->entries()) {
            auto item = originals.value(e.key);
            item["key"] = e.key;
            item["value"] = e.value;
            items.append(item);
        }
        if (!items.isEmpty()) {
            section["items"] = items;
            sections.append(section);
        }
    };
    options(m_options, "vanillaOptions");
    options(m_keys, "vanillaKeys");
    for (const auto &part : QStringList{"resourcePacks", "dataPacks", "shaderPacks"}) {
        QMap<QString, QJsonArray> groups;
        auto *table = m_packs[part];
        for (int i = 0; i < table->rowCount(); ++i) {
            auto item = table->item(i, 0)->data(Qt::UserRole).toJsonObject();
            if (part == "resourcePacks" &&
                item.value("path").toString().startsWith("resourcepacks/") &&
                table->item(i, 1)->checkState() != Qt::PartiallyChecked) {
                item["enabled"] = table->item(i, 1)->checkState() == Qt::Checked;
                item["priority"] = table->rowCount() - 1 - i;
            }
            groups[item.value("path").toString().section('/', 0, 0)].append(item);
        }
        for (auto it = groups.begin(); it != groups.end(); ++it) {
            QJsonObject section{{"id", part},
                                {"kind", part == "dataPacks" ? "custom" : "genericFiles"},
                                {"mode", "copyIfMissing"}};
            for (const auto &v : m_base.value("sections").toArray()) {
                const auto original = v.toObject();
                const auto originalItems = original.value("items").toArray();
                if (partFor(original) == part && !originalItems.isEmpty() &&
                    originalItems.first().toObject().value("path").toString().section('/', 0, 0) ==
                        it.key()) {
                    section = original;
                    break;
                }
            }
            section["items"] = it.value();
            sections.append(section);
        }
    }
    state["sections"] = sections;
    QJsonArray refs;
    for (const auto &e : m_mods->entries())
        refs.append(QJsonObject{{"platform", e.platform},
                                {"projectId", e.projectId},
                                {"name", e.name},
                                {"gameVersion", e.gameVersion},
                                {"loader", e.loader},
                                {"required", e.required}});
    state["modReferences"] = refs;
    return state;
}
bool PresetEditorPage::isDirty() const { return document() != m_original; }
void PresetEditorPage::markSaved(const QJsonObject &document) {
    setDocument(document);
    m_original = this->document();
}
void PresetEditorPage::showProblem(const QString &text) {
    m_problem->setText(text);
    m_problem->setVisible(!text.isEmpty());
}
