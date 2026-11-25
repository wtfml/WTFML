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
#include <QResizeEvent>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QApplication>
#include <cmath>
#include <functional>
#include <utility>

namespace {
using namespace PresetOptionCatalog;
constexpr int kFiltered = Qt::UserRole + 1;

// 原版 Java 1.21.1 的选项槽位。空槽对应预设目录中没有的游戏选项。
using Pair = std::pair<const char *, const char *>;
const auto videoRows = {
    Pair{"graphicsMode", "renderDistance"}, Pair{"prioritizeChunkUpdates", "simulationDistance"}, Pair{"ao", "maxFps"},
    Pair{"enableVsync", "bobView"}, Pair{"guiScale", "attackIndicator"}, Pair{"gamma", "renderClouds"},
    Pair{"fullscreen", "particles"}, Pair{"mipmapLevels", "entityShadows"}, Pair{"cloudRange", "biomeBlendRadius"},
    Pair{"entityDistanceScaling", "weatherRadius"}, Pair{"cutoutLeaves", "improvedTransparency"},
    Pair{"textureFiltering", "maxAnisotropyBit"}, Pair{"inactivityFpsLimit", "exclusiveFullscreen"},
    Pair{"showAutosaveIndicator", "vignette"}, Pair{"chunkSectionFadeInTime", "preferredGraphicsBackend"},
};
const auto mouseRows = {
    Pair{"mouseSensitivity", "mouseWheelSensitivity"}, Pair{"discrete_mouse_scroll", "invertXMouse"},
    Pair{"invertYMouse", "allowCursorChanges"}, Pair{"rawMouseInput", nullptr},
};
const auto controlRows = {
    Pair{"toggleCrouch", "toggleSprint"}, Pair{"toggleAttack", "toggleUse"}, Pair{"autoJump", "sprintWindow"},
    Pair{"operatorItemsTab", "pauseOnLostFocus"},
};
const auto accessibilityRows = {
    Pair{"narrator", "showSubtitles"}, Pair{"highContrast", "highContrastBlockOutline"},
    Pair{"menuBackgroundBlurriness", "textBackgroundOpacity"}, Pair{"backgroundForChatOnly", "notificationDisplayTime"},
    Pair{"screenEffectScale", "fovEffectScale"}, Pair{"darknessEffectScale", "damageTiltStrength"},
    Pair{"glintSpeed", "glintStrength"}, Pair{"hideLightningFlash", "darkMojangStudiosBackground"},
    Pair{"panoramaScrollSpeed", "hideSplashTexts"}, Pair{"narratorHotkey", "rotateWithMinecart"},
};
const auto chatRows = {
    Pair{"chatVisibility", "chatColors"}, Pair{"chatLinks", "chatLinksPrompt"},
    Pair{"chatOpacity", "chatDelay"}, Pair{"chatScale", "chatLineSpacing"},
    Pair{"chatWidth", "chatHeightFocused"}, Pair{"chatHeightUnfocused", "autoSuggestions"},
    Pair{"hideMatchedNames", "reducedDebugInfo"}, Pair{"onlyShowSecureChat", "saveChatDrafts"},
};
const auto soundRows = {
    Pair{"soundCategory_master", nullptr}, Pair{"soundCategory_music", "soundCategory_record"},
    Pair{"soundCategory_weather", "soundCategory_block"}, Pair{"soundCategory_hostile", "soundCategory_neutral"},
    Pair{"soundCategory_player", "soundCategory_ambient"}, Pair{"soundCategory_voice", "directionalAudio"},
    Pair{"musicFrequency", "musicToast"},
};
const auto skinRows = {
    Pair{"modelPart_cape", "modelPart_jacket"}, Pair{"modelPart_left_sleeve", "modelPart_right_sleeve"},
    Pair{"modelPart_left_pants_leg", "modelPart_right_pants_leg"}, Pair{"modelPart_hat", "mainHand"},
};

class PairedOptionsTable final : public QTableWidget {
public:
    explicit PairedOptionsTable(QWidget *parent) : QTableWidget(0, 4, parent) {}

    void fitLabels(int viewportWidth) {
        // 长标签允许省略，给每个值控件和重置按钮留出宽度。
        for (const int column : {0, 2}) {
            const int labelWidth = qMin(sizeHintForColumn(column), viewportWidth / 5);
            if (labelWidth > 0 && columnWidth(column) != labelWidth) setColumnWidth(column, labelWidth);
        }
    }

protected:
    bool viewportEvent(QEvent *event) override {
        const bool handled = QTableWidget::viewportEvent(event);
        if (event->type() == QEvent::Resize) fitLabels(static_cast<QResizeEvent *>(event)->size().width());
        return handled;
    }
};

class OptionSlider final : public QWidget {
public:
    OptionSlider(const Option &spec, QWidget *parent)
        : QWidget(parent), m_spec(spec), m_logarithmic(spec.key == "mouseWheelSensitivity"),
          m_scale(m_logarithmic ? 100 : spec.step > 0 ? 1.0 / spec.step : spec.kind == Kind::Fraction ? 1000 : 1) {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 0, 8, 0);
        m_slider = new QSlider(Qt::Horizontal, this);
        m_slider->setAccessibleName(spec.name);
        m_slider->setRange(sliderPosition(spec.minimum), sliderPosition(spec.maximum));
        m_label = new QLabel(this);
        m_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(m_slider, 1);
        layout->addWidget(m_label);
        setFocusProxy(m_slider);
        connect(m_slider, &QSlider::valueChanged, this, [this] { updateLabel(); });
    }

    void load(const QString &value) {
        const QSignalBlocker blocker(m_slider);
        const double number = value.toDouble();
        m_slider->setValue(sliderPosition(m_spec.key == "fov" ? 70 + 40 * number : number));
        updateLabel();
    }

    QString value() const {
        const double position = displayValue();
        if (m_spec.key == "fov") return QString::number((position - 70) / 40.0);
        return QString::number(position);
    }

    QSlider *slider() const { return m_slider; }

private:
    int sliderPosition(double value) const {
        return qRound((m_logarithmic ? std::log10(value) : value) * m_scale);
    }

    double displayValue() const {
        const double position = m_slider->value() / m_scale;
        return m_logarithmic ? std::pow(10.0, position) : position;
    }

    void updateLabel() {
        const double position = displayValue();
        QString text = QString::number(position);
        if (m_spec.key == "fov") {
            if (position == 70) text = QCoreApplication::translate("OptionValueDelegate", "中");
            if (position == 110) text = QCoreApplication::translate("OptionValueDelegate", "广角");
        } else if (m_spec.key == "gamma") {
            const int brightness = m_slider->value() / 10;
            text = QString::number(brightness);
            if (brightness == 0) text = QCoreApplication::translate("OptionValueDelegate", "昏暗");
            if (brightness == 50) text = QCoreApplication::translate("OptionValueDelegate", "默认");
            if (brightness == 100) text = QCoreApplication::translate("OptionValueDelegate", "明亮");
        } else if (m_spec.key == "maxFps") {
            text = position == 260 ? QCoreApplication::translate("OptionValueDelegate", "无限制")
                                   : QCoreApplication::translate("OptionValueDelegate", "%1 fps").arg(position);
        } else if (m_spec.key.endsWith("Distance") || m_spec.key == "cloudRange") {
            text = QCoreApplication::translate("OptionValueDelegate", "%1个区块").arg(int(position));
        } else if (m_logarithmic) {
            text = QString::number(position, 'f', 2);
        } else if (m_spec.key == "maxAnisotropyBit") {
            text = QStringLiteral("%1×").arg(1 << int(position));
        } else if (m_spec.key == "chatDelay" || m_spec.key == "chunkSectionFadeInTime") {
            text = QCoreApplication::translate("OptionValueDelegate", "%1秒").arg(QString::number(position));
        } else if (m_spec.key == "notificationDisplayTime") {
            text = QStringLiteral("%1×").arg(QString::number(position));
        } else if (m_spec.key == "chatWidth" || m_spec.key == "chatHeightFocused" || m_spec.key == "chatHeightUnfocused") {
            const bool width = m_spec.key == "chatWidth";
            text = QCoreApplication::translate("OptionValueDelegate", "%1像素").arg(int(position * (width ? 280 : 160) + (width ? 40 : 20)));
        } else if (m_spec.key == "chatOpacity") {
            // 游戏将 0–1 的保存值显示为 10%–100%。
            text = QString::number(10 + m_slider->value() * 9 / 100) + '%';
        } else if (m_spec.kind == Kind::Fraction) {
            const bool sensitivity = m_spec.key == "mouseSensitivity";
            const int percent = qRound(position * (sensitivity ? 200 : 100));
            text = QString::number(percent) + '%';
            if (sensitivity && position == 0) text = QCoreApplication::translate("OptionValueDelegate", "*哈欠*");
            if (sensitivity && position == 1) text = QCoreApplication::translate("OptionValueDelegate", "超高速！！！");
            if (position == 0 && m_spec.key.startsWith("soundCategory_"))
                text = QCoreApplication::translate("OptionValueDelegate", "关");
        }
        m_label->setText(text);
    }

    const Option &m_spec;
    const bool m_logarithmic;
    const double m_scale;
    QSlider *m_slider;
    QLabel *m_label;
};

QString keyLabel(const QString &value) {
    static const QHash<QString, QString> names{
        {"key.keyboard.space", QCoreApplication::translate("PresetOptionsEditor", "空格")},
        {"key.keyboard.left.shift", QCoreApplication::translate("PresetOptionsEditor", "左Shift")},
        {"key.keyboard.left.control", QCoreApplication::translate("PresetOptionsEditor", "左Ctrl")},
        {"key.keyboard.left.alt", QCoreApplication::translate("PresetOptionsEditor", "左Alt")},
        {"key.keyboard.left.win", QCoreApplication::translate("PresetOptionsEditor", "左Win")},
        {"key.keyboard.right.shift", QCoreApplication::translate("PresetOptionsEditor", "右Shift")},
        {"key.keyboard.right.control", QCoreApplication::translate("PresetOptionsEditor", "右Ctrl")},
        {"key.keyboard.right.alt", QCoreApplication::translate("PresetOptionsEditor", "右Alt")},
        {"key.keyboard.right.win", QCoreApplication::translate("PresetOptionsEditor", "右Win")},
        {"key.keyboard.tab", QStringLiteral("Tab")},
        {"key.keyboard.enter", QStringLiteral("Enter")},
        {"key.keyboard.keypad.enter", QCoreApplication::translate("PresetOptionsEditor", "小键盘 Enter")},
        {"key.keyboard.up", QCoreApplication::translate("PresetOptionsEditor", "上方向键")},
        {"key.keyboard.down", QCoreApplication::translate("PresetOptionsEditor", "下方向键")},
        {"key.keyboard.left", QCoreApplication::translate("PresetOptionsEditor", "左方向键")},
        {"key.keyboard.right", QCoreApplication::translate("PresetOptionsEditor", "右方向键")},
        {"key.keyboard.insert", QStringLiteral("Insert")},
        {"key.keyboard.delete", QStringLiteral("Delete")},
        {"key.keyboard.home", QStringLiteral("Home")},
        {"key.keyboard.end", QStringLiteral("End")},
        {"key.keyboard.page.up", QStringLiteral("Page Up")},
        {"key.keyboard.page.down", QStringLiteral("Page Down")},
        {"key.keyboard.minus", QStringLiteral("-")},
        {"key.keyboard.equal", QStringLiteral("=")},
        {"key.keyboard.left.bracket", QStringLiteral("[")},
        {"key.keyboard.right.bracket", QStringLiteral("]")},
        {"key.keyboard.semicolon", QStringLiteral(";")},
        {"key.keyboard.apostrophe", QStringLiteral("'")},
        {"key.keyboard.comma", QStringLiteral(",")},
        {"key.keyboard.period", QStringLiteral(".")},
        {"key.keyboard.slash", QStringLiteral("/")},
        {"key.keyboard.backslash", QStringLiteral("\\")},
        {"key.keyboard.grave.accent", QStringLiteral("`")},
        {"key.keyboard.caps.lock", QStringLiteral("Caps Lock")},
        {"key.keyboard.escape", QCoreApplication::translate("PresetOptionsEditor", "Esc")},
        {"key.keyboard.backspace", QStringLiteral("Backspace")},
        {"key.keyboard.pause", QStringLiteral("Pause")},
        {"key.keyboard.num.lock", QStringLiteral("Num Lock")},
        {"key.keyboard.scroll.lock", QStringLiteral("Scroll Lock")},
        {"key.keyboard.print.screen", QStringLiteral("Print Screen")},
        {"key.keyboard.menu", QCoreApplication::translate("PresetOptionsEditor", "菜单")},
        {"key.keyboard.world.1", QStringLiteral("World 1")},
        {"key.keyboard.world.2", QStringLiteral("World 2")},
        {"key.keyboard.keypad.add", QCoreApplication::translate("PresetOptionsEditor", "小键盘 +")},
        {"key.keyboard.keypad.decimal", QCoreApplication::translate("PresetOptionsEditor", "小键盘 .")},
        {"key.keyboard.keypad.divide", QCoreApplication::translate("PresetOptionsEditor", "小键盘 /")},
        {"key.keyboard.keypad.equal", QCoreApplication::translate("PresetOptionsEditor", "小键盘 =")},
        {"key.keyboard.keypad.multiply", QCoreApplication::translate("PresetOptionsEditor", "小键盘 *")},
        {"key.keyboard.keypad.subtract", QCoreApplication::translate("PresetOptionsEditor", "小键盘 -")},
        {"key.keyboard.unknown", QCoreApplication::translate("PresetOptionsEditor", "未指定")},
        {"key.mouse.left", QCoreApplication::translate("PresetOptionsEditor", "左键")},
        {"key.mouse.middle", QCoreApplication::translate("PresetOptionsEditor", "中键")},
        {"key.mouse.right", QCoreApplication::translate("PresetOptionsEditor", "右键")},
    };
    if (const auto known = names.constFind(value); known != names.cend()) return *known;
    if (value.startsWith("key.keyboard.keypad.")) {
        const QStringView digit = QStringView(value).sliced(qsizetype(sizeof("key.keyboard.keypad.") - 1));
        if (digit.size() == 1 && digit.front().isDigit())
            return QCoreApplication::translate("PresetOptionsEditor", "小键盘 %1").arg(digit);
    }
    if (value.startsWith("key.mouse.")) {
        bool numeric = false;
        const int button = value.mid(10).toInt(&numeric);
        if (numeric && button > 0)
            return QCoreApplication::translate("PresetOptionsEditor", "鼠标按键%1").arg(button);
    }
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
    void setValue(const QString &value) {
        armed = false;
        setProperty("wtfmlKeyCaptureArmed", false);
        current = value;
        setText(keyLabel(value));
    }
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
    // 表格挂了滑条列让位时，挨着滑条那一格里的控件也停在同一条右缘（#136）。
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        DesignDefaults::RowDelegate::updateEditorGeometry(editor, option, index);
        const auto *view = qobject_cast<const QAbstractScrollArea *>(option.widget);
        const int gap = view ? view->property("wtfmlScrollContentGap").toInt() : 0;
        if (gap <= 0) return;
        const int over = editor->geometry().right() + 1 - (view->viewport()->width() - gap);
        if (over > 0) editor->setGeometry(editor->geometry().adjusted(0, 0, -over, 0));
    }
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto *spec = PresetOptionCatalog::find(index.siblingAtColumn(index.column() - 1).data().toString());
        if (index.column() % 2 == 1 && spec) {
            if (spec->kind == Kind::Key) return new KeyCapture(parent);
            if (spec->kind == Kind::Boolean || spec->kind == Kind::Choice || spec->key == "guiScale") {
                auto *combo = new QComboBox(parent);
                if (spec->kind == Kind::Boolean) {
                    combo->addItem(QCoreApplication::translate("OptionValueDelegate", "关"), "false");
                    combo->addItem(QCoreApplication::translate("OptionValueDelegate", "开"), "true");
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
            const auto value = index.data().toString();
            int selected = combo->findData(value);
            if (selected < 0 && value.startsWith('"') && value.endsWith('"') && value.size() >= 2)
                selected = combo->findData(value.mid(1, value.size() - 2));
            if (selected < 0) { combo->addItem(value, value); selected = combo->count()-1; }
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
    auto *pairedTable = paired ? new PairedOptionsTable(this) : nullptr;
    m_table = pairedTable ? pairedTable : new QTableWidget(0, 2, this);
    m_table->setObjectName(QStringLiteral("presetOptionsEditorTable"));
    auto *header = m_table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Stretch);
    if (paired) {
        header->setSectionResizeMode(0, QHeaderView::Fixed);
        header->setSectionResizeMode(2, QHeaderView::Fixed);
    }
    header->setVisible(mode == Mode::All);
    if (mode == Mode::All) {
        m_table->setHorizontalHeaderLabels({
            QCoreApplication::translate("PresetOptionsEditor", "纳入预设"),
            QCoreApplication::translate("PresetOptionsEditor", "设置值"),
        });
    }
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
        addGroup(PresetOptionCatalog::find(QStringLiteral("mouseSensitivity"))->group);
        addPairs(mouseRows);
        addGroup(PresetOptionCatalog::find(QStringLiteral("highContrast"))->group);
        addPairs(accessibilityRows);
        addGroup(PresetOptionCatalog::find(QStringLiteral("chatVisibility"))->group);
        addPairs(chatRows);
        addGroup(PresetOptionCatalog::find(QStringLiteral("soundCategory_master"))->group);
        addPairs(soundRows);
        addGroup(PresetOptionCatalog::find(QStringLiteral("mainHand"))->group);
        addPairs(skinRows);
    } else if (mode == Mode::Keys) {
        QString activeGroup;
        for (const auto &spec : PresetOptionCatalog::all()) {
            if (spec.kind != Kind::Key) continue;
            if (spec.group != activeGroup) {
                activeGroup = spec.group;
                addGroup(activeGroup);
            }
            place(spec, newRow(), 0);
        }
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
    if (pairedTable) pairedTable->fitLabels(pairedTable->viewport()->width());
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
    const bool chooseIndividually = m_mode == Mode::All;
    key->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    if (chooseIndividually) {
        key->setFlags(key->flags() | Qt::ItemIsUserCheckable);
        key->setCheckState(included ? Qt::Checked : Qt::Unchecked);
    }
    const auto *spec = PresetOptionCatalog::find(entry.key);
    key->setToolTip(spec ? spec->name + QLatin1Char('\n') + spec->group + " / " + entry.key : entry.key);
    auto *value = new QTableWidgetItem(entry.value);
    value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    m_table->setItem(row, column, key);
    m_table->setItem(row, column + 1, value);
    QWidget *cell = nullptr;
    if (spec) {
        cell = new QWidget(m_table);
        auto *layout = new QHBoxLayout(cell);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        layout->addWidget(createValueEditor(value, cell), 1);
        auto *reset = new QPushButton(QCoreApplication::translate("PresetOptionsEditor", "重置"), cell);
        reset->setObjectName(QStringLiteral("presetReset_") + entry.key);
        reset->setAutoDefault(false);
        Theme::markButton(reset, Theme::ButtonKind::Neutral);
        reset->setAccessibleDescription(spec->name);
        const auto canReset = [spec](const QString &raw) {
            if (raw == spec->initial) return false;
            if (spec->kind != Kind::Integer && spec->kind != Kind::Fraction) return true;
            bool numeric = false;
            const double number = raw.toDouble(&numeric);
            return !numeric || number != spec->initial.toDouble();
        };
        reset->setEnabled(canReset(entry.value));
        layout->addWidget(reset);
        connect(m_table, &QTableWidget::itemChanged, reset, [value, reset, canReset](const QTableWidgetItem *changed) {
            if (changed == value) reset->setEnabled(canReset(value->text()));
        });
        connect(reset, &QPushButton::clicked, this, [this, cell, layout, value, spec] {
            value->setText(spec->initial);
            auto *editor = layout->itemAt(0)->widget();
            if (qobject_cast<QLineEdit *>(editor)) {
                delete editor;
                layout->insertWidget(0, createValueEditor(value, cell), 1);
            } else {
                m_table->itemDelegate()->setEditorData(editor, m_table->indexFromItem(value));
            }
        });
    } else {
        cell = createValueEditor(value, m_table);
    }
    cell->setEnabled(!chooseIndividually || included);
    m_table->setCellWidget(row, column + 1, cell);
    return key;
}

QWidget *PresetOptionsEditor::createValueEditor(QTableWidgetItem *value, QWidget *parent) {
    const QModelIndex index = m_table->indexFromItem(value);
    const QString key = index.siblingAtColumn(value->column() - 1).data().toString();
    const auto *spec = PresetOptionCatalog::find(key);
    // 未识别或超出控件范围的导入值仍按原文保留，不能静默截断。
    bool valid = true;
    const double number = value->text().toDouble(&valid);
    const double display = key == "fov" ? 70 + 40 * number : number;
    bool supported = spec != nullptr;
    if (spec && (spec->kind == Kind::Integer || spec->kind == Kind::Fraction))
        supported = valid && display >= spec->minimum && display <= spec->maximum;
    QWidget *editor = supported ? m_table->itemDelegate()->createEditor(parent, {}, index)
                                : new QLineEdit(parent);
    if (supported) m_table->itemDelegate()->setEditorData(editor, index);
    else qobject_cast<QLineEdit *>(editor)->setText(value->text());
    editor->setObjectName(QStringLiteral("presetValue_") + key);
    const auto commit = [this, editor, value, supported] {
        if (supported) m_table->itemDelegate()->setModelData(editor,m_table->model(),m_table->indexFromItem(value));
        else value->setText(qobject_cast<QLineEdit *>(editor)->text());
    };
    if (auto *capture = dynamic_cast<KeyCapture *>(editor))
        capture->changed = [value](const QString &binding) { value->setText(binding); };
    else if (auto *slider = dynamic_cast<OptionSlider *>(editor)) connect(slider->slider(), &QSlider::valueChanged, this, commit);
    else if (auto *combo = qobject_cast<QComboBox *>(editor)) connect(combo,qOverload<int>(&QComboBox::currentIndexChanged),this,commit);
    else if (auto *line = qobject_cast<QLineEdit *>(editor)) connect(line,&QLineEdit::textChanged,this,commit);
    return editor;
}
QList<PresetOptionsEditor::Entry> PresetOptionsEditor::entries() const {
    QList<Entry> result;
    for (const auto *item : m_options) {
        if (m_mode != Mode::All || item->checkState() == Qt::Checked) {
            result.append({item->text().trimmed(), m_table->item(item->row(), item->column() + 1)->text()});
        }
    }
    return result;
}
