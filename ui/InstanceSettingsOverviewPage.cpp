/**
 * @file InstanceSettingsOverviewPage.cpp
 * @brief 「实例设置」右栏第一页：概览那四行（图标 + 实例名 / 两颗按钮 / 置顶勾选框 / 导出）
 *        与它底下的维护入口。
 *
 * 类是同一个类（`InstanceSettingsDialog`），只是这一摊的实现住在自己的翻译单元里 ——
 * 理由见 `InstanceSettingsDialogInternals.hpp` 文件头：按"一页一摊"分 TU，而不是按行数切。
 *
 * 搭控件用的小工具全在 `InstanceSettingsDialogInternals.hpp`：原来它们住在主 .cpp 顶上的
 * 匿名命名空间里，拆开之后另外几个 .cpp 看不见（匿名命名空间是内部链接）。
 */

#include "MainWindowPageLayout.hpp"
#include <QCoreApplication>
#include "InstanceSettingsDialog.hpp"
#include "InputSizing.hpp"
#include "ResourcePackPage.hpp"
#include "WheelSafeComboBox.hpp"

#include "Animations.hpp"
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
#include <QFrame>
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

/// 八颗候选按钮上那张图多大。比预览小一档 —— 它们是"挑哪一类"，预览才是"结果"。
constexpr int kIconCandidateSize = 32;
/// 候选按钮一排摆几颗。八颗正好两排。
constexpr int kIconCandidatesPerRow = 4;

#include "InstanceSettingsDialogInternals.hpp"

using namespace InstanceSettingsDialogInternals;

namespace {

/// 小节标题（基本 / 更新 / 文件与导出）：系统字 16 半粗，上面空一段。
QLabel *overviewSection(const QString &text, QWidget *parent, bool first = false) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("instanceOverviewSection"));
    label->setFont(Theme::textFont(label->font(), Theme::TextRole::Heading));
    label->setContentsMargins(0, first ? 0 : 18, 0, 4);
    return label;
}

/// 八格统一成最宽那一格的宽度（四列等宽）。弹出前再算一次：字号 / 主题换过之后旧下限会偏。
void equalizeCandidateWidths(const QList<QToolButton *> &buttons) {
    int widest = 0;
    for (QToolButton *button : buttons) {
        button->setMinimumWidth(0);
        button->ensurePolished();
        widest = qMax(widest, button->sizeHint().width());
    }
    for (QToolButton *button : buttons) button->setMinimumWidth(widest);
}

/// 提示里的第一句（到第一个句号）；超过 40 字就不摆，整段留在悬停提示里。
QString firstSentence(const QString &hint) {
    const int stop = hint.indexOf(QChar(0x3002));
    const QString head = stop < 0 ? hint : hint.left(stop + 1);
    return head.size() <= 40 ? head : QString();
}

}  // namespace

/// 概览那一栏（四行：图标+实例名 / 两颗按钮 / 置顶勾选框 / 导出）与它底下的维护入口。
///
/// 从构造函数里按"一栏一摊"搬出来的第一摊。这几栏原来挤在同一个函数里、**共用一份 `grid`**
/// （换一栏就重新赋值、行号从 0 数），现在每摊自己 `addForm()` 一份，谁也不再借谁的表格。
void InstanceSettingsDialog::setupOverviewPage(const QString &instanceId) {
    // ---------- 概览（四行）----------
    //
    // 概览四行：第一行 图标 + 名称（可编辑文本）；第二行 更改图标（按钮）+ 分组…（按钮）；
    // 第三行 显示在置顶组中（勾选框：在被设为置顶组的那个组里再放一个同名的，下面给到
    // 注释提醒）；第四行 导出。
    //
    // 这一栏**没有"标题在左、内容在右"的行名**：四行分别是名称输入框、两颗按钮、
    // 勾选框上的字、第四行那颗占位按钮 —— 名字都在控件自己身上。这也是这一栏和别的那几栏
    // 长得不一样的原因，是刻意的。
    //
    // NOTE: 每一栏**各自**一张表格（各自的 objectName）、行号各从 0 数。原来这几栏挤在
    // 同一个构造函数里，共用一份 `grid`、换一栏就重新赋值；按栏拆开之后各摊自己
    // `addForm()` 一份，"这一行算在哪一栏里"不再靠"上一句把 row 清零了没有"。
    QGridLayout *grid = addForm(overviewCategoryKey(), addPage(overviewCategoryKey()));
    int row = 0;

    // ---------- 第一行：图标预览 + 实例名 ----------
    //
    // 名字（= 改名）和图标并排摆成一行。它不是一项"覆盖"：别的几行都有「跟随全局 / 自定义」那个档位下拉框（那些是
    // 启动参数，全局有一份、实例可以盖过它），而这个是这个实例自己的身份，没有"全局"可比
    // —— 所以它不走 addOverrideRow()。
    //
    // 真的动手改名的是 MainWindow（先改名、再写设置），这一层只负责让用户改到那个格子
    // （见 instanceName()）。
    {
        m_iconPreview = new QLabel(this);
        m_iconPreview->setObjectName(iconPreviewName());
        // 行里摆 40 px（设计稿），图照旧按 iconPreviewSize() 那一档解好、缩进来。
        m_iconPreview->setFixedSize(40, 40);
        m_iconPreview->setScaledContents(true);
        m_iconPreview->setAlignment(Qt::AlignCenter);

        m_instanceNameEdit = new QLineEdit(instanceId, this);
        m_instanceNameEdit->setObjectName(instanceNameEditName());
        // 初值是**目录名**（不是页眉上那个显示名）—— 见构造函数的 @param instanceId。
        m_instanceNameEdit->setPlaceholderText(instanceNameText());
        m_instanceNameEdit->setToolTip(instanceNameHint());
        InputSizing::shortValue(m_instanceNameEdit, InputSizing::kNameChars, InputSizing::Script::Han);
        m_instanceNameEdit->setFixedWidth(260);
        m_lastLaunchLabel = new QLabel(this);
        {
            QPalette pal = m_lastLaunchLabel->palette();
            pal.setColor(QPalette::WindowText, Theme::tokens().secondary);
            m_lastLaunchLabel->setPalette(pal);
        }
        m_lastLaunchLabel->setObjectName(QStringLiteral("instanceLastLaunch"));
        // 这一行只有一句状态，**不折行**：折行标签的 sizeHint 是"按要折成的宽度量出来的一行"，
        // 而这一行是按 sizeHint 宽摆的（makeSettingRow 里 AlignLeft），于是日期被折到第二行、
        // 行高只够第一行 —— 屏幕上就只剩「最后启动：」。
        m_lastLaunchLabel->setWordWrap(false);
        setLastLaunch(QDateTime());
        m_playTimeLabel = new QLabel(this);
        m_playTimeLabel->setObjectName(QStringLiteral("instancePlayTime"));
        m_playTimeLabel->setPalette(m_lastLaunchLabel->palette());
        setPlayTime(QStringLiteral("0:00:00"), QString());

        grid->addWidget(overviewSection(tr("基本"), this, true), row++, 0, 1, 2);
        grid->addWidget(MainWindowPageLayout::makeSettingRow(this, MainWindowPageLayout::makeSettingRowTitle(instanceNameText(), this), QString(), m_instanceNameEdit),
                        row++, 0, 1, 2);
    }

    // ---------- 第二行：两颗按钮（更改图标 / 分组…），各开一个浮层 ----------
    //
    // 第二行是两颗按钮：更改图标 + 分组… —— 点它们各弹一个浮层，
    // **而不是把候选项铺在这一页上**。
    {
        m_changeIconButton = new QPushButton(changeIconText(), this);
        m_changeIconButton->setObjectName(changeIconButtonName());
        // 「更换图标…」「移入分组…」「导出…」都是**打开一个菜单或另一个框**（一般性）。
        Theme::markButton(m_changeIconButton, Theme::ButtonKind::Neutral);
        m_changeIconButton->setToolTip(iconHint());
        connect(m_changeIconButton, &QPushButton::clicked, this,
                [this]() {
                    equalizeCandidateWidths(m_iconButtons);
                    popUpMenuBelow(m_iconMenu, m_changeIconButton);
                });

        m_groupButton = new QPushButton(groupButtonText(), this);
        m_groupButton->setObjectName(groupButtonName());
        Theme::markButton(m_groupButton, Theme::ButtonKind::Neutral);
        // tooltip 由 refreshGroupMenuState() 写（它要说清"现在归在哪一组"）。
        connect(m_groupButton, &QPushButton::clicked, this,
                [this]() { popUpMenuBelow(m_groupMenu, m_groupButton); });

        auto *iconControl = new QWidget(this);
        auto *iconLine = new QHBoxLayout(iconControl);
        iconLine->setContentsMargins(0, 0, 0, 0);
        iconLine->setSpacing(10);
        iconLine->addWidget(m_iconPreview);
        iconLine->addWidget(m_changeIconButton);
        grid->addWidget(MainWindowPageLayout::makeSettingRow(this, MainWindowPageLayout::makeSettingRowTitle(tr("图标"), this), QString(), iconControl), row++, 0, 1, 2);
        grid->addWidget(MainWindowPageLayout::makeSettingRow(this, MainWindowPageLayout::makeSettingRowTitle(tr("分组"), this), QString(), m_groupButton), row++, 0, 1, 2);
    }

    // ---------- 「更改图标」那个浮层 ----------
    //
    // **图标**：十档 —— 默认 + 八个内置候选 + 本地那张。内置那八张是版本分类 / 加载器的
    // 标志图（不用担心侵权），另外允许用户自己挑一张本地图片。**每一档都有对应的按钮**
    // （八个候选各一颗、「恢复默认」、「选一张图片…」），没有"换过去就回不来"的死角。
    //
    // NOTE: 那一排按钮**不是**当前值的出处（它们只是 m_iconChoice 的一幅画）—— 理由见 .hpp
    // 里 setIcon() 上面那一段：本地那一档没有控件能表示它。
    //
    // NOTE: 【浮层是**常驻**的 —— 一次建好、以后每次点按钮只是再弹一遍】
    // 三件事都指望这个：①验证工程按 objectName 找得到里面那几颗按钮（**没弹出来的
    // 时候也要能找**，那些断言关心的是"按了会变成什么"，不是"它当时开没开"）；
    // ②每点一次就新建一个的话，连点两次就会同时存在两个同名浮层，按名字找控件的
    // 那一套当场失去意义；③refreshIconRow() 在关着的时候也得把勾选状态摆对（否则
    // 弹出来那一瞬间画的是上一次的样子）。
    // **判"此刻开着没开着"的判据是 isVisible()**，不是它存不存在。
    // 同一段理由对整个这一页的两个浮层都成立。
    {
        auto *menu = new MenuPopup(this);
        m_iconMenu = menu;
        m_iconMenu->setObjectName(iconMenuName());
        auto *box = new QVBoxLayout(menu);
        box->setContentsMargins(kMenuPadding, kMenuPadding, kMenuPadding, kMenuPadding);
        box->setSpacing(kMenuRowSpacing);

        auto *candidates = new QGridLayout;
        candidates->setSpacing(4);
        const QStringList keys = VersionIcons::iconCandidateKeys();
        const QStringList names = iconCandidateTexts();
        for (int i = 0; i < keys.size(); ++i) {
            auto *button = new QToolButton(menu);
            button->setObjectName(iconCandidateButtonName(i));
            button->setText(names.value(i));
            // 图在上、字在下：这一排是"挑哪一张"，光看图认不出 Fabric 和 NeoForge。
            button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            button->setIconSize(QSize(kIconCandidateSize, kIconCandidateSize));
            // 每颗按钮都得 `setIcon`：`setIconSize` 摆了尺度、`TextUnderIcon` 摆了位置，
            // 少了 `setIcon` 八颗按钮就全是光秃秃一个词，用户看着「草方块 / 覆雪草块 /
            // 土块 / 幸运方块」选图，而上面那一格预览的是**另一张**（默认图标）。
            // 这一格的全部意义就是"挑哪一张"，看不见图等于没得挑。
            // 图由 VersionIcons 按同一份候选表解出来（键和画法都只有那一处），这里只管贴。
            button->setIcon(QIcon(VersionIcons::iconCandidateMark(i, kIconCandidateSize)));
            button->setCheckable(true);
            // 「挑一个」的样子与状态筛选胶囊同一档：淡底、悬停加深、勾上强调色淡底 + 强调字。
            Theme::markButton(button, Theme::ButtonKind::Neutral);
            // 和设置页那些"点一下换个样子"的按钮同一条：不该抢键盘焦点（抢了之后
            // 键盘上那几下就不再是"在这张浮层上操作"了）。
            button->setFocusPolicy(Qt::NoFocus);
            const QString key = keys.at(i);
            connect(button, &QToolButton::clicked, this, [this, key]() {
                m_iconChoice = VersionIcons::iconValueForKey(key);
                // 换了内置候选 = 用户不要刚才挑的那张本地图了。**不删盘上那份**：
                // 它现在还在原处，而实例目录里那份要等这一次改动落到盘上才被换掉
                // （见 pickedIconPath()）。
                m_pickedIconPath.clear();
                refreshIconRow();
                // 挑完就收：这颗按钮是"选一个"，不是"切换一个开关"。留着框开着挡住的
                // 正是用户接下来要看的地方（那一行里的预览）。
                m_iconMenu->close();
            });
            m_iconButtons.append(button);
            candidates->addWidget(button, i / kIconCandidatesPerRow, i % kIconCandidatesPerRow);
        }
        equalizeCandidateWidths(m_iconButtons);
        box->addLayout(candidates);

        auto *actions = new QHBoxLayout;
        actions->setSpacing(8);
        auto *pick = new QPushButton(iconPickText(), menu);
        pick->setObjectName(iconPickButtonName());
        Theme::markButton(pick, Theme::ButtonKind::Neutral);
        connect(pick, &QPushButton::clicked, this, [this]() {
            // 模态，验证工程驱动不了它 —— 和「浏览…」选 java.exe 同一个道理（开的是
            // Windows 自己的文件选择框，不是我们的窗）。可测的那一半是"挑完之后
            // iconChoice() / pickedIconPath() 变成什么"，那两问都是现场读成员、不碰盘。
            const QString picked = QFileDialog::getOpenFileName(
                this, iconPickText(), pickedIconPath(),
                tr("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;所有文件 (*)"));
            if (picked.isEmpty()) {
                return;  // 取消：一个字都不改（当前那一档原样留着）
            }
            m_iconChoice = localIconValue();
            m_pickedIconPath = picked;
            refreshIconRow();
            // 这一发 `clicked` **在框弹出来之前**就发完了（用户挑完图是那之后的事）
            // ⇒ 改完自己补一发（见 .hpp 里 watchEditors() 那段 NOTE 点的第二处）。
            notifyEdited();
            m_iconMenu->close();
        });
        actions->addWidget(pick);
        auto *reset = new QPushButton(iconResetText(), menu);
        reset->setObjectName(iconResetButtonName());
        Theme::markButton(reset, Theme::ButtonKind::Neutral);
        connect(reset, &QPushButton::clicked, this, [this]() {
            // 「默认」= **这个键不存在**（而不是"写一个等于默认的值"）—— 全项目同一个约定，
            // 见 VersionManager::updateVersionMeta() 里那段关于 QJsonValue::Null 的说明。
            // 存成"等于默认"的话，用户之后换了版本分类（比如从原版换成一个 Fabric 实例），
            // 这个实例就不会跟着换标志了，而他要的明明是"默认"。
            m_iconChoice.clear();
            m_pickedIconPath.clear();
            refreshIconRow();
            m_iconMenu->close();
        });
        actions->addWidget(reset);
        actions->addStretch(1);
        box->addLayout(actions);
    }

    // ---------- 「分组…」那个浮层 ----------
    //
    // 候选项由调用方灌（setGroupChoices）—— 组名是用户可以改的、id 是稳定身份，这一页
    // 只负责显示。第一项「未分组」由这里自己加（它是保留值，不是某一组）。
    // 浮层里那几行**是每次 setGroupChoices() 的时候重建的**（行数会变），理由写在那个
    // 函数里。
    {
        auto *menu = new MenuPopup(this);
        m_groupMenu = menu;
        m_groupMenu->setObjectName(groupMenuName());
        auto *box = new QVBoxLayout(menu);
        box->setContentsMargins(kMenuPadding, kMenuPadding, kMenuPadding, kMenuPadding);
        box->setSpacing(kMenuRowSpacing);
    }

    // ---------- 第三行：显示在置顶组中（勾选框 + 注释）----------
    //
    // 第三行：勾选框 + 下面那句注释。那句解释必须留着 —— 它说的是
    // "在被设为置顶组的那个组里再放一个同名的"。
    {
        // 一直生效的状态 ⇒ 开关（步骤 133）：行名在左、开关在行尾，和上面几行同一个排法。
        m_pinCheck = new QCheckBox(pinCheckText(), this);
        m_pinCheck->setObjectName(pinCheckName());
        // **这儿不接 `toggled`**：勾选框和「分组」那一格说的是**两件互不相干的事**
        // （一个管"要不要在置顶组里多摆一条"，一个管"老家在哪"）—— 它们之间没有任何
        // 要同步的东西。
        Theme::markSwitch(m_pinCheck);
        m_pinCheck->setToolTip(pinHint());
        grid->addWidget(MainWindowPageLayout::makeSettingRow(this, MainWindowPageLayout::makeSettingRowTitle(pinCheckText(), this), firstSentence(pinHint()), m_pinCheck),
                        row++, 0, 1, 2);
        // 「最后启动」那一行：时间当标题，口径说明在悬停提示里（setLastLaunch 写的）。
        grid->addWidget(MainWindowPageLayout::makeSettingRow(this, m_lastLaunchLabel, QString(), nullptr), row++, 0, 1, 2);
        auto *resetPlayTime = new QPushButton(tr("重置"), this);
        resetPlayTime->setObjectName(QStringLiteral("instancePlayTimeReset"));
        resetPlayTime->setToolTip(tr("清掉这个实例已记录的累计游玩时间和上次游玩时长。"));
        Theme::markButton(resetPlayTime, Theme::ButtonKind::Danger);
        connect(resetPlayTime, &QPushButton::clicked, this,
                &InstanceSettingsDialog::resetPlayTimeRequested);
        grid->addWidget(MainWindowPageLayout::makeSettingRow(this, m_playTimeLabel, QString(), resetPlayTime),
                        row++, 0, 1, 2);
    }

    // ---------- 五颗「对整个实例动手」的按钮（一张三列网格）----------
    //
    // 导出整合包 / 检查更新 / 检查并修复 / 复制实例 / 删除实例 —— 按"恒定宽度和高度、
    // 每一行三个、间距适中且左右铺满"的规格收成一张网格：每行三颗，每格还是"按钮 +
    // 它那句解释"（**解释一个字都没丢**，只是从整幅宽缩成三分之一宽，靠
    // `setWordWrap(true)` 自己折行）。
    //
    // NOTE: 五颗一行摆三颗 ⇒ 最后那一行只填得满前两格。**空着的那一格不摊开**是写意的
    // （见 layoutActionCells() 里那段）：五颗里有一颗比别人宽就不是"恒定"了。
    auto *actionGrid = newActionGrid();
    QList<QWidget *> actionCells;
    {
        auto [cell, column] = makeZeroColumn(this);
        auto *button = new QPushButton(exportText(), cell);
        button->setObjectName(exportButtonName());
        Theme::markButton(button, Theme::ButtonKind::Neutral);
        button->setToolTip(exportButtonTooltip());
        connect(button, &QPushButton::clicked, this, [this]() {
            // 默认文件名用**这个实例的目录名**（同「导出启动脚本」那一段的理由：它一定
            // 是合法文件名）。扩展名给 `.mrpack`：两个筛选项里 Modrinth 那一份是默认，
            // 而 Windows 的保存框会按**当前选中的筛选项**补扩展名。
            QString dir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
            if (dir.isEmpty()) {
                dir = QDir::homePath();
            }
            const QString suggested =
                dir + QLatin1Char('/') + m_instanceId + QStringLiteral(".mrpack");
            // 开的是 **Windows 自己的保存框**（和「导出启动脚本」「浏览…选 java.exe」
            // 同一档：全项目就剩这几处独立窗）。那个框上的「取消」= 什么都不做。
            //
            // NOTE: 两个筛选项的顺序**就是默认格式的顺序**（保存框默认选第一个），所以
            // Modrinth 那一份排在前面 —— 它不需要 API key、也不受"作者关了第三方
            // 分发"影响，是能完整还原的那一份。
            const QString modrinthFilter = tr("Modrinth 整合包 (*.mrpack)");
            const QString curseForgeFilter = tr("CurseForge 整合包 (*.zip)");
            QString selectedFilter = modrinthFilter;
            QString picked = QFileDialog::getSaveFileName(
                this, exportText(), suggested,
                modrinthFilter + QStringLiteral(";;") + curseForgeFilter, &selectedFilter);
            if (picked.isEmpty()) {
                return;
            }
            // 格式这一件事**只认扩展名**（见 .hpp 里那个信号的 NOTE）。用户自己敲一个
            // `我的包` 时保存框不一定补扩展名（补不补是那个框的事，我们管不着）——
            // 那就按他选中的筛选项补一个，别让盘上出现一个叫 `我的包`、内容是 Modrinth
            // 清单的文件：名字和正文对不上，正是这一整套里最容易悄悄发生的一种错。
            const QString lower = picked.toLower();
            if (!lower.endsWith(QStringLiteral(".mrpack")) &&
                !lower.endsWith(QStringLiteral(".zip"))) {
                picked += selectedFilter == curseForgeFilter ? QStringLiteral(".zip")
                                                             : QStringLiteral(".mrpack");
            }
            emit exportModpackRequested(picked);
        });
        prepareActionButton(button);
        column->addWidget(button);
        cell->setToolTip(exportHint());
        actionCells.append(cell);
    }

    // ---------- 「检查并修复」----------
    //
    // 摆在「检查更新」旁边：两颗都是"检查…"这一档（点了之后**会出现一个结果**，而不是改一格
    // 设置就写盘），只是一颗查的是**模组有没有新版本**、这一颗查的是**游戏文件齐不齐**。
    {
        auto [cell, column] = makeZeroColumn(this);
        auto *button = new QPushButton(integrityCheckText(), cell);
        button->setObjectName(integrityCheckButtonName());
        button->setToolTip(integrityCheckTooltip());
        Theme::markButton(button, Theme::ButtonKind::Neutral);
        connect(button, &QPushButton::clicked, this,
                [this]() { emit integrityCheckRequested(); });
        prepareActionButton(button);
        column->addWidget(button);
        cell->setToolTip(integrityCheckHint());
        actionCells.append(cell);
    }

    // ---------- 「复制实例」----------
    //
    // 这一颗让人挑一个目标文件夹、再给复制出来的实例起个名字。摆在「检查更新」下面 ——
    // 它和上面两颗是同一档（一颗按钮 + 底下那句解释），而这一颗的落点比上面那两颗重
    // （会多出一整个实例目录）。
    //
    // NOTE: 这一颗**只发信号**（和「导出…」「检查更新」同一个形状）：挑哪个游戏文件夹、
    // 问新名字、跑真那份拷贝，全在 MainWindow —— 那几步要 AppPaths（目标在**另一个**
    // 游戏文件夹里，见 VersionManager::copyInstance）和 InlineMessage 的"问一句名字"
    // （都要覆盖层），这一层两样都没有。
    {
        auto [cell, column] = makeZeroColumn(this);
        auto *button = new QPushButton(copyText(), cell);
        button->setObjectName(copyButtonName());
        button->setToolTip(copyButtonTooltip());
        connect(button, &QPushButton::clicked, this,
                [this]() { emit copyInstanceRequested(); });
        prepareActionButton(button);
        column->addWidget(button);
        cell->setToolTip(copyHint());
        actionCells.append(cell);
    }

    // ---------- 两颗「快照」----------
    //
    // 摆在「复制实例」和「删除实例」中间：这一行是"整个实例"这一档的事儿 —— 左边那颗多出
    // 一个实例、这两颗是把这一份**留起来 / 换回去**、右边那颗少掉一个。三颗都不动一格设置，
    // 都是"对着整个实例做一件事"。
    //
    // NOTE: 「创建快照…」带省略号是**故意的**：按下去之后 MainWindow 会问一句"要不要连着
    // 能重新下回来的那部分一起打包"（`InlineMessage::ask` 那三个选项）。省略号在这套
    // 界面里就是"后面还会问一句"的意思，别为了好看去掉。
    {
        auto [cell, column] = makeZeroColumn(this);
        auto *button = new QPushButton(createSnapshotText(), cell);
        button->setObjectName(createSnapshotButtonName());
        button->setToolTip(createSnapshotTooltip());
        connect(button, &QPushButton::clicked, this,
                [this]() { emit createSnapshotRequested(); });
        prepareActionButton(button);
        column->addWidget(button);
        cell->setToolTip(createSnapshotHint());
        actionCells.append(cell);
    }
    {
        auto [cell, column] = makeZeroColumn(this);
        auto *button = new QPushButton(restoreSnapshotText(), cell);
        button->setObjectName(restoreSnapshotButtonName());
        button->setToolTip(restoreSnapshotTooltip());
        // 红：它把**现在这一份**整个换掉 —— 和「删除实例」同一档（`Danger` 那一档的界定
        // 是"删了就拿不回来"，里面点名的几条里就有**还原**："还原上一次"）。
        //
        // NOTE: 还原之前自动留的那一份安全快照（对面那颗「打开备份目录」翻得到）是**兜底**，
        // 不是"这一步没风险"：用户按下去那一刻，屏幕上这一份就已经不在了，想回来得
        // 自己再走一趟还原。红色说的正是这件事。
        Theme::markButton(button, Theme::ButtonKind::Danger);
        connect(button, &QPushButton::clicked, this,
                [this]() { emit restoreSnapshotRequested(); });
        prepareActionButton(button);
        column->addWidget(button);
        auto *hint = new QLabel(restoreSnapshotHint(), cell);
        hint->setWordWrap(true);
        column->addWidget(hint);
        auto *inspect = new QPushButton(tr("检查中断恢复…"), cell);
        inspect->setObjectName(QStringLiteral("inspectRecoveryButton"));
        Theme::markButton(inspect, Theme::ButtonKind::Neutral);
        connect(inspect, &QPushButton::clicked, this, &InstanceSettingsDialog::inspectRecoveryRequested);
        column->addWidget(inspect);
        actionCells.append(cell);
    }

    // ---------- 「删除实例」----------
    //
    // 删除实例的按钮摆在「复制实例」下面 ——
    // 那两颗是这一页上仅有的两个"对整个实例动手"的动作，一个多出一份、一个少掉一份。
    //
    // 见 .hpp 那段：删除这件事在实例行行尾那颗垃圾桶上已经有了，这一颗是它的第二个
    // 入口 —— 真去删的那条路（两道闸 + 问一句 + 删目录）一行都没重写。
    //
    // 红的（`ButtonKind::Danger`）：这一档就是「不可撤销（如修改存档，删除实例）」，
    // 而这一页上它是唯一一颗真会丢东西的按钮。紧挨着它的「复制实例」是绿的 —— 同一个
    // 位置上一红一绿，正好是"这两个不一样"最省字的说法。
    {
        auto [cell, column] = makeZeroColumn(this);
        auto *button = new QPushButton(deleteText(), cell);
        button->setObjectName(deleteButtonName());
        button->setToolTip(deleteButtonTooltip());
        Theme::markButton(button, Theme::ButtonKind::Danger);
        connect(button, &QPushButton::clicked, this,
                [this]() { emit deleteInstanceRequested(); });
        prepareActionButton(button);
        column->addWidget(button);
        auto *hint = new QLabel(deleteHint(), cell);
        hint->setWordWrap(true);
        column->addWidget(hint);
        actionCells.append(cell);
    }

    // 概览只留基本信息和快捷方式；维护与实例操作由 buildLifecyclePage() 负责。
    // 更新入口已从概览移除，这一张网格里的控件全部隐藏，只剩布局本身要收掉。
    for (QWidget *cell : actionCells) {
        cell->hide();
    }
    delete actionGrid;

    // ---------- 常用目录那一排按钮 ----------
    //
    // 「快捷方式」**不是一页**：这一排按钮就是概览页的一部分（键、objectName、底下那句
    // 话都在这一块里，和别的页没有关系）。
    //
    // 这一排**不是写死的网格**，而是两段 —— 真有东西的摆在外面、其余收在「更多目录…」
    // 里。摆法的那一份唯一出处是 relayoutShortcuts()（这一段只管把控件建出来、按**表序**
    // 收进 m_shortcutButtons）。
    {
        auto *cell = new QWidget(this);
        auto *column = new QVBoxLayout(cell);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(6);
        // 记住这两样：relayoutShortcuts() 之后还要拿它们重摆（那时早在别的函数里了）。
        m_shortcutCell = cell;

        // NOTE: `shortcutPerRow()`（一排几颗）和这一套的 `kActionButtonsPerRow` **必须
        // 是同一个数**（眼下都是 3）。排布改由 `layoutActionButtons()` 按后者算，
        // 前者只剩"这颗按钮是哪一路目录"那一层意思；两边哪天分叉了，会排成六列或者
        // 三条 —— 界面上一眼看得出来，但没有任何断言拦得住。
        auto *buttonGrid = newActionGrid();
        m_shortcutGrid = buttonGrid;
        const QStringList texts = shortcutTexts();
        const QStringList keys = shortcutPathKeys();
        for (int i = 0; i < texts.size(); ++i) {
            auto *button = new QPushButton(texts.value(i), cell);
            // 这一排 = "开一个目录"（一般性）。
            Theme::markButton(button, Theme::ButtonKind::Neutral);
            button->setObjectName(shortcutButtonName(i));
            const QString key = keys.value(i);
            // 路径**现场问 m_instanceDir / m_fileDirs**（不是捕获一份快照）：setInstanceDir()
            // 是在构造函数之后才被调的，捕获快照的话拿到的永远是空串 —— 这颗按钮点了什么都
            // 不会发生，而且不报错。
            connect(button, &QPushButton::clicked, this, [this, key] {
                emit openInstanceFolderRequested(
                    key == QStringLiteral("dir") ? m_instanceDir : m_fileDirs.value(key));
            });
            // **按表序**收着（不是按摆出来的次序）—— `shortcutButtonName(i)` 那句
            // "第 i 颗是第 i 路目录"就还成立，两段怎么挪都不影响它。
            m_shortcutButtons.append(button);
        }
        // 摆法整份交给它：这会儿 m_shortcutPresentKeys 还是"全都在"，于是这五颗全在外面、
        // 也没有「更多目录」（见 .hpp 里 setFolderPresentKeys() 那条 NOTE）。
        relayoutShortcuts();
        column->addLayout(buttonGrid);
        cell->setToolTip(filesHint(QStringLiteral("shortcut")));
        grid->addWidget(overviewSection(tr("文件与导出"), this), row++, 0, 1, 2);
        grid->addWidget(MainWindowPageLayout::makeSettingRow(this, MainWindowPageLayout::makeSettingRowTitle(tr("打开目录"), this),
                                    QString(), cell, true),
                        row++, 0, 1, 2);
    }

    // ---------- 「显示哪些页」----------
    //
    // 「概览里调整是否显示」：七条一条一个勾选框（顺序见 optionalPageKeys()），勾/不勾
    // 直接决定左栏那一列里有没有这一条；勾掉的记在实例自己的 wtfml.meta 里（见
    // pageChoices()）。**不可用的那一条不勾、灰着、tooltip 说为什么** —— 灰着而不是藏
    // 起来：用户得看得见"有这么一页、但这个实例上用不上、为什么"。
    //
    // NOTE: 这些勾选框会被 watchEditors() 按类型自动接上（它接 `QAbstractButton::clicked`）
    // ⇒ 勾一下就走"改了就写"那条路。程序化 `setChecked()`（refreshPageChecks()）
    // **不发 clicked**，所以重建/灌值不会白白写一次盘。
    {
        auto *cell = new QWidget(this);
        auto *column = new QVBoxLayout(cell);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(6);


        auto *checkGrid = new QGridLayout;
        checkGrid->setContentsMargins(0, 0, 0, 0);
        checkGrid->setHorizontalSpacing(8);
        checkGrid->setVerticalSpacing(6);
        const QStringList keys = optionalPageKeys();
        const QStringList texts = optionalPageTexts();
        for (int i = 0; i < keys.size(); ++i) {
            const QString key = keys.at(i);
            auto *check = new QCheckBox(texts.value(i), cell);
            check->setObjectName(pageCheckName(key));
            // 一开始全是勾上的：**可用就显示**是默认（见 .hpp 里 pageChoices() 那条）。
            // 真正的样子由 refreshPageChecks() 摆（构造末尾、以及每一次可用性变化之后）。
            check->setChecked(true);
            m_pageChecks.insert(key, check);
            // 两条一排。
            // 勾一下：先把这一格的样子**记进那一份选择**，再让左栏跟着变。
            //
            // 少了这一发，用户勾掉「存档」之后**左栏里那一条还在** —— `shownEntryKeys()`
            // 读的是那一份选择（`m_pageOverrides`，勾选框只是它的画），而"写盘那条线"
            // （watchEditors() → settingsEdited）要等 MainWindow 把那一次改动写进
            // wtfml.meta、再灌回来，左栏才掉。用户眼前就是一颗已经空了的勾选框配着一条
            // 还在的左栏，中间隔着一次写盘。「记到盘上」和「当场看见」是两件事，两条都得接。
            //
            // NOTE: 顺序 —— 这一发接在 watchEditors() 那一发**之前**（构造函数先跑），
            // 所以 MainWindow 收到 settingsEdited 去读 pageChoices() 时，左栏与那一份
            // 选择都已经是最新的。
            connect(check, &QAbstractButton::clicked, this, [this, key](bool checked) {
                m_pageOverrides.insert(key, checked);
                rebuildEntryList();
            });
            checkGrid->addWidget(check, i / 2, i % 2, Qt::AlignLeft);
        }
        column->addLayout(checkGrid);
        grid->addWidget(MainWindowPageLayout::makeSettingRow(this, MainWindowPageLayout::makeSettingRowTitle(showPagesLabelText(), this), QString(), cell, true), row++, 0, 1, 2);
    }

}
