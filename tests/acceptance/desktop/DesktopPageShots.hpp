#pragma once
// 页面对照截图（--page-shots）：设置、活动中心、任务详情三态、账号与外观、游戏日志、对话框。
// 和 DesktopDesignShots 一样只截图：任务是写进活动中心的夹具，日志是灌进窗口的假文本，不启动游戏。
#include "ActivityPopover.hpp"
#include "DesktopDesignShots.hpp"
#include "AccountSkinDialog.hpp"
#include "GameLogWindow.hpp"
#include "InlineMessage.hpp"
#include "JavaMismatchDialog.hpp"
#include "OverlayHost.hpp"
#include "controllers/ActivityCenterController.hpp"
#include "controllers/CrashReportController.hpp"
#include "controllers/TaskDetailController.hpp"
#include "SkinModel.hpp"
#include "WorldSettingsPage.hpp"
#include <QDialog>
#include <QVBoxLayout>
#include <QAction>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>

namespace PageShots {

struct Fixture {
    QString crash, running, failed;
};

inline Fixture seed(MainWindow &window) {
    Fixture f;
    auto *activity = window.findChild<ActivityCenterController *>();
    auto *crash = window.findChild<CrashReportController *>();
    if (!activity) return f;
    const QList<Tasks::TaskStep> install{
        {"下载版本清单", Tasks::TaskStepState::Done, "0.4 s"},
        {"下载客户端 1.21.4.jar", Tasks::TaskStepState::Done, "25.6 MB"},
        {"下载库文件", Tasks::TaskStepState::Running, "128 / 342"},
        {"下载资源文件", Tasks::TaskStepState::Pending, {}},
        {"安装 Fabric Loader 0.16.9", Tasks::TaskStepState::Pending, {}}};
    const auto running = activity->begin(Tasks::TaskKind::Install, QStringLiteral("安装 Fabric 1.21.4"), "spring", install);
    activity->tasks()->setProgress(running, 37, 100);
    activity->tasks()->setDetail(running, QStringLiteral("春日生存 · 库文件 128 / 342"));
    const QList<Tasks::TaskStep> download{
        {"下载文件", Tasks::TaskStepState::Done, "12.0 MB"},
        {"校验并保存", Tasks::TaskStepState::Running, {}}};
    const auto failed = activity->begin(Tasks::TaskKind::Download, QStringLiteral("下载光影包 Complementary"), {}, download);
    activity->tasks()->setDetail(failed, QStringLiteral("ComplementaryReimagined_r5.2.2.zip"));
    activity->tasks()->setProgress(failed, 100, 100);
    activity->finish(failed, false, QStringLiteral("校验失败：SHA1 不一致"), false);
    f.running = QString::number(running);
    f.failed = QString::number(failed);
    if (crash) {
        CrashReportController::LaunchFacts facts;
        facts.instanceId = "create";
        facts.instanceName = QStringLiteral("机械动力 Create");
        facts.requiredJavaMajor = 17;
        facts.loaderName = "Forge";
        facts.loaderVersion = "47.3.0";
        f.crash = QString::number(crash->report(0, int(0xC0000005), true, facts, {}, {}));
    }
    return f;
}

inline QString logFixture() {
    return QStringLiteral(
        "[14:02:10] [main/INFO]: [net.minecraftforge.fml.loading.moddiscovery.ModDiscoverer/SCAN]: Found mod file create-1.20.1-0.5.1.f.jar of type MOD\n"
        "[14:02:10] [main/INFO]: [net.minecraftforge.fml.loading.moddiscovery.ModDiscoverer/SCAN]: Found mod file jei-1.20.1-forge-15.3.0.4.jar of type MOD\n"
        "[14:02:10] [main/DEBUG]: [cpw.mods.modlauncher.LaunchServiceHandler/MODLAUNCHER]: Found launch services [fmlclientdev,forgeclient]\n"
        "[14:02:11] [main/INFO]: [mixin/]: SpongePowered MIXIN Subsystem Version=0.8.5 Source=union:/D:/Games/.minecraft/libraries/org/spongepowered/mixin/0.8.5/mixin-0.8.5.jar\n"
        "[14:02:11] [main/INFO]: [mixin/]: Compatibility level set to JAVA_17\n"
        "[14:02:11] [main/INFO]: [cpw.mods.modlauncher.Launcher/MODLAUNCHER]: ModLauncher running: args [--username, Wanderer, --version, 1.20.1-forge-47.3.0]\n"
        "[14:02:13] [main/INFO]: [net.minecraftforge.fml.earlydisplay.DisplayWindow/]: Trying GL version 4.6\n"
        "[14:02:15] [main/WARN]: [mixin/]: Reference map 'xaerominimap.refmap.json' for xaerominimap.mixins.json could not be read.\n"
        "[14:02:19] [Render thread/INFO]: [com.simibubi.create.Create/]: Create 0.5.1.f initializing! Commit hash: 2bb7a4d\n"
        "[14:02:22] [Render thread/WARN]: [net.minecraftforge.registries.GameData/REGISTRIES]: Registry minecraft:menu: The object steam_rails:conductor_whistle has been registered twice\n"
        "[14:02:31] [Render thread/INFO]: [net.minecraft.server.packs.resources.ReloadableResourceManager/]: Reloading ResourceManager: vanilla, mod_resources\n"
        "[14:02:38] [Render thread/INFO]: [com.mojang.blaze3d.audio.Library/]: OpenAL initialized on device OpenAL Soft\n"
        "[14:02:44] [Render thread/INFO]: [net.minecraft.client.Minecraft/]: Connecting to localhost, 25565\n"
        "[14:03:02] [Render thread/INFO]: [net.minecraft.client.gui.components.ChatComponent/]: [CHAT] Wanderer joined the game\n"
        "[14:03:05] [Render thread/WARN]: [net.minecraft.client.multiplayer.ClientPacketListener/]: Unknown custom packet identifier: create:schematics\n"
        "[14:03:09] [Render thread/ERROR]: [net.minecraft.client.renderer.texture.TextureAtlas/]: Missing texture create:block/cogwheel_shaft\n"
        "[14:03:17] [Render thread/INFO]: [net.minecraft.client.gui.components.ChatComponent/]: [CHAT] [Wanderer: Set own game mode to Creative Mode]\n");
}

struct Shot {
    QString file;
    QString theme;
    std::function<QWidget *(MainWindow &, const Fixture &)> setup;
    bool popups = false;  // 弹出菜单是独立窗口，grab() 不含它：true 时把可见的 QMenu 合成进图
    QSize size = {1280, 800};
};

/// 夹具用的演示皮肤：整张铺衣服色，头正面 8×8 在 (8,8) 画肤色、头发、眼睛，足够认出头像和立绘。
inline QImage demoSkin(const QColor &skin, const QColor &hair, const QColor &eyes) {
    QImage image(64, 64, QImage::Format_ARGB32);
    image.fill(hair.lighter(160));
    QPainter p(&image);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(32, 0, 32, 16, Qt::transparent);  // 头部第二层（帽子）不盖住脸
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.fillRect(8, 8, 8, 8, skin);
    p.fillRect(8, 8, 8, 2, hair);
    p.fillRect(8, 10, 1, 2, hair);
    p.fillRect(15, 10, 1, 2, hair);
    p.fillRect(9, 11, 2, 1, eyes);
    p.fillRect(13, 11, 2, 1, eyes);
    return image;
}

/// 账号页夹具（演示数据，只喂这个窗、不写盘）：两个微软账号 + 两个离线账号。
/// @param current 当前账号的 uuid；@param localSkinForCurrent 当前离线账号用本地皮肤。
inline AccountSkinDialog *accountFixture(MainWindow &w, const QString &current, bool reloginDemo = false) {
    auto *dialog = w.findChild<AccountSkinDialog *>();
    if (!dialog) return nullptr;
    dialog->clearOfflineNameDraft();            // 页面是同一个对象：上一张样张展开过的离线行先收起
    dialog->setOnlineLoginUnavailable(QString());  // 演示数据按「已配置微软登录」摆
    const QImage wanderer = demoSkin(QColor(0xE0, 0xAC, 0x82), QColor(0x4A, 0x32, 0x20), QColor(0x30, 0x40, 0xA0));
    const QImage notch = demoSkin(QColor(0xC9, 0x90, 0x6A), QColor(0x20, 0x20, 0x28), QColor(0x20, 0x80, 0x30));
    const QImage local = demoSkin(QColor(0x8F, 0xC8, 0x6A), QColor(0x2E, 0x7D, 0x32), QColor(0x10, 0x10, 0x10));
    const auto face = [](const QImage &skin) { return SkinModel::headAvatar(skin, 64); };
    auto row = [&](const char *uuid, const char *name, const char *saved, bool offline, const QImage &faceImage) {
        AccountSkinDialog::AccountRow r;
        r.uuid = QString::fromLatin1(uuid);
        r.name = QString::fromLatin1(name);
        r.savedAt = QString::fromLatin1(saved);
        r.offline = offline;
        r.current = r.uuid == current;
        r.face = faceImage;
        r.needsRelogin = reloginDemo && r.uuid == QLatin1String("ms-notch");  // #67：刷新令牌失效的那个微软账号
        return r;
    };
    dialog->setAccounts({row("ms-wanderer", "Wanderer", "2026-11-09T10:00:00Z", false, face(wanderer)),
                         row("ms-notch", "Notch_fan", "2026-11-02T08:30:00Z", false, face(notch)),
                         row("off-steve", "Steve_test", "", true, face(local)),
                         row("off-builder", "Builder", "", true, QImage())});
    const bool offline = current.startsWith(QLatin1String("off-"));
    const bool localSkin = current == QLatin1String("off-steve");
    dialog->setOfflineSkinState(offline, localSkin);
    const QString name = current == QLatin1String("ms-wanderer") ? QStringLiteral("Wanderer")
                         : current == QLatin1String("off-steve") ? QStringLiteral("Steve_test")
                                                                  : QStringLiteral("Builder");
    dialog->setStatusText(offline ? AccountSkinDialog::offlineIdentityText(name) : AccountSkinDialog::identityText(name));
    dialog->setLooks(current == QLatin1String("ms-wanderer") ? wanderer : localSkin ? local : QImage(), QImage(), false);
    dialog->setPreviewNote(localSkin ? AccountSkinDialog::previewNoteLocalSkin()
                                     : offline ? AccountSkinDialog::previewNoteSteve() : QString());
    if (!offline) {
        dialog->setCapes({{"migrator", "Migrator", true}, {"vanilla", "Vanilla", false},
                          {"cherry", "Cherry Blossom", false}});
        dialog->setCapesOwner(current);
    } else {
        dialog->setCapes({});
    }
    return dialog;
}

/// 打开账号页，450 ms 之后（页面自己那发刷新跑完）摆演示数据，再过 250 ms 跑 after。
inline QWidget *accountPage(MainWindow &w, const QString &current, std::function<void(AccountSkinDialog *)> after = {},
                            bool reloginDemo = false) {
    w.ui()->getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    QMetaObject::invokeMethod(&w, "onAccountSkinButtonClicked", Qt::QueuedConnection);
    QTimer::singleShot(450, &w, [&w, current, after, reloginDemo] {
        if (auto *dialog = accountFixture(w, current, reloginDemo)) {
            // 列表高度变了要等一轮布局，菜单按按钮位置弹，晚一点再点。
            if (after) QTimer::singleShot(250, dialog, [dialog, after] { after(dialog); });
        }
    });
    return &w;
}

inline QWidget *settings(MainWindow &window, int row) {
    window.ui()->getTabWidget()->setCurrentIndex(MainWindowUI::settingsTabIndex());
    window.ui()->showSettingsCategory(MainWindowUI::settingsCategoryKeys().value(row));
    return &window;
}

inline QWidget *task(MainWindow &window, const QString &key) {
    window.ui()->getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    if (auto *detail = window.findChild<TaskDetailController *>()) detail->open(key);
    return &window;
}

/// 提示是同步的（嵌一层事件循环），所以下一拍再开，开着的时候截、截完 evict 收掉。
inline QWidget *prompt(MainWindow &window, std::function<void(OverlayHost *)> ask) {
    window.ui()->getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
    DesignShots::seedHome(window);
    QTimer::singleShot(0, &window, [&window, ask] { ask(window.ui()->overlay()); });
    return &window;
}

inline QWidget *gameLog(const QString &text, bool filterEmpty) {
    auto *log = new GameLogWindow(QStringLiteral("机械动力 Create"));
    log->setAttribute(Qt::WA_DeleteOnClose);
    log->appendText(text);
    if (filterEmpty) {
        log->setLevelVisible(GameLogWindow::LogLevel::Info, false);
        log->getSearchEdit()->setText(QStringLiteral("blockentity"));
    }
    log->show();
    log->resize(1280, 800);
    return log;
}

/// #74：世界设置页（页眉面包屑是真覆盖层；页面内容摆演示世界）。@p open 展开「修改存档文件」，
/// @p confirm 再弹展开时那句备份警告（参数和 InstanceDialogController 里一致）。
inline QWidget *worldPage(MainWindow &w, bool open, bool confirm) {
    w.ui()->getTabWidget()->setCurrentIndex(MainWindowUI::instancesTabIndex());
    auto *host = new QDialog;
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *page = new WorldSettingsPage(host);
    layout->addWidget(page);
    WorldSettingsPage::WorldView view;
    view.dirName = QStringLiteral("New World");
    view.hasLevelName = true;
    view.levelName = QStringLiteral("New World");
    view.hasVersionName = true;
    view.versionName = QStringLiteral("1.21.4");
    view.hasSize = true;
    view.sizeText = QStringLiteral("42.8 MB");
    view.hasDifficulty = true;
    view.difficulty = 2;
    view.hasRules = true;
    using R = WorldSettingsPage::Rule;
    using K = WorldSettingsPage::Kind;
    view.rules = {R{QStringLiteral("keepInventory"), QStringLiteral("true"), K::Bool, true},
                  R{QStringLiteral("randomTickSpeed"), QStringLiteral("3"), K::Int, true},
                  R{QStringLiteral("doDaylightCycle"), QStringLiteral("true"), K::Bool, false},
                  R{QStringLiteral("doMobSpawning"), QStringLiteral("true"), K::Bool, false},
                  R{QStringLiteral("mobGriefing"), QStringLiteral("true"), K::Bool, true},
                  R{QStringLiteral("spawnRadius"), QStringLiteral("10"), K::Int, false}};
    page->setWorld(view);
    if (open) page->setFileEditingOpen(true);
    w.ui()->overlay()->showPage(host, QStringLiteral("实例设置"), MainWindowUI::instancesTabIndex());
    w.ui()->overlay()->setPageTitle(page->breadcrumbTitle());
    if (confirm) {
        QTimer::singleShot(200, &w, [&w] {
            InlineMessage::ask(w.ui()->overlay(), MessageKind::Question, QStringLiteral("改动存档文件"),
                               QStringLiteral("将直接修改“New World”的存档名称、游戏模式、难度及规则。建议先备份存档信息。"),
                               {QStringLiteral("取消"), QStringLiteral("先创建备份"), QStringLiteral("继续修改")},
                               1, 2, false, QStringLiteral("备份当前 level.dat，不修改存档。"));
        });
    }
    return &w;
}

inline QList<Shot> shots() {
    QList<Shot> list;
    for (const QString theme : {QStringLiteral("dark"), QStringLiteral("emerald"), QStringLiteral("ocean")}) {
        const bool dark = theme == QLatin1String("dark");
        list.append({"settings-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) { return settings(w, 2); }});
        // 整窗衔接自检（轨、顶栏、内容区是不是同一张底）：管理、下载、智能体三页原样截一张。
        for (const auto &[name, index] : {std::pair{"manage", MainWindowUI::instancesTabIndex()},
                                          std::pair{"download", MainWindowUI::downloadTabIndex()},
                                          std::pair{"agent", MainWindowUI::agentTabIndex()}}) {
            list.append({QStringLiteral("seam-%1-").arg(QLatin1String(name)) + theme + ".png", theme,
                         [index](MainWindow &w, const Fixture &) {
                             w.ui()->getTabWidget()->setCurrentIndex(index);
                             return &w;
                         }});
        }
        if (theme != QLatin1String("ocean")) {
            list.append({"world-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) { return worldPage(w, false, false); }});
            list.append({"world-open-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) { return worldPage(w, true, false); }});
            list.append({"world-confirm-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) { return worldPage(w, true, true); }});
        }
        list.append({"task-crash-" + theme + ".png", theme, [](MainWindow &w, const Fixture &f) { return task(w, f.crash); }});
        if (dark) {
            list.append({"settings-launch-dark.png", theme, [](MainWindow &w, const Fixture &) { return settings(w, 0); }});
            list.append({"task-running-dark.png", theme, [](MainWindow &w, const Fixture &f) { return task(w, f.running); }});
            list.append({"task-failed-dark.png", theme, [](MainWindow &w, const Fixture &f) { return task(w, f.failed); }});
            list.append({"panels-tasks-dark.png", theme, [](MainWindow &w, const Fixture &) {
                w.ui()->getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
                w.ui()->getTaskSummaryButton()->click();
                return &w;
            }});
        }
        list.append({"account-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) {
            w.ui()->getTabWidget()->setCurrentIndex(MainWindowUI::homeTabIndex());
            QMetaObject::invokeMethod(&w, "onAccountSkinButtonClicked", Qt::QueuedConnection);
            return &w;
        }});
        // issue #66：每个账号自己的头像和来源、添加账号菜单、离线账号用本地皮肤、请求失败（演示数据）。
        list.append({"account-list-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) {
            return accountPage(w, QStringLiteral("ms-wanderer"));
        }});
        // issue #67：一个微软账号需要重新登录（Notch_fan），Wanderer 正常（当前、选中），另有离线账号。
        list.append({"account-relogin-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) {
            return accountPage(w, QStringLiteral("ms-wanderer"), {}, true);
        }});
        list.append({"account-relogin-selected-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) {
            return accountPage(w, QStringLiteral("ms-wanderer"), [](AccountSkinDialog *dialog) {
                if (auto *list = dialog->findChild<QListWidget *>(AccountSkinDialog::accountListName())) list->setCurrentRow(1);
            }, true);
        }});
        list.append({"account-add-menu-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) {
            return accountPage(w, QStringLiteral("ms-wanderer"), [](AccountSkinDialog *dialog) {
                if (auto *button = dialog->findChild<QPushButton *>(AccountSkinDialog::addAccountButtonName())) button->click();
            });
        }, true});
        list.append({"account-add-offline-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) {
            return accountPage(w, QStringLiteral("ms-wanderer"), [](AccountSkinDialog *dialog) {
                if (auto *action = dialog->findChild<QAction *>(AccountSkinDialog::addOfflineActionName())) action->trigger();
            });
        }});
        if (dark) {  // 微软登录没配置：菜单里「微软账号」置灰
            list.append({"account-add-menu-unavailable-dark.png", theme, [](MainWindow &w, const Fixture &) {
                return accountPage(w, QStringLiteral("ms-wanderer"), [](AccountSkinDialog *dialog) {
                    dialog->setOnlineLoginUnavailable(QStringLiteral("这个版本暂时不能登录微软账号。"));
                    if (auto *button = dialog->findChild<QPushButton *>(AccountSkinDialog::addAccountButtonName())) button->click();
                });
            }, true});
        }
        list.append({"account-offline-local-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) {
            return accountPage(w, QStringLiteral("off-steve"));
        }});
        if (dark) {  // 最小窗口 900×590 下的预览说明
            list.append({"account-offline-local-min-dark.png", theme, [](MainWindow &w, const Fixture &) {
                return accountPage(w, QStringLiteral("off-steve"), [](AccountSkinDialog *dialog) {
                    for (QWidget *p = dialog->parentWidget(); p; p = p->parentWidget())
                        if (auto *scroll = qobject_cast<QScrollArea *>(p)) {  // 窄窗口页面要滚，滚到底看预览说明
                            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                            break;
                        }
                });
            }, false, {900, 590}});
        }
        list.append({"account-error-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) {
            return accountPage(w, QStringLiteral("ms-wanderer"), [](AccountSkinDialog *dialog) {
                dialog->setStatusError(QStringLiteral("无法连接到 Minecraft 服务，请检查网络后重试。"));
            });
        }});
        list.append({"gamelog-running-" + theme + ".png", theme, [](MainWindow &, const Fixture &) { return gameLog(logFixture(), false); }});
        if (dark) {
            list.append({"gamelog-empty-dark.png", theme, [](MainWindow &, const Fixture &) { return gameLog(logFixture(), true); }});
            list.append({"gamelog-exited-dark.png", theme, [](MainWindow &, const Fixture &) {
                auto *log = static_cast<GameLogWindow *>(gameLog(logFixture(), false));
                log->setExited(-1, false);
                log->appendText(QStringLiteral("\n[游戏已退出，退出码：-1]\n"));
                return log;
            }});
        }
        list.append({"dialogs-java-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) {
            return prompt(w, [](OverlayHost *host) {
                JavaMismatchDialog dialog(QStringLiteral("机械动力 Create"), 17, 8, QStringLiteral("1.8.0_421"),
                                          QStringLiteral("C:\\Program Files\\Java\\jre1.8.0_421\\bin\\java.exe"));
                host->execPrompt(&dialog);
            });
        }});
        list.append({"dialogs-discard-" + theme + ".png", theme, [](MainWindow &w, const Fixture &) {
            return prompt(w, [](OverlayHost *host) {
                InlineMessage::question(host, QStringLiteral("改过还没保存"),
                    QStringLiteral("这个文件刚才改的地方还没写到盘上，离开就会丢掉这些改动。\n\n要丢掉吗？"),
                    QStringLiteral("丢掉改动"), QStringLiteral("留在这儿"), false);
            });
        }});
        if (dark) {
            list.append({"dialogs-warn-dark.png", theme, [](MainWindow &w, const Fixture &) {
                return prompt(w, [](OverlayHost *host) {
                    InlineMessage::warning(host, QStringLiteral("收藏未保存"), QStringLiteral("没能写入实例配置，收藏状态下次启动会恢复原样。"));
                });
            }});
            list.append({"dialogs-error-dark.png", theme, [](MainWindow &w, const Fixture &) {
                return prompt(w, [](OverlayHost *host) {
                    InlineMessage::critical(host, QStringLiteral("删除失败"),
                        QStringLiteral("有文件正被占用，没能删除：\nD:\\Games\\.minecraft\\versions\\机械动力 Create\\mods\\create-1.20.1-0.5.1.f.jar"));
                });
            }});
            list.append({"dialogs-input-dark.png", theme, [](MainWindow &w, const Fixture &) {
                return prompt(w, [](OverlayHost *host) {
                    bool ok = false;
                    InlineMessage::getText(host, QStringLiteral("复制实例"), QStringLiteral("副本名称："),
                                           QStringLiteral("机械动力 Create 2"), &ok);
                });
            }});
        }
    }
    return list;
}

inline void run(QApplication &app, MainWindow &window, const QString &root) {
    // 环境变量 WTFML_SHOT_FILTER 非空时只拍文件名里含这段的样张（开发时挑着拍）。
    static const QList<Shot> list = [] {
        QList<Shot> all = shots();
        const QString filter = qEnvironmentVariable("WTFML_SHOT_FILTER");
        if (!filter.isEmpty()) all.removeIf([&](const Shot &shot) { return !shot.file.contains(filter); });
        return all;
    }();
    static Fixture fixture;
    static int index = 0;
    static QJsonArray report;
    static std::function<void()> step;
    fixture = seed(window);
    step = [&app, &window, root]() {
        if (index >= list.size()) {
            QFile out(root + QStringLiteral("/design-shots.json"));
            const bool ok = out.open(QIODevice::WriteOnly) &&
                            out.write(QJsonDocument(QJsonObject{{"devicePixelRatio", window.devicePixelRatioF()},
                                                                {"shots", report}}).toJson()) > 0;
            app.exit(ok ? 0 : 1);
            return;
        }
        const Shot shot = list.at(index++);
        Theme::setSpec(Theme::presetSpec(shot.theme));
        Theme::apply(app);
        window.showNormal();
        window.resize(shot.size);
        QPointer<QWidget> target = shot.setup(window, fixture);
        QTimer::singleShot(300, &window, [&window, size = shot.size]() { window.resize(size); });
        QTimer::singleShot(1000, &window, [&window, root, shot, target]() {
            QImage image = target ? target->grab().toImage() : QImage();
            if (shot.popups && !image.isNull()) {
                QPainter painter(&image);
                for (QWidget *top : QApplication::topLevelWidgets())
                    if (auto *menu = qobject_cast<QMenu *>(top); menu && menu->isVisible())
                        painter.drawPixmap(target->mapFromGlobal(menu->pos()), menu->grab());
            }
            const bool saved = !image.isNull() && image.save(root + QLatin1Char('/') + shot.file);
            report.append(QJsonObject{{"file", shot.file}, {"saved", saved}});
            if (target && target != &window) target->close();
            if (auto *pop = window.findChild<QFrame *>(ActivityPopover::objectNameValue())) pop->close();
            window.ui()->overlay()->evict();
            if (auto *popup = QApplication::activePopupWidget()) popup->close();
            QTimer::singleShot(200, &window, [&window]() {
                window.ui()->overlay()->evict();
                QTimer::singleShot(150, &window, step);
            });
        });
    };
    QTimer::singleShot(1200, &window, step);
}

}  // namespace PageShots
