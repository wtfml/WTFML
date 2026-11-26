#pragma once

/**
 * @file main.cpp
 * @brief 一次性验证：offscreen 起真的 MainWindow，断言「启动游戏」按钮第二行 /
 * 「已安装版本」下拉框 / 占位文案 / 上次启动的版本 这几条不变式。
 *
 * 为什么必须立真的 MainWindow：这条同步有一条路径靠信号接不到 ——
 * refreshInstalledVersions() 内部用 QSignalBlocker 挡住重灌时的信号，所以
 * "刷新"必须自己收尾。只接 currentIndexChanged 的写法在"用户手选"这条路上
 * 看着是对的，一旦刷新就会露出陈旧文字。ui_check 只搭 MainWindowUI，验不到。
 *
 * 沙盒：跑之前先在工作目录（= exe 所在目录）下造一个假 .minecraft/versions，
 * 这样 applicationDirPath() 一解析就能扫到可控的版本清单，绝不碰真的存档目录。
 * QSettings 也重定向到同一个工作目录下的 check-settings/ —— 不这么做的话
 * "上次启动的版本"会写进**用户的真注册表**，而且读的也是用户真配置：
 * 启动恢复那条分支在测试里就永远是走的别人机器上的状态，等于没测。
 *
 * 用法：mw_check
 *
 * 另有五条**要联外网**的一次性探针（都不进默认那一跑，默认那一跑必须离线、不碰用户配置）：
 * mw_check.exe auth-live 真注册表 + 真网络：盘上那份令牌还能不能换出会话
 * mw_check.exe resource-live [类] [搜索词] [来源]
 * 从资源表里真装一个模组/资源包/光影到沙盒实例
 * （来源 = modrinth（默认）/ curseforge）
 * mw_check.exe icon-live [数量] 真网络量一次"一页图标多久回来、几张空手"
 * mw_check.exe icon-lookup-live <目录> 真网络 + **用户自己的盘**：一个目录里的模组
 * 各认出了什么、拿到几条真图标地址（**只读**）
 * mw_check.exe modpack-import-live <包.zip> <实例名> <游戏根目录> [java]
 * 真跑一次整合包导入（几个 GB，几个钟头）
 */
#include "SettingsPageParts.hpp"
#include "SupportCore.hpp"
#include "SupportFakes.hpp"

// ---------- 模态窗：登录成功/失败都要弹窗，而模态窗会卡住 exec() ----------
// 产品约定是"挡住操作的错误必须弹窗"（不能只写日志），所以登录失败会弹，成功也会弹。
// 弹出来的那一刻 exec() 就在等用户点 —— 测试里不安排人把它点掉就会永远卡在那儿。
// 做法是先排一个 0 毫秒的定时器：它在模态窗自己那个事件循环里立刻响，把窗关掉，顺手
// 把上面写了什么记下来，好让断言能检查"弹窗说的对不对"。
/// 覆盖层页眉那行字；实例设置页的页眉只写「实例设置」，是哪个实例由页内大标题
/// （`instanceHeaderTitle`）说，这里拼成「实例设置：<实例名>」让"页眉写着它"的断言仍能认出实例。
inline QString overlayHeading(OverlayHost *host) {
    QString heading;
    if (auto *title = host->findChild<QLabel *>(OverlayHost::overlayTitleLabelName())) heading = title->text();
    if (auto *name = host->findChild<QLabel *>(QStringLiteral("instanceHeaderTitle"))) {
        heading += QStringLiteral("：") + name->text();
    }
    return heading;
}

inline QString g_modalTitle;
inline QString g_modalText;
inline bool g_modalWasWarning = false;

/// 下面那几个助手共用的**"这一发还作不作数"**的凭据：每调用一次助手就加一，过期的
/// 定时器到点自己退出去。
///
/// NOTE: 为什么必须有它 —— 兜底那条等的是**事件循环**里的 300 毫秒。前面某一段要是卡了
/// 一下（起游戏进程、同步等），那条定时器会晚到很久，落在**后面某一段**弹出来的
/// 东西上；它认不出那已经不是自己等的那个，照旧把它点掉。实测见过：U1 的兜底跑到
/// U5 的「Java 版本对不上」那一块上，把它按"取消"关了 —— 用户视角就是"这次启动被
/// 莫名取消了"，而测试红的是 U5 那两条。这不是产品的问题（关掉那个框本来就该取消
/// 启动），是测试自己两只手打架。
inline int g_modalHandlerGeneration = 0;

/// 现在**真的摆在屏幕上**的顶层窗有几个。
///
/// 这是用户给的那条方向（「最终不出现独立的窗口」）唯一能量出来的形状：提示摆出来之前
/// 数一遍、摆出来之后再数一遍，两个数必须一样。
///
/// NOTE: 判据是 `isWindow() && isVisible()` 而不是只数 `topLevelWidgets().size()` ——
/// Qt 里"没有父控件的 QWidget 就是窗口"，覆盖层自己那个 `m_parking`（寄存还回去的
/// 内容用的空控件，见 OverlayHost.hpp）也算一个，可它从来没显示过，不该算"多了一个窗"。
void runPromptProbeChecks();
int topLevelWindowCount();

/// 现在开着的那一层覆盖层（设置页和各种提示都开在它里面）。找不到返回 nullptr。
OverlayHost *currentOverlay();

/// 现在开着的**整页**那一层（「实例设置」这种）。找不到返回 nullptr。
OverlayHost *currentPageOverlay();

/// 现在开着的那一条**提示**。找不到返回 nullptr（= 这一发根本没弹东西）。
InlineMessageBox *currentPrompt();

/// 把这一条说了什么记进 g_modal* 那几个全局里（断言读的就是它们）。
void recordPrompt(InlineMessageBox *box);

/// 点这一条提示上的第 index 颗按钮。点不着就把它整块收掉（等于"没选"，见 InlineMessage）。
void clickPromptButton(int index);

/// `dismissNextPrompt()` 的语义是**不选**（不是把框 `close()` 掉）。
/// 在这一层里"没选"就是 `clickedIndex()` 停在 -1。映射过来的那些问句里**最后一颗恒是
/// 「否 / 取消」**（`InlineMessage::question` 的按钮表就是 `{是要问的那件事, 否}`），
/// 所以点最后一颗得到的是同一个结果 —— 这也是为什么这里要数到最后一颗，而不是点第一颗。
void dismissNextPrompt();

/// 点**一颗具体的按钮**（不是"不选"）。
///
/// 为什么不能靠 dismissNextPrompt()：那一颗是最后一颗（否 / 取消），而"我要答是"和
/// "框被关了"在断言里必须是两件不同的事。位置按**下标**认（不是按
/// QMessageBox::StandardButton 认）：映射过来时按钮表就是 `{是, 否}`，所以
/// `QMessageBox::Yes` → 下标 0、`QMessageBox::No` → 下标 1。
void answerNextPrompt(int index);

/// 底栏左面板里那颗电源点出来的「停止游戏」那一问：**点那颗"停止"**。
///
/// 那一问的按钮表是 `{停止游戏, 取消}`（见 MainWindow::confirmStopGame），停止是**第一颗**。
/// 这一层里没有角色这一说："哪一颗是危险动作"是按**下标**给的（`destructiveIndex`，
/// 它还顺带决定那颗按钮染不染红，见 InlineMessageBox 的 ctor），所以只能按下标认。
/// **两颗的字都断言一遍**：光按下标点，将来按钮顺序换了也照样绿。
void answerStopGamePrompt();

/// @param instanceOut 页眉那句话（调用方在 processEvents() 之后读它 —— 这个助手只是把
/// 定时器排上去，**不**在这里等）。
///
/// NOTE: 判据还是"页眉上那句话"，不是"有没有这一页" —— 那样的话"开错了实例"这条断言
/// 就永远绿。弹出的是别的东西时，这里也会把它记进 g_modalTitle/g_modalText 再收掉，
/// 于是断言看到的是"多弹了一个"。
void closeInstanceSettingsPage(QString *instanceOut);

/// 那个「Java 版本对不上」的三岔口上**点具体的按钮**：不同的按钮通向三条不同的路，
/// 而光把它收掉只能得到"不启动"那一条。
inline QString g_mismatchLabel;

void answerJavaMismatchDialog(const QString &buttonName, bool checkDontRemind);

/// 「连着答几条提示」的那台小答话器（AJ 那一节用）。
///
/// 为什么不能用上面那几个"排一发"的助手（`answerNextPrompt()` 那一族）：启动前那两条提醒
/// **可以前后一起来**（同一个实例上连点两下、两条都开着时，先问「重复启动」再问
/// 「进度提醒」），而排一发那套里 300 毫秒的兜底会在第二问开着时**再点一次** ——
/// `recordPrompt(nullptr)` 把刚记下来的标题清掉，断言读到的是空的（红的理由看着像"产品
/// 没弹"，方向正好反了）。
///
/// 所以这里改用**自走的定时器**：把"要答的按钮下标"排成脚本，每一拍看一眼有没有提示开着，
/// 有就记下它说了什么（标题 / 正文 / 小字 / 按钮表）、按脚本点下一发。产品那条路是**在
/// 嵌套事件循环里**弹框的（`execPrompt`），所以定时器只要在跑，它自己就会在框摆出来的
/// 那一刻醒过来 —— 调用方那边是阻塞的，不用管。
class PromptAnswerer {
public:
    /// @param script 第 1、2、3… 条提示各点第几颗按钮（不够用时最后一格顶上）。
    explicit PromptAnswerer(const QList<int> &script, const QList<bool> &checkScript = {})
        : m_script(script), m_checkScript(checkScript) {
        m_timer.setInterval(10);
        QObject::connect(&m_timer, &QTimer::timeout, [this] { tick(); });
    }
    void start() {
        // 排发那套（`dismissNextPrompt()` / `answerNextPrompt()`）留下的**那一条 300 毫秒
        // 兜底可能还挂在事件循环里**（就是 `g_modalHandlerGeneration` 那段 NOTE 说的
        // "测试自己两只手打架"）。前面那一段要是跑了很久（这一节前面在写盘、压 gzip，
        // 一次 PowerShell 就是小一秒），那条定时器**一进事件循环就立刻到期**，落在
        // **我们**等来的第一问上，照旧把它点掉 —— 症状是"脚本一条都没记着、框像是压根
        // 没弹过"，而下面那些断言会红在"页没打开"上，方向正好反了。
        // 这一加等于说：从这一刻起，排发那套一律作废（它们自己认代数）。
        ++g_modalHandlerGeneration;
        m_clock.start();
        m_timer.start();
    }
    void stop() { m_timer.stop(); }

    /// 一条提示说了什么（按弹出的先后顺序）。
    struct Answered {
        QString title;
        QString text;
        QString note;
        QString checkBoxText;
        bool hasCheckBox = false;
        QStringList buttons;
    };
    QList<Answered> answered;

private:
    void tick() {
        if (m_clock.elapsed() > 20000) {
            // 兜底：不让测试挂在"等一个永远不来的框"上（正常情况下一拍就答完了）。
            m_timer.stop();
            return;
        }
        InlineMessageBox *box = currentPrompt();
        if (box == nullptr) {
            return;  // 这会儿没有框开着：下一拍再看
        }
        Answered one;
        if (auto *title = box->findChild<QLabel *>(InlineMessageBox::titleLabelName())) {
            one.title = title->text();
        }
        if (auto *text = box->findChild<QLabel *>(InlineMessageBox::textLabelName())) {
            one.text = text->text();
        }
        one.note = box->noteText();
        if (QCheckBox *check = box->checkBox()) {
            one.hasCheckBox = true;
            one.checkBoxText = check->text();
        }
        for (int i = 0;; ++i) {
            auto *button = box->findChild<QPushButton *>(InlineMessageBox::buttonName(i));
            if (button == nullptr) {
                break;
            }
            one.buttons.append(button->text());
        }
        answered.append(one);
        const int nth = answered.size();  // 这是第几条（1 起）
        if (QCheckBox *check = box->checkBox();
            check != nullptr && nth <= m_checkScript.size() && m_checkScript.at(nth - 1)) {
            check->setChecked(true);
        }
        const int index = nth <= m_script.size() ? m_script.at(nth - 1)
                                                : m_script.value(m_script.size() - 1, 0);
        clickPromptButton(index);
    }
    QTimer m_timer;
    QElapsedTimer m_clock;
    QList<int> m_script;
    QList<bool> m_checkScript;
};

/// 把**整页**（`OverlayHost::showPage()` 摆出来的那种）收掉：它们自己开出来、调用方早已
/// 返回，测试这边得有人点返回箭头。
///
/// 为什么不用 `closeInstanceSettingsPage()`（那一族是 0 毫秒 + 300 毫秒排两发）：这一页是
/// **等后台那一趟跑完才开出来的**，落在兜底那一发之后 —— 排发那套全落空，测试就挂在一个
/// 没人收的页面上。所以这里也用**自走的定时器**：每一拍看一眼有没有整页开着，有就记下它长
/// 什么样、点返回（见 PromptAnswerer 那段同一套理由）。
class PageCloser {
public:
    void start() {
        ++g_modalHandlerGeneration;  // 排发那套一律作废（理由见 PromptAnswerer::start）
        m_clock.start();
        m_timer.setInterval(10);
        QObject::connect(&m_timer, &QTimer::timeout, [this] { tick(); });
        m_timer.start();
    }
    void stop() { m_timer.stop(); }

    /// 只认**这个标题**的那一页（留空 = 每一页都算，默认如此）。
    void setOnlyTitle(const QString &title) { m_onlyTitle = title; }

    /// 开过又收掉的每一页说了什么（按先后顺序）。
    struct Seen {
        QString title;    ///< 覆盖层页眉上那句话
        QString summary;  ///< 体检报告页顶部那句总述（别的页上没有这一格，留空）
        /// 更新中心那一页上的东西：总述那一句、两组的标题、
        /// 每一行的名字（左栏）和状态（右边那一格）。**收页之前**读下来。
        ///
        /// 为什么读的是**控件上的字**而不是哪一份数据：这一页是 MainWindow 那一层
        /// 翻好的（`updateCenterReportForPage()`），"翻得对不对"只有看屏幕上的字才算数
        /// （读数据等于把翻译那一层绕过去自证）。
        QString updateCenterSummary;
        QStringList updateCenterGroupTitles;
        QStringList updateCenterRowNames;
        QStringList updateCenterRowStates;
    };
    QList<Seen> seen;

private:
    void tick() {
        if (m_clock.elapsed() > 20000) {
            m_timer.stop();
            return;  // 兜底：不让测试挂在"等一页永远不来的页"上
        }
        OverlayHost *host = currentOverlay();
        if (m_closing) {
            // 收页只是**请求**（见 OverlayHost::evict() 那段）：它要等这一趟调用栈退出去
            // 才真的收干净。在这之前别再记一遍（同一页会被记成两条）。
            if (host == nullptr || !host->pageOpen()) {
                m_closing = false;
            }
            return;
        }
        if (host == nullptr || !host->pageOpen()) {
            return;
        }
        Seen one;
        one.title = overlayHeading(host);
        // 指定了标题就只认那一页（见 setOnlyTitle 那段）：**不记也不收**，让它继续开着。
        if (!m_onlyTitle.isEmpty() && one.title != m_onlyTitle) {
            return;
        }
        if (auto *summary =
                host->findChild<QLabel *>(MainWindowUI::integrityReportSummaryName())) {
            one.summary = summary->text();
        }
        // 更新中心：总述 + 两组的标题 + 每一行那两格。组的数量是**固定的两个**
        // （见 UpdateCenterPage::rebuild() —— 一组空了也摆），行数按 `updateCenterRowN`
        // 往后数到找不到为止。
        if (auto *summary =
                host->findChild<QLabel *>(MainWindowUI::updateCenterSummaryName())) {
            one.updateCenterSummary = summary->text();
        }
        for (int group = 0; group < 8; ++group) {
            auto *title = host->findChild<QLabel *>(MainWindowUI::updateCenterGroupName(group));
            if (title == nullptr) {
                break;
            }
            one.updateCenterGroupTitles.append(title->text());
        }
        for (int index = 0; index < 512; ++index) {
            auto *row = host->findChild<QWidget *>(MainWindowUI::updateCenterRowName(index));
            if (row == nullptr) {
                break;
            }
            auto *name = row->findChild<QLabel *>(MainWindowUI::updateCenterRowNameLabelName(index));
            auto *state = row->findChild<QLabel *>(MainWindowUI::updateCenterRowStateName(index));
            one.updateCenterRowNames.append(name == nullptr ? QString() : name->text());
            one.updateCenterRowStates.append(state == nullptr ? QString() : state->text());
        }
        seen.append(one);
        m_closing = true;
        if (auto *back = host->findChild<QPushButton *>(OverlayHost::overlayBackButtonName())) {
            back->click();  // 返回箭头 = reject（见 OverlayHost::showPage）
            return;
        }
        host->closeOverlay();  // 兜底：至少别让测试卡在这一层上
    }
    QTimer m_timer;
    QElapsedTimer m_clock;
    bool m_closing = false;
    QString m_onlyTitle;
};

/// 让底栏**重画一次**（走 updateBottomBar() 那条真路）。
///
/// updateBottomBar() 是私有的，只能由公开的那条路触发。这里用的是**切一下选项卡**：
/// 那个信号在 setupConnections() 里接着 onMainTabChanged() → updateBottomBar()，而且
/// 它一行状态都不改 —— 这一点是必须的：AB / AD 两节量的都是"重画之后那几个数是从记着的
/// 那份里捞回来的"，扳机自己要是先把数写一遍（比如重灌一次 onVersionDownloadStats），
/// 那就成了自证。
///
/// 为什么不用主页那个「离线模式」勾选框：它已经去掉了（离线身份由当前账号决定）。
/// 为什么
/// 不用 setDownloadUiBusy()：那是全项目唯一的 Q_INVOKABLE，但它会把下载那几个数清零，
/// 正好把要量的现场毁掉。真正在生产里触发这次重画的是「游戏吐出第一行输出」
/// （onProcessOutput / onGameFinished），那两条路要有真的进程起来，验不了。
inline void repaintBottomBar(QWidget *window) {
    auto *tabs = window->findChild<QTabWidget *>(MainWindowUI::tabWidgetName());
    if (tabs == nullptr) {
        return;
    }
    const int back = tabs->currentIndex();
    // 换到主页再换回来。**绝不用设置页**：切到那边会顺带扫一遍 Java（要起进程）。
    tabs->setCurrentIndex(back == MainWindowUI::homeTabIndex()
                              ? MainWindowUI::downloadTabIndex()
                              : MainWindowUI::homeTabIndex());
    tabs->setCurrentIndex(back);
    QApplication::processEvents();
}

  // namespace

// QTreeWidgetItem 不是 QObject，Qt 没给它带 Q_DECLARE_METATYPE —— 而下面 Y6 要按名字
// 调 QTreeWidget::itemClicked（那条信号的参数就是它），invokeMethod 得先在元系统里
// 认得这个名字。声明放在 main 外面：Q_DECLARE_METATYPE 只能在文件作用域展开。
Q_DECLARE_METATYPE(QTreeWidgetItem *)

/// 把 `QDesktopServices::openUrl()` **截胡**下来的一只替身（P2 段用）。
///
/// 为什么非得截：齿轮菜单里的「打开」走的就是 `QDesktopServices::openUrl()`（Windows 上
/// 底下是 ShellExecute），真跑一次 = 在跑测试这台机器上**弹出一个真的资源管理器窗口**。
/// 那既污染了这台机器，也让"路径对不对"变成肉眼看一眼才能知道的事。
///
/// `setUrlHandler("file", ...)` 之后那一发就直接送到这里，不再往下走到系统那儿 ——
/// 于是"点了「打开」，启动器请求打开的到底是哪个目录"变成了一条可断言的事。
///
/// NOTE: 必须是**插槽**（moc 那一套按名字找它）。这也是这个文件里唯一的 Q_OBJECT ——
/// 元对象只由 wtfml_mw_fixture 对本头文件生成，测试模块复用它。
class UrlSpy : public QObject {
    Q_OBJECT
public:
    /// 截下来的每一发（按顺序）。用字符串比用 QUrl：断言里打出来是人能读的那一行。
    QStringList urls;
public slots:
    void handle(const QUrl &url) { urls.append(url.toLocalFile()); }
};

// =========================================================================
// ZB8：「核对那一发」的**三种出路**，端到端跑一遍（本地假服务器，不碰外网）。
//
// 这是用户 那条「缓存默认不过期，每次打开用网络刷新，比较两个文件是否有
// 区别（如果这个会严重占用网络的话就不做了）」在**代码**上的正脸：同一个图标，
// 这一趟第一次被要的时候会带条件头发一发，然后三条路各走各的 ——
// * **304**（没变）→ 不写盘、不发信号、界面上一格都不动；
// * **200 且字节一模一样**（主机不认条件头时就是这样）→ 同上；
// * **200 且字节不一样**（作者换了图标）→ 写盘、存新凭据、再发一次 iconReady。
// 第一条错了（比如把 304 当成失败）就会用一个空 QPixmap 把已经画上去的那一格**擦掉**；
// 第三条错了就是"作者换了图标，用户永远看不到新的"。两条都**不报任何错**，
// 所以只能在本地假服务器上钉住 —— 真网络那条探针（下面 icon-live）只报数、不断言。
//
// 【为什么住在 mw_check 而不是 verify】
//
// 这条路要 **QPixmap**，而 QPixmap 没有 QGuiApplication 根本建不出来 —— verify 那个工程
// 是 QCoreApplication（纯控制台），在那里跑出来的症状是
// 「QPixmap cannot be created without a QGuiApplication」，然后**每一条都红**，
// 而且红得莫名其妙：盘上那份解不出来 → 被当成"盘上没有"，走到另一个分支去了。
// 这里是真的 QApplication，所以跑得起来。
//
// 前提：只有**要 webp 那条路**的地址是测试能定的（那条用的是接口给的 hit.iconUrl；
// 要 PNG 时是 ResourceCatalog 里写死的 CDN 地址）。所以这一节跑在"这台机器解得了 webp"
// 之上 —— 本机（用户这台）实测是真的；真换了台没装那个编解码器的机器，这里打一句
// 说明就跳过（那台机器上产品走的是 PNG 那条路，逻辑一样，只是这里够不着那个地址）。
// =========================================================================


void runIconRevalidateTests();


// ---------------------------------------------------------------------------
// ZH. 下载页第二取数源：ResourceStore 按来源发请求（本地假服务器，不碰外网）
//
// 这是 "搜索加上来源"那件事的**接线**那一半。地址怎么拼、正文怎么读是
// ResourceCatalog 的纯函数，那两条在 verify 的 Z7 一节逐字钉着；这一节钉的是**这一层
// 到底把哪一发发出去了、带了什么头、回来交给了哪个解析器** —— 纯函数全绿而这里错了的话，
// 症状正是用户看到的那种："选了 CurseForge，界面还是 Modrinth 的东西"、或者"CurseForge
// 那一页永远说版本列表读不懂"。
//
// 四件事，每一件都有个"不查就永远不知道"的形状：
// ① `x-api-key` **只在有 key 时发**（默认走社区镜像、一个头都不带），而**两个来源里只有
// CurseForge 认这个头** —— 给 Modrinth 那发带上它是白发一个别人看不懂的头；
// ② 搜索回来的正文走**对的那个解析器**（CF 的根在 `data`，Modrinth 在 `hits`）；
// ③ 版本列表那道"200 但正文不成形"的闸**按来源分叉**：CF 要 `{"data":[…]}`、
// Modrinth 要数组。只看 isArray() 的话 CF 每一页都会说"版本列表读不懂"；
// ④ 那两条地址（key / override）是**这一次请求读的**，不是构造时读的（用户填完 key
// 回到下载页再点一次搜索就该走新的那条）。
//

void runResourceSourceTests();

// ---------------------------------------------------------------------------
// ZI. 下载页那一行筛子的接线：控件 → 请求地址（本地假 CF 接口，不碰外网）
//
// 要求：「搜索，加入按版本 模组加载器 来源 类型 名称搜索，一个搜索键和一个清空条件键，
// 名字/版本可自行输入，这两个框选中时按回车等同于按搜索键…整合包那一页带一个导入功能」。
//
// 界面那一半（这几个控件长什么样、摆在哪、模组加载器只在模组页有）在 ui_check 那边量过；
// 数据那一半（这几个参数拼出来的地址对不对）由 ZH 那一节量过。**中间这一段** —— 用户在框里
// 挑的那一项有没有真的变成发出去的那一发请求上的参数 —— 这一段要盯住，而它错起来一个错都
// 不报：搜索照发、结果照回，只是筛子根本没生效（用户看到的是"筛了跟没筛一样"）。
void runDownloadFilterTests();

// ---------------------------------------------------------------------------
// ZJ. 资源详情页那一整条路：下载页点某一行 → 整页详情 → 中间那排 chip → 底下点某一版
//
// 要求：「下载页选择了模组以后进入一个独立界面，上方写模组图像 名字 来源，然后有转到
// modrinth/curseforge/mcmod.cn 的选项，中间一排版本，下方排列版本，和pcl类似」。
//
// 五件事：
// ① 箭头那一格点它**不开页**（那一格是展开/收起，判据和代理画箭头是同一个 arrowRect）；
// ② 点行体开的是**这一行**那一页，而且**不会顺手把这一行收起**（三发事件都被吃掉了）；
// ③ 开页只发一发，而且是"整份清单"那一发（地址里**不带** gameVersion = 不筛）；
// ④ 中间那排 chip 是**本地筛**：点一颗一个请求都不发，筛出来的组/条跟着变；
// ⑤ 点某一版 = 走到「装进哪个实例」那条路。这一节故意**不选实例**，该弹的就是那句提醒 ——
// 这是"行上那把钥匙有没有认对"的唯一凭据：钥匙对不上（比如拿行号当 version id）时
// `downloadResourcePageVersion` 会**静静地什么都不做**，不弹框也不报错。
void runResourceDetailPageTests();

// ---------------------------------------------------------------------------
// T19. 实例设置「启动」栏那两颗按钮：导出启动脚本 / 补全文件
//
// 「补全文件」（repairInstanceFiles → VersionManager::repairVersionFiles）要钉住的是它
// 和"装一个版本"那几条路的区别：
// ① 判据是 **sha1**，不是字节数：摆一份"大小一样、内容不对"的 jar，它必须被换回来；
// ② 本来就是好的文件**一个字节都不许动**（"只补缺的和坏的"，不是全量重写）；
// ③ 它**不写 version.json**（那是 downloadVersion 的事，数据层那边有断言，这里只认
// 界面这一半）；
// ④ 期间这个实例从「能启动的实例」里滤掉，结束之后**回来并且被选中**；
// ⑤ 收尾那句话说的是**用户认得的实例名**，不是裸目录名 —— 这一条钉的是
// onVersionDownloadFinished() 里"先清「补全」那一档占用、再 setDownloadUiBusy(false)"
// 那个顺序：反过来（或者顺序颠倒）时它还在过滤名单里，instanceDisplayName() 只能
// 退回目录名，而两句日志/两个框都会跟着说错是谁。
//
// 「导出启动脚本」要钉的是**那一份 .bat 的内容**：它必须和真正启动那一次同源（两边调的
// 是同一个 launchArguments()），离线身份走 --accessToken 0 那一条，而"令牌拿不到"时
// **一份都不导** —— 导一份起不来的脚本比不导更坏（用户以为导出来的能用）。
//
// 远端全用 `file://`（这一节自己那个 remote 目录），一个请求都不出网。
// ---------------------------------------------------------------------------
void runInstanceStartActionsTests();

// ---------------------------------------------------------------------------
// T33. 完整性检查里"从私有槽点下去"的那一半
//
// 闸口那几条为什么是**确定性**的：判「正在体检」的唯一出处是
// `MainWindow::integrityScanningVersion()`，那一位在 `submitIntegrityCheck()` 里
// **同步**写下去；后台那一趟的收尾（清这一位）是 `invokeMethod(QueuedConnection)` 排回
// 事件循环的。所以「点下去之后**不 pump 事件**就调闸口」这中间它一直是真的。
//
// 为什么每一发闸口都配一趟自己的体检：闸口函数会**弹提示**（挡住操作的必须让用户看见），
// 而弹提示是**嵌套事件循环** —— 那一趟里后台那一趟正好跑完（夹具就两个文件），「正在体检」
// 这位就清了。第二发闸口要是不重新点一次体检，它走的就不是闸口那一条、而是**真去删**了。
//
// 全部离线：身份是这一节自己加的离线账号，远端是 `file://`（其实体检**一条请求都不发**，
// 这里造远端只是让 version.json 里那两条 sha1/size 有个说得通的来路）。
// ---------------------------------------------------------------------------
void runIntegrityScanTests();

// T34. 游戏异常退出里"从 onGameFinished 那一路补出来"的那一半
//
// 分三处验：
// ① **那几句话本身**（每一条提示从哪个字段推出来、退出码怎么写成人话）在 verify 的
// ZP 段（纯函数，逐格念一遍）；
// ② **这一节**：真的把 `onGameFinished` 那一槽打进去 → 账本上**多出一项** kind=crash 的
// 失败、标题/目标/那句原因都对得上；而且**以 0 正常退出的那一档一项都不补**；
// ③ **那一项在界面上露什么**（「导出错误报告」「打开崩溃报告目录」两颗键只在它身上露、
// 详情页那两块字）在 ui_check 的 U29 段。
//
// 为什么这一节**不换数据根目录**：账本落在**启动器自带**那一个固定位置
// （`TaskHistory::defaultHistoryPath()` 用的是 `defaultMinecraftDir()`，故意不跟当前
// 游戏文件夹走 —— 见那边的说明）。所以这里只在跑之前数一遍"已经有几条 crash"，之后
// 看**多了几条**：同一个可执行文件里别的节也会往这本账里写东西，绝对值靠不住。
//
// 为什么那一格 Java 要**造一个假的**：`buildCrashRecord()` 报的"这一趟实际用的 Java"
// 是**现读** `<javaPath>/release` 得到的（`probeJavaMajorVersion()` 先读那个文件，
// 读不到才去真起一个 java -version）。造一个只有 `release` 的目录，那一句就是确定的
// —— 整个这一节因此一条外网请求、一个子进程都不发。
// ---------------------------------------------------------------------------
void runGameCrashTests();

// ---------------------------------------------------------------------------
// T35. 世界与实例备份里"从那几个入口点下去"的那一半
//
// 分三处验：
// ① **引擎**（清单怎么读写、目录怎么走、还原之前那份安全快照、还原之后的对账）在 verify
// 的 ZQ 段（真打 zip、真删目录、纯函数那一层，连目录都不换）；
// ② **这一节**：`snapshotInstance()` / `backupWorldNow()` / `startBackupRestore()` 打下去
// → 账本上那一项（`TaskKind::Backup`）的标题 / 目标 / 收尾都对得上 → 盘上真多出一个包
// → 拿产品的读侧把那个包**读回来**核对（该进的进了、能重新下回来的没进去）→ 还原
// 回来内容一字不差、还原前那一份安全快照也真在；
// ③ **那几颗按钮和那个分岔框长什么样**在 ui_check 的 U30 段。
//
// 这一节**换数据根目录**（和 T34 相反）：备份落在 `<当前游戏文件夹>/cache/backups` 底下，
// 本来就是跟着当前游戏文件夹走的（见 AppPaths::backupDir() 那段）。账本那一份反过来挂在
// 启动器自带的固定位置，所以"这一节新补出来的那几项"一律**按号认**，不按位置。
//
// 那一发「创建快照」的分岔框（默认排除 / 完整 / 取消）用 `PromptAnswerer` 答：它按
// **条数**给按钮下标，第一问点第 0 颗 = 默认那一档 —— 要的正是"默认排除能重新
// 下回来的那部分"，所以这一节量的就是默认那一档。
void runBackupTaskTests();

// ---------------------------------------------------------------------------
// T36. 「上一次启动」那张卡：主页左栏那一小块（离线）
//
// 要求的是「一小块最近活动提示，不是一个仪表盘」，所以这一段分三处验：
//
// ① **账怎么写**：`LaunchHistoryController::recordStart()` / `recordFinish()` 是产品唯一的
// 写点（主窗口只剩一个补"启动那一刻的显示名 / 当时是谁"的转手），
// 摆账就摆这两条（`seedLaunchRecordForTest` 那一族全走它们），摆完看盘上那份
// `wtfml.history` 逐格对不对。判据层（格式 / 时间怎么说 / 目标没了怎么降级 / 上限）
// 在 verify 的 ZR 段逐条念过，这里验的是**接线**：产品读的是同一本账。
// ② **卡片上写出什么**：三行字 + 那颗按钮写「继续游玩」还是「启动实例」。
// ③ **点下去走哪条路**：目标还在 ⇒ 快捷启动（完整入口）；目标没了 / 版本不认那两条
// 参数 ⇒ 降级成「把这一份开起来」（= 用户自己在下拉框里选好实例再点「启动游戏」
// 那条路）；那一份实例自己也已经没了 ⇒ **说一句、不启动**。
//
// 全程离线：身份是这一节自己加的离线账号；三个夹具实例的 `wtfml.meta` 都钉着一个
// **不存在的** Java 路径 ⇒ 真点到启动那一步时它停在「这个实例的 Java 用不了」那一问上，
// **一个进程都不起、一个请求都不发**（U 段那套手法的同一份）。
// ---------------------------------------------------------------------------
#if 0
void runHomeRecentCardTests();
#endif

// ---------------------------------------------------------------------------
// T20. 实例设置「文件管理」那八页：真正碰盘的这一半
//
// 那八页**长什么样**在 ui_check 的 U10 段里验（列得对不对、三条开关摆得对不对、
// "用不上"那一页说的是哪句）。这里验的是另一半 —— 目录从哪儿来、改名和删除到底动了
// 盘上的什么。三件事，每件都有一个"不报错但全错"的失败形状：
//
// ① **目录从哪儿来**。四个能下载的类必须走 `ResourceCatalog::folderForKind()`（那是
// 目录名的唯一出处），另外三个写在 `instanceFileDirs()` 里。把 `datapacks` 写成
// `datapack`、或者漏掉一个键，页面上不会报任何错 —— 它只是安静地看着一个**不存在
// 的目录**，用户以为"我一个数据包都没装"。
// ② **「启用 / 禁用」= 改名**：`X.jar` ⇄ `X.jar.disabled`。这里唯一不可接受的失败是
// **撞车时静默盖掉一份**（目标名已经存在还照改，就等于用户手打的那一份没了）。
// ③ **「删除」得先问一句**，而问的时候按「取消」，盘上必须一个字节都没少。
//
// 顺带把 `loadServerList()` 那条映射也验了（它只有三行，但"文件不在盘上"被当成
// problem 递上去的话，用户看到的是「你的服务器列表读不动」而其实只是没有）。
//
// 全部离线：身份是这一节自己加的离线账号，一个请求都不出网。
// ---------------------------------------------------------------------------
void runInstanceFilesPagesTests();

// ---------------------------------------------------------------------------
// T21. 导出整合包：那颗「导出…」按下去之后到底发生了什么（离线，假接口）
//
// 【和 verify 的 V5 段怎么分工】
// V5 钉的是**数据层**：排除表、两张清单的正文、往返解析、zip 的条目名。那些全是纯函数，
// 在那边一条一条钉得住。这一节钉的是**接上去之后才存在**的东西，三件：
//
// ① 设置键 → 数据层那段**接线**。`Launcher/ModrinthApiBase` 在盘上，最后必须变成
// `POST http://127.0.0.1:<port>/v2/version_files`。中间隔着 LauncherSettings →
// syncCurseForgeSettings → VersionManager::m_modrinthApiBaseOverride 三段，任何一段
// 没接上，发出去的就是 api.modrinth.com —— 而**那一发在离线的机器上只会静静超时
// 15 秒**，症状是"导出一个包卡了半分钟、然后说没连上"，谁也不会往"设置没推下去"上想。
// 所以断言里既有"假服务器收到了那一发"，也有请求行里的那个路径。
// ② 那条**继承链**。`gameVersion` 与加载器那一行都是从盘上真实的版本 json 走出来的
// （实例的 json 指着 fabric-loader-…，那一份又指着 1.20.1）。这两格写错了的后果是
// "包看着是好的、别人装不回来"，界面上一个字都看不出来。
// 链末端那个版本号必须是一个**真版本号**：`instanceGameVersion()` 那一格在"在线清单
// 已经拿到"时以清单为准（`isKnownGameVersion()`），编一个 `1.99.9` 出来的话，这一节
// 的结果就取决于启动时那次清单请求到没到 —— 同一份夹具在两种模式下给出两种答案。
// ③ 门口那三个问句和它下面两条岔路：OptiFine（认得出、写不下去）、读不出原版、
// 反查没成要不要继续。它们的形状都是"弹一句、然后**盘上什么都不许留**" ——
// 留半个包比不导出更坏（用户会以为做出来了）。
//
// 假服务器一台罩两条路（`/v2/version_files` 与 `/v1/fingerprints`）：两个地址都是从
// **产品的设置键**推下去的，所以"推没推、推对了没"本身就是被验的东西。
// ---------------------------------------------------------------------------


void runModpackExportTests();

// ---------------------------------------------------------------------------
// T22. 「文件管理」那几页的图标：按文件哈希反查出"这是哪个资源"（离线，假接口）
//
// 要求：「模组和资源包等都有一个图标，把图标显示出来」。这一节钉的是
// **反查那一半**：盘上那个文件名**不是身份**（用户改过名、各家起名不一样、同一个资源
// 两版的名字可以完全不同），能把它和一个资源对上的只有**文件内容** —— sha1 问
// Modrinth（`POST /v2/version_files`）、指纹问 CurseForge（`POST /v1/fingerprints`），
// 再拿项目号去 `POST /v1/mods` 换 `logo` 那两条地址。
//
// 这一节唯一值钱的地方是**发出去的正文是不是真算出来的**：哈希算错了（少读一趟、
// 指纹初值不对、拿文件名去凑），症状是"图标永远是那张占位拼图"，不报任何错、屏幕上
// 也看不出来。所以假服务器**按收到的正文答话**，而断言里对着的是 `ModpackExport::
// hashFile()` 独立算出来的那两份值（它是导出整合包那条路在用的同一份实现）。
//
// 【为什么量到"身份"为止，不量屏幕上那张图】
//
// 【★ 补的那一发：图标地址】
//
// 要求「模组的图标没有正确加载」—— 根子是"Modrinth 那两支反查都给不出图标地址，
// 于是退回 `Hit::iconPngUrl()` 自己拼的 `<id>/icon.png`"，而那条只是**老项目的别名**
// （实测 20 个模组里 11 个回 404）。真正的地址在带哈希的文件名底下，只有接口说得出来，
// 所以现在认出来之后**多发一发** `GET /v2/projects?ids=…`（④b 量它的形状与正文，
// ⑤ 量它自己挂掉时"身份照旧留着、而 problem 不许被它带歪"）。
// ---------------------------------------------------------------------------

void runInstanceIconLookupTests();

// ---------------------------------------------------------------------------
// T23. 服务器那点东西落盘（`cache/servers/<地址 sha1>.json`）
//
// 要求：「测出来的 motd 和图案保留在缓存里」。这一节钉的是**那几个字节
// 来回一趟不走样** —— 一次测速要真的连上去（每台最多几秒超时），服务器列表又是"每次
// 打开实例设置都会看见"的一页；缓存读歪了的症状是"那一页上一次明明问到过、这次又是
// 一片空白"，不报任何错。
//
// 【最值钱的一条：`-1` 那个哨兵】
//
// 【为什么在这里而不是 verify】
//
// `ServerStatusCache.cpp` 只有这一份可执行文件链进来了（它读 AppPaths，而 AppPaths 要
// 一个进程级的数据根目录）。这一节把数据根目录换到一个临时目录，跑完换回来。
// ---------------------------------------------------------------------------

void runServerStatusCacheTests();

// ---------------------------------------------------------------------------
// ZG. 离线账号那张脸：算得出来就用**游戏会给的那一张**，算不出来才退回随机那张
//
// 这一节要的是**真主窗口的那条路**。verify 的 ZF 段已经把"名字 → 哪一张图"立住了
// （纯函数 + 真 JAR），这里验的是它有没有接到屏幕上：选中哪个版本、右栏画的是哪张、
// 底栏那句话说的是哪一档、换版本时会不会跟着换。
//
// 四个假版本各管一条路（都在临时根目录里，跑完随根目录一起消失）：
// 1.20.1-defskin 新规矩那条在 → 算得出来
// 1.18.2-defskin 只有旧版布局那条在 → 换一套布局也算得出来
// 1.19.9-noskin 两条都不在 → **常态**：退回随机那张
// 1.7.10-broken jar 根本不是 zip（第三方安装器的薄壳那种）→ 记一行日志再退回随机
// ---------------------------------------------------------------------------
void runOfflineDefaultSkinTests();

// ---------------------------------------------------------------------------
// AH. 底栏那两块上拉面板：左「正在跑的实例」/ 右「最近登录的三个账户」
// ---------------------------------------------------------------------------
// 这一节的判据全部来自用户 那两句话：
// 「一行一个会话，有时长是好主意，做进去」—— 左面板**一份会话一行**（不是"一个实例
// 一行"：同一个实例开两份就是两行），行首那个方块按这条实例的分类给，行尾两颗按钮
// （电源 = 关掉**这一份**、齿轮 = 进**这一行那个实例**的设置），第二行写它跑了多久；
// 「最后登录的三个账户就拿缓存放（包含离线和正版，总共三个）」—— 右面板读的是账号列表
// 那条缓存（AccountStore::accounts()），按 saved_at 倒序取三个，离线那颗写「选择」、
// 正版写「登录」；而「登录」的意思比"切过去"多一步：**看一眼盘上那张令牌够不够用**
// （剩不到 6 小时就顺手换一张，见 AuthManager::tokenRefreshWindowSecs）。
//
// 【这一节为什么要真起会话，以及"假 java"是什么】
//
// 「一份会话一行」这句话只有真的有两份会话在跑才验得了。真的 JVM 在这儿是个又贵又脆的
// 依赖（机器上未必有、起来了还得等它写日志），所以 javaPath 指到**启动器自己这个 exe**，
// argv 里塞一个 `session-holder`：main() 最前面认这个词就睡够四分钟（见那里的说明）。
// 于是"有一个进程真的在跑"和"跑的是什么"解耦了，而 QProcess 那条路本身是真跑的 ——
// 真起来、真 kill、handleProcessFinished 真收会话。
//
// NOTE: 正因为 holder 就是 mw_check.exe 自己，**这一节的每一份会话都必须停掉**：活着的
// holder 会把这个 exe 文件锁住，下一次构建直接写不进去（而且一锁四分钟）。所以
// 下面每一段都成对（起了几份就停几份），收尾那一段还会再数一遍。
//
// 全程**不联网**：三份会话是本地进程；右面板那三个账号里，两个的令牌够用（Bob 那张到
// 2999 年）、一个的凭据根本换不出新令牌（Carol 那份微软 payload 里**没有 refresh_token**，
// refreshMinecraftToken() 第一道判据就拦下了，一个包都不发）。真要验那条刷新路走到哪儿，
// 那是 mw_check 的一次性探针 auth-live，不是这一节。
void runBottomBarPanelTests();

/// AJ. 启动前那两条提醒（设置页「启动提醒」那一组）。
///
/// 要求：「设置里加两条 在10s内用同一用户启动同一实例时弹出提示：已经有
/// 一个相同的运行中游戏进程了！（小字写 可以在设置中关闭此提示）下方 取消/仍要启动 在启动
/// 同一个实例时（在前面选择仍要启动的情况下/不同账户）弹出进入单人世界可能会丢失进度的
/// 提示…也写明可以关掉，下方 取消/仍要启动」。
///
/// 这一节量三层：
/// · **接线**：点「启动游戏」那一下真的会走到这两问（`onStartButtonClicked()` 里那处调用）；
/// · **拦得住 / 放得行**：选「取消」⇒ 一份新的游戏进程都没起来；选「仍要启动」⇒ 那条路
/// 照旧走到底（这里用**假 java** 真起一份进程来证，和 AG 那一节同一套办法 —— 只看
/// "框收掉了没有"是分不出"放行"和"静静地把启动掐掉"的）；
/// · **两条各自的判据**：十秒窗口的两侧、同不同一个身份、同不同一个实例，以及
/// "① 之后接着问 ②"那个配对（要求要的就是"前后两个框"，不是二选一）。
///
/// 全程离线：身份是一个**离线账号**（不碰令牌、一个请求都不发），实例指着沙盒里的假 java。
void runLaunchGuardTests();

// ---------------------------------------------------------------------------
// T24. 实例设置里**点一条行尾的「详情」** ⇒ 开那个资源自己的那一页（离线，假接口）
//
// 要求：「除了模组以外的几个都是点击模组名称自动进入下载界面（前提是
// 匹配上了，模组的待会再说）」； 又把它挪了个地方：「把点击条目进入对应详情页的
// 功能变成一个详情按钮，放到开关和删除中间，单击改为选中」。这一节走的就是那条路本身，
// 而且**一整条都走**：
//
// 反查那一趟真跑（后台线程 + 假接口）→ 行尾那颗「详情」真的从灰的变亮 → 真的按它一下
// → 实例设置那一页收起来 → 资源详情页真的开出来、标题就是接口给的那个名字。
//
// NOTE: 名字那一格**不是链接**（单击 = 选中这一行）；"这一条是哪个资源"
// 整件事就落在那颗按钮上，所以这一节量的就是那颗按钮。
//
// 【为什么非要端到端，不满足于"两个部件各自对"】
// 这一条路上有**三处**只有接起来才存在的判据，任何一处错了，界面上都是"点一下没反应"：
// ① 身份表（`m_instanceFileIdentities`）有没有被那一趟填上、钥匙（路径 + 大小 + 时间）
// 对不对得上 —— 对不上就查不到那一条是哪个资源；
// ② 收页那一下**必须是 dismiss()**（它经过页面自己的 reject：`closeOverlay()` 不走那条
// 路，页面的收尾和收页钩子都不跑）；
// ③ 开新页那一下**必须排到下一轮事件循环**（这一刻还在这次点击的调用栈里，
// `openResourceDetailPage()` 开头那条 `overlay->isOpen()` 会把这次点击当场吃掉）。
//
// 【模组那一页是**故意**点不动的】
// 用户那句里后半截是「模组的待会再说」⇒ 判据落在 `filesNameOpensResource()` 一处。这一节
// 用一个**已经认出来了的**模组来量它：它的项目号确实被问回来了（下一发里带着它）、
// 图标照旧会出来，可它的名字既没有链接的样子、点下去也什么都不发生。
// 拿一个"本来就没认出来"的文件来量这件事是量不出来的（那种情况下名字点不动是应该的）。

void runInstanceFileResourceTests();

// ---------------------------------------------------------------------------
// T25. 「从实例设置里点『下载新模组』」那条路
//
// 要求：「点击下载新模组/资源包/...时左上角保留一个返回键，返回到实例设置里面，和
// 资源包详情啥的一致」，以及「在设置里加一个选项 …… 勾选->下载新模组就自动放进当前实例
// 文件夹 …… 不勾选->不论是在哪里进入的下载页都弹explorer选择保存位置」。
//
// 这一节钉的是**接线**：界面层那两颗控件长什么样在 ui_check 里量（Z3c / C 两节），
// 「点下去到底发生了什么事」只有把真的 MainWindow 拿起来才量得到 —— 中间任何一段断了，
// 屏幕上的样子都是"按了那颗键没反应"，而那既不像崩溃也不像报错，只是**没动**。
void runDownloadFromInstanceTests();

/// T26. 「Java 管理」那一页：四行 + 两颗动作键 + 那颗「选用」（离线）
///
/// 要求：「把设置里 java 版本换成一个单独的按钮 java 管理…然后里面就直接是
/// Java25：C:\... 重新下载（按钮）/ Java21: ... / Java17：缺失 下载 / Java 8：C:\ 重新下载
/// / 下载所有缺少的Java（按钮） 检测已有java（按钮） 左上角放一个返回键」。
///
/// 这一节**一颗「下载 / 重新下载」都不按**：那四颗按下去是真的往 Adoptium 发请求、几百 MB
/// 同步下完才返回（`GlobalJavaSettings::downloadJavaFromMojang`）。这一节量的是"这一页长
/// 什么样、按哪一颗会走到哪儿"—— 下载那一趟是它自己的事，在这儿按下去这一跑就既不离线、
/// 也不再是几十秒。「检测已有 Java」和「选用」两颗**可以**按：前者只扫盘，后者只写设置。
void runJavaManageTests();

// =========================================================================
// T27. 「复制实例」和「存档」那一页那三条线
//
// 要求：「实例设置的概览里面加一条复制实例，点击以后让用户选择复制到的文件夹，然后
// 给定复制后的实例名称 / 在实例设置里面存档那一栏每个世界都显示存档大小和最后游玩时间，
// 右边画一支笔意为编辑，和基岩版一样，顶端加个导入世界按钮和批量编辑按钮（这个先做占位）」。
//
// 这一节验的是**接线**：那颗按钮在不在、点下去真的动了盘没有、那一页发出来的三发信号是不是
// 接到了 MainWindow 上（接到之后**屏幕上真的出现东西**）。判据本身（拷树的规矩、重名的
// 挡法、名字的校验）在 verify 的 ZJ 节里，那一节是纯值的。
//
// NOTE: 两颗原生选择框（"复制到哪个游戏文件夹"、"要导入哪个世界"）在测试里**驱动不了**
// —— 那一步在选中之前根本进不去（一个模态的原生窗口），所以：
// · 「复制实例」走**动手的那一半** `copyInstanceTo()`（收三个字符串，见 .hpp）；
// · 「导入世界」只验它动手的那一半 `importWorldInto()`，**那一颗按钮本身不点**
// （点了就卡在原生框上）。这一点和产品里另外四处 QFileDialog 是同一个待遇。
// =========================================================================
void runCopyAndSavePageTests();

// =========================================================================
// T44. 管理页右侧那张实例树：**打开时不许停在最底下**
//
// 症状是"每次进管理页，右边那列已经滚到底了"。真因不是选中项挑错：syncTreeToCombo()
// 选的是当前实例，Qt 会滚到它那儿；开机那一趟树还藏着，这一滚被攒到第一次显示、
// 排完版才补（藏起来时 range 还是 0，挡在 setCurrentItem() 周围的 scrollTo() 拦不住）。
// 所以修法只能是"记下重建前的位置、排版结束后放回去"，这一节就钉那个机制的三个面：
// Qt 的行为还在不在、放回去之后选中项丢没丢、树露着的那一趟是不是当场放回。
// =========================================================================
void runInstanceTreeScrollTests();

// =========================================================================
// T28. 文件管理那几页顶上那条搜索 + 按钮下面那栏状态里的「可更新」
//
// 要求：「模组等的下载新/刷新/打开文件夹的**上方**加一个搜索，三个按钮的**下方**一个
// 选择 全部/启用/禁用/可更新」。
//
// 【和 ui_check 的 U10P 怎么分工】
// U10P 量的是**屏幕上那一半**：两排控件在不在、摆在按钮的哪一边（按 y 坐标量）、筛得对不对、
// 「筛完一条不剩」和「目录本来就是空的」是不是两个状态。这一节量的是**那一格「可更新」的
// 数据从哪来** —— MainWindow 那趟检测查出来的结果，除了弹一份报告，还得**留下来**并喂给那几页：
// * 不留：关掉设置页再进来，那一格永远挂着"还没有查过更新"，而那一趟刚刚才跑完；
// * 不喂：筛出来是空的，用户以为"这个实例干净得很"。
// 两样在屏幕上的样子都**不像坏**（不崩、不报错、连日志都正常），所以只有把真的 MainWindow
// 立起来才量得到 —— 而这也正是"检测结果只活在那一个框里"这件事唯一露馅的地方。
void runInstanceUpdateFilterTests();

// =========================================================================
// T32. 更新检测：**一个文件只查一家** + 两家都认出来时退回问 CurseForge
//
// 两条都坏的时候屏幕上**不像坏**：弹一份报告、里头几条「说不清」，和"网络不好"一个样子 ——
// 而 429 那条真因（"一次问得太多"）更是一个字都露不出来。所以只有把真的 MainWindow 立起来、
// 让那一趟跑完再读**它发出去的那几发请求**，才量得到。
//
// 夹具：两个模组文件，一个**两家都认得出**（Modrinth 有哈希表 + CurseForge 有指纹），
// 一个**只有 CurseForge 认得出**。批量那一发对第一个故意**什么都不答**（`{}`）——
// 这正是真机上"这个哈希在 Modrinth 的表里没有"的样子。
// =========================================================================
void runUpdateCheckSourceFallbackTests();

// =========================================================================
// T32b. 同一个项目底下的**两个文件**，批量那一发只答得出其中一个
//
// 夹具：两个**不同内容**的文件（sha1 / 指纹都不同），反查后都指向**同一个 CurseForge
// 项目**；批量那一发只给 A 那个 sha1 答案（B 那个哈希"这一家没有"），CurseForge 的
// 版本列表给的正是 B 那一份 ⇒ 两条都该有结论，而且**各只有一条**。
// =========================================================================
void runUpdateCheckSameProjectFallbackTests();

/// 按 objectName 找**最新那一代**控件（同一个名字下面同时躺着好几代的时候取最后一个）。
///
/// 为什么需要它：更新中心那一页重喂走的是 `deleteLater()`，而**从嵌套事件循环里发出的
/// 延迟删除要等那一层退出去才真的派发**（Qt 的规矩：延迟删除事件带着发出时的循环层号，
/// 只有回到那一层才放它走）。所以这一页开着的整段时间里，同一个 objectName 底下会同时
/// 躺着好几代控件 —— 取第一个读到的是上一代（还写着「可更新到 2.0」），断言会红在一个
/// 假象上。新控件是后 append 进父对象那张孩子表的，所以**最后一个**就是最新那一代。
template <typename T> T *newestByObjectName(QObject *parent, const QString &name) {
    const QList<T *> all = parent->findChildren<T *>(name);
    return all.isEmpty() ? nullptr : all.constLast();
}

// =========================================================================
// T38. 「更新中心」上那几颗动作
//
// · 「忽略这一版」→ 账本（`<数据目录>/wtfml.updates`）里记下"要换上的**那一版**的 id"，
// 那一行当场写成「已忽略这一版（2.0）」、那一颗翻成「不再忽略」；
// · 「不再忽略」/「不再固定」→ 那一笔记录清掉、那一行回到「可更新到 2.0」；
// · 「固定这一版」→ 记的是**盘上那一份的内容 sha1**（不是版本号），那一行写「已固定」；
// · 那一行自己的「更新」→ **先摆回滚点**再动手。这一批我们**故意让它失败**（那一版给的
// 地址指向假服务器上一个不存在的路径）⇒ 正好量到"失败了能退回去"这条承诺：失败框里
// **逐条**说着每个资源的结果、更新中心自己又开回来了、底下那颗「撤销上一次更新」露着；
// · 「撤销上一次更新」→ 问一句 → 把文件退回更新前那一份 → **用掉**那份回滚点。
//
// 这几段任何一处断了，屏幕上的样子都是"按了没反应"或者"那句话没变" —— 既不像崩溃也不像
// 报错。只有把真的 MainWindow 立起来、真按那几颗 chip、真读盘才量得到。
//
// 走法上有一处**不是随手写的**：这一页开出来之后调用方早就返回了（`showPage()`），而上面
// 那些动作全在页面上发生（"收页"是 reject、下一件事由 MainWindow 排到下一轮）。所以这里用
// 一台 10 毫秒的自走定时器按阶段往下推，每一阶段**先等到该看到的样子再动手** —— 谁先谁后
// 由产品那边决定，不由测试决定（"点完就往下走"的量法会读到上一代控件，见
// `newestByObjectName()` 那段）。外面那一层只 `pumpUntil` 到最后一阶段为止（和 T28/T32
// 是同一个形状）。
// =========================================================================
void runUpdateCenterActionTests();

void runThemeImageRowTests();
void runFileDragIntentTests();

/// T39. 设置页「主题」那一节：控件 → 主题规格 → 盘。
///
/// 两处**故意不点**（底下是覆盖层 / 原生框，测试里点下去就是卡住）：
/// · 那三块色板（主题色 + 四个语义色）—— 底下是 `InlineMessage::pickColor()`；
/// · 「导出 / 导入」—— 底下是 `QFileDialog`。
/// 「挑完一个色之后」那一态照产品那两步搭出来：直接改规格 + 按名字调那一发重画
/// （产品里"挑完色"走的正是 `applyThemeSpec()` 那条路）。
void runThemeControlsTests();

/// 把一个本地文件"拖"到某个控件上（真发那一串事件：`DragEnter` 然后 `Drop`）。
///
/// 走的是**真的那几发事件**（`QApplication::sendEvent`），不是直接调那个槽：这条路上要
/// 验的恰恰有"这一页/这个窗到底收不收拖放"（`acceptDrops()` 那一格），绕过事件派发就
/// 等于把准入判据一起绕过去了。
///
/// 两发**必须成对**发，这是 Qt 自己的规矩，不是这里的规定：`QApplication::notify()` 里
/// `Drop`/`DragMove`/`DragLeave` 那一段**根本不看接收者是谁** —— 它把事件交给
/// `QDragManager::self()->currentTarget()`，而那个 `currentTarget` **只有**被接受的那一发
/// `DragEnter` 才会设上（见 `case QEvent::DragEnter` 里那句 `setCurrentTarget(w)`）；一个
/// 没有 currentTarget 的 `Drop` 会被**直接扔掉**（那一段开头的 `else break;`）。
/// 于是"只发 Drop"这件事看起来像发了，其实什么都没发生 —— 更坏的情况是**跑去别人身上**：
/// 上一发被接受的 `DragEnter` 还记着它，这一发 Drop 就落到**上一个控件**头上了。
/// 实测踩到过：T30.3 的 Drop 被 T30.1 留下的 currentTarget 收走，落进
/// `MainWindow::dropEvent`（症状是"这一页一个字都没收到，主窗那张「装到哪个实例」的
/// 列表倒弹出来了"）。
///
/// NOTE: 返回值是 `isAccepted()` —— 收没收，由控件自己说（`dragEnterEvent`/`dropEvent`
/// 里那一句 `acceptProposedAction()`），不是我们猜的。准入那一发就没过的话，真机上
/// 也不会有 Drop 这一发，所以直接返回 false（不硬塞一发进去）。
static bool dropFilesOn(QWidget *target, const QStringList &paths, bool enterOnly = false) {
    if (target == nullptr) {
        return false;
    }
    QMimeData mime;
    QList<QUrl> urls;
    for (const QString &path : paths) {
        urls.append(QUrl::fromLocalFile(path));
    }
    mime.setUrls(urls);
    const QPoint pos(target->width() / 2, target->height() / 2);
    const QPointF posF(pos);
    QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    if (enterOnly || !enter.isAccepted()) {
        return enter.isAccepted();
    }
    QDropEvent event(posF, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
    return event.isAccepted();
}

/// T30. 拖进来的文件：认类型 → 问不问 → 落到哪个目录 → 哪个实例。
///
/// 要求：「打开某个实例设置的时候如果拖入.jar/.zip之类的文件，检测是整合包还是资源包
/// 等，如果是模组且正好在模组设置那一页，那就直接塞进去，不是对应页面就弹窗提示：将在此
/// 实例中安装 资源包/...（判断出的文件类型），是否继续；…在一些其它的地方拖入模组等时
/// 选择要安装在的目标实例」。
///
/// 这一节量的是**接线与落地**（认类型那半在 verify 的 V5z 里，喂的是真压缩包）：
/// · 主窗自己收不收拖放；
/// · 「正好在那一页」⇒ 一句话不问、文件真的进了 `mods/`；
/// · 不在那一页 ⇒ 弹的是**那一句原话**，答"不装"⇒ 盘上什么都不多；
/// · 认不出来的东西 ⇒ 一句人话，别静默；
/// · 同名文件 ⇒ 问一句，答"不盖"⇒ 旧那份一个字节没动；
/// · 没开实例设置时 ⇒ 问"装到哪个实例"，而"一行都没选中"**不算**选了第一个。
///
/// **一个原生框都不点**：整合包那一路第一步是 `QFileDialog::getExistingDirectory`
/// （选游戏文件夹），测试里点下去就是卡死 —— 所以这一节不拖整合包（那两条路
/// ——"离开实例设置"和"选文件夹"—— 在别的节里量过它们的零件）。
void runDroppedFileTests();

// ---------------------------------------------------------------------------
// T31. 前置依赖弹窗 + 从实例点进详情页再下载（落进**那一个**实例）
// + 批量启用 / 禁用 / 删除真改盘
//
// 要求（这一节两件事的出处）：
// · 「点击下载模组时检测当前实例里面是否存在所有的前置，如果有缺失的前置，弹出此模组
// 缺失必要的前置：（模组名称，一行一个…）」+「弹窗里灰字写可以在设置里面关掉弹窗」；
// · 「在模组/资源包等的设置里面把点击条目进入对应详情页的功能变成一个详情按钮…在选中
// 任意数量模组时在列表下方的位置显示几个按钮：更新 禁用 启用 全选 反选 取消 删除」。
//
// 这一节量的是**屏幕上的字和盘上的字节**，不是"接线接上了没有"：
// · 弹窗那三样（标题 / 正文 / 灰字）一律取判据层 `ModDependencies::*Text()` 逐字比，
// 测试里不另写一句 —— 写一句就等于把"唯一出处"这条规矩在测试里破一个口子；
// · 「关掉之后什么都不装」的判据是**那条前置查询压根没发**（`/v2/projects?ids=DEPPROJB`）
// + **前置那份文件没被要求下**（`/dep-b.jar`）—— 那是"要不要一起下"那条路上
// 唯一一次、也是第一次问出这个问题的地方，它没发出去就说明整段被跳过了；
// · 「落进那一个实例」的判据是产品自己写的那行日志里带着**这个实例的 mods 目录**。
//
// 【为什么整条路要点两次「详情」】
//
// 这一节的两趟（开关开着 / 关掉）走的是**同一条**用户路（实例设置 → 模组 → 详情 →
// 点某一版），所以写成同一个 lambda 跑两遍。写成两份的话，第二趟迟早会和第一趟长歪。
// ---------------------------------------------------------------------------

/// 在树的行体上按一下（按下 + 松开两发，**不**顺手泵事件）。
///
/// 和 ZJ/T24 那几段里的 `clickRow` 是同一个手法（产品认"行上点一下"走的是
/// `RowActivation` 那层事件过滤器，所以两发都要发）。**不泵事件**是有意的：产品的
/// "开新页"是排到下一轮才落地的，泵的那一下交给调用方，不然顺序就反了。
static void clickDetailTreeRow(QTreeWidget *tree, const QPoint &pos) {
    QWidget *viewport = tree->viewport();
    const QPointF local(pos);
    const QPointF global(viewport->mapToGlobal(pos));
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(viewport, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(viewport, &release);
}

void runDependencyAndBatchTests();

// =========================================================================
// T41. 服务端「跑起来 / 停下来」（M4-3）
//
// 那一层叫 `ServerRun::ServerProcess` —— 它管一条真进程的整条命：起、读它的 stdout、
// 往它的 stdin 写、正常停、宽限到点强杀。这条路上任何一处断了，屏幕上的样子都是
// 「点了没反应」或者「那行字一直是启动中」，既不像崩溃也不像报错。
//
// 这一节把"服务端"换成了**这个 exe 自己**（`--fake-server`，见 `main()` 最前面那一支）：
// 真服务端要几个 GB 的下载 + 联网，在这个工程里是个又贵又脆的依赖；而 `ServerProcess`
// 本来就不认识 java —— 它只认识"一个会吐行、会读行的进程"。所以这里量得到的东西
// （状态怎么走、日志怎么切行、`stop` 写进去了没有、宽限到点强杀）和在真机上量到的
// 是同一件事。
//
// 这一节**不造 MainWindow、不碰数据根目录**：这一层只看它拿到的那几个入参（工作目录
// 只是 `QProcess` 的 cwd），盘上什么都不写。所以它可以排在哪儿都行 —— 放在最后只是
// 图个整齐。
//
// 判据那一半（`-Xmx` 插到第几位、`Done (` 认哪一行、退出码算哪一档状态、宽限时间那张表）
// 在 verify 的 ZZ 一节里钉死，这一节不重复量 —— 这里量的是**它们真的被用上了**。
// =========================================================================
void runServerProcessTests();

// =========================================================================
// T42. 「服务端设置」那一整页（M4-4）
//
// 这一节量的是**只能在这一层说得清**的事：那一页经真 `MainWindow` 开到内容区上之后，
// 按下去的那几发到底有没有落到盘上 —— 表单改一格 → 保存 → 盘上那份 `server.properties`
// 只动了那一行（别的行一个字节都不碰）；切到「原始文本」时表单的补丁**已经合进那份文本**
// （两页看到的不是两份东西）；文本框里改一行 → 保存 → 整份写回去；「立即备份」→ 盘上真
// 多一个 zip；体检单里那些条目是 `ServerCheck` 现算的。
void runServerSettingsPageTests();

// =========================================================================
// T43. 关窗那一刻：worker 线程 / 子进程 / 还在飞的下载各自怎么收场
//
// 那条问的是「关掉启动器的时候，后台那些活分别怎么收尾」。这一段钉的就是它们：
// 这几条路各自都能把"关个窗"变成"崩一下"，或者"把用户的东西弄丢"：
//
// · **worker 线程**：`MainWindow` 手上那几趟后台活（体检 / 备份 / 图标反查 / 检查更新）
// 都是 `QThread::create()` + `setParent(this)` 出来的。线程还在跑、窗口先散 ⇒ Qt 直接
// `qFatal("QThread: Destroyed while thread is still running")`，也就是 abort。
// 体检和备份那两趟在析构里会被等（那两段写着 `~MainWindow()` 里），**图标反查和
// 检查更新那两趟没人等** —— T43.3 给它们摆了钉子。
// · **子进程（游戏）**：关窗**不许杀**它（用户那句「关掉启动器游戏继续跑」）。窗口一关，
// `LauncherManager` 就跟着析构，而 `QProcess` 的析构里第一件事就是 `kill()` ——
// 所以那一问走完必须先 `detachRunningGame()` 把进程**交出去**。T43.2 量到的是"窗口连
// 对象都拆干净了，那个进程还活在系统里"。
// · **还在飞的下载**：那批 `QNetworkReply` 随窗口一起散，不许崩（T43.4）。
//
// 全程离线：唯一的"网络"是本机两个**接了不答话**的假端口（下面 `StallingApi` 那个跑在
// 自己的线程里）—— 不然那一趟一秒内就跑完，"关窗时它还在跑"这个现场根本摆不出来。
// =========================================================================


/// 按 PID 问操作系统要一次「这个进程还在吗」。
///
/// 判据只能**从外面看**：我们自己手上那份 `QProcess` 在 `detachRunningGame()` 之后就
/// **故意不管它了**（那一段写得很清楚 —— 释放那些壳子等于执行析构、等于把游戏强杀），
/// 所以"它还活着"这件事没有别的问法。
inline bool processAlive(qint64 pid) {
    if (pid <= 0) {
        return false;
    }
    HANDLE handle =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(handle, &code) != 0 && code == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
}

/// 收拾干净：这一节起的那个替身进程**必须**在节尾走掉 —— 它攥着 `mw_check.exe` 这个文件
/// （真机上就是"残留 exe 占住文件、重新链接 Permission denied"那条）。
inline void killProcess(qint64 pid) {
    if (pid <= 0) {
        return;
    }
    HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return;
    }
    TerminateProcess(handle, 0);
    CloseHandle(handle);
}

void runShutdownTests();


int MwHighSpeedDownloadCheck(QApplication &app);
int MwDownloadRetryCheck();
int MwActivitySelectionCheck();
int MwWorkspaceCheck(QApplication &app);
int MwPlayerSkinChecks(MainWindow &window);
int MwHomeQuickCollapseChecks(MainWindow &window);
int MwJavaPreferencesChecks(const bool settingsWired, MainWindow &window, QPushButton * &javaManageButton, QLabel * &javaLabel, QCheckBox * &javaWarn, QComboBox * &memoryMode, QLabel * &memoryHint, QSpinBox * &maxSpin, QComboBox * &behaviourCombo, QComboBox * &sourceCombo, QPlainTextEdit * &jvmEdit, MemorySlider * &maxSlider);
int MwMemoryPreferencesChecks(const bool settingsWired, const QString &kDecorated, MainWindow &window, QComboBox * &installed, QComboBox * &memoryMode, QLabel * &memoryHint, QSpinBox * &maxSpin, QComboBox * &behaviourCombo, QComboBox * &sourceCombo, QPlainTextEdit * &jvmEdit, SettingsParts::CommitOnFocusOut * &applyJvm, MemorySlider * &maxSlider);
int MwJavaSelectionChecks(const QDir &root);
int MwFolderSwitchingChecks(const QDir &root, MainWindow &window);
int MwLoginCompletionChecks();
int MwRenameStateChecks(const QDir &versionsDir);
int MwInstanceRenameChecks(const QDir &versionsDir, MainWindow &window);
int MwInstanceGroupsChecks(const QDir &root, const QDir &versionsDir);
int MwInstanceFiltersChecks();
int MwServerCreationChecks(const QDir &root);
int MwIntegrityActionsChecks(const QDir &root);
int MwInstanceOverviewChecks(const QDir &root, const QDir &versionsDir);
int MwOverridePersistenceChecks(const QDir &root, const QString &settingsDir, const QDir &versionsDir, QSpinBox * &maxSpin, MemorySlider * &maxSlider);
int MwRunStateLifecycleChecks(const QDir &root, const QDir &versionsDir);
int MwInstalledVersionsChecks(const QDir &versionsDir, MainWindow &window, QStackedWidget * &gameStack, QLineEdit * &installNameEdit, QLabel * &installNameError, QPushButton * &installBtn, QPushButton * &installBackBtn, GlobalJavaSettings * &windowJava, const std::function<void(const QString &)> &pinGlobalJava, const std::function<void()> &unpinGlobalJava);
int MwDownloadNavigationChecks(MainWindow &window);
int MwVersionActivationChecks(MainWindow &window, QTreeWidget * &versionTree, QStackedWidget * &gameStack, QLineEdit * &installNameEdit, QLabel * &installTargetLabel, QPushButton * &installBackBtn);
int MwRapidRowActivationChecks(MainWindow &window, QTreeWidget * &versionTree, QStackedWidget * &gameStack, QLineEdit * &installNameEdit, QLabel * &installTargetLabel, QPushButton * &installBackBtn);
int MwAccountAppearanceChecks();
int MwModpackImportChecks(const QDir &versionsDir, MainWindow &window, GlobalJavaSettings * &windowJava, const std::function<void(const QString &)> &pinGlobalJava, const std::function<void()> &unpinGlobalJava);
int MwTaskSummaryChecks(MainWindow &window);
int MwTaskStressChecks(MainWindow &window);
int MwGameLogLifecycleChecks(const bool settingsWired, MainWindow &window, QCheckBox * &showLogCheck, QComboBox * &priorityCombo, QSpinBox * &windowWidthSpin, QSpinBox * &windowHeightSpin, QComboBox * &windowModeCombo, QLabel * &windowSizeTimes);



/// 智能体页 + 假模型客户端的真主窗口检查（专门模块 mw_check_agent）。
int MwAgentChecks(QApplication &app);
