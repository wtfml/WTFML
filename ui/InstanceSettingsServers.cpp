// Server favorites, selection actions, MOTD and status rendering.
#include "InstanceSettingsDialog.hpp"
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
#include <QSettings>
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


void InstanceSettingsDialog::replayServerPage() {
    // 顺序照调用方开页时的先后（列表 → 状态 → 收藏 → 加入 → 正在测速）。
    // NOTE: 列表那一发会把状态清成"还没测"（配的是旧的行），所以状态排在它后面重放一遍 ——
    // 那一份早就存在 m_serverStatuses 里了。
    const QList<ServerRowStatus> statuses = m_serverStatuses;
    setServerList(m_serverRows, m_serverProblem);
    if (!statuses.isEmpty()) {
        setServerStatuses(statuses);
    }
    refreshServerFavorites();
    if (!m_serverJoinText.isEmpty()) {
        setServerJoinText(m_serverJoinText, m_serverJoinTipPending);
    }
    setServerPinging(m_serverPinging);
}

void InstanceSettingsDialog::updateServerSelectionActions() {
    if (m_serverEdit == nullptr || m_serverJoin == nullptr || m_serverDelete == nullptr) {
        return;
    }
    const bool hasRow = selectedServerItem() != nullptr;
    if (m_serverFavorite) {
        const bool favorite = hasRow && m_serverFavorites.contains(
            selectedServerItem()->data(1, MainWindowUI::serverAddressRole()).toString());
        m_serverFavorite->setEnabled(hasRow);
        m_serverFavorite->setChecked(favorite);
        m_serverFavorite->setText(favorite ? tr("取消收藏") : tr("收藏服务器"));
    }
    // 「添加」**不在这儿** —— 它不依赖选中行（要加的那一台还没出现在表里），常亮。
    m_serverEdit->setEnabled(hasRow);
    m_serverDelete->setEnabled(!selectedServerItems().isEmpty());
    m_serverJoin->setEnabled(hasRow);
    // 灰着的时候悬停提示换成"先说清为什么灰"—— 一颗点不动的按钮上写着"改选中的那一台
    // 的名字和地址"，用户会以为自己点错了地方。
    m_serverEdit->setToolTip(hasRow ? serverEditTip() : serverSelectFirstHint());
    m_serverDelete->setToolTip(m_serverDelete->isEnabled() ? serverDeleteTip() : serverSelectFirstHint());
    if (!m_serverJoinTip.isEmpty()) {
        m_serverJoin->setToolTip(hasRow ? m_serverJoinTip : serverSelectFirstHint());
    }
}

void InstanceSettingsDialog::setServerJoinText(const QString &text, const QString &tip) {
    // 页是第一次切过去才建的（见 ensurePage()）：开页那一刻这一句就会递进来，先存着，
    // replayServerPage() 建页时再摆。
    m_serverJoinText = text;
    m_serverJoinTipPending = tip;
    if (m_serverJoin == nullptr) {
        return;
    }
    // 空文字 = 收起来（见 .hpp 那条 NOTE）：这一个版本没有 `--quickPlayMultiplayer`，
    // 露着它等于给用户摆一个"按下去游戏立刻报错退出"的键。
    m_serverJoin->setText(text);
    m_serverJoin->setVisible(!text.isEmpty());
    m_serverJoinTip = text.isEmpty() ? QString() : tip;
    // 提示和灰/亮都跟着"有没有选中"走 —— 复用它，别在这儿再写一遍那套判断。
    updateServerSelectionActions();
}

void InstanceSettingsDialog::setServerList(const QList<QPair<QString, QString>> &servers,
                                           const QString &problem) {
    // 同 setServerJoinText()：先存着，页建出来再摆（replayServerPage()）。
    m_serverRows = servers;
    m_serverProblem = problem;
    if (m_serverTree == nullptr) {
        return;
    }
    m_serverListReadable = problem.isEmpty();
    // 这一份是**换掉整张表**：那些"现在什么样"配的是旧的行，一起清掉（见 .hpp 那条 NOTE）。
    m_serverStatuses.clear();
    m_serverStatuses.resize(servers.size());

    m_serverTree->clear();
    const bool hide = hideServerAddress();
    for (const QPair<QString, QString> &server : servers) {
        auto *item = new QTreeWidgetItem(m_serverTree);
        item->setText(0, server.first.isEmpty() ? unnamedMark() : server.first);
        // 那一格里**真正**的名字（可以是空串）。屏幕上空名字写的是「（无名）」——那是我们
        // 编的一句话，「编辑」那颗要预填的是文件里原本那一串（见 MainWindowUI::serverNameRole()）。
        item->setData(0, MainWindowUI::serverNameRole(), server.first);
        // 地址那一栏默认写用户填的原文，「隐藏服务器地址」打开时写遮过的（`m-.h-.c-`）。
        // 放不下时由那一列的代理省略中间（见 InstanceSettingsFilePages.cpp）。
        item->setText(1, shownServerAddress(server.second, hide));
        // 完整的地址挂在这一格上：右键「复制地址」和「编辑 / 加入」都取它，不取屏幕上的字
        // （见 MainWindowUI::serverAddressRole()）。悬停提示给完整地址，开关打开时不给，
        // 否则截图时一悬停就露出来。
        item->setData(1, MainWindowUI::serverAddressRole(), server.second);
        item->setToolTip(1, hide ? QString() : server.second);
        // 状态那一格**一开始就有字**（「还没测」）。空着和"测了没通"在屏幕上是同一个样子，
        // 而这两件事对用户完全不是一回事（一个是"点一下测速"、一个是"这台死了"）。
        item->setText(2, serverUntestedText());
        // 靠右（和 setServerStatuses 里那几行同一条规矩）。每一行都要写一次 —— 这事
        // 记在**条目**上，不是记在列上。
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        // 行高**比别处那张表高一档**：这一行底下那一格画的是 MOTD，而 MOTD 是**两行**
        // （游戏里就是两行，见 MotdText::parse 的 maxLines）。46 px 装不下"标题 + 两行小字"。
        // 走 `Qt::SizeHintRole`（也就是 setSizeHint 这一句）交给代理 —— 它读的就是这一格
        // （见 VersionTreeDelegate::sizeHint 那段）。
        // NOTE: 这一页开着 uniformRowHeights(true)，所以**每一条都得是同一个高度**
        // （少设一条，那一行就是 46，屏幕上看着像"这一行矮了一截"）。
        item->setSizeHint(0, QSize(0, MainWindowUI::serverRowHeight()));
    }
    // 「读不动」和「读到了但一条都没有」是**两句不同的话**（和版本树那条"问到了就是
    // 没有" / "列表没取到"同一条规矩）：说成一句的话，用户会照着错的那句去修 ——
    // 一边是"我确实没加过服务器"，另一边是"你的 servers.dat 坏了，加多少条都白搭"。
    const bool readable = problem.isEmpty();
    const bool empty = servers.isEmpty();
    m_serverTree->setVisible(readable && !empty);
    m_serverNotice->setVisible(!readable || empty);
    m_serverNotice->setText(!readable ? problem : (empty ? serverEmptyText() : QString()));
    // 换掉整张表 ⇒ 选中没了 ⇒ 「编辑 / 加入」那两颗回到灰的（见 updateServerSelectionActions()）。
    // `clear()` 会发一次 itemSelectionChanged，但那一发**不能指望**：表本来就是空的时候
    // （第一次灌）clear() 什么都不发。所以在这儿明喊一次。
    updateServerSelectionActions();
    refreshServerFavorites();
}

void InstanceSettingsDialog::setServerFavorites(const QStringList &addresses) {
    m_serverFavorites = QSet<QString>(addresses.begin(), addresses.end());
    refreshServerFavorites();
}

QStringList InstanceSettingsDialog::serverAddresses() const {
    QStringList addresses;
    if (m_serverTree == nullptr) {
        // 页还没建（第一次切过去才建）：读存着的那一份。调用方开页时拿它算收藏，
        // 要的是"盘上有哪几台"，不是"表上画了几行"。
        for (const auto &row : m_serverRows) {
            addresses.append(row.second);
        }
        return addresses;
    }
    if (m_serverTree) {
        for (int i = 0; i < m_serverTree->topLevelItemCount(); ++i)
            addresses.append(m_serverTree->topLevelItem(i)->data(1, MainWindowUI::serverAddressRole()).toString());
    }
    return addresses;
}

void InstanceSettingsDialog::refreshServerFavorites() {
    if (!m_serverTree || !m_serverFavoritesOnly || !m_serverListReadable) return;
    int visible = 0;
    for (int i = 0; i < m_serverTree->topLevelItemCount(); ++i) {
        auto *item = m_serverTree->topLevelItem(i);
        const bool favorite = m_serverFavorites.contains(item->data(1, MainWindowUI::serverAddressRole()).toString());
        const auto name = item->data(0, MainWindowUI::serverNameRole()).toString();
        item->setText(0, (favorite ? QStringLiteral("★ ") : QString()) + (name.isEmpty() ? unnamedMark() : name));
        item->setHidden(m_serverFavoritesOnly->isChecked() && !favorite);
        if (!item->isHidden()) ++visible;
        if (item->isHidden() && item->isSelected()) {
            m_serverTree->clearSelection();
            m_serverTree->setCurrentItem(nullptr);
        }
    }
    // Never reorder rows: ping results refer to the original servers.dat order.
    if (m_serverTree->topLevelItemCount() > 0) {
        m_serverTree->setVisible(visible > 0);
        m_serverNotice->setVisible(visible == 0);
        if (visible == 0) m_serverNotice->setText(tr("暂无收藏服务器"));
    }
    updateServerSelectionActions();
}

QStringList InstanceSettingsDialog::serverStatusLines(const ServerRowStatus &status) {
    if (!status.tested) {
        return {serverUntestedText()};
    }
    // 这一格**只有一种写法**：缓存里那一份和当场测出来那一份说同一句话，没有任何
    // "上次"之类的过去时前缀。（`Entry::timedOut` 这一位必须留着：正因为没有前缀，
    // "超时"和"没连上"必须是**同一个判据**得出的同一种字，不然同一台服务器前后两次进
    // 这一页看见的是两个说法。）
    if (!status.online) {
        // 缓存里"没连上"那一档**不重说理由**：那句话是**当时**的（"连接被拒绝"指的是上一次
        // 那一刻），搬到现在来说等于替这一次下结论。就写结论，理由等真的测完那一趟。
        //
        // NOTE: 这一支和下面"当场测出来"那一支**看着几乎一样**，可别合并：这一支只认
        // `timedOut` 这一位（盘上只存了这一位，见 ServerStatusCache::Entry），而下面
        // 那一支拿得到 `error` 那句全话。
        if (status.fromCache) {
            return {status.timedOut ? serverTimeoutText() : tr("没连上")};
        }
        // 超时那一档**只写两个字**。判据是
        // `Status::timedOut` 那一位，不是这句 error 的前缀 —— 「网络不通」和「它连上了却
        // 不答话」在 ServerPing 里是两处不同的判定（见那边 isTimeoutError()），只是对这个
        // 格子来说结论是同一个：现在连不上。
        if (status.timedOut) {
            return {serverTimeoutText()};
        }
        // 其余没通的**理由来自 ServerPing**（中文，见那边的 connectionErrorText）。真要是
        // 空的也别留一格空白 —— 那就是"看着像没测过"。没通这一档**只有一行**：那句话本来
        // 就长（「没有回应（对面没开，或者端口不对）」），拆成两行反而更难读。
        return {status.error.isEmpty() ? tr("连接失败") : status.error};
    }
    // 通了就是两行：第一行延迟、第二行几个人。人**不并进第一行**（那是另一件事，而且这一格
    // 窄）—— 在线延迟和人数就是这两行。
    //
    // 「在线」那两个字不写：`42 ms` 摆在那儿本来就说明它通着，那两个字占的是这一栏的宽度。
    QStringList lines;
    if (status.latencyMs >= 0) {
        lines.append(QStringLiteral("%1 ms").arg(status.latencyMs));
    }
    if (status.playersOnline >= 0 && status.playersMax >= 0) {
        lines.append(tr("%1/%2 人").arg(status.playersOnline).arg(status.playersMax));
    } else if (status.playersOnline >= 0) {
        lines.append(tr("%1 人").arg(status.playersOnline));
    }
    if (lines.isEmpty()) {
        // 通了、但一个数都没量到。**这一档生产里到不了**（ok 为真时 latencyMs 必然是量过的，
        // 见 ServerPing.cpp），可这一格里空着一行和"还没测"在屏幕上是同一个样子 —— 留一句
        // 兜底「在线」。
        lines.append(tr("在线"));
    }
    // 通了这一档**两处（缓存 / 当场）写的是同一份字**，没有任何前缀可加（见上面那段）。
    return lines;
}

QString InstanceSettingsDialog::serverStatusText(const ServerRowStatus &status) {
    return serverStatusLines(status).join(QStringLiteral(" / "));
}

QString InstanceSettingsDialog::serverStatusTooltip(const ServerRowStatus &status) {
    QStringList parts{serverStatusText(status)};
    // 版本名是服务器**自己说的**，原样挂着；只是它不属于"现在几个人"，所以只进提示。
    const QString version = status.versionName.trimmed();
    if (!version.isEmpty()) {
        parts.append(version);
    }
    // 在线玩家名同一档：洗过的名字再 trim 一遍（缓存 / 调用方递进来的可能带空白），
    // 空格子跳过。最多列五个 —— 提示框跟着服务器给的名单无限变长不是个事。
    constexpr int kMaxTooltipPlayers = 5;
    QStringList names;
    for (const QString &name : status.samplePlayers) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty()) {
            names.append(trimmed);
        }
    }
    if (!names.isEmpty()) {
        QString line =
            tr("在线：%1").arg(names.mid(0, kMaxTooltipPlayers).join(tr("、")));
        // 还有没列出来的：名单本身更长，或者服务器只给了其中一段（在线人数 > 名单）。
        if (names.size() > kMaxTooltipPlayers || status.playersOnline > names.size()) {
            line += tr(" 等");
        }
        parts.append(line);
    }
    return parts.join(QStringLiteral(" / "));
}

bool InstanceSettingsDialog::hideServerAddress() {
    return QSettings().value(QStringLiteral("Look/HideServerAddress"), false).toBool();
}

QString InstanceSettingsDialog::shownServerAddress(const QString &address, bool hide) {
    return hide ? maskedServerAddress(address) : address.trimmed();
}

QString InstanceSettingsDialog::elidedServerAddress(const QString &text, const QFontMetrics &metrics, int width) {
    if (metrics.horizontalAdvance(text) <= width) {
        return text;
    }
    // `:端口` 单独留着：主机名从右边收，省略号接在端口前面。冒号右边不是全数字（IPv6、没端口）就不拆。
    const int colon = text.lastIndexOf(QLatin1Char(':'));
    const QString port = text.mid(colon + 1);
    const QString head = text.left(colon);
    const bool hasPort = colon > 0 && !port.isEmpty() && (!head.contains(QLatin1Char(':')) || head.endsWith(QLatin1Char(']'))) &&
                         std::all_of(port.begin(), port.end(), [](QChar c) { return c.isDigit(); });
    if (!hasPort) {
        return metrics.elidedText(text, Qt::ElideMiddle, width);
    }
    const QString tail = text.mid(colon);
    return metrics.elidedText(head, Qt::ElideRight, qMax(0, width - metrics.horizontalAdvance(tail))) + tail;
}

QString InstanceSettingsDialog::maskedServerAddress(const QString &address) {
    const QString trimmed = address.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    // ---------- ① 把"要遮的那个主机"和"照抄的尾巴"分开 ----------
    //
    // 方括号那一档（`[2001:db8::1]:25566`）**方括号要留着**，这一格和连接那一步是两回事：
    // 不带方括号的 IPv6 后面又跟端口时，哪几个冒号是地址、哪一个是端口分不出来
    // （ServerPing::splitAddress 里那条"看不懂就不猜"的规矩就是为这个）。其余情况按
    // **最后一个**冒号分 —— 而那个冒号左边要是还带着冒号，那这串本来就是裸的 IPv6，
    // 没有端口可拆。
    bool bracketed = false;
    QString host = trimmed;
    QString tail;  // 端口那一截（**带**冒号），没有就是空
    if (trimmed.startsWith(QLatin1Char('['))) {
        const int close = trimmed.indexOf(QLatin1Char(']'));
        if (close > 0) {
            bracketed = true;
            host = trimmed.mid(1, close - 1);
            tail = trimmed.mid(close + 1);
        }
    } else {
        const int colon = trimmed.lastIndexOf(QLatin1Char(':'));
        if (colon > 0 && !trimmed.left(colon).contains(QLatin1Char(':'))) {
            host = trimmed.left(colon);
            tail = trimmed.mid(colon);
        }
    }

    // ---------- ② 这一截是"名字"还是"数字地址" ----------
    //
    // 两种要遮的东西不是一回事，所以判据也不是一条。带冒号的（`[::1]` 是这一支）和"正好
    // 四段、段段是数字"的按数字地址处理 —— 数字地址留头两段就够认出网段了，剩下的打掉。
    // 别的一律当域名：域名要遮的是**那个名字**。
    const QChar separator = host.contains(QLatin1Char(':')) ? QLatin1Char(':') : QLatin1Char('.');
    const QStringList groups = host.split(separator);
    bool numeric = separator == QLatin1Char(':');
    if (!numeric && groups.size() == 4) {
        numeric = true;
        for (const QString &group : groups) {
            if (group.isEmpty() || group.size() > 3) {
                numeric = false;
                break;
            }
            for (const QChar character : group) {
                if (!character.isDigit()) {
                    numeric = false;
                    break;
                }
            }
            if (!numeric) {
                break;
            }
        }
    }

    QStringList masked;
    if (numeric) {
        for (int i = 0; i < groups.size(); ++i) {
            if (i < 2) {
                masked.append(groups.at(i));
            } else {
                // 空的那一段保持空 —— 数字地址里那个空段就是 `::`（压缩写法）本身，
                // 填成 `*` 的话 `[2001:db8::1]` 会遮成 `[2001:db8:*:*]`：四个段、一个
                // 双冒号都没有，看着像另一个地址。留着空段，遮出来是 `[2001:db8::*]`。
                masked.append(groups.at(i).isEmpty() ? QString() : QStringLiteral("*"));
            }
        }
    } else {
        for (const QString &group : groups) {
            // 空的那一段保持空（`a..b` 这种坏地址不该被我们连成 `a-.b-` 那样的假名字）
            masked.append(group.isEmpty() ? QString() : group.left(1) + QStringLiteral("-"));
        }
    }

    QString out = masked.join(separator);
    if (bracketed) {
        out = QStringLiteral("[") + out + QStringLiteral("]");
    }
    return out + tail;
}

void InstanceSettingsDialog::setServerStatuses(const QList<ServerRowStatus> &statuses) {
    // **先存再判空**：页是第一次切过去才建的（见 ensurePage()），这一份来得比页早 ——
    // 存下来，`replayServerPage()` 建页时会照着它再摆一遍（那条注释成立的前提就是这一行
    // 排在判空之前）。
    m_serverStatuses = statuses;
    if (m_serverTree == nullptr) {
        return;
    }
    // 行数是"盘上那份 servers.dat"说了算，状态是"刚问回来的那一趟"说了算 —— 两边对不上
    // 时**按行来写**：多出来的状态丢掉，行多出来的那些回到「还没测」（不是留着上一次那几句
    // ——那些话配的是上一份列表的行）。两条都不越界。
    const int rows = m_serverTree->topLevelItemCount();
    for (int i = 0; i < rows; ++i) {
        QTreeWidgetItem *item = m_serverTree->topLevelItem(i);
        if (item == nullptr) {
            continue;
        }
        const ServerRowStatus status = i < statuses.size() ? statuses.at(i) : ServerRowStatus();
        // 图标走 `Qt::DecorationRole`（就是 setIcon 那一格）—— 代理读的正是它；
        // 第二行小字走 rowSubTextRole()（版本树上放的是发布时间，这里放 MOTD）。
        item->setIcon(0, status.icon);
        item->setData(0, MainWindowUI::rowSubTextRole(), status.motd);
        // MOTD 的**原样**（带 `§` 码和换行）走另一个角色：代理按它分色、分两行画
        // （见 VersionTreeDelegate 里 rowMotdRawRole() 那一段）。两个都写 —— 代理那边
        // 是"有原串就画原串、没有才退回洗过那一行"，而 tooltip 用的永远是洗过那一份。
        item->setData(0, MainWindowUI::rowMotdRawRole(), status.motdRaw);
        // 状态那一格是**两行**（`42 ms` / `7/20 人`）：`\n` 在这里不是排版上的
        // 小把戏 —— 控件是开了 wordWrap 的，条目代理照 QTextLayout 摆，一个换行就是
        // 真的一行（见建表那段）。悬停提示仍旧给**单行**那份：提示框里没有"两行"这回事，
        // 它要的是一句话。
        item->setText(2, serverStatusLines(status).join(QLatin1Char('\n')));
        item->setToolTip(2, serverStatusTooltip(status));
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        // 名字单独给一句 tooltip：MOTD 那一行装不下时是被代理省略掉的，而这一行**没有**
        // 别的地方能看全（那一格只画得下一行）。**无条件写**（MOTD 是空串时这一句就是把
        // 上一次那行字抹掉）—— 不然"上一次那台有 MOTD、这一台没有"会留下一句别人的话。
        item->setToolTip(0, status.motd);
    }
}

void InstanceSettingsDialog::setServerPinging(bool pinging) {
    m_serverPinging = pinging;
    if (m_serverRefresh == nullptr) {
        return;
    }
    m_serverRefresh->setText(pinging ? serverPingingText() : serverRefreshText());
    // 连点三下发三遍请求是用户没要的，而且每一遍都要等满超时。
    m_serverRefresh->setEnabled(!pinging);
}
