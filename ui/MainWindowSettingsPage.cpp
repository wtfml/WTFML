// 设置页：顶部 4 个分段页签（启动 / 提示 / 个性化 / 下载）+ 单栏表单（最宽 720，居中）。
// 每个分组 = 小标题 + 一层极淡分组底（方案 B）；行 = 左标签右 220px 控件列，内容宽 < 600 时上下叠放；
// 输入失焦或回车即存，右上淡出「已保存」；少用项收进每页底部「高级」展开行。
// 部件在 SettingsPageParts.*；这里只摆控件。
#include "MainWindowUI.hpp"
#include "HomeQuickLimit.hpp"
#include <QCoreApplication>
#include "InputSizing.hpp"

#include "Animations.hpp"
#include "ElidedLabel.hpp"
#include "Glass.hpp"
#include "MemorySlider.hpp"
#include "SettingsPageParts.hpp"
#include "Theme.hpp"
#include "ThemeAdvancedEditor.hpp"
#include "ThemeBackground.hpp"
#include "TitleBar.hpp"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QAbstractButton>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPalette>
#include <QPointer>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include "MainWindowPageLayout.hpp"
using namespace MainWindowPageLayout;
using SettingsParts::ExpandSection;
using SettingsParts::Placement;
using SettingsParts::RowSpec;
using SettingsParts::SettingsGroup;

QWidget *MainWindowUI::setupSettingsTab() {
    QWidget *settingsTab = new ThemeBackground(Theme::Part::PageBackground, tabWidget);
    settingsTab->setObjectName(settingsTabName());
    setupSettingsPage(settingsTab);
    return settingsTab;
}

namespace {

/// 单击（按下与松开基本在同一点，拖选文字不算）标签 = 点一下那颗按钮。
class LabelClickToButton : public QObject {
public:
    LabelClickToButton(QObject *parent, QAbstractButton *button) : QObject(parent), m_button(button) {}

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease) return false;
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton) return false;
        if (event->type() == QEvent::MouseButtonPress) {
            m_press = mouse->position().toPoint();
        } else if (m_button != nullptr && (mouse->position().toPoint() - m_press).manhattanLength() <= 4) {
            m_button->click();
        }
        return false;
    }

private:
    QPointer<QAbstractButton> m_button;
    QPoint m_press;
};

/// 页面尺寸变化时把「已保存」贴回右上角。
class AnchorFilter : public QObject {
public:
    AnchorFilter(QObject *parent, SettingsParts::SavedToast *toast) : QObject(parent), m_toast(toast) {}

protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Resize && m_toast != nullptr) QTimer::singleShot(0, m_toast, [t = m_toast] { t->anchor(); });
        return false;
    }

private:
    QPointer<SettingsParts::SavedToast> m_toast;
};

struct SettingsPanel {
    QScrollArea *scroll = nullptr;
    QWidget *form = nullptr;
    QVBoxLayout *layout = nullptr;
};

constexpr int kSidePadding = 28;

/// 往堆叠页加一栏：一层滚动区（滚动条走现有动效），里面一块最宽 720、在窗口里居中的表单。
/// 加进去的顺序就是页签的顺序。
SettingsPanel addSettingsPanel(QStackedWidget *stack, const QString &key) {
    SettingsPanel panel;
    panel.scroll = new Theme::GutterScrollArea(stack);
    panel.scroll->setObjectName(MainWindowUI::settingsPanelName(key));
    panel.scroll->setWidgetResizable(true);
    panel.scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    panel.scroll->setFrameShape(QFrame::NoFrame);
    // 两侧留白放在 holder 上、表单本身限宽并居中（页头同样居中，左缘 = 页标题左缘）：窄窗时表单收窄，不会横向溢出。
    auto *holder = new QWidget(panel.scroll);
    holder->setObjectName(QStringLiteral("settingsHolder-") + key);
    auto *holderLayout = new QHBoxLayout(holder);
    holderLayout->setContentsMargins(Theme::pageTitleInsetLeft(), 4, kSidePadding, 40);
    panel.form = new QWidget(holder);
    panel.form->setObjectName(MainWindowUI::settingsFormName(key));
    panel.form->setMaximumWidth(MainWindowUI::settingsFormMaxWidth());
    panel.form->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // 居中靠两侧弹簧，不靠对齐（带对齐的子项只拿 sizeHint 宽，表单会缩成内容那么窄）。
    holderLayout->addStretch(1);
    holderLayout->addWidget(panel.form, 1000);
    holderLayout->addStretch(1);
    panel.layout = new QVBoxLayout(panel.form);
    panel.layout->setContentsMargins(0, 0, 0, 0);
    panel.layout->setSpacing(22);
    panel.scroll->setWidget(holder);
    stack->addWidget(panel.scroll);
    return panel;
}

SettingsGroup *addGroup(const SettingsPanel &panel, const QString &title) {
    auto *group = new SettingsGroup(title, panel.form);
    panel.layout->addWidget(group);
    return group;
}

SettingsParts::SettingsRow *addRow(SettingsGroup *group, const QString &label, QWidget *control,
                                   Placement placement = Placement::Column, const QString &help = QString(),
                                   const QString &tip = QString()) {
    return group->addRow(RowSpec{label, help, tip, placement}, control);
}

/// 开关行：勾选框自己画成 40×22 的开关，标签在左。
SettingsParts::SettingsRow *addSwitch(SettingsGroup *group, QCheckBox *check, const QString &label,
                                      const QString &help = QString(), const QString &tip = QString()) {
    Theme::markSwitch(check);
    check->setAccessibleDescription(help.isEmpty() ? tip : help);  // 给读屏与测试：这一项的按需说明
    return addRow(group, label, check, Placement::Natural, help, tip);
}

/// 「滑块 + 数值」：滑块吃剩余宽度，数值 44px 右对齐。
QWidget *sliderWithValue(QSlider *slider, QLabel *value, QWidget *parent) {
    auto *box = new QWidget(parent);
    auto *layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    slider->setMinimumWidth(0);
    slider->setMaximumWidth(QWIDGETSIZE_MAX);
    slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(slider, 1);
    value->setFixedWidth(44);
    value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont font = value->font();
    font.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
    value->setFont(font);
    markTextRole(value, Theme::secondaryTextRoleValue());
    layout->addWidget(value, 0);
    return box;
}

/// 按行摆一串控件（无外边距横排）。
QWidget *hbox(QWidget *parent, const QList<QWidget *> &items, int spacing = 8) {
    auto *box = new QWidget(parent);
    auto *layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(spacing);
    for (QWidget *item : items) layout->addWidget(item);
    return box;
}

QPushButton *textButton(const QString &text, QWidget *parent) {
    auto *button = new QPushButton(text, parent);
    Theme::markButton(button, Theme::ButtonKind::Ghost);
    button->setCursor(Qt::PointingHandCursor);
    QFont font = QApplication::font();  // 按钮用系统字（手写体只留品牌 / 主页大标题 / 页标题 / 空状态）
    font.setPixelSize(Theme::textSize(Theme::TextRole::Body));
    font.setWeight(QFont::Medium);
    button->setFont(font);
    return button;
}

}  // namespace

void MainWindowUI::setupSettingsPage(QWidget *page) {
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    // ---- 页头：标题「设置」 + 4 个分段页签 + 右侧搜索（最宽 720，和表单同宽、同样居中） ----
    auto *header = new QWidget(page);
    header->setObjectName(pageHeaderName(QStringLiteral("settings")));
    auto *headerOuter = new QHBoxLayout(header);
    // 右边多让出滚动条那一格：表单在视口里居中，视口比页窄这一格，页头这样才和表单对齐。
    headerOuter->setContentsMargins(Theme::pageTitleInsetLeft(), Theme::pageTitleInsetTop(),
                                    kSidePadding + Theme::scrollBarThickness(), 14);
    auto *headInner = new QWidget(header);
    headInner->setMaximumWidth(settingsFormMaxWidth());
    headInner->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    headerOuter->addStretch(1);
    headerOuter->addWidget(headInner, 1000);
    headerOuter->addStretch(1);
    auto *headLayout = new QHBoxLayout(headInner);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(20);

    auto *title = new QLabel(settingsPageTitle(), headInner);
    title->setObjectName(pageTitleName(QStringLiteral("settings")));
    markTextRole(title, Theme::pageTitleTextRoleValue());
    Theme::markDisplay(title, Theme::textFont(title->font(), Theme::TextRole::Display));  // 页标题：手写体 Light 26，字距 2
    headLayout->addWidget(title);

    settingsTabs = new SettingsParts::SlidingTabs(headInner);
    settingsTabs->setObjectName(settingsTabsName());
    for (const QString &text : settingsCategoryTexts()) settingsTabs->addTab(text);
    headLayout->addWidget(settingsTabs);
    headLayout->addStretch(1);

    auto *search = new QLineEdit(headInner);
    search->setObjectName(settingsSearchEditName());
    search->setPlaceholderText(QCoreApplication::translate("MainWindowSettingsPage", "搜索设置"));
    search->setClearButtonEnabled(true);
    search->setMinimumWidth(120);
    search->setMaximumWidth(240);
    search->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    addSearchIcon(search);
    headLayout->addWidget(search);
    pageLayout->addWidget(header);

    // ---- 页体：四页堆叠，各自一层滚动区 ----
    settingsCategoryStack = new QStackedWidget(page);
    settingsCategoryStack->setObjectName(settingsCategoryStackName());
    pageLayout->addWidget(settingsCategoryStack, 1);
    // 加页顺序 = settingsCategoryKeys() 的顺序 = 页签顺序（ui_check 逐页核）。
    setupSettingsLaunchPanel(settingsCategoryStack);
    setupSettingsWarnPanel(settingsCategoryStack);
    setupSettingsLookPanel(settingsCategoryStack);
    setupSettingsDownloadPanel(settingsCategoryStack);

    QObject::connect(settingsTabs, &SettingsParts::SlidingTabs::currentChanged, settingsCategoryStack,
                     [this](int index) {
                         if (index >= 0 && index < settingsCategoryStack->count())
                             settingsCategoryStack->setCurrentIndex(index);
                     });

    // 设置页里的折叠行（「高级」和高级配色那一行）：标题靠左、整行 44 高。
    // 应用级样式表没写 text-align，这里用实例级样式只作用于折叠行按钮。
    for (QPushButton *fold : settingsCategoryStack->findChildren<QPushButton *>()) {
        if (!fold->property("wtfmlFoldRow").toBool()) continue;
        fold->setStyleSheet(QStringLiteral(
            "QPushButton[wtfmlFoldRow=\"true\"] { text-align: left; padding-left: 14px; min-height: 44px; max-height: 44px; }"));
        fold->setFixedHeight(44);
    }

    // 「账号与外观」窗的入口只在主页右上账号胶囊的浮层里；设置里不再有账户一节。
    // 这颗按钮留作隐藏的兼容入口（MainWindow / 验证工程按名字取它，直通同一个窗）。
    accountSkinSettingsButton = new QPushButton(accountSkinButtonText(), page);
    accountSkinSettingsButton->setObjectName(accountSkinSettingsButtonName());
    accountSkinSettingsButton->hide();

    // ---- 「已保存」：页面右上角；任一设置被用户改过后淡入 ----
    settingsSavedToast = new SettingsParts::SavedToast(page);
    page->installEventFilter(new AnchorFilter(page, settingsSavedToast));
    SettingsParts::watchSaved(settingsCategoryStack, [toast = QPointer<SettingsParts::SavedToast>(settingsSavedToast)] {
        if (toast) toast->ping();
    });

    // ---- 搜索：按行名 / 分组名找，切到那一页并滚到那一行 ----
    const auto findSetting = [this](const QString &text) {
        const QString needle = text.trimmed();
        if (needle.isEmpty()) return;
        for (int i = 0; i < settingsCategoryStack->count(); ++i) {
            for (QLabel *label : settingsCategoryStack->widget(i)->findChildren<QLabel *>()) {
                const bool name = label->property("settingsItemLabel").toBool() ||
                                  label->property(Theme::textRolePropertyName().toUtf8().constData()).toString() ==
                                      Theme::sectionTitleTextRoleValue();
                if (!name || !label->text().contains(needle, Qt::CaseInsensitive)) continue;
                showSettingsCategory(settingsCategoryKeys().value(i), label->text());
                return;
            }
        }
    };
    QObject::connect(search, &QLineEdit::textChanged, search, findSetting);
    QObject::connect(search, &QLineEdit::returnPressed, search, [search, findSetting] { findSetting(search->text()); });
    settingsTabs->setCurrentIndex(0);
    settingsCategoryStack->setCurrentIndex(0);
}

bool MainWindowUI::showSettingsCategory(const QString &key, const QString &rowLabel) {
    const int index = settingsCategoryKeys().indexOf(key);
    if (index < 0 || settingsTabs == nullptr || settingsCategoryStack == nullptr) return false;
    settingsTabs->setCurrentIndex(index);
    settingsCategoryStack->setCurrentIndex(index);
    if (rowLabel.isEmpty()) return true;
    auto *scroll = qobject_cast<QScrollArea *>(settingsCategoryStack->widget(index));
    if (scroll == nullptr) return false;
    for (QLabel *label : scroll->findChildren<QLabel *>()) {
        const bool name = label->property("settingsItemLabel").toBool() ||
                          label->property(Theme::textRolePropertyName().toUtf8().constData()).toString() ==
                              Theme::sectionTitleTextRoleValue();
        if (!name || label->text() != rowLabel) continue;
        // 行在「高级」里：先展开。
        for (QWidget *w = label->parentWidget(); w != nullptr; w = w->parentWidget()) {
            if (auto *section = qobject_cast<ExpandSection *>(w)) section->setExpanded(true, false);
        }
        QWidget *target = label;
        while (target->parentWidget() != nullptr && !target->property("settingsItemRow").toBool() &&
               target->property("settingsSectionTitle").isNull())
            target = target->parentWidget();
        QTimer::singleShot(0, scroll, [scroll, target] { scroll->ensureWidgetVisible(target, 0, 24); });
        return true;
    }
    return false;
}

void MainWindowUI::setupSettingsLaunchPanel(QStackedWidget *stack) {
    const SettingsPanel panel = addSettingsPanel(stack, QStringLiteral("launch"));
    QWidget *form = panel.form;

    // ---------- 运行时 ----------
    SettingsGroup *runtime = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "运行时"));
    javaManageButton = textButton(QCoreApplication::translate("MainWindowSettingsPage", "Java 管理…"), form);
    javaManageButton->setObjectName(javaManageButtonName());
    runtime->setHeaderAction(javaManageButton);
    javaPathLabel = new QLabel(form);
    javaPathLabel->setObjectName(javaPathLabelName());
    javaPathLabel->setWordWrap(true);
    // 控件外观（淡底圆角，同输入框一族），点一下 = 点「Java 管理…」。路径仍可框选复制，
    // 单击（没拖动）才算点：按下与松开在同一点。
    javaPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    javaPathLabel->setProperty("wtfmlFieldLabel", true);
    javaPathLabel->setCursor(Qt::PointingHandCursor);
    javaPathLabel->setToolTip(QCoreApplication::translate("MainWindowSettingsPage", "点击打开 Java 管理"));
    javaPathLabel->installEventFilter(new LabelClickToButton(javaPathLabel, javaManageButton));
    // 全局这一层挑中的 Java 对不上时直接报错，不替用户换也不问；实例层单独钉的 Java 对不上时会弹框问
    // 要不要改用匹配的（开关在「提示」页）。
    addRow(runtime, QCoreApplication::translate("MainWindowSettingsPage", "Java 版本"), javaPathLabel, Placement::Wide, QString(),
           QCoreApplication::translate("MainWindowSettingsPage", "自动选择符合游戏版本要求的 Java；可在实例设置中单独指定。"));

    // ---------- 内存 ----------
    SettingsGroup *memory = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "内存"));
    memoryModeCombo = new QComboBox(form);
    memoryModeCombo->setObjectName(memoryModeComboName());
    memoryModeCombo->addItem(QCoreApplication::translate("MainWindowSettingsPage", "自动分配"), true);
    memoryModeCombo->addItem(QCoreApplication::translate("MainWindowSettingsPage", "手动分配"), false);
    memoryModeCombo->setCurrentIndex(1);
    WheelGuard::install(memoryModeCombo);
    addRow(memory, QCoreApplication::translate("MainWindowSettingsPage", "分配方式"), memoryModeCombo);
    {
        // 「最大内存」= 一根条 + 一个数字框（条是主入口，框用来打精确值）。
        auto *box = new QWidget(form);
        auto *boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(0, 0, 0, 0);
        boxLayout->setSpacing(4);
        auto *line = new QWidget(box);
        auto *lineLayout = new QHBoxLayout(line);
        lineLayout->setContentsMargins(0, 0, 0, 0);
        lineLayout->setSpacing(12);
        memoryMaxSlider = new MemorySlider(line);
        memoryMaxSlider->setObjectName(memoryMaxSliderName());
        // 上限先用内置常数；真跑时 MainWindow 按本机物理内存再收口（界面层不读系统内存）。
        memoryMaxSlider->setRangeMb(memoryMaxMinMb(), memoryLimitMaxMb());
        memoryMaxSlider->setMinimumWidth(0);
        WheelGuard::install(memoryMaxSlider);
        lineLayout->addWidget(memoryMaxSlider, 1);
        memoryMaxSpin = new QSpinBox(line);
        memoryMaxSpin->setObjectName(memoryMaxSpinName());
        memoryMaxSpin->setRange(memoryMaxMinMb(), memoryLimitMaxMb());
        memoryMaxSpin->setSingleStep(memoryStepMb());
        memoryMaxSpin->setSuffix(QStringLiteral(" MB"));
        memoryMaxSpin->setFixedWidth(kMemorySpinWidth);
        WheelGuard::install(memoryMaxSpin);
        lineLayout->addWidget(memoryMaxSpin, 0);
        boxLayout->addWidget(line);
        // 文字由 MainWindow 填（系统总内存 / 可用 / 当前用量）；先给占位，免得空标签把行高压成 0。
        // 说明可能长到一行放不下：换行显示整句，不用省略号截掉。
        auto *hint = new QLabel(box);
        hint->setText(QCoreApplication::translate("MainWindowSettingsPage", "正在读取系统内存…"));
        hint->setWordWrap(true);
        QFont hintFont = hint->font();
        hintFont.setPixelSize(Theme::textSize(Theme::TextRole::Caption));
        hint->setFont(hintFont);
        markTextRole(hint, Theme::secondaryTextRoleValue());
        hint->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        hint->setObjectName(memoryHintName());
        boxLayout->addWidget(hint);
        memoryHint = hint;
        addRow(memory, QCoreApplication::translate("MainWindowSettingsPage", "最大内存"), box, Placement::Wide);
    }
    // 「最小内存」（-Xms）不在界面上：值照旧存在设置里（默认 1024 MB，命令行照发）。

    // ---------- 游戏窗口 ----------
    SettingsGroup *windowGroup = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "游戏窗口"));
    gameWindowModeCombo = new QComboBox(form);
    gameWindowModeCombo->setObjectName(gameWindowModeComboName());
    WheelGuard::install(gameWindowModeCombo);
    addRow(windowGroup, QCoreApplication::translate("MainWindowSettingsPage", "窗口模式"), gameWindowModeCombo);
    {
        gameWindowWidthSpin = new QSpinBox(form);
        gameWindowWidthSpin->setObjectName(gameWindowWidthSpinName());
        gameWindowWidthSpin->setRange(windowMinWidth(), windowMaxWidth());
        gameWindowWidthSpin->setSingleStep(windowSizeStep());
        gameWindowWidthSpin->setSuffix(QStringLiteral(" px"));
        gameWindowWidthSpin->setFixedWidth(kWindowSizeSpinWidth + 22);
        WheelGuard::install(gameWindowWidthSpin);
        gameWindowSizeTimesLabel = new QLabel(QStringLiteral("×"), form);
        gameWindowSizeTimesLabel->setObjectName(gameWindowSizeTimesLabelName());
        gameWindowHeightSpin = new QSpinBox(form);
        gameWindowHeightSpin->setObjectName(gameWindowHeightSpinName());
        gameWindowHeightSpin->setRange(windowMinHeight(), windowMaxHeight());
        gameWindowHeightSpin->setSingleStep(windowSizeStep());
        gameWindowHeightSpin->setSuffix(QStringLiteral(" px"));
        gameWindowHeightSpin->setFixedWidth(kWindowSizeSpinWidth + 22);
        WheelGuard::install(gameWindowHeightSpin);
        // 「恢复默认」（UI 问题 #11）：回到出厂的那一对 854×480。风格、尺寸照 155 那颗
        // （设置·个性化·外观里的「窗口大小 · 恢复默认」），摆在这一行最右边。
        gameWindowSizeResetButton = new QPushButton(QCoreApplication::translate("MainWindowSettingsPage", "恢复默认"), form);
        gameWindowSizeResetButton->setObjectName(gameWindowSizeResetButtonName());
        Theme::markButton(gameWindowSizeResetButton, Theme::ButtonKind::Neutral);
        // 点一下就把两个框设回默认值。**刻意不挡信号**：这两个框的 valueChanged 就是落盘那条线
        // （MainWindow::onGameWindowSizeChanged），挡掉就成了"框里变了、盘上没变"。
        // 一次点两框会写两趟，值是同一个终态，代价可以接受。
        QObject::connect(gameWindowSizeResetButton, &QPushButton::clicked, gameWindowSizeResetButton, [this] {
            gameWindowWidthSpin->setValue(windowDefaultWidth());
            gameWindowHeightSpin->setValue(windowDefaultHeight());
        });
        // 值一变就重算置灰。窗口模式（最大化）那一侧由 MainWindow 调 syncGameWindowSizeResetButton()。
        const auto syncReset = [this](int) { syncGameWindowSizeResetButton(); };
        QObject::connect(gameWindowWidthSpin, &QSpinBox::valueChanged, gameWindowWidthSpin, syncReset);
        QObject::connect(gameWindowHeightSpin, &QSpinBox::valueChanged, gameWindowHeightSpin, syncReset);
        addRow(windowGroup, QCoreApplication::translate("MainWindowSettingsPage", "窗口大小"),
               hbox(form, {gameWindowWidthSpin, gameWindowSizeTimesLabel, gameWindowHeightSpin,
                           gameWindowSizeResetButton}, kWindowSizeRowSpacing),
               Placement::Natural, QCoreApplication::translate("MainWindowSettingsPage", "游戏可能沿用自身保存的尺寸。"),
               QCoreApplication::translate("MainWindowSettingsPage", "退出全屏后使用此尺寸。"));
        syncGameWindowSizeResetButton();
    }

    // ---------- 运行与日志 ----------
    SettingsGroup *runGroup = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "运行与日志"));
    launcherBehaviourCombo = new QComboBox(form);
    launcherBehaviourCombo->setObjectName(launcherBehaviourComboName());
    WheelGuard::install(launcherBehaviourCombo);
    addRow(runGroup, QCoreApplication::translate("MainWindowSettingsPage", "启动后"), launcherBehaviourCombo, Placement::Column,
           QCoreApplication::translate("MainWindowSettingsPage", "收起仅隐藏窗口，游戏不受影响。"));
    showGameLogCheck = new QCheckBox(showGameLogCheckText(), form);
    showGameLogCheck->setObjectName(showGameLogCheckName());
    addSwitch(runGroup, showGameLogCheck, showGameLogRowText(), QCoreApplication::translate("MainWindowSettingsPage", "关闭后仍写入日志文件。"));
    processPriorityCombo = new QComboBox(form);
    processPriorityCombo->setObjectName(processPriorityComboName());
    WheelGuard::install(processPriorityCombo);
    addRow(runGroup, QCoreApplication::translate("MainWindowSettingsPage", "进程优先级"), processPriorityCombo, Placement::Column,
           QCoreApplication::translate("MainWindowSettingsPage", "提高进程优先级可能影响其他程序。"));

    // ---------- 高级：JVM 参数 ----------
    auto *advanced = new ExpandSection(QCoreApplication::translate("MainWindowSettingsPage", "高级"), form);
    advanced->setObjectName(QStringLiteral("settingsAdvanced-launch"));
    panel.layout->addWidget(advanced);
    SettingsGroup *jvm = new SettingsGroup(QCoreApplication::translate("MainWindowSettingsPage", "JVM 参数"), advanced);
    advanced->bodyLayout()->addWidget(jvm);
    extraJvmArgsEdit = new QPlainTextEdit(form);
    extraJvmArgsEdit->setObjectName(extraJvmArgsEditName());
    extraJvmArgsEdit->setPlaceholderText(QStringLiteral("-XX:+UseG1GC -XX:MaxGCPauseMillis=50"));
    extraJvmArgsEdit->setFixedHeight(kJvmArgsEditHeight);
    {
        QFont mono(QStringLiteral("Cascadia Mono"));
        mono.setStyleHint(QFont::Monospace);
        mono.setPixelSize(13);
        extraJvmArgsEdit->setFont(mono);
    }
    // 失焦（或 Ctrl+Enter）即存：边打字边存会把半条参数写进命令行。
    extraJvmArgsCommit = new SettingsParts::CommitOnFocusOut(extraJvmArgsEdit);
    addRow(jvm, QCoreApplication::translate("MainWindowSettingsPage", "全局额外参数"), extraJvmArgsEdit, Placement::Wide,
           QCoreApplication::translate("MainWindowSettingsPage", "实例参数优先于全局。"));
    panel.layout->addStretch(1);
}

void MainWindowUI::setupSettingsWarnPanel(QStackedWidget *stack) {
    const SettingsPanel panel = addSettingsPanel(stack, QStringLiteral("warn"));
    QWidget *form = panel.form;

    // 前三条都挡在「点启动之后、游戏起来之前」：挡的是同一份存档被两个进程同时打开，事后无法补救，
    // 所以默认都开。勾选状态由 MainWindow 从盘上灌。
    SettingsGroup *launch = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "启动前"));
    javaWarnMismatchCheck = new QCheckBox(javaWarnMismatchRowText(), form);
    javaWarnMismatchCheck->setObjectName(javaWarnMismatchCheckName());
    addSwitch(launch, javaWarnMismatchCheck, javaWarnMismatchRowText(), QCoreApplication::translate("MainWindowSettingsPage", "停用提醒后，Java 与游戏版本不兼容时仍可能启动失败。"));
    warnDuplicateLaunchCheck = new QCheckBox(warnDuplicateLaunchCheckText(), form);
    warnDuplicateLaunchCheck->setObjectName(warnDuplicateLaunchCheckName());
    addSwitch(launch, warnDuplicateLaunchCheck, warnDuplicateLaunchRowText(),
              QCoreApplication::translate("MainWindowSettingsPage", "10 秒内再次启动同一实例时询问。"));
    warnWorldLossCheck = new QCheckBox(warnWorldLossCheckText(), form);
    warnWorldLossCheck->setObjectName(warnWorldLossCheckName());
    addSwitch(launch, warnWorldLossCheck, warnWorldLossRowText(), QCoreApplication::translate("MainWindowSettingsPage", "同时运行可能损坏存档。"));

    SettingsGroup *download = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "下载时"));
    warnMissingDependenciesCheck = new QCheckBox(warnMissingDependenciesCheckText(), form);
    warnMissingDependenciesCheck->setObjectName(warnMissingDependenciesCheckName());
    addSwitch(download, warnMissingDependenciesCheck, warnMissingDependenciesRowText(),
              QCoreApplication::translate("MainWindowSettingsPage", "关闭依赖检查后，安装的模组可能因缺少依赖而无法运行。"));
    panel.layout->addStretch(1);
}

void MainWindowUI::setupSettingsLookPanel(QStackedWidget *stack) {
    const SettingsPanel panel = addSettingsPanel(stack, QStringLiteral("look"));
    QWidget *form = panel.form;

    // ---------- 主题 ----------
    // 这里只搭控件。改完立刻生效也立刻落盘（改规格 → 落盘 → Theme::apply()）在 MainWindow::applyThemeSpec()。
    SettingsGroup *theme = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "主题"));
    themePresetCombo = new QComboBox(form);
    themePresetCombo->setObjectName(themePresetComboName());
    for (const QString &id : Theme::presetIds()) themePresetCombo->addItem(Theme::presetName(id), id);
    addRow(theme, themePresetRowText(), themePresetCombo, Placement::Column, QString(),
           QCoreApplication::translate("MainWindowSettingsPage", "可选择固定主题或跟随系统。"));
    {
        // 磨砂：总开关 + 材质（亚克力 / 云母 / 风景底）+ 两条强度。关掉 = 纯色令牌。
        auto *glass = new QCheckBox(QCoreApplication::translate("MainWindowSettingsPage", "磨砂效果"), form);
        glass->setObjectName(glassEffectCheckName());
        glass->setChecked(Glass::mode() != Glass::Mode::Solid);
        addSwitch(theme, glass, QCoreApplication::translate("MainWindowSettingsPage", "磨砂效果"), QCoreApplication::translate("MainWindowSettingsPage", "关闭可降低显卡占用。"));

        auto *materialBox = new QWidget(form);
        auto *materialLayout = new QVBoxLayout(materialBox);
        materialLayout->setContentsMargins(0, 0, 0, 0);
        materialLayout->setSpacing(4);
        auto *material = new QWidget(materialBox);
        material->setObjectName(QStringLiteral("backdropModeSegments"));
        auto *segLayout = new QHBoxLayout(material);
        segLayout->setContentsMargins(3, 3, 3, 3);
        segLayout->setSpacing(2);
        QList<QPushButton *> segments;
        for (Glass::Mode m : {Glass::Mode::Acrylic, Glass::Mode::Mica, Glass::Mode::Scene}) {
            auto *segment = new QPushButton(Glass::modeShortName(m), material);
            segment->setProperty("glassMode", int(m));
            segLayout->addWidget(segment);
            segments.append(segment);
        }
        Theme::markSegmented(material, segments);
        const auto currentMode = [segments]() {
            for (QPushButton *segment : segments)
                if (segment->isChecked()) return Glass::Mode(segment->property("glassMode").toInt());
            return Glass::Mode::Scene;
        };
        bool picked = false;
        for (QPushButton *segment : segments) {
            if (segment->property("glassMode").toInt() == int(Glass::mode())) {
                segment->setChecked(true);
                picked = true;
            }
        }
        if (!picked) segments.last()->setChecked(true);
        material->setEnabled(glass->isChecked());
        auto *reason = new QLabel(materialBox);
        reason->setObjectName(QStringLiteral("backdropFallbackLabel"));
        reason->setWordWrap(true);
        const auto refreshReason = [reason]() {
            QPalette p = reason->palette();
            p.setColor(QPalette::WindowText, p.color(QPalette::PlaceholderText));
            reason->setPalette(p);
            reason->setText(Glass::fallbackReason());
            reason->setVisible(!reason->text().isEmpty());
        };
        const auto apply = [glass, material, currentMode, refreshReason]() {
            material->setEnabled(glass->isChecked());
            Glass::setMode(glass->isChecked() ? currentMode() : Glass::Mode::Solid);
            refreshReason();
        };
        QObject::connect(glass, &QCheckBox::toggled, glass, apply);
        for (QPushButton *segment : segments) QObject::connect(segment, &QPushButton::clicked, segment, apply);
        refreshReason();
        Theme::follow(reason, refreshReason);
        materialLayout->addWidget(material);
        materialLayout->addWidget(reason);
        addRow(theme, QCoreApplication::translate("MainWindowSettingsPage", "材质"), materialBox, Placement::Column, QCoreApplication::translate("MainWindowSettingsPage", "亚克力、云母需 Windows 11。"));
    }
    {
        // 着色 / 模糊强度：拖完才存（拖的过程中不重建）。
        const auto makeStrength = [form](const QString &name, int value, auto setter) {
            auto *slider = new QSlider(Qt::Horizontal, form);
            slider->setObjectName(name);
            slider->setRange(0, 100);
            slider->setValue(value);
            WheelGuard::install(slider);
            QObject::connect(slider, &QSlider::sliderReleased, slider, [slider, setter]() { setter(slider->value()); });
            QObject::connect(slider, &QSlider::valueChanged, slider, [slider, setter](int v) {
                if (!slider->isSliderDown()) setter(v);
            });
            return slider;
        };
        const auto addStrength = [&](const QString &label, const QString &name, int value, auto setter,
                                     const QString &help, const QString &tip) {
            QSlider *slider = makeStrength(name, value, setter);
            auto *valueLabel = new QLabel(themePercentText(value), form);
            valueLabel->setObjectName(name + QStringLiteral("Value"));
            QObject::connect(slider, &QSlider::valueChanged, valueLabel,
                             [valueLabel](int v) { valueLabel->setText(themePercentText(v)); });
            addRow(theme, label, sliderWithValue(slider, valueLabel, form), Placement::Column, help, tip);
        };
        addStrength(QCoreApplication::translate("MainWindowSettingsPage", "着色强度"), QStringLiteral("backdropTintSlider"), Glass::tintStrength(),
                    [](int v) { Glass::setTintStrength(v); }, QCoreApplication::translate("MainWindowSettingsPage", "过淡时自动加深以保证可读。"), QString());
        addStrength(QCoreApplication::translate("MainWindowSettingsPage", "模糊强度"), QStringLiteral("backdropBlurSlider"), Glass::blurStrength(),
                    [](int v) { Glass::setBlurStrength(v); }, QString(), QCoreApplication::translate("MainWindowSettingsPage", "仅风景底可调。"));
    }
    {
        // 主题色（标题栏 / 选中标记 / 主要操作按钮）：色块 + 恢复默认。
        themeAccentButton = new QPushButton(form);
        themeAccentButton->setObjectName(themeAccentButtonName());
        themeAccentButton->setFixedSize(themeSwatchWidth(), themeSwatchHeight());
        Theme::markColorSwatch(themeAccentButton, Theme::current().accent);
        themeAccentResetButton = new QPushButton(themeAccentResetText(), form);
        themeAccentResetButton->setObjectName(themeAccentResetName());
        Theme::markButton(themeAccentResetButton, Theme::ButtonKind::Ghost);
        themeAccentResetButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        addRow(theme, themeAccentRowText(), hbox(form, {themeAccentButton, themeAccentResetButton}, themeSwatchRowSpacing()),
               Placement::Natural);
    }
    {
        themeTextOpacitySlider = new QSlider(Qt::Horizontal, form);
        themeTextOpacitySlider->setObjectName(themeTextOpacitySliderName());
        themeTextOpacitySlider->setRange(0, 100);
        themeTextOpacitySlider->setValue(Theme::current().textOpacity);
        themeTextOpacityPercent = new QLabel(themePercentText(Theme::current().textOpacity), form);
        themeTextOpacityPercent->setObjectName(themeTextOpacityPercentName());
        addRow(theme, themeTextOpacityRowText(), sliderWithValue(themeTextOpacitySlider, themeTextOpacityPercent, form));
    }

    // ---------- 背景图（一块底一组） ----------
    // 挑图那一下不是记用户那个路径：文件会被拷进数据目录。这里只摆控件，收图和落盘在 MainWindow。
    const auto addImageGroup = [&](Theme::Part part, const QString &groupTitle) {
        const QString key = Theme::partKey(part);
        SettingsGroup *group = addGroup(panel, groupTitle);
        ThemeImageRow widgets;
        widgets.partKey = key;
        widgets.choose = new QPushButton(themeImageChooseText(), form);
        widgets.choose->setObjectName(themeImageChooseName(key));
        widgets.choose->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        widgets.clear = new QPushButton(themeImageClearText(), form);
        widgets.clear->setObjectName(themeImageClearName(key));
        Theme::markButton(widgets.clear, Theme::ButtonKind::Neutral);
        widgets.clear->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        widgets.state = new ElidedLabel(form);
        widgets.state->setText(themeImageNoneText());
        widgets.state->setObjectName(themeImageStateName(key));
        widgets.state->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *imageBox = new QWidget(form);
        auto *imageLayout = new QVBoxLayout(imageBox);
        imageLayout->setContentsMargins(0, 0, 0, 0);
        imageLayout->setSpacing(6);
        auto *buttons = hbox(imageBox, {widgets.choose, widgets.clear}, themeImageRowSpacing());
        static_cast<QHBoxLayout *>(buttons->layout())->addStretch(1);
        imageLayout->addWidget(buttons);
        imageLayout->addWidget(widgets.state);
        widgets.state->setParent(imageBox);
        // 「图片」这一行不给 ⓘ（UI 问题 #21）：原来那颗 i 的悬停内容是「哪几页铺这张图」，
        // 删掉之后那一句也不再传进来（`scope` 参数随之取消）。
        addRow(group, QCoreApplication::translate("MainWindowSettingsPage", "图片"), imageBox, Placement::Wide,
               QCoreApplication::translate("MainWindowSettingsPage", "会复制到启动器数据目录。"));

        widgets.fit = new QComboBox(form);
        widgets.fit->setObjectName(themeImageFitName(key));
        for (int i = 0; i < Theme::imageFitCount(); ++i) {
            const auto fit = static_cast<Theme::ImageFit>(i);
            widgets.fit->addItem(Theme::imageFitName(fit), Theme::imageFitKey(fit));
        }
        addRow(group, QCoreApplication::translate("MainWindowSettingsPage", "窗口布局"), widgets.fit);
        widgets.opacity = new QSlider(Qt::Horizontal, form);
        widgets.opacity->setObjectName(themeImageOpacityName(key));
        widgets.opacity->setRange(0, 100);
        widgets.opacity->setValue(100);
        widgets.percent = new QLabel(themeImagePercentText(100), form);
        widgets.percent->setObjectName(themeImagePercentName(key));
        addRow(group, QCoreApplication::translate("MainWindowSettingsPage", "不透明度"), sliderWithValue(widgets.opacity, widgets.percent, form));
        m_themeImageRows.append(widgets);
    };
    addImageGroup(Theme::Part::PageBackground, QCoreApplication::translate("MainWindowSettingsPage", "背景图"));

    // ---------- 外观 ----------
    // NOTE: 「主页标题」那一格没有了（UI 问题 #3）：条上那行字固定是商标 `TitleBar::defaultBrandText()`，
    // 不给用户改，也就不留这一项。
    SettingsGroup *look = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "外观"));
    // 「恢复默认」点了做什么在 MainWindow（要顺手存盘）；置灰跟着窗口大小走（setupUi 末尾那个监视器）。
    windowSizeResetButton = new QPushButton(QCoreApplication::translate("MainWindowSettingsPage", "恢复默认"), form);
    windowSizeResetButton->setObjectName(windowSizeResetButtonName());
    Theme::markButton(windowSizeResetButton, Theme::ButtonKind::Neutral);
    addRow(look, QCoreApplication::translate("MainWindowSettingsPage", "窗口大小"), windowSizeResetButton, Placement::Natural,
           QCoreApplication::translate("MainWindowSettingsPage", "启动时按上次关闭时的大小打开。"),
           QCoreApplication::translate("MainWindowSettingsPage", "默认 %1×%2，最小 %3×%4。")
               .arg(kMainWindowDefaultWidth).arg(kMainWindowDefaultHeight).arg(kMainWindowMinWidth).arg(kMainWindowMinHeight));
    {
        // 切换界面语言后，重启以重建界面控件。
        languageCombo = new QComboBox(form);
        languageCombo->setObjectName(languageComboName());
        languageCombo->addItem(QCoreApplication::translate("MainWindowUI", "跟随系统"),
                               QStringLiteral("system"));
        languageCombo->addItem(QStringLiteral("简体中文"), QStringLiteral("zh_CN"));  // i18n-ignore
        languageCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
        WheelGuard::install(languageCombo);
        addRow(look, QCoreApplication::translate("MainWindowUI", "语言"), languageCombo,
               Placement::Column);
    }
    {
        auto *colors = new QCheckBox(QCoreApplication::translate("MainWindowSettingsPage", "日志颜色显示"), form);
        colors->setObjectName(QStringLiteral("logColorsCheck"));
        colors->setChecked(QSettings().value(QStringLiteral("Look/LogColors"), true).toBool());
        QObject::connect(colors, &QCheckBox::toggled, colors,
                         [](bool enabled) { QSettings().setValue(QStringLiteral("Look/LogColors"), enabled); });
        addSwitch(look, colors, QCoreApplication::translate("MainWindowSettingsPage", "日志颜色"), QCoreApplication::translate("MainWindowSettingsPage", "对新日志窗口生效。"));
    }
    {
        auto *hide = new QCheckBox(QCoreApplication::translate("MainWindowSettingsPage", "隐藏服务器地址"), form);
        hide->setObjectName(QStringLiteral("hideServerAddressCheck"));
        hide->setChecked(QSettings().value(QStringLiteral("Look/HideServerAddress"), false).toBool());
        QObject::connect(hide, &QCheckBox::toggled, hide,
                         [](bool enabled) { QSettings().setValue(QStringLiteral("Look/HideServerAddress"), enabled); });
        addSwitch(look, hide, QCoreApplication::translate("MainWindowSettingsPage", "隐藏服务器地址"),
                  QCoreApplication::translate("MainWindowSettingsPage", "实例设置的服务器列表中，域名只显示每段首字母，IP 只显示前两段。"));
    }
    {
        // 「每日一言」：默认开。checked 状态由 MainWindow 灌（值在 LauncherSettings，
        // 和上面「日志颜色」那种自己落盘的不一样 —— 它还要驱动主页那一行）。
        auto *quote = new QCheckBox(QCoreApplication::translate("MainWindowSettingsPage", "每日一言"), form);
        quote->setObjectName(dailyQuoteCheckName());
        addSwitch(look, quote, QCoreApplication::translate("MainWindowSettingsPage", "每日一言"),
                  QCoreApplication::translate("MainWindowSettingsPage", "每天向 hitokoto 和 ZenQuotes（zenquotes.io）取一句，不带个人信息；" "关闭后不请求。"));
        dailyQuoteCheck = quote;
    }

    // NOTE: 「版本名格式」那一组（自定义名 / 版本名 / 加载器名 / 加载器版本四个开关）在
    // UI 问题 #14 里撤掉了：显示名固定按出厂那一份拼（见 manage/VersionDisplay.hpp）。

    // ---------- 主页快速启动 ----------
    SettingsGroup *quick = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "主页快速启动"));
    const auto addCount = [&](const QString &label, const QString &name, QSpinBox **out) {
        auto *spin = new QSpinBox(form);
        spin->setObjectName(name);
        spin->setRange(0, kHomeQuickMaxCount);
        spin->setSuffix(QCoreApplication::translate("MainWindowSettingsPage", " 个"));
        spin->setSpecialValueText(QCoreApplication::translate("MainWindowSettingsPage", "隐藏"));  // 0 直接显示「隐藏」
        WheelGuard::install(spin);
        // 0–kHomeQuickMaxCount 的小整数：「− 值 +」步进器（值仍在这颗 QSpinBox 上，范围 / valueChanged / 持久化不动）。
        addRow(quick, label, Theme::makeNumberStepper(form, spin, 72), Placement::Natural,
               QCoreApplication::translate("MainWindowSettingsPage", "数量较多时，放不下的卡片只显示图标和启动按钮。"));
        *out = spin;
    };
    addCount(QCoreApplication::translate("MainWindowSettingsPage", "收藏"), homeFavoriteCountSpinName(), &homeFavoriteCountSpin);
    addCount(QCoreApplication::translate("MainWindowSettingsPage", "上次游玩"), homeRecentCountSpinName(), &homeRecentCountSpin);

    // ---------- 高级：语义色 · 高级配色 · 整份主题 ----------
    auto *advanced = new ExpandSection(QCoreApplication::translate("MainWindowSettingsPage", "高级"), form);
    advanced->setObjectName(QStringLiteral("settingsAdvanced-look"));
    panel.layout->addWidget(advanced);
    SettingsGroup *advancedGroup = new SettingsGroup(QCoreApplication::translate("MainWindowSettingsPage", "主题（高级）"), advanced);
    advanced->bodyLayout()->addWidget(advancedGroup);
    {
        // 语义色（危险 / 警告 / 正常 / 报错）：两行两列，窄窗下形状不变。
        auto *box = new QWidget(form);
        auto *boxLayout = new QHBoxLayout(box);
        boxLayout->setContentsMargins(0, 0, 0, 0);
        boxLayout->setSpacing(themeSwatchGroupSpacing());
        auto *grid = new QWidget(box);
        auto *gridLayout = new QGridLayout(grid);
        gridLayout->setContentsMargins(0, 0, 0, 0);
        gridLayout->setHorizontalSpacing(themeSwatchRowSpacing() * 2);
        gridLayout->setVerticalSpacing(themeSwatchRowSpacing());
        m_themeSemanticButtons.clear();
        for (int i = 0; i < themeSemanticCount(); ++i) {
            auto *swatch = new QPushButton(grid);
            swatch->setObjectName(themeSemanticButtonName(i));
            swatch->setFixedSize(themeSwatchWidth(), themeSwatchHeight());
            // 尺寸那一档要自己挂：应用级样式表的 min-height 会把 setFixedSize 按回去。颜色由 MainWindow 按主题现推。
            Theme::markIconButton(swatch);
            m_themeSemanticButtons.append(swatch);
            auto *label = new QLabel(themeSemanticText(i), grid);
            gridLayout->addWidget(label, i / 2, (i % 2) * 2);
            gridLayout->addWidget(swatch, i / 2, (i % 2) * 2 + 1);
        }
        gridLayout->setColumnStretch(4, 1);
        themeSemanticResetButton = new QPushButton(themeSemanticResetText(), box);
        themeSemanticResetButton->setObjectName(themeSemanticResetName());
        Theme::markButton(themeSemanticResetButton, Theme::ButtonKind::Ghost);
        themeSemanticResetButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        boxLayout->addWidget(grid);
        boxLayout->addWidget(themeSemanticResetButton, 0, Qt::AlignTop);
        boxLayout->addStretch(1);
        addRow(advancedGroup, themeSemanticRowText(), box, Placement::Wide, QString(),
               QCoreApplication::translate("MainWindowSettingsPage", "自定义错误、警告和成功状态的颜色；「跟随预设」恢复主题配色。"));
    }
    m_themeAdvancedEditor = new ThemeAdvancedEditor(form);
    addRow(advancedGroup, QCoreApplication::translate("MainWindowSettingsPage", "高级配色"), m_themeAdvancedEditor, Placement::Wide);
    {
        themeResetAllButton = new QPushButton(themeResetAllText(), form);
        themeResetAllButton->setObjectName(themeResetAllButtonName());
        Theme::markButton(themeResetAllButton, Theme::ButtonKind::Danger);  // 抹掉这一页所有改动，含背景图
        themeExportButton = new QPushButton(themeExportText(), form);
        themeExportButton->setObjectName(themeExportButtonName());
        Theme::markButton(themeExportButton, Theme::ButtonKind::Ghost);
        themeImportButton = new QPushButton(themeImportText(), form);
        themeImportButton->setObjectName(themeImportButtonName());
        Theme::markButton(themeImportButton, Theme::ButtonKind::Accent);
        QWidget *buttons = hbox(form, {themeResetAllButton, themeExportButton, themeImportButton}, themeSwatchRowSpacing());
        static_cast<QHBoxLayout *>(buttons->layout())->addStretch(1);
        for (QPushButton *button : {themeResetAllButton, themeExportButton, themeImportButton})
            button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        addRow(advancedGroup, themeFileRowText(), buttons, Placement::Wide, QCoreApplication::translate("MainWindowSettingsPage", "导出不含图片，仅保存路径。"),
               QCoreApplication::translate("MainWindowSettingsPage", "恢复全部默认包含背景设置。"));
    }

    // Tab 顺序：主题那一段从上到下、从左到右钉住（以后中间插控件，默认顺序会变，这一串不会）。
    {
        QVector<QWidget *> chain;
        chain << themePresetCombo << themeAccentButton << themeAccentResetButton << themeTextOpacitySlider;
        for (QPushButton *swatch : m_themeSemanticButtons) chain << swatch;
        chain << themeSemanticResetButton << themeResetAllButton << themeExportButton << themeImportButton;
        for (int i = 1; i < chain.size(); ++i)
            if (chain.at(i - 1) != nullptr && chain.at(i) != nullptr) QWidget::setTabOrder(chain.at(i - 1), chain.at(i));
    }
    panel.layout->addStretch(1);
}

void MainWindowUI::setupSettingsDownloadPanel(QStackedWidget *stack) {
    const SettingsPanel panel = addSettingsPanel(stack, QStringLiteral("download"));
    QWidget *form = panel.form;

    // ---------- 下载源 ----------
    // 各档位的表在 src/ 里（唯一出处），这里只有框没有项，项和选中档由 MainWindow 灌。
    SettingsGroup *source = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "下载源"));
    downloadSourceCombo = new QComboBox(form);
    downloadSourceCombo->setObjectName(downloadSourceComboName());
    WheelGuard::install(downloadSourceCombo);
    addRow(source, QCoreApplication::translate("MainWindowSettingsPage", "来源"), downloadSourceCombo, Placement::Column,
           QCoreApplication::translate("MainWindowSettingsPage", "优先源速度缓慢时，自动改用另一个源。"),
           QCoreApplication::translate("MainWindowSettingsPage", "镜像服务由 BMCLAPI 提供。"));
    downloadConcurrencyCombo = new QComboBox(form);
    downloadConcurrencyCombo->setObjectName(downloadConcurrencyComboName());
    WheelGuard::install(downloadConcurrencyCombo);
    addRow(source, downloadConcurrencyRowText(), downloadConcurrencyCombo, Placement::Column,
           QCoreApplication::translate("MainWindowSettingsPage", "调低可减少限流与超时。"));
    downloadSpeedLimitCombo = new QComboBox(form);
    downloadSpeedLimitCombo->setObjectName(downloadSpeedLimitComboName());
    WheelGuard::install(downloadSpeedLimitCombo);
    addRow(source, downloadSpeedLimitRowText(), downloadSpeedLimitCombo, Placement::Column,
           QCoreApplication::translate("MainWindowSettingsPage", "不含工具箱直链下载。"));
    {
        // 代理：三档由 DownloadProxy.hpp 定；手动地址那一格只在「手动」档露出来（开关与回填由 MainWindow 管）。
        auto *box = new QWidget(form);
        auto *boxLayout = new QVBoxLayout(box);
        boxLayout->setContentsMargins(0, 0, 0, 0);
        boxLayout->setSpacing(6);
        downloadProxyCombo = new QComboBox(box);
        downloadProxyCombo->setObjectName(downloadProxyComboName());
        WheelGuard::install(downloadProxyCombo);
        boxLayout->addWidget(downloadProxyCombo);
        downloadProxyManualEdit = new QLineEdit(box);
        downloadProxyManualEdit->setObjectName(downloadProxyManualEditName());
        downloadProxyManualEdit->setPlaceholderText(QStringLiteral("127.0.0.1:7890"));
        downloadProxyManualEdit->setVisible(false);
        boxLayout->addWidget(downloadProxyManualEdit);
        addRow(source, downloadProxyRowText(), box, Placement::Column, QCoreApplication::translate("MainWindowSettingsPage", "对之后发起的请求生效。"));
    }
    // CurseForge API key：明文显示（贴错了要能一眼看见；它本来就明文躺在配置里），可以留空（走社区镜像）。
    curseForgeApiKeyEdit = new QLineEdit(form);
    curseForgeApiKeyEdit->setObjectName(curseForgeApiKeyEditName());
    InputSizing::longValue(curseForgeApiKeyEdit, QCoreApplication::translate("MainWindowSettingsPage", "API 密钥：约 60 个字符，明文显示便于核对"));
    curseForgeApiKeyEdit->setPlaceholderText(curseForgeApiKeyPlaceholder());
    addRow(source, curseForgeApiKeyRowText(), curseForgeApiKeyEdit);

    // ---------- 下载的文件 ----------
    SettingsGroup *files = addGroup(panel, QCoreApplication::translate("MainWindowSettingsPage", "下载的文件"));
    downloadAutoIntoInstanceCheck = new QCheckBox(downloadAutoIntoInstanceCheckText(), form);
    downloadAutoIntoInstanceCheck->setObjectName(downloadAutoIntoInstanceCheckName());
    addSwitch(files, downloadAutoIntoInstanceCheck, downloadAutoIntoInstanceRowText(),
              QCoreApplication::translate("MainWindowSettingsPage", "下载或拖入资源时选择目标实例；关闭后改用系统文件夹选择。"));
    panel.layout->addStretch(1);
}

QPushButton *MainWindowUI::getThemeSemanticButton(int index) const {
    // 和上面同一个路数：越界给 nullptr，调用方自己判空。
    if (index < 0 || index >= m_themeSemanticButtons.size()) {
        return nullptr;
    }
    return m_themeSemanticButtons.at(index);
}

QString MainWindowUI::themeSemanticKey(int index) {
    // 这四串是 `Theme::Spec` 里那四位（以及盘上 `Theme/Accents/<Key>` 那四个后缀）。
    // 认的是**名字**不是下标：以后加一个语义色、或者把顺序动了，这边改一处就够
    // （`specToJson()` / `accentSettingKey()` 那两边照旧）。
    switch (index) {
    case 0:
        return QStringLiteral("danger");
    case 1:
        return QStringLiteral("warning");
    case 2:
        return QStringLiteral("ok");
    case 3:
        return QStringLiteral("errorText");
    default:
        // 越界给空串：`themeSemanticButtonName()` 拿它拼名字，空串拼出来是个别的控件名
        // （不会撞上任何一颗真按钮），调用方按"认不出"处理就行。
        return QString();
    }
}

QString MainWindowUI::themeSemanticText(int index) {
    // 上下文写死 "MainWindowUI"：这个类不是 QObject（没有 tr()），和别的表同一个写法。
    switch (index) {
    case 0:
        return QCoreApplication::translate("MainWindowUI", "危险");
    case 1:
        return QCoreApplication::translate("MainWindowUI", "警告");
    case 2:
        return QCoreApplication::translate("MainWindowUI", "正常");
    case 3:
        return QCoreApplication::translate("MainWindowUI", "报错");
    default:
        return QString();
    }
}


