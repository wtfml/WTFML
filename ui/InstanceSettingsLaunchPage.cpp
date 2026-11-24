/**
 * @file InstanceSettingsLaunchPage.cpp
 * @brief 「实例设置」右栏「启动」那一栏（Java 路径 / 内存 / 窗口 / 高级选项 / 两颗按钮）。
 *
 * 类是同一个类（`InstanceSettingsDialog`），这一摊按"一页一摊"分 TU，理由见
 * `InstanceSettingsDialogInternals.hpp` 文件头。下面那几个宽度是**这一栏独有**的，
 * 所以留在这一份里；三栏共用的按钮排布工具在那个头文件里。
 */

#include "InstanceSettingsDialog.hpp"
#include "InputSizing.hpp"
#include "ResourcePackPage.hpp"
#include "WheelSafeComboBox.hpp"

#include "Animations.hpp"
#include "SettingsPageParts.hpp"
#include "Theme.hpp"   // 浮层的底色（Theme::Part::Panel）

#include <QAbstractItemView>  // eventFilter()：那两件导航控件共同的兜底（focusInEvent 替人挑第一条）
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>   // 左栏那一列的宽度（见 navListWidth()）
#include <QGuiApplication>
#include <QMimeData>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSize>
#include <QUrl>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QDirIterator>
#include <QSet>
#include <QImageReader>
#include <QVBoxLayout>
#include <utility>
#include <algorithm>
#include <functional>

#include "ConfigFileEditorPage.hpp"
#include "InstanceFilesPage.hpp"
#include "MainWindowUI.hpp"
#include "MemorySlider.hpp"
#include "VersionIcons.hpp"
// 第 1 格那个世界设置。头文件那边已经 include 了它（进参是个嵌套类型），
// 这一句是给"这一份 .cpp 自己用得上它的完整定义"留的。
#include "WorldSettingsPage.hpp"
#include "VersionTreeDelegate.hpp"

/// 数字框的宽度。和设置页那两个用同一个值 —— 同一个"内存 MB 数"在两处宽度不一样
/// 会显得像是两种东西。
constexpr int kMemorySpinWidth = 120;
/// 「最大内存」那一行那根条的最短宽度。比设置页那份（260）窄一点：这一页那一格左边还占着
/// 档位下拉框，条再要 260 就得把数字框挤出门外了。
constexpr int kMemorySliderMinWidth = 200;
/// 「窗口大小」那一行**两个**数字框各自的宽度。比上面那个窄：这一行右边那两个框是并排的，
/// 每个都 120 就得 240 + 那个 × + 档位下拉框，整行会被挤出门外；而这里填的是四位像素数，
/// 80 已经能把「7680」连框一起摆下。
constexpr int kWindowSizeSpinWidth = 80;
/// 追加参数那一行的输入高度（三四行够放几条常见参数）。
constexpr int kAppendJvmArgsHeight = 76;

#include "InstanceSettingsDialogInternals.hpp"

using namespace InstanceSettingsDialogInternals;

/// 「启动」那一栏（Java 路径 / 内存 / 窗口 / 高级选项 / 两颗按钮）。
void InstanceSettingsDialog::setupLaunchPage(const GlobalValues &global, const QStringList &appendJvmArgs,
                                             bool warnJavaMismatch,
                                             const QList<WindowModeChoice> &windowModes) {
    // ---------- 启动（Java 选项 / 内存 / 高级选项）----------
    QGridLayout *grid = addForm(launchCategoryKey(), addPage(launchCategoryKey()));
    // 名字在左（定宽一列，见 addTitledRow()）、控件在右吃满。
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);
    int row = 0;

    // 这一句解释的是**这几项**的属性（「跟随全局 / 自定义」），所以它摆在「启动」栏里、
    // 紧挨着那五项，而不是整页顶上 —— 摆在顶上会让「文件管理」那几栏也像有这么一对档位。
    grid->parentWidget()->setToolTip(tr("「跟随全局」使用全局设置，「自定义」仅用于本实例。"));

    // ---------- Java 路径 ----------
    {
        QWidget *cell = addOverrideRow(grid, row, javaPathField(), tr("Java 路径"),
                                       tr("留空时使用全局 Java。"));
        row += 2;
        auto *choiceRow = choiceRowOf(javaPathField());
        m_javaPathEdit = new QLineEdit(cell);
        m_javaPathEdit->setObjectName(javaPathEditName());
            InputSizing::longValue(m_javaPathEdit, "java.exe 路径：深度不定");  // i18n-ignore
        m_javaPathEdit->setPlaceholderText(tr("java.exe 的完整路径"));

        auto *browse = new QPushButton(tr("浏览…"), cell);
        Theme::markButton(browse, Theme::ButtonKind::Neutral);
        browse->setObjectName(browseJavaButtonName());
        connect(browse, &QPushButton::clicked, this, [this]() {
            // 模态，验证工程驱动不了它 —— 那一半是"问路径"，可测的那一半是这个框里的
            // 文字（override() 直接读它）。和 GroupDialog 里问组名的拆法一样。
            // NOTE: "驱动不了"的原因和内嵌提示那几条不一样：这里开的是 **Windows 自己的
            // 文件选择框**，不是我们的窗。**全项目一共 16 处**：
            // MainWindow.cpp 十一处（选文件夹 / 选整合包 / 挑各处的目录与文件）、
            // AccountSkinDialog.cpp 一处（选皮肤图）、这个文件四处（选 java.exe / 选实例图标 /
            // 导出整合包的保存框 / 导出 .bat 的保存框）。理由和取舍见 MainWindow.cpp 里
            // onAddFolderClicked() 上面那一段。
            const QString picked = QFileDialog::getOpenFileName(
                this, tr("选择 java.exe"), m_javaPathEdit->text(),
                tr("Java 可执行文件 (java.exe);;所有文件 (*)"));
            if (!picked.isEmpty()) {
                m_javaPathEdit->setText(picked);
                // 选了一个具体文件 = 用户明确要这个，顺手把这一行切成「自定义」。
                // 不切的话他选了路径、这一行还在"跟随全局"，这一次选择等于什么都没发生。
                setChoiceCustom(javaPathField(), true);
                // 这两句都是**程序化**改值（`setText` 不发 editingFinished，
                // `setChoiceCustom` 自己挡着信号）⇒ watchEditors() 那几条按类型的线一条
                // 都不响。改完自己补一发（见 .hpp 里 watchEditors() 那段 NOTE 里点名的
                // 两处，这是第一处）。
                notifyEdited();
            }
        });
        // NOTE: 这颗按钮**贴着它那个框**摆、不挪到别处 —— 它改的就是这个框里的字。
        // 全项目唯一一颗这样的按钮，所以它是这一套里唯一不能独自成行的。
        auto *pathGrid = newActionGrid();
        pathGrid->addWidget(m_javaPathEdit, 0, 0, 1, 2);
        prepareActionButton(browse);
        pathGrid->addWidget(browse, 0, 2);
        for (int column = 0; column < kActionButtonsPerRow; ++column) {
            pathGrid->setColumnStretch(column, 1);
        }
        choiceRow->addLayout(pathGrid, 1);
    }

    // ---------- 内存：最大 / 最小 / 分配方式 ----------
    {
        QWidget *cell = addOverrideRow(grid, row, xmxField(), tr("最大内存"),
                                       tr("当前实例的最大内存。"));
        row += 2;
        m_xmxFollowCell = cell;
        auto *choiceRow = choiceRowOf(xmxField());
        // 这一格和设置页那一行一样：**一根条 + 一个数字框**（PCL 那种拖着改的手感）。
        // 条占满剩下的宽度，数字框窄窄一条 —— 顺序也是这个，条在左边。
        // 上限是**这台机器的物理内存**（调用方算好放在 global 里），不是写死的
        // 65536。两页用同一个上限：让一个实例填出比整机内存还大的数，启动时只会拿到一句
        // JVM 的内存分配错误，而界面上从头到尾没有一个字拦得住它。
        // `<= 0` 是"调用方没给"（只验结构的构造点），退回内置上限。
        const int memoryCapMb = m_global.memoryCapMb > 0 ? m_global.memoryCapMb
                                                         : MainWindowUI::memoryLimitMaxMb();
        m_memoryMaxSlider = new MemorySlider(cell);
        m_memoryMaxSlider->setObjectName(memoryMaxSliderName());
        m_memoryMaxSlider->setRangeMb(MainWindowUI::memoryMaxMinMb(), memoryCapMb);
        m_memoryMaxSlider->setMinimumWidth(kMemorySliderMinWidth);
        // 没有焦点时滚轮也会改值（见 MainWindowUI.hpp 里的 WheelGuard）。
        WheelGuard::install(m_memoryMaxSlider);
        choiceRow->addWidget(m_memoryMaxSlider, 1);
        m_memoryMaxSpin = new QSpinBox(cell);
        m_memoryMaxSpin->setObjectName(memoryMaxSpinName());
        m_memoryMaxSpin->setRange(MainWindowUI::memoryMaxMinMb(), memoryCapMb);
        m_memoryMaxSpin->setSingleStep(MainWindowUI::memoryStepMb());
        m_memoryMaxSpin->setSuffix(QStringLiteral(" MB"));
        // 和设置页那两个内存数字框同一份滚轮防护（见 MainWindowUI.hpp 里的 WheelGuard）：
        // 数字框在没有焦点时也吃滚轮，改完界面上看不出来，下次启动用的就是那个被滚出来的数。
        WheelGuard::install(m_memoryMaxSpin);
        m_memoryMaxSpin->setFixedWidth(kMemorySpinWidth);
        choiceRow->addWidget(m_memoryMaxSpin);

        // 条和数字框是同一个值的两半 ⇒ 双向同步，各自挡掉回头那一发（不挡就会来回弹）。
        connect(m_memoryMaxSlider, &QSlider::valueChanged, this, [this]() {
            const QSignalBlocker block(m_memoryMaxSpin);
            m_memoryMaxSpin->setValue(m_memoryMaxSlider->valueMb());
        });
        connect(m_memoryMaxSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() {
            const QSignalBlocker block(m_memoryMaxSlider);
            m_memoryMaxSlider->setValueMb(m_memoryMaxSpin->value());
        });

        // 「改了就写」这一页是**每一发都写盘**的（判据在 `ui/InstanceSettingsSignature` 那一层），
        // 所以拖动中那几十发不能每一发都写一次 —— 和设置页同一个拆法：
        // - 拖动中：只跟着走数字（上面那条同步线），落盘等松手；
        // - 不是拖动中（键盘、滚轮、点滑槽跳一格）：一次就是一格，立刻报"值变了"。
        // 上面那条同步线用的是**挡了信号**的 setValue，所以 watchEditors() 按类型接的那条
        // 线在拖动时一发都不响 —— 报"值变了"这件事得由条自己说。
        connect(m_memoryMaxSlider, &QSlider::valueChanged, this, [this]() {
            if (!m_memoryMaxSlider->isSliderDown()) {
                notifyEdited();
            }
        });
        connect(m_memoryMaxSlider, &QSlider::sliderReleased, this,
                [this]() { notifyEdited(); });
    }
    {
        // 这一整行**不显示**：xms 按默认值走。对象照旧建、照旧跟着盘上的值走 —— 收起来的
        // 是"给人改"这一件事，不是替用户删掉某个实例上已经存过的那份覆盖：那条覆盖照样由
        // override() 透传回去（见 override() 里那段）。整行 hide() 而不是不建，也是为了让
        // 验证工程还能按 objectName 把它找回来 —— "这一项查得到、但看不见"正是现在的意思。
        const int titleRow = row;   // 标题在 addTitledRow() 里就摆在 (row, 0) 这一格上
        QWidget *cell = addOverrideRow(grid, row, xmsField(), tr("最小内存"),
                                       tr("当前实例的初始内存。"));
        row += 2;
        m_xmsFollowCell = cell;
        auto *choiceRow = choiceRowOf(xmsField());
        m_memoryMinSpin = new QSpinBox(cell);
        m_memoryMinSpin->setObjectName(memoryMinSpinName());
        m_memoryMinSpin->setRange(MainWindowUI::memoryMinMinMb(), MainWindowUI::memoryLimitMaxMb());
        m_memoryMinSpin->setSingleStep(MainWindowUI::memoryStepMb());
        m_memoryMinSpin->setSuffix(QStringLiteral(" MB"));
        // 同上：滚轮防护（见 MainWindowUI.hpp 里的 WheelGuard）。
        WheelGuard::install(m_memoryMinSpin);
        m_memoryMinSpin->setFixedWidth(kMemorySpinWidth);
        choiceRow->addWidget(m_memoryMinSpin);
        // 连**标题**一起藏。只藏下面那一格的话，表上会留下孤零零一个「最小内存」（底下
        // 空一块），看着像界面坏了 —— 藏的是"这一行"，不是"这一行里的控件"。
        // 标题是 addTitledRow() 摆在 (titleRow, 0) 上那个 QLabel；QGridLayout 对藏起来的
        // 控件不算高度，所以这两下一做，那一行就整个塌掉、不留空隙。
        if (QLayoutItem *titleItem = grid->itemAtPosition(titleRow, 0)) {
            if (QWidget *title = titleItem->widget()) {
                title->hide();
            }
        }
        cell->hide();
    }
    // NOTE: 「内存分配」（自动 / 手动那一行）**整行不显示** —— 它和「最大内存」说的是
    // 同一件事，只保留那个拖动的。收起来的是这一页对 autoMemory 的**入口**，盘上已经存过的
    // autoMemory 覆盖照样由 JavaSettingsOverride 透传（见
    // src/settings/EffectiveJavaSettings.cpp），不会因为开一次这一页就被抹掉 —— 和上面
    // 「最小内存」那一行同一个态度。


    // ---------- 窗口模式 / 窗口大小 ----------
    //
    // 这两行是「游戏的启动参数 小窗/最大化/全屏 以及游戏分辨率 放到设置和实例设置里面」
    // 的**实例设置那一半**（全局那一半在设置页「启动」栏，见 MainWindowUI）。
    // 和上面几项同一个形状：能「跟随全局」，也能给这一个实例钉一份 —— 绝大多数实例是跟随。
    //
    // NOTE: 档位那几项**由调用方灌**（windowModes 参数），这一页只认「标签 + 代号」那一对。
    // 理由见 .hpp 里 WindowModeChoice 那一段（ui/ 不许 include src/，档位那个枚举
    // 和那三个代号串的唯一出处都在 src/）。
    // NOTE: 三个串（两个键名 + 那三个代号）在这一页里**一个字都不重写**：键名是上面那三个
    // Field() 函数，代号是调用方喂进来的。这正是这条分层规矩要保住的性质。
    if (!windowModes.isEmpty()) {
        QWidget *cell = addOverrideRow(grid, row, windowModeField(), tr("窗口模式"),
                                       tr("当前实例启动时的窗口模式。"));
        row += 2;
        auto *choiceRow = choiceRowOf(windowModeField());
        m_windowModeCombo = new QComboBox(cell);
        m_windowModeCombo->setObjectName(windowModeComboName());
        // 下拉框也吃滚轮（在选项之间来回跳），而且没有焦点时改完看不出来 —— 见 WheelGuard。
        WheelGuard::install(m_windowModeCombo);
        for (const WindowModeChoice &choice : windowModes) {
            m_windowModeCombo->addItem(choice.label, choice.token);
        }
        // 换档位**也要发 `settingsEdited()`** —— 这一页是「改了就写」，而按类型接的那几条
        // 线（QAbstractButton / QLineEdit / QSpinBox / QPlainTextEdit）**一条都接不到
        // QComboBox**，漏了它的症状很具体 —— 用户把这行切到「自定义」（那一发是档位那格
        // 发的，写盘了），再从下拉框里换一档 ⇒ **盘上还是老那一档**，而他看着屏幕上写着
        // 「全屏」。下次动别的开关才会顺带写进去，也就是"改了有时候生效"。
        // 下面灌值那一段是 setCurrentIndex + QSignalBlocker，不会把"打开这一页"当成一次编辑。
        // NOTE: 「跟随全局」那句提示不用跟着重刷 —— 它写的是**全局**那一档，和这一格选了
        // 什么无关（见 refreshHints()）。
        // NOTE: `watchEditors()` 里那一循环把这一页上**所有** `QComboBox` 一次接上（含这个
        // 和那七格档位），这一格不用单独接。那条"容易在测试里被盖住"的教训原样有效 ——
        // mw_check 的 T20.11b 仍然把换档位摆成那一页上最后一个动作。
        choiceRow->addWidget(m_windowModeCombo, 0, Qt::AlignLeft);
    }
    {
        // 分辨率：**一行两个数字框**（宽 × 高）。它和「窗口模式」是两行、各有一个档位
        // 下拉框，但那两个数本身是一对 —— override() 那边要么两个一起收、要么两个都不收
        // （见 .hpp 里那段）。
        QWidget *cell = addOverrideRow(grid, row, windowWidthField(), tr("窗口大小"),
                                       tr("窗口尺寸，单位为像素。"));
        row += 2;
        auto *choiceRow = choiceRowOf(windowWidthField());
        m_windowWidthSpin = new QSpinBox(cell);
        m_windowWidthSpin->setObjectName(windowWidthSpinName());
        m_windowWidthSpin->setRange(MainWindowUI::windowMinWidth(), MainWindowUI::windowMaxWidth());
        m_windowWidthSpin->setSingleStep(MainWindowUI::windowSizeStep());
        m_windowWidthSpin->setMinimumWidth(kWindowSizeSpinWidth);
        WheelGuard::install(m_windowWidthSpin);
        choiceRow->addWidget(m_windowWidthSpin, 0, Qt::AlignLeft);

        auto *times = new QLabel(QStringLiteral("×"), cell);
        times->setObjectName(windowSizeTimesName());
        choiceRow->addWidget(times);

        m_windowHeightSpin = new QSpinBox(cell);
        m_windowHeightSpin->setObjectName(windowHeightSpinName());
        m_windowHeightSpin->setRange(MainWindowUI::windowMinHeight(),
                                     MainWindowUI::windowMaxHeight());
        m_windowHeightSpin->setSingleStep(MainWindowUI::windowSizeStep());
        m_windowHeightSpin->setMinimumWidth(kWindowSizeSpinWidth);
        WheelGuard::install(m_windowHeightSpin);
        choiceRow->addWidget(m_windowHeightSpin, 0, Qt::AlignLeft);
        choiceRow->addStretch(1);
        // 顶上那句「跟随全局（当前 854×480）」要写全局那一份，从构造函数给的那份值里取。
        // **只在调用方给了正数时才记**：`<= 0` 是"调用方没给"的哨兵（见 .hpp 里
        // GlobalValues 那一段），那时那句话里不写尺寸。
        if (global.windowWidth > 0 && global.windowHeight > 0) {
            m_globalWindowWidth = global.windowWidth;
            m_globalWindowHeight = global.windowHeight;
        }
    }

    // ---------- 高级选项（默认折叠） ----------
    {
        // 折叠行：整行一颗按钮，右边「展开 / 收起」，不放 ▸ 方框钮（8 号反馈）。
        m_advancedToggle = Theme::makeFoldRow(this, advancedToggleText());
        m_advancedToggle->setObjectName(advancedToggleName());
        m_advancedToggle->setChecked(false);
        // 默认折叠。里面几项都是"改错了游戏直接起不来"的东西，一打开对话框就摆在
        // 眼前等于诱导用户去动它。
        grid->addWidget(m_advancedToggle, row++, 0, 1, 2);

        m_advancedBox = new QWidget(this);
        m_advancedBox->setObjectName(advancedBoxName());
        auto *advancedGrid = new QGridLayout(m_advancedBox);
        advancedGrid->setContentsMargins(16, 0, 0, 0);
        advancedGrid->setHorizontalSpacing(12);
        advancedGrid->setVerticalSpacing(10);
        advancedGrid->setColumnStretch(0, 0);
        advancedGrid->setColumnStretch(1, 1);
        m_advancedBox->setHidden(true);
        grid->addWidget(m_advancedBox, row++, 0, 1, 2);

        // 开合动效与设置页「高级」同一份（200ms、每帧排完版面）；窗口缩回去等动画落定后再做。
        auto *advancedMotion = new SettingsParts::FoldMotion(m_advancedBox);
        connect(m_advancedToggle, &QAbstractButton::toggled, this, [this, advancedMotion](bool expanded) {
            // 展开前这个窗有多高：折回时按它还原（用户自己拉过的也算数 —— 记的是"展开
            // 那一刻"的高度，不是构造时那个 820x560）。
            if (expanded) {
                m_heightBeforeAdvanced = height();
            }
            advancedMotion->setOpen(expanded);
        });
        connect(advancedMotion, &SettingsParts::FoldMotion::settled, this, [this](bool expanded) {
            // 折回之后得让窗口**缩回去**：Qt 只在"窗口比内容还小"时把它撑大（展开那一下
            // 就是它干的），内容变少时它一动不动 —— 不补这一句，展开一次再收起，底下就
            // 永远留着那一截空白。
            //
            // NOTE: 必须**晚一拍**再缩，而且缩之前得把布局那一串**推一遍**。这一下
            // `setHidden()` 只是把"重算布局"排进事件队列，此刻这个窗的最小高度还是
            // 展开着的那个数，当场 `resize()` 会被它钳回去 —— 看着像"修了但没生效"。
            // 排一发 0 毫秒的还不够：这一页套了**好几级**布局（表格 → 表单 → 那一页 →
            // 堆叠页 → 两栏 → 整页），一级算完才把下一级的 LayoutRequest 排进去，最小
            // 高度要**两拍**才降回来。所以这儿不数拍子，而是**由内到外把每一级的布局
            // `activate()` 一遍**再缩 —— 收敛是当场问出来的，跟套了几级无关。
            // 收尾那一发挂在 this 上，窗先没了就不会再响。
            if (!expanded) {
                QTimer::singleShot(0, this, [this] {
                    if (m_heightBeforeAdvanced <= 0) {
                        return;
                    }
                    // 由内到外（m_advancedBox 一路往上到 this）：每一级自己重算一次。
                    for (QWidget *widget = m_advancedBox; widget != nullptr;
                         widget = widget->parentWidget()) {
                        if (widget->layout() != nullptr) {
                            widget->layout()->activate();
                        }
                        if (widget == this) {
                            break;
                        }
                    }
                    // 只动高、不动宽：宽度是用户自己拉的（那几个"比分组对话框宽一点"的
                    // 起点），那一块藏在下面，跟宽度没有关系。
                    resize(width(), m_heightBeforeAdvanced);
                });
            }
        });

        // 高级选项里这三行也照「上一行设置名称，下一行设置选项」排（**这一栏整个**都是
        // 这个排法，不只是上面那四项）——所以 `advancedRow` 一律 `+= 2`。
        int advancedRow = 0;
        {
            QWidget *cell =
                addOverrideRow(advancedGrid, advancedRow, extraJvmArgsField(), tr("额外 JVM 参数"),
                               tr("替代全局参数，以空格分隔。"));
            advancedRow += 2;
            auto *choiceRow = choiceRowOf(extraJvmArgsField());
            m_extraJvmArgsEdit = new QLineEdit(cell);
            m_extraJvmArgsEdit->setObjectName(extraJvmArgsEditName());
            InputSizing::longValue(m_extraJvmArgsEdit, "JVM 参数：一串参数，长度不定");  // i18n-ignore
            m_extraJvmArgsEdit->setPlaceholderText(QStringLiteral("-XX:+UseG1GC"));
            choiceRow->addWidget(m_extraJvmArgsEdit, 1);
        }
        {
            // 追加参数**没有**「跟随全局 / 自定义」那一对按钮：它不是覆盖层的一项，
            // meta 里也没有对应的全局值可以跟随 —— 全局那一份是 extraJvmArgs，两个是
            // 不同的东西（一个取代、一个追加）。
            QVBoxLayout *cellLayout = nullptr;
            QWidget *cell =
                addTitledRow(advancedGrid, advancedRow, tr("追加参数"), m_advancedBox, &cellLayout);
            advancedRow += 2;
            m_appendJvmArgsEdit = new QPlainTextEdit(cell);
            m_appendJvmArgsEdit->setObjectName(appendJvmArgsEditName());
            m_appendJvmArgsEdit->setFixedHeight(kAppendJvmArgsHeight);
            m_appendJvmArgsEdit->setPlainText(appendJvmArgs.join(QLatin1Char('\n')));
            cellLayout->addWidget(m_appendJvmArgsEdit);
            cell->setToolTip(appendJvmArgsHint());
        }
        {
            // 提醒开关。和上面那行一样：**不是覆盖层的一项**（没有「跟随全局 / 自定义」
            // 那个档位下拉框），它写的是 meta 里的另一个键，由 MainWindow 合并写进去。
            //
            // 放在「高级选项」里而不是摆在五项旁边：它不是一个"值"，摆在那五行之间会让人
            // 以为它也能跟随全局。藏在这里的代价是"关了之后不好找回来"，所以启动时那个
            // 对话框的正文里点名说了它在哪。
            QVBoxLayout *cellLayout = nullptr;
            QWidget *cell = addTitledRow(advancedGrid, advancedRow, tr("版本提醒"), m_advancedBox,
                                         &cellLayout);
            advancedRow += 2;
            m_warnJavaMismatchCheck = new QCheckBox(tr("Java 版本与版本要求不符时提醒"), cell);
            m_warnJavaMismatchCheck->setObjectName(warnJavaMismatchCheckName());
            m_warnJavaMismatchCheck->setChecked(warnJavaMismatch);
            cellLayout->addWidget(m_warnJavaMismatchCheck);
            m_warnJavaMismatchCheck->setToolTip(tr("关闭后使用指定 Java，不再提示版本不符。"));
        }
    }

    // ---------- 导出启动脚本 / 补全版本文件（两颗按钮并排一行）----------
    //
    // 这两件是**动作**，不是设置项：它们不改 wtfml.meta 里的任何一格，所以没有
    // 「跟随全局 / 自定义」那一格档位 —— 和上面「追加参数」「版本提醒」同一个形状
    // （那两个也不是覆盖层的一项）。
    //
    // 按钮文本**直接就是设置名称**（「导出启动脚本」「补全版本文件」并排一行）⇒ 这一对
    // **没有行名**（按钮上那几个字就是名字），两句解释挪去 tooltip + 底下那一行合起来的
    // 说明。
    //
    // NOTE: 说明**不能只放 tooltip**：「导出的那份里带着登录令牌，别发给别人」是一句安全
    // 警告，而 tooltip 得悬停才看得见（理由见 .hpp 里 actionsHint() 那段）。
    // NOTE: 摆在「高级选项」**之后**：这一页从上到下是"这个实例拿什么启动"（Java、内存、
    // 额外参数），两件动作是收尾。而且高级选项默认折叠着，所以打开这一页看到的
    // 顺序是 Java 路径 → 最大内存 → 最小内存 → 两件动作。
    {
        auto [cell, column] = makeZeroColumn(this);

        auto *pairGrid = newActionGrid();
        auto *exportScript = new QPushButton(exportScriptText(), cell);
        exportScript->setObjectName(exportScriptButtonName());
        Theme::markButton(exportScript, Theme::ButtonKind::Neutral);
        exportScript->setToolTip(exportScriptHint());
        connect(exportScript, &QPushButton::clicked, this, [this]() {
            // 默认文件名用**这个实例的目录名**（= 构造时那个 instanceId）：它一定是合法
            // 文件名（建/改名那一关挡过非法字符），而显示名不一定。目录也顺带兜一层底。
            QString dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
            if (dir.isEmpty()) {
                dir = QDir::homePath();
            }
            const QString suggested =
                dir + QLatin1Char('/') + m_instanceId + QStringLiteral(".bat");
            // 开的是 **Windows 自己的保存框**（和上面「浏览…」选 java.exe 同一档：全项目
            // 就剩这几处独立窗，理由见 MainWindow.cpp 里 onAddFolderClicked() 上面那段）。
            // 那个框上的「取消」= 什么都不做（**不是"取消这一页的设置"** —— 这一页没有
            // 那种东西，改过的值早就写进盘里了，见文件头那一段）。
            const QString picked = QFileDialog::getSaveFileName(
                this, exportScriptText(), suggested, tr("批处理文件 (*.bat)"));
            if (picked.isEmpty()) {
                return;
            }
            // 真正"写什么"在 MainWindow 里：这一层够不着 authManager / versionManager
            // （ui/ 不 include src/ 下的头）。见 .hpp 里那个信号的说明。
            emit exportLaunchScriptRequested(picked);
        });
        auto *repair = new QPushButton(repairFilesText(), cell);
        repair->setObjectName(repairFilesButtonName());
        repair->setToolTip(repairFilesHint());
        // 只报"用户想干这件事"：能不能开始（有没有下载在跑 / 这个实例装没装）由
        // MainWindow 说了算，它才知道。**不问路径**，所以这一层没什么可问的了。
        connect(repair, &QPushButton::clicked, this, &InstanceSettingsDialog::repairFilesRequested);
        // 补全文件只在维护页保留入口，启动设置里没有它。
        repair->hide();
        layoutActionButtons(pairGrid, {exportScript});
        column->addLayout(pairGrid);

        cell->setToolTip(actionsHint());
        grid->addWidget(cell, row++, 0, 1, 2);
    }

}


/// 把构造函数那份「现状」灌进「启动」这一页。
///
/// 这一页是**第一次切过去才建**的（见 `ensurePage()`），所以灌值不能留在构造函数里：
/// 页晚建，值就跟着页一起灌。顺序不能动 —— 条的另一半要等数字框到位、每一行那句提示
/// 要等所有值灌完才刷（段落里各自的理由都还留着）。
void InstanceSettingsDialog::applyLaunchValues(const Override &current, const GlobalValues &global) {
    // 这一位先立起来：下面 `refreshHints()` 有一道"页还没建就别碰控件"的闸。
    m_launchPageBuilt = true;
    // ---------- 灌值 ----------
    // 「有值 = 自定义，没值 = 跟随全局」这一条两边都要对上：磁盘上有一项覆盖时，
    // 重开这个对话框必须停在「自定义」上，否则用户会以为自己的设置丢了。
    setChoiceCustom(javaPathField(), current.javaPath.has_value());
    m_javaPathEdit->setText(current.javaPath.value_or(global.javaPath));

    setChoiceCustom(xmxField(), current.xmx.has_value());
    m_memoryMaxSpin->setValue(current.xmx.value_or(global.xmx));
    // 条那半边也要摆过来：数字框本来就已经是这个值时上面那一句不发信号，光靠同步那条线
    // 等不到（条会停在上一次那一格上）—— 一进这一页看见的条和数字框就不是一个数。
    // 挡信号：这一发是**灌值**，不是用户改的 —— 不挡的话它会顺手报一次"值变了"
    // （条自己接的那条线，见上面那段），而那时候 `m_lastSignature` 还是空的，
    // 于是打开这一页本身就等于替用户写了一次盘。
    {
        const QSignalBlocker blockSlider(m_memoryMaxSlider);
        m_memoryMaxSlider->setValueMb(m_memoryMaxSpin->value());
    }

    setChoiceCustom(xmsField(), current.xms.has_value());
    m_memoryMinSpin->setValue(current.xms.value_or(global.xms));

    setChoiceCustom(extraJvmArgsField(), current.extraJvmArgs.has_value());
    m_extraJvmArgsEdit->setText(current.extraJvmArgs.value_or(global.extraJvmArgs));

    // ---------- 窗口模式 / 窗口大小 ----------
    if (m_windowModeCombo != nullptr) {
        // 档位：有覆盖就用覆盖那一档、没有就摆**全局那一档**（那一档这时是禁用的，摆出来的
        // 意义是"你跟随的是这一档"）。认不出来的代号（手改过 meta、或者别处写进去的名字）
        // 就退回第 0 项 —— 和 `GlobalValues::windowModeLabel` 那边同一个态度：看不懂的值
        // 不猜它是什么档，摆最保守的那一档（调用方喂进来的第 0 项就是「小窗」，见
        // MainWindow 构造这张表的地方）。
        const QString token = current.windowMode.value_or(global.windowMode);
        const int index = m_windowModeCombo->findData(token);
        {
            // 挡信号：这一发是**灌值**不是用户改的（和上面那条注释同一个理由 —— 不挡的话
            // 打开这一页本身就等于替用户写一次盘，见 notifyEdited() 那条路）。
            const QSignalBlocker block(m_windowModeCombo);
            m_windowModeCombo->setCurrentIndex(index >= 0 ? index : 0);
        }
        setChoiceCustom(windowModeField(), current.windowMode.has_value());
    }
    if (m_windowWidthSpin != nullptr) {
        // 分辨率那两格：**以窗口宽那一格有没有值为准**（它们是同一个决定的两半，见 .hpp
        // 里 override() 那段），值本身各自取覆盖或全局那一份。
        setChoiceCustom(windowWidthField(), current.windowWidth.has_value());
        m_windowWidthSpin->setValue(current.windowWidth.value_or(global.windowWidth));
        m_windowHeightSpin->setValue(current.windowHeight.value_or(global.windowHeight));
    }

    // 每一行下面那句话要等值灌完才刷（它写的是"当前是什么"）。
    //
    // NOTE: 上面那一串是 `setChoiceCustom()`，它**自己挡着信号**（档位下拉框的
    // `currentIndexChanged` 既接 refreshHints 也接写盘那条线，灌值的时候一条都不该
    // 响）—— 所以这一发是这个函数里**唯一**一次算提示的时机，不是"再刷一遍"。
    refreshHints();
}
