#include "PresetOptionsEditor.hpp"
#include <QCoreApplication>
#include "DesignDefaults.hpp"
#include "Theme.hpp"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QSet>
#include <QSlider>
#include <QStyledItemDelegate>

#include "PresetOptionCatalog.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QApplication>
#include <QLocale>
#include <functional>
#include <utility>

namespace {
using namespace PresetOptionCatalog;
constexpr int kFiltered = Qt::UserRole + 1;

// 原版 Java 1.21.1 的选项槽位。空槽对应预设目录中没有的游戏选项。
using Pair = std::pair<const char *, const char *>;
const auto videoRows = {
    Pair{"graphicsMode", "renderDistance"}, Pair{nullptr, "simulationDistance"}, Pair{nullptr, "maxFps"},
    Pair{"enableVsync", "bobView"}, Pair{"guiScale", nullptr}, Pair{"gamma", "renderClouds"},
    Pair{"fullscreen", "particles"}, Pair{nullptr, "entityShadows"},
};
const auto mouseRows = {Pair{"mouseSensitivity", "invertYMouse"}, Pair{nullptr, "rawMouseInput"}};
const auto controlRows = {Pair{"toggleCrouch", "toggleSprint"}, Pair{"autoJump", nullptr}};
const auto accessibilityRows = {
    Pair{nullptr, "showSubtitles"}, Pair{"highContrast", nullptr},
    Pair{"screenEffectScale", "fovEffectScale"}, Pair{"darknessEffectScale", nullptr},
    Pair{"hideLightningFlash", nullptr},
};
const auto chatRows = {
    Pair{"chatVisibility", "chatColors"}, Pair{"chatLinks", "chatLinksPrompt"},
    Pair{"chatOpacity", nullptr}, Pair{"chatScale", nullptr},
};
const auto soundRows = {
    Pair{"soundCategory_master", nullptr}, Pair{"soundCategory_music", "soundCategory_record"},
    Pair{"soundCategory_weather", "soundCategory_block"}, Pair{"soundCategory_hostile", "soundCategory_neutral"},
    Pair{"soundCategory_player", "soundCategory_ambient"}, Pair{"soundCategory_voice", nullptr},
};

class OptionSlider final : public QWidget {
public:
    OptionSlider(const Option &spec, QWidget *parent) : QWidget(parent), m_spec(spec) {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 0, 8, 0);
        m_slider = new QSlider(Qt::Horizontal, this);
        m_slider->setAccessibleName(spec.name);
        const int factor = spec.kind == Kind::Fraction ? 1000 : 1;
        m_slider->setRange(qRound(spec.minimum * factor), qRound(spec.maximum * factor));
        m_label = new QLabel(this);
        m_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(m_slider, 1);
        layout->addWidget(m_label);
        setFocusProxy(m_slider);
        connect(m_slider, &QSlider::valueChanged, this, [this] { updateLabel(); });
    }

    void load(const QString &value) {
        const double number = value.toDouble();
        m_slider->setValue(qRound(m_spec.key == "fov" ? 70 + 40 * number
            : m_spec.kind == Kind::Fraction ? 1000 * number : number));
        updateLabel();
    }

    QString value() const {
        const int position = m_slider->value();
        if (m_spec.key == "fov") return QString::number((position - 70) / 40.0);
        if (m_spec.kind == Kind::Fraction) return QString::number(position / 1000.0);
        return QString::number(position);
    }

    QSlider *slider() const { return m_slider; }

private:
    void updateLabel() {
        const int position = m_slider->value();
        QString text = QString::number(position);
        if (m_spec.kind == Kind::Fraction) text = QLocale().toString(position / 10.0, 'f', position % 10 == 0 ? 0 : 1) + '%';
        else if (m_spec.key == "fov") text += QStringLiteral("°");
        else if (m_spec.key == "maxFps") text += QStringLiteral(" FPS");
        else if (m_spec.key.endsWith("Distance")) text += QCoreApplication::translate("OptionValueDelegate", " 区块");
        m_label->setText(text);
    }

    const Option &m_spec;
    QSlider *m_slider;
    QLabel *m_label;
};

QString keyLabel(QString value) {
    const QHash<QString, QString> names{
        {"key.keyboard.space", QCoreApplication::translate("PresetOptionsEditor", "空格")},
        {"key.keyboard.left.shift", QCoreApplication::translate("PresetOptionsEditor", "左 Shift")},
        {"key.keyboard.left.control", QCoreApplication::translate("PresetOptionsEditor", "左 Ctrl")},
        {"key.keyboard.left.alt", QCoreApplication::translate("PresetOptionsEditor", "左 Alt")},
        {"key.keyboard.tab", "Tab"},
        {"key.mouse.left", QCoreApplication::translate("PresetOptionsEditor", "鼠标左键")},
        {"key.mouse.right", QCoreApplication::translate("PresetOptionsEditor", "鼠标右键")},
        {"key.mouse.middle", QCoreApplication::translate("PresetOptionsEditor", "鼠标中键")},
        {"key.keyboard.unknown", QCoreApplication::translate("PresetOptionsEditor", "未绑定")},
    };
    if (names.contains(value)) return names.value(value);
    if (value.startsWith("key.keyboard.")) return value.mid(13).toUpper();
    return value;
}
class KeyCapture : public QPushButton {
public:
    KeyCapture(QWidget *parent) : QPushButton(parent) {
        setAutoDefault(false);
        Theme::markButton(this, Theme::ButtonKind::Neutral);
        setToolTip(QCoreApplication::translate("KeyCapture", "点击后按下键盘或鼠标按键；Esc 取消，退格清除"));
        connect(this, &QPushButton::clicked, this, [this] {
            armed = true; setProperty("wtfmlKeyCaptureArmed", true); setText(QCoreApplication::translate("KeyCapture", "请按键…")); setFocus();
        });
    }
    void setValue(const QString &value) { current = value; setText(keyLabel(value)); }
    std::function<void(const QString &)> changed;
protected:
    void focusOutEvent(QFocusEvent *event) override {
        armed = false; setProperty("wtfmlKeyCaptureArmed", false); setText(keyLabel(current)); QPushButton::focusOutEvent(event);
    }
    bool event(QEvent *event) override {
        if (armed && event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
        if (armed && event->type() == QEvent::KeyPress) {
            keyPressEvent(static_cast<QKeyEvent *>(event)); return true;
        }
        return QPushButton::event(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (!armed) { QPushButton::keyPressEvent(event); return; }
        if (event->isAutoRepeat()) return;
        if (event->key() == Qt::Key_Escape) { armed = false; setProperty("wtfmlKeyCaptureArmed", false); setText(keyLabel(current)); return; }
        QString suffix;
        const int key = event->key();
        if (key >= Qt::Key_A && key <= Qt::Key_Z) suffix = QChar('a' + key - Qt::Key_A);
        else if (key >= Qt::Key_0 && key <= Qt::Key_9) suffix = QChar('0' + key - Qt::Key_0);
        else if (key >= Qt::Key_F1 && key <= Qt::Key_F25) suffix = "f" + QString::number(key - Qt::Key_F1 + 1);
        else {
            const QHash<int, QString> names{{Qt::Key_Space,"space"},{Qt::Key_Shift,"left.shift"},
                {Qt::Key_Control,"left.control"},{Qt::Key_Alt,"left.alt"},{Qt::Key_Meta,"left.win"},
                {Qt::Key_Tab,"tab"},{Qt::Key_Return,"enter"},{Qt::Key_Enter,"keypad.enter"},
                {Qt::Key_Up,"up"},{Qt::Key_Down,"down"},{Qt::Key_Left,"left"},{Qt::Key_Right,"right"},
                {Qt::Key_Insert,"insert"},{Qt::Key_Delete,"delete"},{Qt::Key_Home,"home"},{Qt::Key_End,"end"},
                {Qt::Key_PageUp,"page.up"},{Qt::Key_PageDown,"page.down"},{Qt::Key_Minus,"minus"},
                {Qt::Key_Equal,"equal"},{Qt::Key_BracketLeft,"left.bracket"},{Qt::Key_BracketRight,"right.bracket"},
                {Qt::Key_Semicolon,"semicolon"},{Qt::Key_Apostrophe,"apostrophe"},{Qt::Key_Comma,"comma"},
                {Qt::Key_Period,"period"},{Qt::Key_Slash,"slash"},{Qt::Key_Backslash,"backslash"},
                {Qt::Key_QuoteLeft,"grave.accent"},{Qt::Key_CapsLock,"caps.lock"}};
            suffix = names.value(key);
        }
        // Qt 合并了左右修饰键，Windows 的虚拟键仍可用于区分。
        if (event->nativeVirtualKey() == 0xA1) suffix = "right.shift";
        if (event->nativeVirtualKey() == 0xA3) suffix = "right.control";
        if (event->nativeVirtualKey() == 0xA5) suffix = "right.alt";
        if ((event->modifiers() & Qt::KeypadModifier) && key >= Qt::Key_0 && key <= Qt::Key_9)
            suffix = "keypad." + suffix;
        if (key == Qt::Key_Backspace) commit("key.keyboard.unknown");
        else if (!suffix.isEmpty()) commit("key.keyboard." + suffix);
        event->accept();
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (!armed) { QPushButton::mousePressEvent(event); return; }
        const QHash<Qt::MouseButton, QString> names{{Qt::LeftButton,"left"}, {Qt::RightButton,"right"},
            {Qt::MiddleButton,"middle"}, {Qt::BackButton,"4"}, {Qt::ForwardButton,"5"}};
        if (names.contains(event->button())) { commit("key.mouse." + names.value(event->button())); suppressRelease = true; }
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (suppressRelease) { suppressRelease = false; event->accept(); return; }
        QPushButton::mouseReleaseEvent(event);
    }
private:
    void commit(const QString &value) {
        armed = false; setProperty("wtfmlKeyCaptureArmed", false); setValue(value);
        if (changed) changed(value);
    }
    QString current;
    bool armed = false, suppressRelease = false;
};
// 从默认行代理派生：格里的字和表头从同一条竖线起，悬停 / 选中也是那块内缩圆角底。
class OptionValueDelegate : public DesignDefaults::RowDelegate {
public:
    using DesignDefaults::RowDelegate::RowDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if (!index.data(kFiltered).toBool()) DesignDefaults::RowDelegate::paint(painter, option, index);
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override {
        return !index.data(kFiltered).toBool() && DesignDefaults::RowDelegate::editorEvent(event, model, option, index);
    }
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto *spec = PresetOptionCatalog::find(index.siblingAtColumn(index.column() - 1).data().toString());
        if (index.column() % 2 == 1 && spec) {
            if (spec->kind == Kind::Key) return new KeyCapture(parent);
            if (spec->kind == Kind::Boolean || spec->kind == Kind::Choice || spec->key == "guiScale") {
                auto *combo = new QComboBox(parent);
                if (spec->kind == Kind::Boolean) {
                    combo->addItem(QCoreApplication::translate("OptionValueDelegate", "关闭"), "false");
                    combo->addItem(QCoreApplication::translate("OptionValueDelegate", "开启"), "true");
                } else if (spec->key == "guiScale") {
                    combo->addItem(QCoreApplication::translate("OptionValueDelegate", "自动"), "0");
                    for (int scale = 1; scale <= spec->maximum; ++scale)
                        combo->addItem(QString::number(scale), QString::number(scale));
                } else {
                    for (const auto &choice : spec->choices) {
                        combo->addItem(choice.label, QString::fromLatin1(choice.value));
                    }
                }
                return combo;
            }
            return new OptionSlider(*spec, parent);
        }
        return QStyledItemDelegate::createEditor(parent, option, index);
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override {
        const QSignalBlocker blocker(editor);
        if (auto *key = dynamic_cast<KeyCapture *>(editor)) { key->setValue(index.data().toString()); return; }
        if (auto *slider = dynamic_cast<OptionSlider *>(editor)) { slider->load(index.data().toString()); return; }
        if (auto *combo = qobject_cast<QComboBox *>(editor)) {
            int selected = combo->findData(index.data().toString());
            if (selected < 0) { combo->addItem(index.data().toString(), index.data().toString()); selected = combo->count()-1; }
            combo->setCurrentIndex(selected); return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }
    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override {
        if (auto *slider = dynamic_cast<OptionSlider *>(editor)) { model->setData(index, slider->value()); return; }
        if (auto *combo = qobject_cast<QComboBox *>(editor)) { model->setData(index, combo->currentData().toString()); return; }
        if (dynamic_cast<KeyCapture *>(editor)) return;
        QStyledItemDelegate::setModelData(editor, model, index);
    }
protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.column() % 2 == 1) {
            // 右栏的输入控件负责显示设置值。透明底下只画行底，原始值留在模型里供搜索和保存。
            option->text.clear();
            option->features.setFlag(QStyleOptionViewItem::HasDisplay, false);
        } else if (const auto *spec = PresetOptionCatalog::find(index.data().toString())) {
            option->text = spec->name;
        }
    }
};
}

PresetOptionsEditor::PresetOptionsEditor(const QList<Entry> &entries, QWidget *parent, Mode mode)
    : QDialog(parent), m_mode(mode) {
    setWindowTitle(QCoreApplication::translate("PresetOptionsEditor", "编辑游戏设置预设")); resize(600, 440);
    auto *layout = new QVBoxLayout(this);
    auto *search = new QLineEdit(this);
    search->setObjectName("presetOptionsSearch");
    search->setPlaceholderText(mode == Mode::Keys
        ? QCoreApplication::translate("PresetOptionsEditor", "搜索操作或按键")
        : QCoreApplication::translate("PresetOptionsEditor", "搜索设置"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    const bool paired = mode == Mode::Options;
    m_table = new QTableWidget(0, paired ? 4 : 2, this);
    m_table->setObjectName(QStringLiteral("presetOptionsEditorTable"));
    m_table->setHorizontalHeaderLabels({
        QCoreApplication::translate("PresetOptionsEditor", "纳入预设"),
        mode == Mode::Keys ? QCoreApplication::translate("PresetOptionsEditor", "按键")
                           : QCoreApplication::translate("PresetOptionsEditor", "设置值"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->horizontalHeader()->setVisible(!paired);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(48);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setItemDelegate(new OptionValueDelegate(m_table));
    layout->addWidget(m_table, 1);
    QHash<QString, int> remaining;
    for (int index = 0; index < entries.size(); ++index) {
        if (!remaining.contains(entries.at(index).key)) remaining.insert(entries.at(index).key, index);
    }
    QList<QTableWidgetItem *> existing(entries.size(), nullptr);
    const auto newRow = [this] {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        return row;
    };
    const auto addGroup = [this, &newRow](const QString &label) {
        const int row = newRow();
        auto *heading = new QTableWidgetItem(label);
        QFont font = m_table->font();
        font.setBold(true);
        heading->setFont(font);
        heading->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(row, 0, heading);
        m_table->setSpan(row, 0, 1, m_table->columnCount());
        m_groups.append(heading);
    };
    QSet<QString> placed;
    const auto place = [&](const Option &spec, int row, int column) {
        const auto index = remaining.find(spec.key);
        if (index != remaining.end()) {
            existing[*index] = append(entries.at(*index), row, column);
            remaining.erase(index);
        } else {
            m_options.append(append({spec.key, spec.initial}, row, column, false));
        }
        if (paired && spec.key == "soundCategory_master") m_table->setSpan(row, 1, 1, 3);
        placed.insert(spec.key);
    };
    const auto addPairs = [&](const auto &rows) {
        for (const auto &[left, right] : rows) {
            const int row = newRow();
            if (const auto *spec = left ? PresetOptionCatalog::find(QString::fromLatin1(left)) : nullptr)
                place(*spec, row, 0);
            if (const auto *spec = right ? PresetOptionCatalog::find(QString::fromLatin1(right)) : nullptr)
                place(*spec, row, 2);
        }
    };
    if (paired) {
        place(*PresetOptionCatalog::find(QStringLiteral("fov")), newRow(), 0);
        addGroup(PresetOptionCatalog::find(QStringLiteral("graphicsMode"))->group);
        addPairs(videoRows);
        addGroup(PresetOptionCatalog::find(QStringLiteral("autoJump"))->group);
        addPairs(controlRows);
        addPairs(mouseRows);
        addGroup(PresetOptionCatalog::find(QStringLiteral("highContrast"))->group);
        addPairs(accessibilityRows);
        addGroup(PresetOptionCatalog::find(QStringLiteral("chatVisibility"))->group);
        addPairs(chatRows);
        addGroup(PresetOptionCatalog::find(QStringLiteral("soundCategory_master"))->group);
        addPairs(soundRows);
    }
    const int groupedEnd = m_table->rowCount();
    for (int index = 0; index < entries.size(); ++index) {
        if (existing.at(index)) continue;
        const auto &entry = entries.at(index);
        existing[index] = append(entry, newRow(), 0);
        remaining.remove(entry.key);
        placed.insert(entry.key);
    }
    for (const auto &spec : PresetOptionCatalog::all()) {
        if (placed.contains(spec.key) || (mode == Mode::Keys && spec.kind != Kind::Key) ||
            (mode == Mode::Options && spec.kind == Kind::Key)) continue;
        place(spec, newRow(), 0);
    }
    m_options = existing + m_options;
    connect(search, &QLineEdit::textChanged, this, [this, groupedEnd](const QString &text) {
        const QString query = text.trimmed();
        QSet<int> matches;
        for (auto *item : std::as_const(m_options)) {
            auto *value = m_table->item(item->row(), item->column() + 1);
            const auto *spec = PresetOptionCatalog::find(item->text());
            const QString haystack = item->text() + " " + (spec ? spec->name + " " + spec->group : QString())
                + " " + value->text();
            const bool match = haystack.contains(query, Qt::CaseInsensitive);
            item->setData(kFiltered, !match);
            value->setData(kFiltered, !match);
            m_table->cellWidget(item->row(), value->column())->setVisible(match);
            if (match) matches.insert(item->row());
        }
        for (int i = 0; i < m_groups.size(); ++i) {
            const int row = m_groups.at(i)->row();
            const int end = i + 1 < m_groups.size() ? m_groups.at(i + 1)->row() : groupedEnd;
            for (int itemRow = row + 1; itemRow < end; ++itemRow) {
                if (matches.contains(itemRow)) { matches.insert(row); break; }
            }
        }
        for (int row = 0; row < m_table->rowCount(); ++row) {
            m_table->setRowHidden(row, !matches.contains(row));
        }
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (!item->flags().testFlag(Qt::ItemIsUserCheckable)) return;
        if (auto *editor = m_table->cellWidget(item->row(), item->column() + 1))
            editor->setEnabled(item->checkState() == Qt::Checked);
    });
    auto *problem = new QLabel(this); problem->setWordWrap(true); layout->addWidget(problem);
    // 不用标准钮（QDialogButtonBox::Save/Cancel/…）：它的字在 LanguageChange 时会被 Qt 刷回自带的英文，
    // 装没装 Qt 中文翻译都一样不稳。自己 addButton(字, 角色)，accepted/rejected 照常发。
    auto *buttons = new QDialogButtonBox(this);
    QPushButton *saveButton = buttons->addButton(
        QCoreApplication::translate("PresetOptionsEditor", "保存"), QDialogButtonBox::AcceptRole);
    QPushButton *cancelButton = buttons->addButton(
        QCoreApplication::translate("PresetOptionsEditor", "取消"), QDialogButtonBox::RejectRole);
    saveButton->setAutoDefault(false);
    cancelButton->setAutoDefault(false);
    Theme::markButton(saveButton, Theme::ButtonKind::Primary);
    Theme::markButton(cancelButton, Theme::ButtonKind::Neutral);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, problem] {
        QSet<QString> keys;
        for (const auto &entry : this->entries()) {
            if (entry.key.isEmpty() || entry.key.contains(QLatin1Char(':')) || entry.key.contains(QLatin1Char('\n')) ||
                entry.key.contains(QLatin1Char('\r')) || entry.value.contains(QLatin1Char('\n')) ||
                entry.value.contains(QLatin1Char('\r')) || keys.contains(entry.key)) {
                problem->setText(QCoreApplication::translate("PresetOptionsEditor", "设置键不能为空或重复，键和值不能含换行，键不能含冒号。")); return;
            }
            keys.insert(entry.key);
        }
        accept();
    });
}
QTableWidgetItem *PresetOptionsEditor::append(const Entry &entry, int row, int column, bool included) {
    using namespace PresetOptionCatalog;
    auto *key = new QTableWidgetItem(entry.key);
    const bool chooseIndividually = m_mode != Mode::Options;
    key->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    if (chooseIndividually) {
        key->setFlags(key->flags() | Qt::ItemIsUserCheckable);
        key->setCheckState(included ? Qt::Checked : Qt::Unchecked);
    }
    const auto *spec = PresetOptionCatalog::find(entry.key);
    key->setToolTip(spec ? spec->group + " / " + entry.key : entry.key);
    auto *value = new QTableWidgetItem(entry.value);
    m_table->setItem(row, column, key);
    m_table->setItem(row, column + 1, value);
    // 未识别或超出控件范围的导入值仍按原文保留，不能静默截断。
    bool valid = true;
    const double number = entry.value.toDouble(&valid);
    const double display = entry.key == "fov" ? 70 + 40 * number : number;
    bool supported = spec != nullptr;
    if (spec && (spec->kind == Kind::Integer || spec->kind == Kind::Fraction))
        supported = valid && display >= spec->minimum && display <= spec->maximum;
    const QModelIndex index = m_table->indexFromItem(value);
    QWidget *editor = supported ? m_table->itemDelegate()->createEditor(m_table, {}, index)
                                : new QLineEdit(m_table);
    if (supported) m_table->itemDelegate()->setEditorData(editor, index);
    else qobject_cast<QLineEdit *>(editor)->setText(entry.value);
    editor->setObjectName("presetValue_" + entry.key);
    editor->setEnabled(!chooseIndividually || included);
    m_table->setCellWidget(row, column + 1, editor);
    const auto commit = [this, editor, value, supported] {
        if (supported) m_table->itemDelegate()->setModelData(editor,m_table->model(),m_table->indexFromItem(value));
        else value->setText(qobject_cast<QLineEdit *>(editor)->text());
    };
    if (auto *capture = dynamic_cast<KeyCapture *>(editor))
        capture->changed = [value](const QString &binding) { value->setText(binding); };
    else if (auto *slider = dynamic_cast<OptionSlider *>(editor)) connect(slider->slider(), &QSlider::valueChanged, this, commit);
    else if (auto *combo = qobject_cast<QComboBox *>(editor)) connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,commit);
    else if (auto *line = qobject_cast<QLineEdit *>(editor)) connect(line,&QLineEdit::textChanged,this,commit);
    return key;
}
QList<PresetOptionsEditor::Entry> PresetOptionsEditor::entries() const {
    QList<Entry> result;
    for (const auto *item : m_options) {
        if (m_mode == Mode::Options || item->checkState() == Qt::Checked) {
            result.append({item->text().trimmed(), m_table->item(item->row(), item->column() + 1)->text()});
        }
    }
    return result;
}
