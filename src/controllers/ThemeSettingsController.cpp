#include "controllers/ThemeSettingsController.hpp"
#include <QCoreApplication>

#include "MainWindowUI.hpp"
#include "InlineMessage.hpp"
#include "ThemeAdvancedEditor.hpp"
#include "settings/LauncherSettings.hpp"
#include "util/ThemeImage.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QTimer>

namespace {

/// 拖不透明度滑块时，"落盘 + 重铺整个应用"最多隔这么久做一次（毫秒）。
///
/// 一次 `Theme::apply()` 是整份应用样式表的重算 + 每个控件的 `PaletteChange`——拖动时
/// 每挪一个像素来一发，几十毫秒就叠上一次，拖起来是卡顿的；而每一下都 `Theme::save()`
/// 则是往 `QSettings` 里写几十遍同一个键。60 ms ≈ 16 fps：画面跟得上手，开销只剩零头。
constexpr int kApplyThrottleMs = 60;


QColor semanticColor(const Theme::Spec &spec, int index) {
    const QPalette palette = QApplication::palette();
    switch (index) {
    case 0: return spec.danger.isValid() ? spec.danger : Theme::dangerColor(palette);
    case 1: return spec.warning.isValid() ? spec.warning : Theme::warningColor(palette);
    case 2: return spec.ok.isValid() ? spec.ok : Theme::okColor(palette);
    case 3: return spec.errorText.isValid() ? spec.errorText : Theme::errorTextColor(palette);
    default: return {};
    }
}

void setSemanticColor(Theme::Spec *spec, int index, const QColor &color) {
    if (spec == nullptr) return;
    switch (index) {
    case 0: spec->danger = color; break;
    case 1: spec->warning = color; break;
    case 2: spec->ok = color; break;
    case 3: spec->errorText = color; break;
    default: break;
    }
}

}  // namespace

ThemeSettingsController::ThemeSettingsController(MainWindowUI *ui, LauncherSettings *settings,
                                                 QWidget *dialogParent, QObject *parent)
    : QObject(parent), m_ui(ui), m_settings(settings), m_dialogParent(dialogParent) {
    m_applyThrottle = new QTimer(this);
    m_applyThrottle->setSingleShot(true);
    m_applyThrottle->setInterval(kApplyThrottleMs);
    connect(m_applyThrottle, &QTimer::timeout, this, &ThemeSettingsController::flushPendingSpec);
    if (auto *editor = m_ui->getThemeAdvancedEditor()) {
        connect(editor, &ThemeAdvancedEditor::specEdited, this, &ThemeSettingsController::applySpec);
        connect(editor, &ThemeAdvancedEditor::roleColorRequested, this, [this, editor](int role) {
            if (role < 0 || role >= Theme::roleCount()) return;
            const auto key = Theme::Role(role);
            const QColor before = Theme::buildPalette(Theme::current(), QApplication::palette()).color(Theme::qtRole(key));
            const auto picked = InlineMessage::pickColor(m_ui->overlay(), Theme::roleName(key),
                tr("只覆盖这一类控件颜色；可随时恢复跟随主题。"), before);
            if (picked.isValid()) editor->setRoleColor(role, picked);
        });
        connect(editor, &ThemeAdvancedEditor::partColorRequested, this, [this, editor](int part, bool foreground) {
            if (part < 0 || part >= Theme::partCount()) return;
            const auto key = Theme::Part(part);
            const auto before = foreground ? Theme::partForeground(key, QApplication::palette().color(QPalette::WindowText))
                                           : Theme::partBackground(key);
            const auto picked = InlineMessage::pickColor(m_ui->overlay(), Theme::partName(key),
                foreground ? tr("调整此区域文字，不改变背景图。") : tr("调整此区域底色，不改变背景图。"), before);
            if (picked.isValid()) editor->setPartColor(part, foreground, picked);
        });
    }
}

ThemeSettingsController::~ThemeSettingsController() {
    // 节流窗口里最后一发还没落盘就关窗口的话，重开启动器会回到老值。
    // 这里只补「写盘」：界面正在拆，不用再铺一遍。
    if (m_applyPending) {
        m_applyPending = false;
        Theme::save(Theme::current());
    }
}

void ThemeSettingsController::chooseImage(int index) {
    const MainWindowUI::ThemeImageRow &row = m_ui->getThemeImageRow(index);
    Theme::Part part;
    if (row.choose == nullptr || !Theme::partFromKey(row.partKey, &part)) return;
    const QString picked = QFileDialog::getOpenFileName(
        m_dialogParent, tr("选择背景图"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        tr("图片 (*.png *.jpg *.jpeg *.bmp *.webp *.gif);;所有文件 (*)"));
    if (picked.isEmpty()) return;
    QString error;
    const QString stored = ThemeImage::store(row.partKey, picked, &error);
    if (stored.isEmpty()) {
        InlineMessage::warning(m_ui->overlay(), tr("这张图用不了"), error);
        return;
    }
    const Theme::PartImage before = Theme::partImage(part);
    setPartImage(row.partKey, stored, QFileInfo(picked).fileName(), int(before.fit),
                 before.opacity);
}

void ThemeSettingsController::clearImage(int index) {
    const MainWindowUI::ThemeImageRow &row = m_ui->getThemeImageRow(index);
    if (row.clear == nullptr || row.partKey.isEmpty()) return;
    ThemeImage::remove(row.partKey);
    setPartImage(row.partKey, {}, {}, int(Theme::ImageFit::Cover), 100);
}

void ThemeSettingsController::changeImageFit(int index) {
    const MainWindowUI::ThemeImageRow &row = m_ui->getThemeImageRow(index);
    Theme::Part part;
    if (row.fit == nullptr || !Theme::partFromKey(row.partKey, &part)) return;
    const Theme::PartImage before = Theme::partImage(part);
    if (!before.isSet()) return;
    Theme::ImageFit fit = before.fit;
    Theme::imageFitFromKey(row.fit->currentData().toString(), &fit);
    setPartImage(row.partKey, before.path, before.name, int(fit), before.opacity);
}

void ThemeSettingsController::changeImageOpacity(int index) {
    const MainWindowUI::ThemeImageRow &row = m_ui->getThemeImageRow(index);
    Theme::Part part;
    if (row.opacity == nullptr || !Theme::partFromKey(row.partKey, &part)) return;
    const Theme::PartImage before = Theme::partImage(part);
    // 拖滑块走节流那一档：**内存里立刻跟上**，落盘 + 重铺整个应用最多每
    // `kApplyThrottleMs` 一次（见 `applySpecThrottled()`）。
    if (before.isSet())
        setPartImageImpl(row.partKey, before.path, before.name, int(before.fit),
                         row.opacity->value(), true);
}

void ThemeSettingsController::setPartImage(const QString &partKey, const QString &imagePath,
                                           const QString &imageName, int fit, int opacity) {
    setPartImageImpl(partKey, imagePath, imageName, fit, opacity, false);
}

void ThemeSettingsController::setPartImageImpl(const QString &partKey, const QString &imagePath,
                                               const QString &imageName, int fit, int opacity,
                                               bool throttled) {
    Theme::Part part;
    if (!Theme::partFromKey(partKey, &part)) return;
    Theme::Spec spec = Theme::current();
    Theme::PartStyle style = spec.parts.value(int(part));
    style.imagePath = imagePath;
    style.hasImage = !imagePath.isEmpty();
    style.imageName = imagePath.isEmpty() ? QString() : imageName;
    style.fit = Theme::ImageFit(fit);
    style.imageOpacity = opacity;
    if (!style.hasImage && !style.hasBackground && !style.hasForeground && style.textOpacity < 0)
        spec.parts.remove(int(part));
    else
        spec.parts.insert(int(part), style);
    if (throttled)
        applySpecThrottled(spec);
    else
        applySpec(spec);
}

void ThemeSettingsController::applySpec(const Theme::Spec &spec) {
    // 这一发已经是完整的一次「落盘 + 铺开」：节流窗口里攒着的那一发不必再来一遍。
    m_applyPending = false;
    Theme::setSpec(spec);
    Theme::save(Theme::current());
    if (qApp != nullptr) Theme::apply(*qApp);
    refresh();
}

void ThemeSettingsController::applySpecThrottled(const Theme::Spec &spec) {
    // 内存里**立刻**跟上：下一次 tick（以及任何读 `Theme::current()` 的地方）看到的
    // 必须是刚拖到的那个值 —— `changeImageOpacity()` 的"原来那张图"就是从这儿读的。
    Theme::setSpec(spec);
    m_applyPending = true;
    if (m_applyThrottle->isActive()) return;  // 已经排上了：这一发并进窗口末尾那一次
    // 头一下**立刻**做（滑块一按下去就看得见效果，测试里 `setValue()` 后也马上落盘），
    // 之后每 `kApplyThrottleMs` 至多一次，最后一发由窗口末尾那次补上。
    flushPendingSpec();
    m_applyThrottle->start();
}

void ThemeSettingsController::flushPendingSpec() {
    if (!m_applyPending) return;
    m_applyPending = false;
    Theme::save(Theme::current());
    if (qApp != nullptr) Theme::apply(*qApp);
    refresh();
}

void ThemeSettingsController::refreshImages() {
    for (int i = 0; i < MainWindowUI::themeImageRowCount(); ++i) {
        const MainWindowUI::ThemeImageRow &row = m_ui->getThemeImageRow(i);
        Theme::Part part;
        if (row.state == nullptr || !Theme::partFromKey(row.partKey, &part)) continue;
        const Theme::PartImage image = Theme::partImage(part);
        const bool has = image.isSet();
        const QSignalBlocker blockFit(row.fit);
        const QSignalBlocker blockOpacity(row.opacity);
        if (has) {
            const int fitIndex = row.fit->findData(Theme::imageFitKey(image.fit));
            if (fitIndex >= 0) row.fit->setCurrentIndex(fitIndex);
        }
        row.opacity->setValue(image.opacity);
        row.percent->setText(MainWindowUI::themeImagePercentText(image.opacity));
        const QString name = image.name.isEmpty() ? QFileInfo(image.path).fileName() : image.name;
        const QString imageText = has ? tr("当前图片：%1").arg(name) : MainWindowUI::themeImageNoneText();
        row.state->setText(imageText);
        row.state->setToolTip(has ? imageText : QString());
        row.fit->setEnabled(has);
        row.opacity->setEnabled(has);
        row.percent->setEnabled(has);
        row.clear->setEnabled(has);
    }
}

void ThemeSettingsController::refresh() {
    const Theme::Spec &spec = Theme::current();
    if (auto *editor = m_ui->getThemeAdvancedEditor()) editor->setSpec(spec);
    if (QComboBox *combo = m_ui->getThemePresetCombo()) {
        const QSignalBlocker block(combo);
        const int index = combo->findData(spec.presetId);
        if (index >= 0) combo->setCurrentIndex(index);
    }
    if (QPushButton *accent = m_ui->getThemeAccentButton())
        Theme::markColorSwatch(accent, spec.accent);
    if (QSlider *slider = m_ui->getThemeTextOpacitySlider()) {
        const QSignalBlocker block(slider);
        slider->setValue(spec.textOpacity);
        if (QLabel *percent = m_ui->getThemeTextOpacityPercent())
            percent->setText(MainWindowUI::themePercentText(spec.textOpacity));
    }
    for (int i = 0; i < MainWindowUI::themeSemanticCount(); ++i)
        if (QPushButton *swatch = m_ui->getThemeSemanticButton(i))
            Theme::markColorSwatch(swatch, semanticColor(spec, i));
    refreshImages();
}

void ThemeSettingsController::changePreset() {
    QComboBox *combo = m_ui->getThemePresetCombo();
    if (combo == nullptr || !Theme::isPresetId(combo->currentData().toString())) return;
    Theme::Spec spec = Theme::current();
    spec.presetId = combo->currentData().toString();
    spec.accent = Theme::tokens(spec.presetId).primary;
    applySpec(spec);
}

void ThemeSettingsController::chooseAccent() {
    const QColor picked = InlineMessage::pickColor(
        m_ui->overlay(), tr("主题色"),
        tr("顶栏、侧栏选中项那根竖条、确认性按钮（启动 / 下载 / 搜索那些）都用它。"),
        Theme::current().accent);
    if (!picked.isValid()) return;
    Theme::Spec spec = Theme::current();
    spec.accent = picked;
    applySpec(spec);
}

void ThemeSettingsController::resetAccent() {
    Theme::Spec spec = Theme::current();
    spec.accent = Theme::tokens(spec.presetId).primary;
    applySpec(spec);
}

void ThemeSettingsController::chooseSemantic(int index) {
    const Theme::Spec before = Theme::current();
    const QColor picked = InlineMessage::pickColor(
        m_ui->overlay(), MainWindowUI::themeSemanticText(index),
        tr("这一处红 / 黄 / 绿在界面上是哪儿（见下面那行说明）。"),
        semanticColor(before, index));
    if (!picked.isValid()) return;
    Theme::Spec spec = before;
    setSemanticColor(&spec, index, picked);
    applySpec(spec);
}

void ThemeSettingsController::resetSemantic() {
    Theme::Spec spec = Theme::current();
    spec.danger = spec.warning = spec.ok = spec.errorText = QColor();
    applySpec(spec);
}

void ThemeSettingsController::resetAll() {
    if (!InlineMessage::question(
            m_ui->overlay(), tr("恢复全部默认"),
            tr("将配色、区域和控件样式覆盖、文字不透明度、语义色及背景图恢复默认设置。自定义内容将被清除。"),
            tr("恢复"), tr("取消"), false, true)) return;
    for (int i = 0; i < MainWindowUI::themeImageRowCount(); ++i) {
        const QString key = m_ui->getThemeImageRow(i).partKey;
        if (!key.isEmpty()) ThemeImage::remove(key);
    }
    applySpec(Theme::defaultSpec());
}

void ThemeSettingsController::exportTheme() {
    const QString suggested = QDir(QStandardPaths::writableLocation(
                                       QStandardPaths::DocumentsLocation))
                                  .filePath(QStringLiteral("wtfml-theme.json"));
    const QString target = QFileDialog::getSaveFileName(
        m_dialogParent, tr("导出主题"), suggested, tr("主题文件 (*.json);;所有文件 (*)"));
    if (target.isEmpty()) return;
    // 写侧走 `QSaveFile`：导出目标可能是用户已有的那一份主题文件，写到一半失败
    // （盘满 / 进程被杀）不该把原来那份完整可用的留下半截。
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) {
        InlineMessage::warning(m_ui->overlay(), tr("导出失败"),
                               tr("无法写入文件：\n%1\n\n%2")
                                   .arg(QDir::toNativeSeparators(target), file.errorString()));
        return;
    }
    const QByteArray json = QJsonDocument(Theme::specToJson(Theme::current()))
                                .toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size()) {
        file.cancelWriting();
        InlineMessage::warning(m_ui->overlay(), tr("导出失败"),
                               tr("文件写入不完整：\n%1").arg(file.errorString()));
        return;
    }
    if (!file.commit()) {
        InlineMessage::warning(m_ui->overlay(), tr("导出失败"),
                               tr("无法保存文件：\n%1\n\n%2")
                                   .arg(QDir::toNativeSeparators(target), file.errorString()));
        return;
    }
    InlineMessage::information(m_ui->overlay(), tr("导出完成"),
                               tr("主题已导出到：\n%1").arg(QDir::toNativeSeparators(target)));
}

void ThemeSettingsController::importTheme() {
    const QString source = QFileDialog::getOpenFileName(
        m_dialogParent, tr("导入主题"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        tr("主题文件 (*.json);;所有文件 (*)"));
    if (source.isEmpty()) return;
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) {
        InlineMessage::warning(m_ui->overlay(), tr("导入失败"),
                               tr("读不了这个文件：\n%1\n\n%2")
                                   .arg(QDir::toNativeSeparators(source), file.errorString()));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    Theme::Spec spec;
    QString error;
    if (document.isNull() || !document.isObject()
        || !Theme::specFromJson(document.object(), &spec, &error)) {
        const QString why = document.isNull() || !document.isObject()
                                ? parseError.errorString() : error;
        InlineMessage::warning(m_ui->overlay(), tr("导入失败"),
                               tr("这不是一份主题文件：\n%1\n\n%2")
                                   .arg(QDir::toNativeSeparators(source), why));
        return;
    }
    applySpec(spec);
    QStringList missing;
    for (int i = 0; i < MainWindowUI::themeImageRowCount(); ++i) {
        Theme::Part part;
        if (!Theme::partFromKey(m_ui->getThemeImageRow(i).partKey, &part)) continue;
        const Theme::PartImage image = Theme::partImage(part);
        if (image.isSet() && !QFileInfo::exists(image.path)) missing.append(Theme::partName(part));
    }
    InlineMessage::information(
        m_ui->overlay(), tr("导入完成"),
        missing.isEmpty() ? tr("主题已应用。")
                          : tr("主题配色已应用。主题文件仅保存背景图路径，以下文件在本机未找到：\n%1")
                                .arg(missing.join(QLatin1Char('\n'))));
}

void ThemeSettingsController::changeTextOpacity(int value) {
    Theme::Spec spec = Theme::current();
    spec.textOpacity = value;
    applySpecThrottled(spec);
}
