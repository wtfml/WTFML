/**
 * @file InstanceSettingsFilePages.cpp
 * @brief 「实例设置」右栏文件那八页（服务器列表 + 七份同一个 `InstanceFilesPage`）。
 *
 * 类是同一个类（`InstanceSettingsDialog`），这一摊按"一页一摊"分 TU，理由见
 * `InstanceSettingsDialogInternals.hpp` 文件头。
 */

#include "InstanceSettingsDialog.hpp"
#include "ItemViewContextMenu.hpp"
#include <QCoreApplication>
#include "ResourcePackPage.hpp"
#include "WheelSafeComboBox.hpp"

#include "Animations.hpp"
#include "Theme.hpp"   // 浮层的底色（Theme::Part::Panel）

#include <QAbstractItemView>
#include <QApplication>
#include <QStyle>
#include <QStyleOptionViewItem>  // eventFilter()：那两件导航控件共同的兜底（focusInEvent 替人挑第一条）
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
#include "DesignDefaults.hpp"
#include "VersionTreeDelegate.hpp"

/// 「文件管理」里哪几页有「启用 / 禁用」。
///
/// 文件改名开关仅用于模组和光影；资源包通过专用页面修改 options.txt。
/// 另外三页给
/// 了也没用 —— 数据包要自己搬进存档，原理图要 Litematica 主动去读，存档干脆不是一个文件；
/// 给它们一颗"禁用"只会让用户以为改名就等于不生效了。
bool fileKeyCanToggle(const QString &key) {
    return key == QStringLiteral("mod") || key == QStringLiteral("shader");
}

#include "InstanceSettingsDialogInternals.hpp"

using namespace InstanceSettingsDialogInternals;

namespace {
/// 服务器列表「地址」那一列的代理：放不下时省略中间、保留开头和端口（见 elidedServerAddress()）。
/// 表开着 wordWrap（状态那一列要两行），这一列关掉换行，自己省略。
class ServerAddressDelegate final : public DesignDefaults::RowDelegate {
public:
    using DesignDefaults::RowDelegate::RowDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        RowDelegate::initStyleOption(option, index);
        option->features &= ~QStyleOptionViewItem::WrapText;
        if (option->rect.width() <= 0) {
            return;  // sizeHint 那一路没有格子宽度
        }
        const int margin = 2 * (QApplication::style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, option->widget) + 1);
        option->text = InstanceSettingsDialog::elidedServerAddress(
            option->text, option->fontMetrics, option->rect.width() - 2 * DesignDefaults::cellTextInset() - margin);
    }
};
}  // namespace

void InstanceSettingsDialog::popUpServerMenu(QTreeWidgetItem *item, const QPoint &globalPos) {
    if (item == nullptr || m_serverTree == nullptr || m_serverEdit == nullptr || m_serverJoin == nullptr) {
        return;
    }
    // 右键先选中那一行（只选它一行）：「加入」「编辑」按的是列表下方那两颗，它们认选中行。
    m_serverTree->clearSelection();
    m_serverTree->setCurrentItem(item);
    item->setSelected(true);
    // 上一次那块还没收走就换掉，免得两个同名浮层并存。
    if (auto *old = findChild<QWidget *>(serverMenuName())) {
        old->hide();
        old->setObjectName(QString());
        old->deleteLater();
    }
    const QString address = item->data(1, MainWindowUI::serverAddressRole()).toString();
    auto *menu = new MenuPopup(this);
    menu->setObjectName(serverMenuName());
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto *box = new QVBoxLayout(menu);
    box->setContentsMargins(kMenuPadding, kMenuPadding, kMenuPadding, kMenuPadding);
    box->setSpacing(kMenuRowSpacing);
    const auto addEntry = [&](const QString &name, const QString &text, bool enabled, std::function<void()> action) {
        auto *button = new QPushButton(text, menu);
        button->setObjectName(name);
        button->setStyleSheet(menuEntryStyle(menu->palette()));
        button->setCursor(Qt::PointingHandCursor);
        button->setEnabled(enabled);
        connect(button, &QPushButton::clicked, this, [menu, action = std::move(action)] {
            menu->close();
            action();
        });
        box->addWidget(button);
    };
    // 「加入」收着时（版本没有快速加入参数）灰掉，和下方那颗的可见性一致。
    addEntry(serverMenuJoinName(), serverMenuJoinText(), !m_serverJoin->isHidden() && m_serverJoin->isEnabled(),
             [this] { m_serverJoin->click(); });
    addEntry(serverMenuEditName(), serverEditText(), m_serverEdit->isEnabled(), [this] { m_serverEdit->click(); });
    addEntry(serverMenuCopyName(), serverMenuCopyText(), !address.isEmpty(),
             [this, address] { emit copyServerAddressRequested(address); });
    menu->popUpAt(globalPos);
}

/// 「文件管理」那八页（服务器列表 + 七份同一个 `InstanceFilesPage`）里**的一页**。
///
/// 目录一律由调用方算好递进来（见这一摊开头那段），这里只搭壳。**第一次切过去才跑**
/// （见 `InstanceSettingsDialog::ensurePage()`）—— 打开对话框那一刻只建概览。
void InstanceSettingsDialog::buildFilePage(const QString &key) {
    if (key == serverPageKey()) {
        buildServerPage();
        return;
    }
    // ---------- 文件那几页（见 .hpp 里 filePageKeys() 那一段）----------
    //
    // 这八页说的是**这个实例自己的目录**：`--gameDir` 就是 `versions/<实例名>/`，所以
    // `mods/ resourcepacks/ shaderpacks/ datapacks/ saves/` 都在里头 —— 实例之间天然隔离，
    // 在这一页里动什么都不会碰到别的实例。
    //
    // 八页分两种形状：服务器列表读一份递进来的解析结果（③），其余七页是同一个
    // InstanceFilesPage（②）。**目录一律由调用方算好递进来**（setInstanceDir / setFileDirs）
    // —— 那几个目录名是 MC 的约定，src/download/ResourceCatalog::folderForKind() 是唯一
    // 出处，这一层不抄第二份。
    //
    // NOTE: 页面标题（那行粗体）**故意没有** —— 左栏那一列一直亮着，写在这里是把同一句话
    // 挨着说两遍。每一页正文底下那行小字才是这一页**特有**的那句。
    {
        // ② 中间那七页：同一个控件，喂不同的目录与三条开关。
            QWidget *page = addPage(key);
            auto *layout = page != nullptr ? qobject_cast<QVBoxLayout *>(page->layout()) : nullptr;
            if (layout == nullptr) {
                return;
            }

            // 「配置文件」那一页是**两格子**（见 .hpp 里 configFilePage() 上面那一段：
            // 开不出第二个整页覆盖层）。第 0 格是这一张列表（连「上一级」和底下那行小字
            // 一起），第 1 格是编辑器。别的几页只有一格 —— 所以下面这些"摆进这一页"的
            // 动作全部改摆进 `target`，而不是一律摆进 `layout`。
            const bool isConfig = key == QStringLiteral("config");
            // 存档那一页（见下面 setCanEdit / setImportText / setBatchText
            // 那三处 —— 支笔、导入世界、批量编辑都只在这一页上）。
            const bool isSave = key == QStringLiteral("save");
            const bool isResourcePack = key == QStringLiteral("resourcepack");
            QStackedWidget *resourceStack = nullptr;
            QPushButton *resourceMode = nullptr;
            if (isResourcePack) {
                resourceStack = new QStackedWidget(page);
                auto *switchMode = new QPushButton(tr("启用与顺序"), page);
                resourceMode = switchMode;
                switchMode->setObjectName(QStringLiteral("resourcePackMode"));
                switchMode->setCheckable(true);
                Theme::markButton(switchMode, Theme::ButtonKind::Neutral);
                layout->addWidget(switchMode, 0, Qt::AlignLeft);
                switchMode->hide();
                connect(switchMode, &QPushButton::toggled, this, [this, resourceStack, switchMode](bool on) {
                    resourceStack->setCurrentIndex(on ? 1 : 0);
                    switchMode->setText(on ? tr("返回文件管理") : tr("启用与顺序"));
                    switchMode->setVisible(on);
                    if (on) emit resourcePackPageEntered();
                });
            }
            QWidget *listHost = page;
            QVBoxLayout *target = layout;
            if (isConfig || isSave || isResourcePack) {
                auto *stack = isResourcePack ? resourceStack : new QStackedWidget(page);
                if (isConfig) {
                    m_configStack = stack;
                } else if (isSave) {
                    m_worldStack = stack;
                }
                listHost = new QWidget(stack);
                auto *listLayout = new QVBoxLayout(listHost);
                // 第 0 格自己不留白：它四周的留白本来就是外面那一页给的，这一格和那一页
                // 是同一块地方（切过来切过去不该看见内容整体缩一下）。
                listLayout->setContentsMargins(0, 0, 0, 0);
                listLayout->setSpacing(layout->spacing());
                stack->addWidget(listHost);
                target = listLayout;
            }

            m_filePageBuilders.insert(key, [this, key, page, layout, listHost, target, resourceStack,
                                            resourceMode, isConfig, isSave, isResourcePack]() {
            auto *files = new InstanceFilesPage(listHost);
            files->setObjectName(filesPageName(key));
            if (resourceMode) {
                auto *mode = new QPushButton(tr("启用与顺序"), files);
                mode->setObjectName(QStringLiteral("resourcePackFileMode"));
                Theme::markButton(mode, Theme::ButtonKind::Neutral);
                connect(mode, &QPushButton::clicked, resourceMode, [resourceMode] { resourceMode->setChecked(true); });
                files->setCompactHeader(mode);
            } else if (key == QStringLiteral("schematic") || key == QStringLiteral("structure")) {
                files->setCompactHeader();
            }
            if (key == QStringLiteral("mod")) {
                files->setActionPanelEnabled(true);
                connect(files, &InstanceFilesPage::checkUpdatesRequested,
                        this, &InstanceSettingsDialog::checkUpdatesRequested);
                connect(files, &InstanceFilesPage::transferSelectionRequested,
                        this, &InstanceSettingsDialog::modTransferRequested);
                connect(files, &InstanceFilesPage::checkSelectedUpdatesRequested,
                        this, &InstanceSettingsDialog::modCheckSelectedUpdatesRequested);
                connect(files, &InstanceFilesPage::projectWebsiteRequested,
                        this, &InstanceSettingsDialog::modWebsiteRequested);
                connect(files, &InstanceFilesPage::inspectDependenciesRequested,
                        this, &InstanceSettingsDialog::modInspectDependenciesRequested);
            }
            // 存档只列文件夹（saves 底下那些 level.dat / session.lock 不是存档）。
            // 「模组」那一页**只列文件** —— `mods/` 底下那些目录不是模组
            // （`mcef-libraries`、Sinytra 的 `.connector` 这种是模组自己用的房子），
            // 而它们在屏幕上占着一行、行尾还挂着开关，点一下是把**整个文件夹**改了名
            // （真会发生）。另外三页不一样 —— 一个资源包 / 数据包 / 光影包
            // **本身就可以是一个目录**（游戏认没解压的那种），那是另一回事。
            files->setMode(key == QStringLiteral("save")   ? InstanceFilesPage::Mode::DirsOnly
                           : key == QStringLiteral("mod") ? InstanceFilesPage::Mode::FilesOnly
                                                          : InstanceFilesPage::Mode::FilesAndDirs);
            // 「原理图」与「配置文件」这两页**没有图**：`.litematic` 不在任何平台的清单里、
            // 配置文件是纯文本 —— 一整列占位拼图白占宽度。那两页干脆不建那一格
            // （判据只有这一处：别的几页都有真图标或游戏自己写的缩略图）。
            files->setRowIconsVisible(key != QStringLiteral("schematic")
                                      && key != QStringLiteral("config"));
            // 只有游戏会自己扫的那三个目录给「禁用」；存档**不给删**（理由见
            // InstanceFilesPage::setCanDelete()）。
            //
            // 「配置文件」也不给删：这一页的存在意义就是**改**（点一行开
            // 编辑器），行尾再挂一颗垃圾桶等于在"只想改一个数"的动线上摆一个删了就没了
            // 的键 —— 而这一页没有"恢复默认"（模组下次启动会重写一份默认的，但那要等它
            // 启动，而且不保证每个模组都这么干）。要删，那颗「打开文件夹」就在上面。
            files->setCanToggle(fileKeyCanToggle(key));
            files->setCanDelete(!isSave && !isConfig && key != QStringLiteral("schematic"));
            // **只有存档那一页**行尾有那支笔，而且只有那一页的目录行会去
            // 算真的大小/时间（见 InstanceFilesPage::setCanEdit()——两件事是一套的）。
            files->setCanEdit(isSave);
            files->setEmptyText(filesEmptyText(key));
            files->setDirectory(m_fileDirs.value(key));
            // 顶上那颗「下载新 X」：只有下载页里真有对应一栏的那四页有
            // （见 filesDownloadText()）。按下去**只报一个分类键**，切选项卡、挑那
            // 一行都在 MainWindow 那一层 —— 这一层够不着选项卡，也不该知道下载页左栏
            // 有几栏（见 browseDownloadsRequested() 那段）。
            files->setDownloadText(filesDownloadText(key), filesDownloadTip(key));
            // 状态那一栏里要不要有「可更新」——**判据和上面那颗「下载新 X」
            // 是同一个**（下载页里真有对应一栏的那四页）：只有那四页上的文件查得动更新，
            // 别的几页（原理图 / 存档 / 配置文件）里的东西根本不在任何平台的清单里。
            // 多一格永远筛不出东西的选项，用户只会以为"筛坏了"。
            files->setCanFilterUpdates(!filesDownloadText(key).isEmpty());
            // **那四页**再开一套「批量管理」（行尾的详情按钮 + 多选 + 底层
            // 那条悬浮操作条 + 顶上四颗横向筛选按钮）。判据**就是上面那个** ——
            // `filesNameOpensResource()` 说的正是"这一条认得出是哪个资源"，而详情按钮
            // 要的恰好是这一件事（见 MainWindow 那边 detailRequested 那一发）。
            // **不新开一张"哪几页算数"的名单** —— 判据就是上面那个函数。
            // 同一个判据还决定**"这一页底下那段说明挂不挂"**（见下面建 hint 那一处）：
            // 这四页（模组 / 资源包 / 数据包 / 光影包，也就是这套批量管理的那四页）底下
            // 不挂那段叙述，好让上面的框拉到底、把空间用满。其余几页（存档 / 原理图 /
            // 配置文件 / 服务器列表 / 快捷方式）底下那行小字**挂着** —— 它们讲的是别的
            // 规矩（比如数据包那句"下了不等于生效、要自己搬进存档"）。
            const bool batch = filesNameOpensResource(key);
            files->setBatchMode(batch);
            if (isSave) {
                files->setImportText(importWorldText(), importWorldTip());
                auto *backups = new QPushButton(tr("世界备份"), listHost);
                backups->setObjectName(QStringLiteral("instanceSaveBackups"));
                Theme::markButton(backups, Theme::ButtonKind::Neutral);
                files->setWorldLayout(backups);
                connect(backups, &QPushButton::clicked, this,
                        [this]() { showPage(QStringLiteral("worldbackup")); });
            }
            connect(files, &InstanceFilesPage::downloadRequested, this, [this, key] {
                const QString category = filesDownloadCategoryKey(key);
                if (!category.isEmpty()) {
                    emit browseDownloadsRequested(category);
                }
            });
            // 行尾那颗「详情」按下去：**只有"点得动"的那几页接**。判据就是
            // filesNameOpensResource() —— 不在表里的那几页既没有那一格按钮，这一条线
            // 也不接。接线这一层是多余的：真的按下去是 InstanceFilesPage 那边认出路径
            // 才发的（认不出来的那些那一颗是灰的，按不动）。
            // **右键点一行**走的也是这一发（**保留上一页的选择**，再进详情页）—— 判据、
            // 落点、收页那一套全都一样，这一层不必知道用户是按了那颗 ⓘ 还是右键了那一行。
            if (filesNameOpensResource(key)) {
                connect(files, &InstanceFilesPage::detailRequested, this,
                        [this, key](const QString &path) { emit fileResourceRequested(key, path); });
                // 这里**不转** `updateCheckRequested`：那一页顶上「可更新」在没查过的
                // 时候根本不摆出来（**没检查的时候先隐藏可更新按钮**）。检查更新这一栏
                // 只有「概览」那一颗（`checkUpdatesRequested()` 那条线）。

                // 批量那三发：**判据同上**（这三件事只在那四页上谈得上）。
                // NOTE: 转发上去的三个动作全**不改盘** —— 问不问那句、怎么落盘、落完要不要
                // 重列，都是 MainWindow 的事（这一层连 AppPaths 都够不着）。
                connect(files, &InstanceFilesPage::batchUpdateRequested, this,
                        [this, key](const QStringList &paths) {
                            emit fileBatchUpdateRequested(key, paths);
                        });
                connect(files, &InstanceFilesPage::batchToggleRequested, this,
                        [this, key](const QStringList &paths, bool disable) {
                            emit fileBatchToggleRequested(key, paths, disable);
                        });
                connect(files, &InstanceFilesPage::batchDeleteRequested, this,
                        [this, key](const QStringList &paths) {
                            emit fileBatchDeleteRequested(key, paths);
                        });
            }

            // 「配置文件」那一页多两样：上面一颗「上级」，以及"双击/回车
            // 有反应"。别的几页看的是一个平铺的目录，config/ 底下却还有层级
            // （`config/ftbquests/`、`config/paxi/`…），得能进去也得出得来。
            if (isConfig) {
                // 一颗也走这一页统一的那一套（三分之一宽、靠左）—— 见 layoutActionCells()
                // 那段：**少一颗的那一行不摊开**，一颗独苗撑满整幅会让它比别处的按钮大一号。
                auto *navRow = newActionGrid();
                m_configUp = new QPushButton(configUpText(), listHost);
                m_configUp->setObjectName(configUpButtonName());
                Theme::markButton(m_configUp, Theme::ButtonKind::Neutral);
                connect(m_configUp, &QPushButton::clicked, this,
                        &InstanceSettingsDialog::leaveConfigDir);
                layoutActionButtons(navRow, {m_configUp});
                target->addLayout(navRow);
                // 摆到"还没接过目录"那一档（m_configRoot 要等 setFileDirs 才有）。
                updateConfigUpButton();

                connect(files, &InstanceFilesPage::activateRequested, this,
                        &InstanceSettingsDialog::onConfigEntryActivated);
            }
            connect(files, &InstanceFilesPage::openFolderRequested, this,
                    &InstanceSettingsDialog::openInstanceFolderRequested);
            connect(files, &InstanceFilesPage::toggleRequested, this,
                    &InstanceSettingsDialog::toggleInstanceFileRequested);
            connect(files, &InstanceFilesPage::deleteRequested, this,
                    &InstanceSettingsDialog::deleteInstanceFileRequested);
            // 存档那一页那三样（那支笔 / 导入世界 / 批量编辑）**故意不在这儿
            // 接线**。上面那几发之所以要绕这一层，是因为它们需要翻译（`downloadRequested`
            // 要把页键翻成下载页的分类键、`toggleRequested` 是整个文件管理共用的）；
            // 而这三发只属于存档那一页、也不用翻任何东西 —— 由 MainWindow 直接连到
            // `filesPage("save")` 上（那一页就是它）。多一层纯转发的信号，只是多一份
            // 要跟着改的声明。
            // 页可能是 setPageUsable() 之后才建出来的：把当前的"用不上"状态补上，
            // 否则这一页会显示成普通空目录。
            if (m_pageAvailable.contains(key) && !m_pageAvailable.value(key))
                files->setUnavailable(pageUnavailableText(key));
            m_filesPages.insert(key, files);
            if (key == QStringLiteral("schematic") || isSave) {
                // 工具条属于列表：挂在文件页自己的布局里，再挪到搜索框之下、列表之上
                // （和截图 / 结构页同一个位置）。
                addLifecycleBatchActions(key, files->list(), qobject_cast<QVBoxLayout *>(files->layout()));
                files->placeAboveList(files->findChild<QWidget *>(QStringLiteral("instanceLifecycleBatch_%1").arg(key)));
            }
            target->addWidget(files, 1);
            if (!batch && !isSave && key != QStringLiteral("schematic") && key != QStringLiteral("structure")) {
                files->setToolTip(filesHint(key));
            }

            // 第 1 格：编辑器。**建一次留着**（见 .hpp 里 configFilePage() 那两条 NOTE）
            // —— 每一趟开文件只是往里灌内容（openConfigEditor()），接线只有一次。
            if (isConfig) {
                m_configEditor = new ConfigFileEditorPage(m_configStack);
                m_configStack->addWidget(m_configEditor);
                layout->addWidget(m_configStack, 1);
            }
            // 存档那一页的第 1 格：世界设置，做法和理由同上（见 .hpp 里
            // openWorldSettings() 上面那一段）。
            if (isSave) {
                m_worldSettings = new WorldSettingsPage(m_worldStack);
                m_worldStack->addWidget(m_worldSettings);
                layout->addWidget(m_worldStack, 1);
            }
            if (isResourcePack) {
                m_resourcePackPage = new ResourcePackPage(resourceStack);
                resourceStack->addWidget(m_resourcePackPage);
                layout->addWidget(resourceStack, 1);
                connect(m_resourcePackPage, &ResourcePackPage::refreshRequested,
                        this, &InstanceSettingsDialog::resourcePackPageEntered);
            }
            if (!m_fileDirs.value(key).isEmpty()) files->setDirectory(m_fileDirs.value(key));
            for (auto it = m_pendingFileIcons.constBegin(); it != m_pendingFileIcons.constEnd(); ++it)
                files->setIconForPath(it.key(), it.value());
            for (auto it = m_pendingFileResources.constBegin(); it != m_pendingFileResources.constEnd(); ++it)
                files->setResourceForPath(it.key(), it.value());
            if (m_hasPendingUpdates)
                files->setUpdateResult(m_pendingOutdatedPaths, m_pendingUpdatesChecked);
            });
    }
}

void InstanceSettingsDialog::buildServerPage() {
    // ③ 服务器列表：读 servers.dat 的结果由调用方递进来（这一层不 include src/）。
    {
            QWidget *page = addPage(QStringLiteral("server"));
            auto *layout = qobject_cast<QVBoxLayout *>(page->layout());
            if (layout != nullptr) {
                auto *top = newActionGrid();
                m_serverRefresh = new QPushButton(serverRefreshText(), page);
                m_serverRefresh->setObjectName(serverRefreshButtonName());
                Theme::markRefreshButton(m_serverRefresh);  // F5 认它
                Theme::markButton(m_serverRefresh, Theme::ButtonKind::Neutral);
                // 重读要碰 NBT 解析（在 src/ 里），所以这一发只能交给 MainWindow。
                connect(m_serverRefresh, &QPushButton::clicked, this,
                        &InstanceSettingsDialog::refreshServerListRequested);
                m_serverEdit = new QPushButton(serverEditText(), page);
                m_serverEdit->setObjectName(serverEditButtonName());
                m_serverEdit->setToolTip(serverEditTip());
                Theme::markButton(m_serverEdit, Theme::ButtonKind::Neutral);
                connect(m_serverEdit, &QPushButton::clicked, this, [this] {
                    QTreeWidgetItem *item = selectedServerItem();
                    if (item == nullptr) {
                        return;  // 没选中（这颗本来就是灰的；这是第二道闸）。
                    }
                    emit editServerRequested(item->data(0, MainWindowUI::serverNameRole()).toString(),
                                             item->data(1, MainWindowUI::serverAddressRole())
                                                 .toString());
                });
                // 「添加」和「删除」：编辑要能改名字改地址、也要能删，服务器那一页里还得
                // 有一颗添加服务器的按钮。和「编辑」同一档：这一层不碰盘、也不问话，
                // 只发一条，落盘和问话都在 MainWindow。
                // 两颗的**可点性不一样**，这是它们和「编辑」唯一的区别：
                // - 「添加」不依赖选中行（要加的那一台还没出现在表里），所以**常亮**；
                // - 「删除」动的是选中那一行，跟着选中走（见 updateServerSelectionActions）。
                // 档位：「添加」是普通动作（Neutral）；「删除」是这一页上唯一一颗真会
                // 从盘上拿掉东西的键 —— 照「删除实例」那一颗（`ButtonKind::Danger`，
                // 见 ui/Theme.hpp 那句"不可撤销"）。
                m_serverAdd = new QPushButton(serverAddText(), page);
                m_serverAdd->setObjectName(serverAddButtonName());
                m_serverAdd->setToolTip(serverAddTip());
                Theme::markButton(m_serverAdd, Theme::ButtonKind::Neutral);
                connect(m_serverAdd, &QPushButton::clicked, this,
                        &InstanceSettingsDialog::addServerRequested);

                m_serverDelete = new QPushButton(serverDeleteText(), page);
                m_serverDelete->setObjectName(serverDeleteButtonName());
                m_serverDelete->setToolTip(serverDeleteTip());
                Theme::markButton(m_serverDelete, Theme::ButtonKind::Danger);
                connect(m_serverDelete, &QPushButton::clicked, this, [this] {
                    const auto items = selectedServerItems();
                    if (items.isEmpty()) {
                        return;  // 同上：这一颗本来就是灰的。
                    }
                    const auto entry = [](const QTreeWidgetItem *item) {
                        return qMakePair(item->data(0, MainWindowUI::serverNameRole()).toString(),
                                         item->data(1, MainWindowUI::serverAddressRole()).toString());
                    };
                    if (items.size() == 1) {
                        const auto one = entry(items.first());
                        emit deleteServerRequested(one.first, one.second);
                        return;
                    }
                    QList<QPair<QString, QString>> entries;
                    for (const auto *item : items) entries.append(entry(item));
                    emit deleteServersRequested(entries);
                });
                // 「加入服务器」：用游戏自己的 `--quickPlayMultiplayer` 直接连进去。
                // 世界和服务器都做一个临时占位（加入选中的世界，仅限有这个参数的版本 /
                // 加入服务器），先看看能不能生效。
                // 默认**收着**（文字一开始是空的，见 setServerJoinText()）：一个没有
                // 那个启动参数的版本上露着它就是"按下去游戏直接报错退出"（jopt-simple
                // 认不出那个选项）。露不露由调用方读了版本 json 之后说了算。
                m_serverJoin = new QPushButton(QString(), page);
                m_serverJoin->setObjectName(serverJoinButtonName());
                m_serverJoin->setVisible(false);
                // 「加入」是这一页上唯一一颗"真的要开游戏"的键（和概览页那颗启动同一档）。
                Theme::markButton(m_serverJoin, Theme::ButtonKind::Accent);
                connect(m_serverJoin, &QPushButton::clicked, this, [this] {
                    QTreeWidgetItem *item = selectedServerItem();
                    if (item == nullptr) {
                        return;  // 同上。
                    }
                    emit joinServerRequested(item->data(0, MainWindowUI::serverNameRole()).toString(),
                                             item->data(1, MainWindowUI::serverAddressRole())
                                                 .toString());
                });
                // 顺序 = 用户给的那一串（刷新 / 编辑 / 添加 / 删除），「加入服务器」压尾。
                // 每行三颗（`kActionButtonsPerRow`）⇒ 自动折成「刷新 编辑 添加 / 删除 加入」。
                int actionColumn = 0;
                for (auto *button : {m_serverRefresh, m_serverEdit, m_serverAdd, m_serverDelete, m_serverJoin})
                    top->addWidget(button, 0, actionColumn++);
                top->setColumnStretch(actionColumn, 1);
                layout->addLayout(top);

                m_serverNotice = new QLabel(page);
                m_serverNotice->setObjectName(serverNoticeName());
                m_serverNotice->setWordWrap(true);
                layout->addWidget(m_serverNotice);

                auto *favoriteTools = new QHBoxLayout;
                m_serverFavoritesOnly = new QCheckBox(tr("仅看收藏"), page);
                m_serverFavoritesOnly->setObjectName(QStringLiteral("serverFavoritesOnly"));
                favoriteTools->addWidget(m_serverFavoritesOnly);
                favoriteTools->addStretch();
                m_serverFavorite = new QPushButton(tr("收藏服务器"), page);
                m_serverFavorite->setObjectName(QStringLiteral("serverFavorite"));
                m_serverFavorite->setCheckable(true);
                Theme::markButton(m_serverFavorite, Theme::ButtonKind::Neutral);
                m_serverFavorite->setToolTip(tr("收藏保存在启动器中，不修改游戏的服务器列表。"));
                favoriteTools->addWidget(m_serverFavorite);
                layout->addLayout(favoriteTools);
                connect(m_serverFavoritesOnly, &QCheckBox::toggled, this,
                        &InstanceSettingsDialog::refreshServerFavorites);
                connect(m_serverFavorite, &QPushButton::clicked, this, [this] {
                    auto *item = selectedServerItem();
                    if (item) {
                        const auto address = item->data(1, MainWindowUI::serverAddressRole()).toString();
                        emit serverFavoriteRequested(address, !m_serverFavorites.contains(address));
                    }
                    // The host acknowledges persistence through setServerFavorites.
                    updateServerSelectionActions();
                });

                m_serverTree = new QTreeWidget(page);
                m_serverTree->setObjectName(serverTreeName());
                m_serverTree->setHeaderLabels({serverNameColumnText(), serverAddressColumnText(),
                                               serverStatusColumnText()});
                // 「一行一个服务器」是平表不是树：不要每行前面那个展开三角。
                m_serverTree->setRootIsDecorated(false);
                m_serverTree->setUniformRowHeights(true);
                m_serverTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
                m_serverTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
                // 状态那一格是两行（`42 ms` / `7/20 人`，见 setServerStatuses）：
                // 这一句是那个换行**真的会换行**的前提 —— 条目代理只在开了 wordWrap 时
                // 才把正文按 QTextLayout 摆，否则那一格里会是"42 ms / 7/20 人"一行
                // （`\n` 被当成一个看不见的字符）。第 0 列不受影响：那两行是代理自己画的。
                m_serverTree->setWordWrap(true);

                // ---------- 三栏的宽度怎么分 ----------
                //
                // 默认是"最后一列吃掉多出来的宽度"，那样一来左边两行 MOTD 会被状态那一栏
                // 挤掉一大截，动不动就出省略号（MOTD 要能显示全）。
                // 这里让**第 0 列把剩下的全吃掉**，另外两列各占自己需要的那点地方 ——
                // · 地址那一栏按原文显示，放不下时省略中间（ServerAddressDelegate）；
                // · 状态那一栏按内容定宽（两行里更宽的那一行），文字**靠右**，所以整栏
                // 贴着右边停住、不跟着窗口一起长。
                QHeaderView *header = m_serverTree->header();
                if (header != nullptr) {
                    header->setStretchLastSection(false);
                    header->setSectionResizeMode(0, QHeaderView::Stretch);
                    header->setSectionResizeMode(1, QHeaderView::Interactive);
                    header->setSectionResizeMode(2, QHeaderView::Interactive);
                    m_serverTree->setColumnWidth(1, 240);
                    m_serverTree->setColumnWidth(2, 144);
                }
                // 表头那一格也跟着靠右，不然「状态」两个字会和它底下的数字错开一个身位。
                m_serverTree->headerItem()->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
                // 名字那一格画成**两行式的行**：前面一枚服务器图标、名字一行、底下小字一行
                // （那就是 MOTD）。和版本树/下载页那几张表是同一个代理 —— 一份实现多处用。
                // 只有第 0 列挂它（另外两列是普通的地址和状态）。
                m_serverTree->setItemDelegateForColumn(
                    0, new VersionTreeDelegate(m_serverTree, /*leafArrow=*/false,
                                               MainWindowUI::serverIconSize()));
                m_serverTree->setItemDelegateForColumn(1, new ServerAddressDelegate(m_serverTree));
                // 图标从表头字那条竖线起（代理自己只让 versionTreeRowPadding()，表头让 cellTextInset()）。
                m_serverTree->setProperty("wtfmlRowLeftPad",
                    DesignDefaults::cellTextInset() - MainWindowUI::versionTreeRowPadding());
                // 悬停底色由代理画（`Theme::rowHoverFill()`），但那一位 `State_MouseOver` 是**视图**填的。
                // 少了这一句，代理里那段一个字都不会生效 —— 不报错、断言也不一定先红，屏幕上就是"悬停没反应"。
                m_serverTree->setMouseTracking(true);
                // 行高**不是**那个常数：这一列逐条 `setSizeHint` 成 `serverRowHeight()`
                // （= versionTreeRowHeight() + 18 = 64，见下面「46 px 装不下」那一句），
                // 这一句是按同一档告诉控件"图标那一格有多大" —— 下载页那几张表也是这么配的
                // （见 MainWindowUI 里 resourceTreeIconSize() 那两行），不配的话条目自带的
                // 图标会被另算一遍尺寸。
                m_serverTree->setIconSize(
                    QSize(MainWindowUI::serverIconSize(), MainWindowUI::serverIconSize()));
                // 右键整行弹出「加入 / 编辑 / 复制地址」（popUpServerMenu()）。走
                // `Qt::CustomContextMenu` 拿"在哪儿按的"这个坐标。
                //
                // NOTE: 不用 QMenu（更别说 QMenu::exec()）：仓库里那几块浮层一律是
                // Qt::Popup 手搭的，理由写在 MainWindowUI.hpp 里 folderMenuName()
                // 那段（offscreen 上的验证工程驱动不了 exec()）。
                m_serverTree->setContextMenuPolicy(Qt::CustomContextMenu);
                ItemViewContextMenu::install(m_serverTree);  // 菜单键 / Shift+F10：弹在当前行
                connect(m_serverTree, &QTreeWidget::customContextMenuRequested, this,
                        [this](const QPoint &pos) {
                            // pos 是 viewport 坐标（条目视图自己就是这么发的）。
                            if (m_serverTree != nullptr) {
                                popUpServerMenu(m_serverTree->itemAt(pos), m_serverTree->viewport()->mapToGlobal(pos));
                            }
                        });
                // 「编辑」和「加入」靠"选中了哪一行"活着：选一下重算一次
                // 那两颗的灰/亮。别的什么都不做 —— 这一发只跟那两颗按钮有关。
                connect(m_serverTree, &QTreeWidget::itemSelectionChanged, this,
                        &InstanceSettingsDialog::updateServerSelectionActions);
                // 初值：现在一条都没有 ⇒ 那两颗是灰的。setServerList() 每次换表还会再喊一遍。
                updateServerSelectionActions();
                layout->addWidget(m_serverTree, 1);
                // 展开/折叠动画（规则见 ui/Animations.hpp）。
                Anim::animateExpansion(m_serverTree);

                m_serverTree->setToolTip(filesHint(QStringLiteral("server")));
            }
            // 页可能是**先收到列表、后建出来**的（调用方开页那一刻就调 setServerList()）：
            // 把存着的那一份摆上去；一次都没收到过就是"还没读到" —— 露出一张空表格比
            // 说一句话更像坏了（验证工程里那些只验结构的构造点走的就是这一支）。
            replayServerPage();
        }
}

