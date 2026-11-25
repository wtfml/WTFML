/**
 * @file UiAgentCheck.cpp
 * @brief 智能体页/设置页的纯 UI 检查（offscreen）：五页导航、双栏几何、空态、消息渲染、
 *        API Key 遮挡、协议适配与隐私说明。
 */

#include "Support.hpp"

#include "AgentMarkdown.hpp"
#include "AgentComposer.hpp"
#include "AgentMessageView.hpp"
#include "AgentMcpPage.hpp"
#include "AgentPage.hpp"
#include "AgentSecretEdit.hpp"
#include "AgentApprovalCard.hpp"
#include "AgentSettingsPage.hpp"
#include "AgentToolCard.hpp"
#include "SettingsPageParts.hpp"
#include "ToggleSwitch.hpp"

#include <QCheckBox>
#include <QElapsedTimer>
#include <QComboBox>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>

namespace {
AgentView::Message messageFor(bool user, const QString &text, AgentView::MessageStatus status) {
    AgentView::Message message;
    message.id = QStringLiteral("m-%1").arg(text.left(4));
    message.user = user;
    message.content = text;
    message.status = status;
    return message;
}
}  // namespace

int UiAgentCheck() {
    QTemporaryDir fixture;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.filePath("system"));
    QCoreApplication::instance()->setProperty("wtfml.testAgentDataDir", fixture.filePath("agent"));

    QMainWindow window;
    MainWindowUI ui(&window);
    window.resize(900, 620);
    window.show();
    QApplication::processEvents();

    QTabWidget *tabs = ui.getTabWidget();
    check(tabs != nullptr && tabs->count() == 6, "智能体 UI：六个选项卡（含工具箱）",
          QString::number(tabs != nullptr ? tabs->count() : -1));
    check(tabs != nullptr && tabs->tabText(4) == QStringLiteral("智能体") &&
              MainWindowUI::agentTabIndex() == 4,
          "智能体 UI：智能体是第五颗（最后）");
    check(tabs != nullptr && tabs->currentIndex() == MainWindowUI::homeTabIndex(),
          "智能体 UI：启动仍停在主页");

    // 106（原则 36 去硬描边）：六个页签里露出来的 QFrame / 滚动区都不带默认那一圈边。
    {
        QStringList framed;
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            QApplication::processEvents();
            for (QFrame *f : window.findChildren<QFrame *>()) {
                if (f->isVisible() && f->frameShape() != QFrame::NoFrame && f->frameWidth() > 0)
                    framed << QStringLiteral("%1(%2)").arg(f->objectName(), QString::fromLatin1(f->metaObject()->className()));
            }
        }
        framed.removeDuplicates();
        check(framed.isEmpty(), "智能体 UI：各页没有带默认边框的 QFrame / 滚动区", framed.join(QStringLiteral(", ")));
    }

    tabs->setCurrentIndex(MainWindowUI::agentTabIndex());
    QApplication::processEvents();
    AgentPage *page = ui.getAgentPage();
    check(page != nullptr && page->objectName() == MainWindowUI::agentTabName(),
          "智能体 UI：按 objectName 找得到页面");
    QWidget *sidebar = page != nullptr ? page->findChild<QWidget *>(QStringLiteral("agentSidebar"))
                                       : nullptr;
    const int windowWidth = page != nullptr ? page->window()->width() : 0;
    check(sidebar != nullptr &&
              sidebar->width() == (windowWidth < 1024 ? AgentPage::sidebarNarrowWidth() : AgentPage::sidebarWidth()),
          "智能体 UI：侧栏宽按窗口宽取 236 / 216（1024 为界）",
          QStringLiteral("窗口 %1 / 侧栏 %2").arg(windowWidth).arg(sidebar != nullptr ? sidebar->width() : -1));
    // "窄"这一档用一个不在布局里的 AgentPage 单独量：自己就是窗口，宽 700 < 1024 → 216。
    {
        AgentPage narrowPage;
        narrowPage.show();
        QApplication::processEvents();
        narrowPage.resize(700, 500);
        QApplication::processEvents();
        QWidget *narrowSidebar =
            narrowPage.findChild<QWidget *>(QStringLiteral("agentSidebar"));
        check(narrowSidebar != nullptr &&
                  narrowSidebar->width() == AgentPage::sidebarNarrowWidth(),
              "智能体 UI：窄页面侧栏降到 216",
              QStringLiteral("侧栏 %1 / 页面宽 %2")
                  .arg(narrowSidebar != nullptr ? narrowSidebar->width() : -1)
                  .arg(narrowPage.width()));
    }

    const auto find = [page](const char *name) {
        return page != nullptr ? page->findChild<QWidget *>(QString::fromLatin1(name)) : nullptr;
    };
    check(find("agentSessionList") != nullptr && find("agentNewSessionButton") != nullptr &&
              find("agentSessionSearch") != nullptr && find("agentChatHeader") != nullptr &&
              find("agentMoreButton") != nullptr && find("agentInstanceLockChip") != nullptr &&
              find("agentApiSettingsButton") != nullptr &&
              find("agentMessageScroll") != nullptr && find("agentComposer") != nullptr &&
              find("agentSendButton") != nullptr && find("agentStopButton") != nullptr &&
              find("agentApiWarnChip") != nullptr && find("agentEmptyState") != nullptr &&
              find("agentEncryptSessionCheck") != nullptr,
          "智能体 UI：关键 objectName 全在");
    auto *empty = find("agentEmptyState");
    check(empty != nullptr && empty->isVisible(), "智能体 UI：空态可见");
    auto *send = qobject_cast<QPushButton *>(find("agentSendButton"));
    auto *stop = qobject_cast<QPushButton *>(find("agentStopButton"));
    check(send != nullptr && !send->isEnabled(), "智能体 UI：未配置 API 时发送禁用");
    check(stop != nullptr && !stop->isVisible(), "智能体 UI：空闲时停止按钮不露头");
    // 101：未配置时空态只给一句说明 + 「配置 API」主按钮，不摆打不了字的输入岛。
    auto *composerRow = find("agentComposerRow");
    auto *configure = find("agentEmptyConfigureButton");
    check(composerRow != nullptr && !composerRow->isVisible() && configure != nullptr && configure->isVisible(),
          "智能体 UI：未配置 API 时收起输入岛，空态露「配置 API」");

    page->setConfigured(true, QStringLiteral("API 已配置"));
    check(composerRow != nullptr && composerRow->isVisible() && configure != nullptr && !configure->isVisible(),
          "智能体 UI：配置好后输入岛回来，「配置 API」收起");
    page->composer()->setPlainText(QStringLiteral("你好"));
    QApplication::processEvents();
    check(send != nullptr && send->isEnabled(), "智能体 UI：配置 + 有文字后发送可用");
    if (send != nullptr && composerRow != nullptr) {
        // 110：发送钮悬停要有一档反馈（圆钮样式表曾盖掉主按钮的控件级悬停色）。离屏得手喂悬停进入 + 移动。
        const auto sample = [&] {
            const QImage row = composerRow->grab().toImage();
            const QPoint at = send->mapTo(composerRow, QPoint(6, 18));  // 圆内、箭头外
            return row.pixelColor((at.toPointF() * row.devicePixelRatio()).toPoint());
        };
        const QColor idle = sample();
        send->setAttribute(Qt::WA_UnderMouse, true);
        const QPointF at(send->rect().center());
        QHoverEvent enter(QEvent::HoverEnter, at, send->mapToGlobal(at), QPointF(-1, -1));
        QApplication::sendEvent(send, &enter);
        QMouseEvent move(QEvent::MouseMove, at, send->mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(send, &move);
        const QColor hovered = sample();
        send->setAttribute(Qt::WA_UnderMouse, false);
        QHoverEvent leave(QEvent::HoverLeave, at, send->mapToGlobal(at), at);
        QApplication::sendEvent(send, &leave);
        check(hovered != idle, "智能体 UI：发送钮悬停色和常态色不同",
              QStringLiteral("常态 %1 / 悬停 %2").arg(idle.name(), hovered.name()));
    }
    page->setStreaming(true);
    QApplication::processEvents();
    check(stop != nullptr && stop->isVisible() && !send->isVisible(),
          "智能体 UI：流式中停止替代发送");
    if (stop != nullptr && composerRow != nullptr) {
        // 110：停止钮要有自己的圆底（r104 冰川碧湖下底被刷透明，只剩小空心框）。
        // 圆内、方块外一点 vs 钮左外侧一点，颜色必须不同。
        const QImage row = composerRow->grab().toImage();
        const qreal dpr = row.devicePixelRatio();
        const QPoint origin = stop->mapTo(composerRow, QPoint(0, 0));
        const QColor inside = row.pixelColor(((origin + QPoint(6, 18)).toPointF() * dpr).toPoint());
        const QColor outside = row.pixelColor(((origin + QPoint(-4, 18)).toPointF() * dpr).toPoint());
        check(inside != outside, "智能体 UI：生成中停止钮有圆底（和输入岛底色分得开）",
              QStringLiteral("圆内 %1 / 钮外 %2").arg(inside.name(), outside.name()));
    }
    page->setStreaming(false);

    QList<AgentView::SessionMeta> sessions;
    for (int i = 0; i < 2; ++i) {
        AgentView::SessionMeta meta;
        meta.id = QStringLiteral("s%1").arg(i);
        meta.title = QStringLiteral("会话 %1").arg(i);
        meta.updatedAt = QDateTime::currentDateTimeUtc().addSecs(-i * 60);
        meta.pinned = i == 1;
        sessions.append(meta);
    }
    page->setSessions(sessions);
    page->setCurrentSession(QStringLiteral("s0"));
    auto *list = qobject_cast<QListWidget *>(find("agentSessionList"));
    // 固定的那条单独一组排最前，今天的另一组；组名是不可点的小标题行。
    QStringList rows;
    for (int i = 0; list != nullptr && i < list->count(); ++i) rows << list->item(i)->text();
    check(rows == QStringList{QStringLiteral("固定"), QStringLiteral("会话 1"), QStringLiteral("今天"), QStringLiteral("会话 0")},
          "智能体 UI：会话按 固定 / 今天 分组", rows.join(QStringLiteral(" | ")));
    check(list != nullptr && list->count() == 4 && list->item(0)->flags() == Qt::NoItemFlags,
          "智能体 UI：分组小标题不可选");
    check(AgentPage::relativeTime(sessions.at(1).updatedAt, QDateTime::currentDateTimeUtc())
              .contains(QStringLiteral("分钟")),
          "智能体 UI：相对时间");

    // 106：会话就地改名（顶行标题点一下 / 列表双击），回车确认、Esc 取消、空串算取消；不弹窗。
    {
        QStringList renames;
        const QMetaObject::Connection spy = QObject::connect(
            page, &AgentPage::sessionRenameRequested,
            [&renames](const QString &id, const QString &title) { renames << id + QLatin1Char('=') + title; });
        const auto key = [](QWidget *w, int k) {
            QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
            QApplication::sendEvent(w, &press);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QApplication::processEvents();
        };
        auto *title = qobject_cast<QLabel *>(find("agentSessionTitle"));
        const auto clickTitle = [title] {
            const QPointF at(4, title->height() / 2.0);
            QMouseEvent release(QEvent::MouseButtonRelease, at, title->mapToGlobal(at), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(title, &release);
            QApplication::processEvents();
        };
        const auto titleEdit = [page] { return page->findChild<QLineEdit *>(QStringLiteral("agentTitleRenameEdit")); };
        clickTitle();
        QLineEdit *edit = titleEdit();
        check(edit != nullptr && edit->isVisible() && title != nullptr && !title->isVisible() &&
                  edit->text() == QStringLiteral("会话 0"),
              "智能体 UI：点顶行标题就地变成输入框（带原名）");
        if (edit != nullptr) {
            edit->setText(QStringLiteral("  显卡驱动  "));
            key(edit, Qt::Key_Return);
        }
        check(renames == QStringList{QStringLiteral("s0=显卡驱动")} && titleEdit() == nullptr && title->isVisible(),
              "智能体 UI：标题编辑回车确认，去掉首尾空白后发改名", renames.join(QLatin1Char('|')));
        renames.clear();
        clickTitle();
        if ((edit = titleEdit()) != nullptr) {
            edit->setText(QStringLiteral("不要了"));
            key(edit, Qt::Key_Escape);
        }
        clickTitle();
        if ((edit = titleEdit()) != nullptr) {
            edit->clear();
            key(edit, Qt::Key_Return);
        }
        check(renames.isEmpty() && titleEdit() == nullptr && title->isVisible(),
              "智能体 UI：Esc 与空串都不改名", renames.join(QLatin1Char('|')));

        QListWidgetItem *other = list != nullptr ? list->item(1) : nullptr;  // 「会话 1」
        if (other != nullptr) emit list->itemDoubleClicked(other);
        QApplication::processEvents();
        auto *listEdit = page->findChild<QLineEdit *>(QStringLiteral("agentSessionRenameEdit"));
        check(listEdit != nullptr && listEdit->isVisible() && listEdit->parentWidget() == list->viewport() &&
                  list->visualItemRect(other).contains(listEdit->geometry()) &&
                  listEdit->text() == QStringLiteral("会话 1"),
              "智能体 UI：列表双击会话，就地在那一行出输入框");
        if (listEdit != nullptr) {
            listEdit->setText(QStringLiteral("模组冲突"));
            key(listEdit, Qt::Key_Enter);
        }
        check(renames == QStringList{QStringLiteral("s1=模组冲突")} &&
                  page->findChild<QLineEdit *>(QStringLiteral("agentSessionRenameEdit")) == nullptr,
              "智能体 UI：列表改名回车确认", renames.join(QLatin1Char('|')));
        QObject::disconnect(spy);
    }

    // 106：点「+」后光标直接进输入框（当前已是空会话、没再新建时也一样）。
    if (auto *plus = qobject_cast<QPushButton *>(find("agentNewSessionButton"))) {
        window.activateWindow();
        list->setFocus();
        QApplication::processEvents();
        plus->click();
        QApplication::processEvents();
        check(page->composer()->hasFocus(), "智能体 UI：点「+」后输入框拿到焦点",
              QApplication::focusWidget() != nullptr ? QApplication::focusWidget()->objectName() : QStringLiteral("无"));
    }

    // 107：实例下拉（PopMenu）是 Qt::Popup 顶层窗，缺无边框时圆角面外套一圈直角黑框。
    {
        page->showInstanceMenu({{QStringLiteral("a"), QStringLiteral("生存 1.21")}}, QString());
        QApplication::processEvents();
        QWidget *menu = page->instanceMenu();
        const QString problem = menu != nullptr ? popupWindowProblem(menu) : QStringLiteral("没弹出来");
        check(menu != nullptr && menu->isVisible() && problem.isEmpty(), "智能体 UI：实例下拉是无边框 + 半透明的浮层",
              problem);
        if (menu != nullptr) menu->close();
        QApplication::processEvents();
    }

    // 106：实例胶囊没经过 setInstanceChip 时也按两颗字给宽（以前裁成「实 无」）。
    {
        AgentPage fresh;
        auto *chip = fresh.findChild<QPushButton *>(QStringLiteral("agentInstanceLockChip"));
        check(chip != nullptr && chip->minimumWidth() >= chip->layout()->sizeHint().width(),
              "智能体 UI：实例胶囊一建出来就装得下「实例」和值",
              chip != nullptr ? QStringLiteral("%1 < %2").arg(chip->minimumWidth()).arg(chip->layout()->sizeHint().width())
                              : QString());
    }
    page->setMessages({messageFor(true, QStringLiteral("启动就崩溃"),
                                  AgentView::MessageStatus::Complete),
                       messageFor(false,
                                  QStringLiteral("<script>alert(1)</script>\n```\ncode\n```"),
                                  AgentView::MessageStatus::Complete)});
    QApplication::processEvents();
    const QList<AgentMessageView *> views = page->findChildren<AgentMessageView *>();
    check(views.size() == 2, "智能体 UI：两条消息各有视图", QString::number(views.size()));
    bool scriptEscaped = true;
    bool codeRendered = false;
    for (AgentMessageView *view : views) {
        QLabel *body = view->findChild<QLabel *>(QStringLiteral("agentMessageBody"));
        if (body == nullptr) {
            scriptEscaped = false;
            continue;
        }
        scriptEscaped = scriptEscaped && !body->text().contains(QStringLiteral("<script"));
        codeRendered = codeRendered || body->text().contains(QStringLiteral("<pre><code>"));
    }
    check(scriptEscaped, "智能体 UI：消息里的原始 script 被转义");
    check(codeRendered, "智能体 UI：围栏代码块渲染成 pre/code");
    check(!AgentMarkdown::toHtml(QStringLiteral("<img src=x onerror=1>"))
               .contains(QStringLiteral("<img")),
          "智能体 UI：不生成远程 img");

    // 性能基线：2000 条消息一次性摆进对话流（Stage 4 记录，见 perf-notes）。
    {
        QList<AgentView::Message> many;
        for (int i = 0; i < 2000; ++i) {
            AgentView::Message message;
            message.id = QStringLiteral("perf-%1").arg(i);
            message.user = (i % 2 == 0);
            message.content = QStringLiteral("第 %1 条：启动失败，日志里说缺少前置模组。").arg(i);
            message.status = AgentView::MessageStatus::Complete;
            many.append(message);
        }
        QElapsedTimer timer;
        timer.start();
        page->setMessages(many);
        QApplication::processEvents();
        const qint64 elapsed = timer.elapsed();
        std::printf("[perf] agent-page-2000-messages=%lldms\n", static_cast<long long>(elapsed));
        std::fflush(stdout);
        check(elapsed < 8000, "智能体 UI：2000 条消息渲染在 8 秒内",
              QStringLiteral("%1ms").arg(elapsed));
        check(page->findChildren<AgentMessageView *>().size() >= 2,
              "智能体 UI：大量消息里最后几条仍在");
        page->clearMessages();
        QApplication::processEvents();
    }

    // 滚动跟随：底部文本更新只在本来就贴底时跟随；上翻后不抢位置；Ctrl+End 回到底部。
    {
        auto *scroll = qobject_cast<QScrollArea *>(find("agentMessageScroll"));
        check(scroll != nullptr, "智能体 UI：找得到对话滚动区");
        if (scroll != nullptr) {
            QList<AgentView::Message> many;
            for (int i = 0; i < 40; ++i) {
                AgentView::Message message;
                message.id = QStringLiteral("scroll-%1").arg(i);
                message.user = false;
                message.content =
                    QStringLiteral("第 %1 条消息：这是一段用来撑高对话流的正文。").arg(i);
                message.status = AgentView::MessageStatus::Complete;
                many.append(message);
            }
            many.first().reasoning = QStringLiteral("先看日志，再看模组清单。");
            page->setMessages(many);
            QScrollBar *bar = scroll->verticalScrollBar();
            // 消息行的几何是排队算的：等到真的溢出，再看跟随行为。
            for (int i = 0; i < 100 && bar->maximum() <= 0; ++i) {
                QApplication::processEvents();
                QThread::msleep(5);
            }
            check(bar->maximum() > 0, "智能体 UI：对话流可以滚动",
                  QString::number(bar->maximum()));
            check(bar->value() == bar->maximum(), "智能体 UI：切会话后贴在底部",
                  QStringLiteral("%1/%2").arg(bar->value()).arg(bar->maximum()));

            // 上翻到顶后，底部文本更新不再把画面拽回最下方。
            bar->setValue(0);
            QApplication::processEvents();
            AgentView::Message updated = many.last();
            updated.content += QStringLiteral("（继续输出）");
            page->upsertMessage(updated);
            QApplication::processEvents();
            check(bar->value() == 0, "智能体 UI：上翻后文本更新不强制回到最下方",
                  QStringLiteral("%1/%2").arg(bar->value()).arg(bar->maximum()));

            // 展开/折叠顶部那条的思考过程也不抢滚动位置。
            const QList<QToolButton *> toggles =
                page->findChildren<QToolButton *>(QStringLiteral("agentReasoningToggle"));
            if (!toggles.isEmpty()) {
                toggles.first()->click();
                QApplication::processEvents();
                check(bar->value() == 0, "智能体 UI：展开/折叠思考过程不抢滚动位置",
                      QString::number(bar->value()));
            }

            // Ctrl+End 回到底部，之后的新内容继续跟随。
            QKeyEvent endKey(QEvent::KeyPress, Qt::Key_End, Qt::ControlModifier);
            QApplication::sendEvent(scroll->viewport(), &endKey);
            QApplication::processEvents();
            check(bar->value() == bar->maximum(), "智能体 UI：Ctrl+End 回到最新消息",
                  QStringLiteral("%1/%2").arg(bar->value()).arg(bar->maximum()));
            AgentView::Message follow = many.last();
            follow.content += QStringLiteral("（再一行）");
            page->upsertMessage(follow);
            QApplication::processEvents();
            check(bar->value() == bar->maximum(), "智能体 UI：贴底后更新继续跟随",
                  QStringLiteral("%1/%2").arg(bar->value()).arg(bar->maximum()));
            page->clearMessages();
            QApplication::processEvents();
        }
    }

    AgentView::ToolCard toolCard;
    toolCard.id = QStringLiteral("call-1");
    toolCard.title = QStringLiteral("查看目录概况");
    toolCard.summary = QStringLiteral("目录：2 个文件");
    toolCard.detail = QStringLiteral("{}");
    toolCard.status = AgentView::ToolCardStatus::Done;
    page->upsertToolCard(toolCard);
    QApplication::processEvents();
    auto *toolView = page->findChild<AgentToolCard *>();
    check(toolView != nullptr && toolView->cardId() == QStringLiteral("call-1"),
          "智能体 UI：工具卡可按 id 更新",
          QStringLiteral("count=%1 id=%2").arg(page->findChildren<AgentToolCard *>().size())
              .arg(toolView != nullptr ? toolView->cardId() : QStringLiteral("<null>")));
    check(toolView != nullptr &&
              toolView->findChild<QLabel *>(QStringLiteral("agentToolTitle")) != nullptr,
          "智能体 UI：工具卡有标题");

    QJsonObject approval{{QStringLiteral("approvalId"), QStringLiteral("a1")},
                         {QStringLiteral("tool"), QStringLiteral("fs.write")},
                         {QStringLiteral("displayName"), QStringLiteral("写入文本配置")},
                         {QStringLiteral("target"), QStringLiteral("config/a.toml")},
                         {QStringLiteral("summary"), QStringLiteral("写入 1 个文件")},
                         {QStringLiteral("preview"), QStringLiteral("配置预览")},
                         {QStringLiteral("diff"), QStringLiteral("- old") + QChar(10) + QStringLiteral("+ new")},
                         {QStringLiteral("backupHint"), QStringLiteral("写前备份")},
                         {QStringLiteral("sessionAllowAvailable"), false},
                         {QStringLiteral("egress"),
                          QJsonObject{{QStringLiteral("kind"), QStringLiteral("config-file")},
                                      {QStringLiteral("reason"), QStringLiteral("逐次确认")}}}};
    // 模式卡片（Stage 2）：五颗胶囊按钮，选中的那颗可勾选；换选中态后状态行跟着变。
    page->setSkills({{QStringLiteral("general"), QStringLiteral("自由对话"),
                      QStringLiteral("对话 + 受控工具")},
                     {QStringLiteral("inspection"), QStringLiteral("实例体检"),
                      QStringLiteral("只读诊断")}},
                    QStringLiteral("general"));
    QApplication::processEvents();
    auto *generalChip = page->findChild<QPushButton *>(QStringLiteral("agentSkill_general"));
    auto *inspectionChip = page->findChild<QPushButton *>(QStringLiteral("agentSkill_inspection"));
    check(generalChip != nullptr && inspectionChip != nullptr,
          "智能体 UI：模式卡片按技能 id 摆出来");
    check(generalChip != nullptr && generalChip->isChecked(),
          "智能体 UI：默认停在自由对话");
    page->setSkills({{QStringLiteral("general"), QStringLiteral("自由对话"),
                      QStringLiteral("对话 + 受控工具")},
                     {QStringLiteral("inspection"), QStringLiteral("实例体检"),
                      QStringLiteral("只读诊断")}},
                    QStringLiteral("inspection"));
    QApplication::processEvents();
    inspectionChip = page->findChild<QPushButton *>(QStringLiteral("agentSkill_inspection"));
    check(inspectionChip != nullptr && inspectionChip->isChecked(),
          "智能体 UI：换模式后选中态跟着走");
    // Auto-safe（S5.7）：只在崩溃急救/模组维护显示；勾选后发信号、可外部复位。
    auto *autoSafe = page->findChild<QPushButton *>(QStringLiteral("agentAutoSafeButton"));
    check(autoSafe != nullptr && autoSafe->isHidden(), "智能体 UI：Auto-safe 默认不出现");
    page->setAutoSafeVisible(true);
    QApplication::processEvents();
    check(autoSafe != nullptr && !autoSafe->isHidden(), "智能体 UI：修复模式下 Auto-safe 出现");
    check(autoSafe != nullptr && !autoSafe->isChecked(), "智能体 UI：Auto-safe 默认关闭");
    bool autoSafeSignal = false;
    QObject::connect(page, &AgentPage::autoSafeToggled, page,
                     [&autoSafeSignal](bool enabled) { autoSafeSignal = enabled; });
    if (autoSafe != nullptr) autoSafe->click();
    QApplication::processEvents();
    check(autoSafeSignal && autoSafe != nullptr && autoSafe->isChecked(),
          "智能体 UI：点 Auto-safe 发出开启信号");
    page->setAutoSafeEnabled(false);
    QApplication::processEvents();
    check(autoSafe != nullptr && !autoSafe->isChecked(), "智能体 UI：外部可以复位 Auto-safe");
    page->setAutoSafeEnabled(true);
    check(page->autoSafeEnabled(), "智能体 UI：Auto-safe 勾选态可读");
    page->setAutoSafeVisible(false);
    QApplication::processEvents();
    check(autoSafe != nullptr && autoSafe->isHidden() && !page->autoSafeEnabled(),
          "智能体 UI：离开展示后 Auto-safe 收起并复位");
    page->setWorkflowStatus(QStringLiteral("模式：实例体检 · 阶段：collect"));
    QApplication::processEvents();
    auto *statusLabel = page->findChild<QLabel *>(QStringLiteral("agentWorkflowStatus"));
    check(statusLabel != nullptr && statusLabel->isVisible() &&
              statusLabel->text().contains(QStringLiteral("实例体检")),
          "智能体 UI：工作流状态行可见");
    page->setWorkflowStatus(QString());
    QApplication::processEvents();
    check(statusLabel != nullptr && !statusLabel->isVisible(),
          "智能体 UI：状态行为空时收起");

    // MCP 服务器页（Stage 2A）：本机权限提示、服务器行、连接按钮、空态。
    {
        AgentMcpPage mcpPage;
        AgentView::McpServerRow row;
        row.id = QStringLiteral("srv-1");
        row.name = QStringLiteral("测试服务器");
        row.command = QStringLiteral("C:/Servers/mcp/server.exe");
        row.identityShort = QStringLiteral("abcdef123456");
        row.state = QStringLiteral("ready");
        row.enabled = true;
        row.toolCount = 3;
        mcpPage.setServers({row});
        mcpPage.show();
        QApplication::processEvents();
        auto *note = mcpPage.findChild<QLabel *>(QStringLiteral("agentMcpNote"));
        check(note != nullptr && note->text().contains(QStringLiteral("Windows 用户权限")),
              "智能体 UI：MCP 页写明本机用户权限");
        auto *name = mcpPage.findChild<QLabel *>(QStringLiteral("agentMcpRowName"));
        check(name != nullptr && name->text() == QStringLiteral("测试服务器"),
              "智能体 UI：MCP 服务器行显示名称");
        auto *identity = mcpPage.findChild<QLabel *>(QStringLiteral("agentMcpRowIdentity"));
        check(identity != nullptr && identity->text().contains(QStringLiteral("abcdef123456")),
              "智能体 UI：MCP 行显示可执行文件身份摘要");
        auto *state = mcpPage.findChild<QLabel *>(QStringLiteral("agentMcpRowState"));
        check(state != nullptr && state->text().contains(QStringLiteral("已连接")),
              "智能体 UI：MCP 行显示连接状态与工具数");
        check(mcpPage.findChild<QPushButton *>(QStringLiteral("agentMcpConnect_srv-1")) != nullptr &&
                  mcpPage.findChild<QPushButton *>(QStringLiteral("agentMcpDisconnect_srv-1")) !=
                      nullptr,
              "智能体 UI：MCP 行有连接/断开按钮");
        check(mcpPage.findChild<QPushButton *>(QStringLiteral("agentMcpConnect_srv-1"))
                      ->text()
                      .contains(QStringLiteral("重新连接")),
              "智能体 UI：已连接时按钮写重新连接");
        mcpPage.setServers({});
        QApplication::processEvents();
        check(mcpPage.findChild<QLabel *>(QStringLiteral("agentMcpEmpty"))->isVisible(),
              "智能体 UI：没有服务器时显示空态");
        mcpPage.close();
    }
    {
        AgentSettingsPage settingsPage;
        settingsPage.setWebSearchEndpoint(QStringLiteral("https://searx.example.com"));
        settingsPage.setDiagnoseCommands(true);
        settingsPage.show();
        QApplication::processEvents();
        auto *searxng = settingsPage.findChild<QLineEdit *>(QStringLiteral("agentSearxngEdit"));
        check(searxng != nullptr &&
                  searxng->text() == QStringLiteral("https://searx.example.com"),
              "智能体 UI：设置页有 SearXNG 地址并回填");
        auto *diagnose = settingsPage.findChild<QWidget *>(
            QStringLiteral("agentDiagnoseCommandsToggle"));
        check(diagnose != nullptr, "智能体 UI：设置页有诊断命令开关");
        check(settingsPage.diagnoseCommands(), "智能体 UI：诊断命令开关状态能读回");
        settingsPage.close();
    }
    {
        AgentSettingsPage settingsPage;
        QApplication::processEvents();
        check(settingsPage.findChild<QPushButton *>(QStringLiteral("agentSettingsMcpButton")) !=
                  nullptr,
              "智能体 UI：API 设置页有「MCP 服务器…」入口");
    }

    page->showApproval(approval);
    QApplication::processEvents();
    auto *approvalCard = page->findChild<AgentApprovalCard *>();
    check(approvalCard != nullptr && approvalCard->approvalId() == QStringLiteral("a1"),
          "智能体 UI：审批卡显示当前请求");
    check(approvalCard != nullptr &&
              approvalCard->findChild<QPushButton *>(QStringLiteral("agentApprovalAllow")) != nullptr &&
              approvalCard->findChild<QPushButton *>(QStringLiteral("agentApprovalDeny")) != nullptr,
          "智能体 UI：审批卡有允许/拒绝");
    auto *sessionAllow =
        approvalCard != nullptr
            ? approvalCard->findChild<QPushButton *>(QStringLiteral("agentApprovalSessionAllow"))
            : nullptr;
    check(sessionAllow != nullptr && !sessionAllow->isVisible(),
          "智能体 UI：Stage 1B 不提供本会话目录放行");
    auto *approvalPreview = approvalCard != nullptr
                                ? approvalCard->findChild<QPlainTextEdit *>(
                                      QStringLiteral("agentEgressPreviewCard"))
                                : nullptr;
    check(approvalPreview != nullptr &&
              approvalPreview->toPlainText().contains(QStringLiteral("new")),
          "智能体 UI：审批卡显示 diff/预览");
    page->hideApproval();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(page->findChild<AgentApprovalCard *>() == nullptr, "智能体 UI：审批卡可收起");

    // 加密存储收进 ⋯ 菜单：开关 + 一句「标题和时间仍为明文。」，右上角不再挂状态字。
    QWidget *more = page->moreMenu();
    page->setEncryptionChecked(true);
    auto *encrypt = more != nullptr ? more->findChild<QCheckBox *>(QStringLiteral("agentEncryptSessionCheck")) : nullptr;
    auto *encryptNote = more != nullptr ? more->findChild<QLabel *>(QStringLiteral("agentEncryptNote")) : nullptr;
    check(encrypt != nullptr && encrypt->isChecked(), "智能体 UI：⋯ 菜单里的加密开关显示当前状态");
    check(encryptNote != nullptr && encryptNote->text().contains(QStringLiteral("明文")),
          "智能体 UI：加密开关下注明标题和时间仍为明文");
    check(page->findChild<QWidget *>(QStringLiteral("agentEncryptionStatus")) == nullptr &&
              page->findChild<QWidget *>(QStringLiteral("agentModeChip")) == nullptr &&
              page->findChild<QWidget *>(QStringLiteral("agentModelCombo")) == nullptr,
          "智能体 UI：不再有加密状态字、「自由对话」假胶囊和单选项模型下拉");
    page->openMoreMenu();
    check(more != nullptr && more->isVisible() && more->findChild<QWidget *>(QStringLiteral("agentExportAction")) != nullptr &&
              more->findChild<QWidget *>(QStringLiteral("agentDeleteAction")) != nullptr,
          "智能体 UI：⋯ 菜单可弹出，含导出与删除会话");
    if (more != nullptr) more->hide();

    AgentSettingsPage settings;
    auto *keyEdit = settings.findChild<QLineEdit *>(QStringLiteral("agentApiKeyEdit"));
    auto *reveal = settings.findChild<QToolButton *>(QStringLiteral("agentApiKeyRevealButton"));
    check(keyEdit != nullptr && keyEdit->echoMode() == QLineEdit::Password,
          "设置页：API Key 默认遮挡");
    check(reveal != nullptr, "设置页：眼睛按钮存在");
    if (reveal != nullptr) {
        reveal->setChecked(true);
        check(keyEdit->echoMode() == QLineEdit::Normal, "设置页：眼睛点开显示明文");
        reveal->setChecked(false);
        check(keyEdit->echoMode() == QLineEdit::Password, "设置页：再点一下恢复遮挡");
    }
    AgentView::ProviderDraft profile;
    profile.id = QStringLiteral("default");
    profile.name = QStringLiteral("测试服务");
    profile.baseUrl = QStringLiteral("https://api.example.com/v1");
    profile.model = QStringLiteral("test-model");
    settings.setProfile(profile, true, QStringLiteral("credential-manager"));
    check(keyEdit->text().isEmpty() && keyEdit->placeholderText().contains(QStringLiteral("留空不修改")),
          "设置页：已保存 key 时不回显，占位文字说明留空不修改");
    auto *baseUrl = settings.findChild<QLineEdit *>(QStringLiteral("agentBaseUrlEdit"));
    auto *preview = settings.findChild<QLabel *>(QStringLiteral("agentFinalUrlPreview"));
    check(baseUrl != nullptr && preview != nullptr &&
              preview->text().contains(QStringLiteral("https://api.example.com/v1/chat/completions")),
          "设置页：显示最终请求 URL 预览", preview != nullptr ? preview->text() : QString());
    // 101：和设置页同一套部件 —— 高级收在 ExpandSection 里（默认收起），开关是 ToggleSwitch，整页没有原生勾选框和带框分组。
    auto *advanced = settings.findChild<SettingsParts::ExpandSection *>(QStringLiteral("agentAdvancedGroup"));
    auto *authMode = settings.findChild<QComboBox *>(QStringLiteral("agentAuthModeCombo"));
    check(advanced != nullptr && !advanced->expanded() && authMode != nullptr &&
              advanced->isAncestorOf(authMode),
          "设置页：协议适配收在默认收起的「高级」里");
    check(settings.findChildren<QCheckBox *>().isEmpty() && settings.findChildren<QGroupBox *>().isEmpty(),
          "设置页：没有原生勾选框和带框分组");
    bool allSwitches = true;
    for (const char *name : {"agentLocalHttpCheck", "agentEgressStrictCheck", "agentStreamCheck",
                             "agentSendTemperatureCheck", "agentSendToolChoiceCheck"})
        allSwitches = allSwitches && settings.findChild<ToggleSwitch *>(QString::fromLatin1(name)) != nullptr;
    check(allSwitches, "设置页：五个开关都是 ToggleSwitch");
    check(reveal != nullptr && reveal->text().isEmpty() && !reveal->icon().isNull(),
          "设置页：眼睛按钮是画出来的图标，不靠可能缺字形的 emoji");
    auto *privacy = settings.findChild<QLabel *>(QStringLiteral("agentPrivacyText"));
    const QString standardText = privacy != nullptr ? privacy->text() : QString();
    settings.setStrictEgress(true);
    check(settings.strictEgress(), "设置页：严格模式可切换");
    check(privacy != nullptr && privacy->wordWrap() && standardText.contains(QStringLiteral("崩溃报告")) &&
              privacy->text() != standardText && privacy->text().contains(QStringLiteral("逐次确认")) &&
              privacy->text().contains(QStringLiteral("永远不会发送")),
          "设置页：隐私说明换行显示，随严格模式切换写明会发出去什么", privacy != nullptr ? privacy->text() : QString());
    bool noCodename = true;
    for (QLabel *label : settings.findChildren<QLabel *>())
        noCodename = noCodename && !label->text().contains(QStringLiteral("Stage"));
    check(noCodename, "设置页：界面上不出现 Stage 1A 之类的内部代号");
    auto *save = settings.findChild<QPushButton *>(QStringLiteral("agentSaveButton"));
    auto *removeProfile = settings.findChild<QPushButton *>(QStringLiteral("agentDeleteProfileButton"));
    check(save != nullptr && removeProfile != nullptr && save->parentWidget() != removeProfile->parentWidget() &&
              !removeProfile->isAncestorOf(save),
          "设置页：「删除此配置」不和「保存」挤在同一行");
    check(settings.findChild<QWidget *>(QStringLiteral("agentMcpList")) == nullptr,
          "设置页：Stage 1A 不显示 MCP 一栏");
    return 0;
}
