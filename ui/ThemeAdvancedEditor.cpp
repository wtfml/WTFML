#include "ThemeAdvancedEditor.hpp"
#include "InputSizing.hpp"
#include "SettingsPageParts.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

ThemeAdvancedEditor::ThemeAdvancedEditor(QWidget *parent) : QWidget(parent), m_spec(Theme::current()) {
    setObjectName(QStringLiteral("themeAdvancedEditor"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // 折叠行：整行一颗按钮，右边「展开 / 收起」，不放 ▸ 方框钮（8 号反馈）。
    auto *toggle = Theme::makeFoldRow(this, tr("细分区域与控件颜色"));
    toggle->setObjectName("themeAdvancedToggle");
    layout->addWidget(toggle);
    auto *body = new QWidget(this);
    body->setObjectName("themeAdvancedBody");
    body->hide();
    layout->addWidget(body);
    auto *motion = new SettingsParts::FoldMotion(body);
    connect(toggle, &QAbstractButton::toggled, this, [motion](bool open) { motion->setOpen(open); });
    auto *form = new QFormLayout(body);
    form->setContentsMargins(0, 4, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_role = new QComboBox(body);
    m_role->setObjectName("themeRoleCombo");
    for (int i = 0; i < Theme::roleCount(); ++i) m_role->addItem(Theme::roleName(Theme::Role(i)), i);
    form->addRow(tr("控件角色"), m_role);
    const auto colorRow = [this, body](const char *name, QPushButton **swatch, auto reset) {
        auto *row = new QWidget(body);
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        *swatch = new QPushButton(tr("选择颜色…"), row);
        (*swatch)->setObjectName(name);
        Theme::markButton(*swatch, Theme::ButtonKind::Neutral);
        auto *follow = new QPushButton(tr("跟随主题"), row);
        follow->setObjectName(QString::fromLatin1(name) + "Reset");
        Theme::markButton(follow, Theme::ButtonKind::Ghost);
        line->addWidget(*swatch);
        line->addWidget(follow);
        line->addStretch();
        connect(follow, &QPushButton::clicked, this, reset);
        return row;
    };
    auto *roleRow = colorRow("themeRoleColor", &m_roleColor, [this] { setRoleColor(m_role->currentData().toInt(), {}); });
    m_roleState = new QLabel(roleRow);
    qobject_cast<QHBoxLayout *>(roleRow->layout())->addWidget(m_roleState);
    form->addRow(tr("角色颜色"), roleRow);
    m_part = new QComboBox(body);
    m_part->setObjectName("themePartCombo");
    for (int i = 0; i < Theme::partCount(); ++i) m_part->addItem(Theme::partName(Theme::Part(i)), i);
    form->addRow(tr("界面区域"), m_part);
    form->addRow(tr("区域底色"), colorRow("themePartBackground", &m_background,
        [this] { setPartColor(m_part->currentData().toInt(), false, {}); }));
    m_foregroundRow = colorRow("themePartForeground", &m_foreground,
        [this] { setPartColor(m_part->currentData().toInt(), true, {}); });
    form->addRow(tr("区域文字"), m_foregroundRow);
    m_opacity = new QSpinBox(body);
    m_opacity->setObjectName("themePartTextOpacity");
    InputSizing::shortValue(m_opacity, 0);
    m_opacity->setRange(-1, 100);
    m_opacity->setSpecialValueText(tr("跟随全局"));
    m_opacity->setSuffix("%");
    m_opacity->setKeyboardTracking(false);
    form->addRow(tr("文字不透明度"), m_opacity);
    auto *reset = new QPushButton(tr("重置此区域配色"), body);
    reset->setObjectName("themePartReset");
    Theme::markButton(reset, Theme::ButtonKind::Ghost);
    form->addRow(reset);
    auto *hint = new QLabel(tr("只调整配色，不改变背景图片。遮罩没有文字设置。"), body);
    hint->setWordWrap(true);
    form->addRow(hint);
    connect(reset, &QPushButton::clicked, this, [this] { resetPart(m_part->currentData().toInt()); });
    connect(m_role, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(m_part, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(m_roleColor, &QPushButton::clicked, this, [this] { emit roleColorRequested(m_role->currentData().toInt()); });
    connect(m_background, &QPushButton::clicked, this, [this] { emit partColorRequested(m_part->currentData().toInt(), false); });
    connect(m_foreground, &QPushButton::clicked, this, [this] { emit partColorRequested(m_part->currentData().toInt(), true); });
    connect(m_opacity, &QSpinBox::valueChanged, this, [this](int value) {
        const int part = m_part->currentData().toInt();
        if (part == int(Theme::Part::Scrim)) return;
        m_spec.parts[part].textOpacity = value;
        publish();
    });
    refresh();
}

void ThemeAdvancedEditor::setSpec(const Theme::Spec &spec) { m_spec = Theme::normalize(spec); refresh(); }
void ThemeAdvancedEditor::publish() { m_spec = Theme::normalize(m_spec); refresh(); emit specEdited(m_spec); }
void ThemeAdvancedEditor::setRoleColor(int role, const QColor &color) {
    if (role < 0 || role >= Theme::roleCount()) return;
    if (color.isValid()) m_spec.roles.insert(role, color); else m_spec.roles.remove(role);
    publish();
}
void ThemeAdvancedEditor::setPartColor(int part, bool foreground, const QColor &color) {
    if (part < 0 || part >= Theme::partCount() || (foreground && part == int(Theme::Part::Scrim))) return;
    auto &style = m_spec.parts[part];
    if (foreground) { style.hasForeground = color.isValid(); style.foreground = color; }
    else { style.hasBackground = color.isValid(); style.background = color; }
    publish();
}
void ThemeAdvancedEditor::resetPart(int part) {
    if (part < 0 || part >= Theme::partCount()) return;
    auto &style = m_spec.parts[part];
    style.hasBackground = style.hasForeground = false;
    style.background = style.foreground = QColor();
    style.textOpacity = -1;
    publish();
}
void ThemeAdvancedEditor::refresh() {
    const int role = m_role->currentData().toInt();
    const auto palette = Theme::buildPalette(m_spec, QApplication::palette());
    Theme::markColorSwatch(m_roleColor, palette.color(Theme::qtRole(Theme::Role(role))));
    m_roleState->setText(m_spec.roles.contains(role) ? tr("自定义") : tr("跟随主题"));
    const int part = m_part->currentData().toInt();
    const auto style = m_spec.parts.value(part);
    Theme::markColorSwatch(m_background, style.hasBackground ? style.background : Theme::partBackground(Theme::Part(part)));
    Theme::markColorSwatch(m_foreground, style.hasForeground ? style.foreground :
        Theme::partForeground(Theme::Part(part), palette.color(QPalette::WindowText)));
    m_background->setText(style.hasBackground ? tr("自定义底色…") : tr("选择底色…"));
    m_foreground->setText(style.hasForeground ? tr("自定义字色…") : tr("选择字色…"));
    const bool hasText = part != int(Theme::Part::Scrim);
    m_foregroundRow->setEnabled(hasText);
    m_opacity->setEnabled(hasText);
    const QSignalBlocker blocker(m_opacity);
    m_opacity->setValue(style.textOpacity);
}
