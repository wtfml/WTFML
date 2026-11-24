/**
 * @file OverlayHost.cpp
 * @brief 实现窗口内覆盖层。
 * @note 页面生命周期和动画约定见 OverlayHost.hpp。
 */

#include "OverlayHost.hpp"
#include "Animations.hpp"
#include <QElapsedTimer>  // 整块覆盖这一层的淡入
#include "Theme.hpp"   // 顶上那颗「‹ 返回」的身份（文字按钮）
#include "Glass.hpp"   // 整页那一档透出整窗磨砂
#include "GlassPopover.hpp"
#include "VersionIcons.hpp"

#include <QApplication>
#include <QDebug>
#include <QDialog>
#include <QEvent>
#include <QEventLoop>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace {

/// 把控件"现在长什么样"抓成一张图 —— **不用 `grab()`**。
QPixmap snapshot(QWidget *widget) {
    QPixmap pixmap(widget->size());
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    // 只画控件自己和子控件，**不铺窗口底色**（默认的 DrawWindowBackground 会先刷一整块
    // Window 色 —— 弹窗一开内容区就成了一块不透明的黑/灰，把磨砂风景整个盖掉）。
    widget->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    return pixmap;
}

/// 提示开着时盖在整个窗口上的那层薄纱，只画「宿主那块以外」（宿主自己会压暗）。
class ScrimVeil : public QWidget {
public:
    ScrimVeil(QWidget *parent, QWidget *hole) : QWidget(parent), m_hole(hole) {
        setObjectName(QStringLiteral("overlayScrimVeil"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setClipRegion(QRegion(rect()).subtracted(m_hole->geometry()));
        painter.fillRect(rect(), OverlayHost::scrimColor());
    }

private:
    QWidget *m_hole;
};

/// 页眉那一行的左右留白、以及内容四周留的那一圈。
/// NOTE: 和 `MainWindowUI` 里页面用的那圈边距是**同一个数**（kPageMargin = 9）——
/// 覆盖层是一整个"页面"，留白和别的页面不一样的话，进设置再退出来会看着跳一下。
constexpr int kOverlayPageMargin = 9;
/// 整页那一档的左右留白：和主页、设置页正文左缘对齐（设计稿内容区左右 40）。
constexpr int kOverlayPageSide = 40;
/// 「这一页收掉之后由主人 deleteLater()」那个属性名（见 OverlayHost::disposesOnCloseProperty()）。
constexpr const char *kDisposesOnCloseProperty = "wtfmlOverlayDisposesOnClose";

/// 卡片（整页里那张 / 小提示那张）那一块布的调色板：底出厂是 `Base`（用户单独改过就是那个）。
QPalette cardChromePalette() {
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, Theme::partBackground(Theme::Part::Card));
    return palette;
}

}  // namespace

OverlayHost::OverlayHost(QWidget *anchor, QWidget *parent) : QWidget(parent), m_anchor(anchor) {
    setObjectName(overlayHostName());
    // **不进任何布局**：一进布局就会把锚点挤走（那是"分成上下两半"，不是"浮在上面"）。
    // 几何全靠 relayout()。所以这一条要是被谁删了，症状是"覆盖层一开，页面被压扁"。
    setVisible(false);

    // 还回来的内容先寄存在这儿（见 closeOverlay 的 NOTE）。
    m_parking = new QWidget(this);
    m_parking->setObjectName(QStringLiteral("overlayParking"));
    m_parking->setVisible(false);
    if (parent != nullptr) {
        m_veil = new ScrimVeil(parent, this);
    }

    // ---------- 页眉（常驻：返回箭头 + 标题） ----------
    m_pageFrame = new QWidget(this);
    m_pageLayout = new QVBoxLayout(m_pageFrame);
    m_pageLayout->setContentsMargins(0, 0, 0, 0);
    m_pageLayout->setSpacing(0);

    QWidget *header = new QWidget(m_pageFrame);
    header->setFixedHeight(pageHeaderHeight());
    QHBoxLayout *headerLayout = new QHBoxLayout(header);
    // 面包屑（tokens-extra §1）：「‹ 返回」文字按钮 + 「/」+ 当前页，左缘比正文多出按钮内边距。
    headerLayout->setContentsMargins(kOverlayPageSide - 10, 6, kOverlayPageSide, 0);
    // 间距 0：「返回」右侧那 8px 是按钮自己的内边距（悬停底要留着），「/」右边再补 8 与之对称
    // （原先布局间距 4 叠在内边距上，返回到斜杠 12、斜杠到标题 4，r104 看着左空右挤）。
    headerLayout->setSpacing(0);
    m_backButton = new QPushButton(backButtonText(), header);
    m_backButton->setObjectName(overlayBackButtonName());
    m_backButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_backButton->setToolTip(tr("未保存的改动会丢失。"));
    // 「← 返回」= 往回走（一般性那一档）—— 全应用每一页顶上那一颗都是它。
    Theme::markButton(m_backButton, Theme::ButtonKind::Ghost);
    m_backButton->setProperty("wtfmlCrumb", true);
    m_backButton->setFixedHeight(28);
    const auto crumbFont = [](QWidget *w) {
        QFont f = w->font();
        f.setPixelSize(13);
        f.setBold(false);
        w->setFont(f);
    };
    crumbFont(m_backButton);
    const auto backIcon = [this]() {
        m_backButton->setIcon(QIcon(VersionIcons::line(QStringLiteral("chevL"), 14, Theme::tokens().secondary, 2.0)));
    };
    m_backButton->setIconSize(QSize(14, 14));
    backIcon();
    Theme::follow(m_backButton, backIcon);
    headerLayout->addWidget(m_backButton);
    auto *slash = new QLabel(QStringLiteral("/"), header);
    crumbFont(slash);
    slash->setForegroundRole(QPalette::PlaceholderText);
    slash->setContentsMargins(0, 0, 8, 0);
    headerLayout->addWidget(slash);
    m_titleLabel = new QLabel(header);
    m_titleLabel->setObjectName(overlayTitleLabelName());
    crumbFont(m_titleLabel);
    headerLayout->addWidget(m_titleLabel);
    headerLayout->addStretch(1);
    m_pageLayout->addWidget(header);

    m_pageContent = new QWidget(m_pageFrame);
    m_pageContent->setObjectName(overlayPageContentName());
    m_pageContentLayout = new QVBoxLayout(m_pageContent);
    // 留白和别的页面一样是那一圈 9 px（见 kOverlayPageMargin）。
    m_pageContentLayout->setContentsMargins(kOverlayPageSide, 4, kOverlayPageSide, 0);
    m_pageContentLayout->setSpacing(0);

    // 整页那一档外面套一层滚动区。**不是"以防万一"**：设置页是按它自己的尺寸设计的
    // （实例设置 820x560），而内容区多大是主窗口说了算 —— 窗口一小，页脚那排
    // 「取消 / 确定」就被切在下面，用户怎么点都点不着，而那正是"这个页面打不开"。
    // 外面套一层，任何窗口尺寸下它都能滚到底。
    auto *pageScroll = new Theme::GutterScrollArea(m_pageFrame);
    pageScroll->setObjectName(overlayPageScrollName());
    pageScroll->setWidgetResizable(true);
    pageScroll->setFrameShape(QFrame::NoFrame);
    pageScroll->setWidget(m_pageContent);
    m_pageScroll = pageScroll;
    m_pageLayout->addWidget(pageScroll, 1);
    m_pageFrame->setVisible(false);
    // 内容变了要自己催滚动条重算，见 eventFilter 里 m_pageContent 那一段。
    m_pageContent->installEventFilter(this);

    // 内容塞进来 / 拿走的时候卡片得重新量一次高矮。**必须有这一条**：`openPrompt()` 只把
    // 宿主摆好，内容是在它**返回之后**才 addWidget 进去的（调用点如此，`execEmbedded`
    // 也如此），那会儿卡片还按"空内容"算过一次高。没有这一条的症状是"提示框的高度只够
    // 装下第一行，剩下的字被裁掉"。
    m_promptContent = new QWidget(this);
    m_promptContent->setObjectName(overlayPromptContentName());
    m_promptContentLayout = new QVBoxLayout(m_promptContent);
    m_promptContentLayout->setContentsMargins(0, 0, 0, 0);
    m_promptContentLayout->setSpacing(0);
    m_promptContent->installEventFilter(this);

    // ---------- 提示那块的卡片（常驻，里面装着上面那个内容容器） ----------
    // 卡片自己画底和边：这样"嵌进来的对话框"和"我们自己搭的提示面板"看起来是同一张卡，
    // 不会一个有线一个没线。
    QFrame *card = new QFrame(this);
    card->setObjectName(overlayPromptHostName());
    // 卡片自己不画：底（玻璃面 + 1px 亮边）由覆盖层在 paintEvent 里画，圆角才干净。
    card->setFrameShape(QFrame::NoFrame);
    card->setAutoFillBackground(false);
    // 卡片底色出厂是 `Base`（和输入框/列表里那层一样），归
    // `Theme::Part::Card` 管（用户能把卡片单独换一个色）。
    // NOTE: 基色取**应用调色板**而不是卡片自己的：卡片身上就写着算好的那一份，
    // 第二次进来再取一次就是"在改过的基色上再算一遍"（TitleBar 那边同一个坑）。
    card->setPalette(cardChromePalette());
    // 卡片是**常驻**的（构造里建一次、跨提示复用）⇒ 主题一换得自己重算一遍。
    Theme::follow(card, [card]() { card->setPalette(cardChromePalette()); });
    m_promptLayout = new QVBoxLayout(card);
    // 卡片自己留 18 px，内容容器**不再留**（两圈叠起来的话内容就被挤窄，而"提示有多宽"
    // 是有唯一出处的（promptContentWidth()），挤一下就不守恒了）。
    m_promptLayout->setContentsMargins(promptPadding(), promptPadding(), promptPadding(),
                                       promptPadding());
    m_promptLayout->setSpacing(0);
    m_promptLayout->addWidget(m_promptContent, 1);
    card->setVisible(false);
    m_promptCard = card;
    // 卡片自己也被盯上：里面那些子控件的 sizeHint 是**晚一步**才定的（标签钉了宽度、
    // 但它自己的高度要等尺寸约束算完），那一步会以 LayoutRequest 的形状落到卡片身上。
    card->installEventFilter(this);

    connect(m_backButton, &QPushButton::clicked, this, [this] { triggerBack(); });

    if (m_anchor != nullptr) {
        // 锚点一动（窗口缩放、标签栏换行、页面换掉）几何就得重算 —— 覆盖层不归布局管，
        // 没人会替它做这件事。
        m_anchor->installEventFilter(this);
        if (auto *tabs = qobject_cast<QTabWidget *>(m_anchor)) {
            // 翻页了：整页那一档要么"摆出来"要么"收着"（见头文件那段），提示那一档换底图。
            connect(tabs, &QTabWidget::currentChanged, this,
                    [this](int) { onAnchorTabChanged(); });
        }
    }
}

QRect OverlayHost::contentRect() const {
    QWidget *host = parentWidget();
    if (m_anchor == nullptr || host == nullptr) {
        return QRect();
    }
    // 内容区 = 锚点里**当前那一页**占的那块。取页控件自己的几何是公开 API ——
    // 不去掏 QTabWidget 内部那个堆叠控件的名字（`qt_tabwidget_stackedwidget` 是实现
    // 细节，抄它等于把 Qt 升级的风险引进界面布局）。
    if (auto *tabs = qobject_cast<QTabWidget *>(m_anchor)) {
        if (QWidget *page = tabs->currentWidget()) {
            return QRect(page->mapTo(host, QPoint(0, 0)), page->size());
        }
        // 一页都还没加（构造中途）：先按整个锚点算，第一次 relayout 就纠回来了。
    }
    return QRect(m_anchor->mapTo(host, QPoint(0, 0)), m_anchor->size());
}

OverlayHost *OverlayHost::forWidget(QWidget *widget) {
    for (QWidget *w = widget; w != nullptr; w = w->parentWidget()) {
        if (auto *host = qobject_cast<OverlayHost *>(w)) {
            return host;
        }
    }
    return nullptr;
}

QWidget *OverlayHost::coveredPage() const {
    if (auto *tabs = qobject_cast<QTabWidget *>(m_anchor)) {
        return tabs->currentWidget();
    }
    return nullptr;
}

void OverlayHost::releaseCoveredPage() {
    if (m_disabledPage != nullptr) {
        m_disabledPage->setEnabled(true);
        m_disabledPage = nullptr;
    }
}

int OverlayHost::anchorTabIndex() const {
    if (auto *tabs = qobject_cast<QTabWidget *>(m_anchor)) {
        return tabs->currentIndex();
    }
    return -1;
}

bool OverlayHost::onOwnersTab() const {
    const int current = anchorTabIndex();
    if (current < 0 || m_pageOwnerTab < 0) {
        // 锚点不是选项卡 / 压根没有页面（纯提示那一档）：没有"别的选项卡"这回事。
        return true;
    }
    return current == m_pageOwnerTab;
}

bool OverlayHost::parked() const {
    return m_mode == Mode::Page && !onOwnersTab();
}

void OverlayHost::applyVisibility() {
    // 整页那一档**只在开它的那颗选项卡上露面**。切到别的选项卡上就"收着"：不关、不销毁
    // —— 收掉了"切回来还是原来那一页（连滚到哪儿都在）"也就没了；用 `execPage()` 开的页
    // 还会让调用方一直等下去（见头文件那段）。
    const bool pageShown = (m_mode == Mode::Page) && onOwnersTab();
    // 提示那一档不跟着选项卡走：它是要人回答的一句话，收起来 = 让调用方在用户看不见的
    // 地方干等。切页时它换的只是底图（见 refreshBackdrop）。
    const bool promptShown = (m_mode == Mode::Prompt);

    if (m_pageFrame != nullptr) {
        if (m_pageFrame->isVisible() && !pageShown) {
            // 要收起来了：**先记一笔焦点在页里哪个控件上**。Qt 的焦点是跟着"看得见"走的
            // （父控件一藏起来就清掉），回来时没人给它摆回去 —— 症状是"切一趟选项卡，
            // 我正在打字的那个格子丢了焦点"（字还在，光标没了）。
            QWidget *focus = QApplication::focusWidget();
            m_pageFocus = (focus != nullptr && m_pageFrame->isAncestorOf(focus)) ? focus : nullptr;
        }
        m_pageFrame->setVisible(pageShown);
        if (pageShown && m_pageFocus != nullptr && m_pageFrame->isAncestorOf(m_pageFocus)) {
            m_pageFocus->setFocus();
        }
    }
    if (m_promptCard != nullptr) {
        const bool wasShown = m_promptCard->isVisible();
        m_promptCard->setVisible(promptShown);
        // 提示那一档：整块**本来就露着头**（比如连着开第二个提示）时，只让新那一块自己
        // 淡进来。整块还没露头的情况归下面那一条（整块一起淡，不然一里一外淡两次、
        // 快的那个看着像"闪"）。
        if (promptShown && !wasShown && isVisible() && !m_promptFullSize) {
            Anim::overlayAppear(m_promptCard);
        }
    }
    const bool wasVisible = isVisible();
    setVisible(promptShown || pageShown);
    if (isVisible() && !wasVisible && promptShown && !m_promptFullSize) {
        // 只有提示卡片需要整层淡入。页面必须立即出现：页面从别的主选项卡返回时若再从
        // 透明度 0 淡入，会先露出底下已禁用页面的灰色外观，看起来既像卡顿又像闪灰。
        // 页面本身已经是导航结果，不需要再用一层 0→1 的覆盖层动画重复确认。
        Anim::overlayAppear(this);
    }
    if (isVisible()) {
        // 浮在上面这一条**每回露面都得重申**：锚点换了一页，新那一页在子控件次序上可能
        // 压过我们（Qt 新加的子控件默认排在最后，也就是"最上面"）。
        raise();
        if (QWidget *win = window(); win != nullptr && win != m_watchedWindow) {
            if (m_watchedWindow != nullptr) {
                m_watchedWindow->removeEventFilter(this);
            }
            m_watchedWindow = win;
            m_watchedWindow->installEventFilter(this);
        }
    }
    retargetCoveredPage();
    relayout();
    placeVeil();
    // 整页 / 铺满的提示盖住主页时，整窗一起换成磨砂底（轨、顶栏和这一层同一张底）。
    // 提示叠在整页上时页面只是暂时藏起来、画在底图里，主页风景照样不许回来。
    if (auto *backdrop = Glass::Backdrop::of(this)) {
        backdrop->setHeroCovered(isVisible() && (pageShown || promptOverShownPage() || (promptShown && m_promptFullSize)));
    }
}

void OverlayHost::placeVeil() {
    if (m_veil == nullptr) {
        return;
    }
    // 整页那一档和铺满的提示（任务中心）本身就站在整窗磨砂上，不压暗；只有小提示卡才要。
    const bool on = isVisible() && m_mode == Mode::Prompt && !m_promptFullSize;
    if (!on) {
        m_veil->hide();
        return;
    }
    m_veil->setGeometry(parentWidget()->rect());
    m_veil->show();
    m_veil->raise();
    raise();
    m_veil->update();
}

QPixmap OverlayHost::captureBackdrop() const {
    // 要变暗的那一块**现在**是谁：整页那一档**正摆在眼前**时是那一层自己（它才是盖在
    // 底下那些内容上面的东西），其余情况都是当前选项卡那一页。
    //
    // NOTE: 判据里那半句 `onOwnersTab()` 不能少：它把"底下压着一页"和"那一页正铺在屏幕上"
    // 分开。页面**收着**（用户在别的选项卡上逛）时这两件事不同：少了这半句，用户在新选项卡
    // 上看到的会是一张**上一个选项卡那一页**的变暗快照。
    if (m_overPage && onOwnersTab() && m_pageFrame != nullptr) {
        return snapshot(m_pageFrame);
    }
    QWidget *page = coveredPage();
    return page != nullptr ? snapshot(page) : QPixmap();
}

void OverlayHost::refreshBackdrop() {
    // 换选项卡 / 那一页收起来 / 又摆出来 —— 底下那一块**换了**，重拍一张。
    //
    // NOTE: 不能只判 `m_overPage`：页面**收着**的时候它此刻不在屏幕上（见头文件那段），
    // 底下压着的是当前选项卡自己的内容，所以得重拍。这里和 openPrompt() 共用同一个
    // 判据（captureBackdrop）。
    m_backdrop = captureBackdrop();
    update();
}

void OverlayHost::onAnchorTabChanged() {
    if (m_mode == Mode::Prompt) {
        refreshBackdrop();
    }
    const bool pageWasShown = m_pageFrame != nullptr && m_pageFrame->isVisible();
    applyVisibility();
    // 切回这一页的选项卡：它和别的换页一样淡进来（选项卡那一层这时不动，见 MainWindowUI）。
    if (!pageWasShown) animatePageIn();
}

bool OverlayHost::promptOverShownPage() const {
    return m_mode == Mode::Prompt && m_overPage && onOwnersTab();
}

void OverlayHost::animatePageIn() {
    if (m_mode != Mode::Page || m_pageFrame == nullptr || !m_pageFrame->isVisible()) return;
    // 布局先落定，幕布照的才是这一页摆好之后的样子：幕布在 pageAppear() 里当场拍一张定住的
    // 快照，拍下的若是半成品，淡入那几帧就是挤着的、摆好后再跳一下。
    //
    // 只 activate 最外层不够：页面套在 widgetResizable 的滚动区里，滚动区要等**排队**的
    // LayoutRequest 才把内容区撑到视口大小（见 eventFilter 里 m_pageContent 那段），页面自己的
    // 布局也是排队算的；按宽窄换横排 / 竖排的页（账号与外观）收到新宽度后还会再排一轮。
    // 于是在这里把这几轮排队的布局请求当场送完。轮数固定：没有请求时一轮几乎不花时间，
    // 而"内容区尺寸没变"不能当收敛判据（页内换方向时内容区可能一样大）。
    // 动效关着时不拍快照，排队的布局在首次绘制前照常送到，这里就不提前冲刷。
    for (int pass = 0; Anim::enabled() && pass < 4; ++pass) {
        if (QLayout *layout = m_pageFrame->layout()) layout->activate();
        if (m_pageContentLayout != nullptr) m_pageContentLayout->activate();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    }
    Anim::pageAppear(m_pageFrame);
}

void OverlayHost::setPromptBlockedWidget(QWidget *widget) {
    if (m_promptBlockedWidget == widget) {
        return;
    }
    // 换人之前先把上一位放开：不然它会被永久禁用（而这位的禁用状态本来就是"临时的"）。
    if (m_promptBlockedWidget != nullptr) {
        m_promptBlockedWidget->setEnabled(true);
    }
    m_promptBlockedWidget = widget;
    retargetCoveredPage();
}

void OverlayHost::retargetCoveredPage() {
    // **提示**那一档还要额外按住一块（底栏）—— 它不在这一层底下，见
    // setPromptBlockedWidget() 那段说明（不按住的症状是"点了没反应、也没有一个字解释"）。
    // 这一句必须在下面那个 `if (!isVisible()) return;` **之前**：提示收掉的那一刻这一层
    // 就不可见了，放在后面的话底栏永远解不开禁用。
    if (m_promptBlockedWidget != nullptr) {
        m_promptBlockedWidget->setEnabled(m_mode != Mode::Prompt);
    }
    // 判据是"**现在真摆在眼前**"（不是"开着"）：整页那一档收在别的选项卡上的时候，
    // 它没盖住任何东西 —— 用户正要用下面那一页，这时候按住它就是"界面活着但点不动"。
    QWidget *page = isVisible() ? coveredPage() : nullptr;
    // 同一页仍被覆盖时不要先启用再禁用整棵控件树；这会触发重复的样式和布局更新。
    if (page == m_disabledPage) return;
    releaseCoveredPage();
    if (page != nullptr) {
        // 被盖住的那一页要**按住**。覆盖层浮在上面能吃掉鼠标，但**键盘吃不下** ——
        // 按 Tab 照样能把焦点挪到下面那些控件上，接着按空格就等于在用户看不见的地方
        // 点了一下。禁用是唯一一处能让"看不见 = 点不着"成立的开关。
        //
        // NOTE: 禁用会改那一页的调色板，正常情况下"变灰"是看得见的 —— 这里不要紧：
        // 页面型那一档它被整块盖住，提示型那一档上面盖的是**开之前拍下来的底图**
        // （见 openPrompt），禁用之后的外观根本没机会露面。
        page->setEnabled(false);
        m_disabledPage = page;
    }
}

void OverlayHost::setPageHeaderWidget(QWidget *widget) {
    if (m_headerWidget == widget) return;
    if (m_headerWidget) {
        auto *old = m_headerWidget.data();
        old->hide();
        old->setParent(m_headerWidgetOwner ? m_headerWidgetOwner.data() : m_parking);
        if (!m_headerWidgetOwner) old->deleteLater();
    }
    m_headerWidget = widget;
    m_headerWidgetOwner = widget ? widget->parentWidget() : nullptr;
    if (widget) {
        if (m_headerWidgetOwner)
            connect(m_headerWidgetOwner.data(), &QObject::destroyed, widget, &QObject::deleteLater);
        m_titleLabel->parentWidget()->layout()->addWidget(widget);
        widget->show();
    }
}

const char *OverlayHost::disposesOnCloseProperty() { return kDisposesOnCloseProperty; }

void OverlayHost::releaseContent(QVBoxLayout *content) {
    if (content == nullptr) {
        return;
    }
    while (content->count() > 0) {
        QLayoutItem *item = content->takeAt(0);
        if (QWidget *widget = item->widget()) {
            // **不删**，还回去：调用方那些对话框是栈对象，必须让它在自己的作用域里正常
            // 析构。这里要是把它当子控件删了，回到调用方就是二次释放。
            //
            // 例外：页自己声明了"收页之后由主人 `deleteLater()`"（`kDisposesOnCloseProperty`）。
            // 那种页只藏起来就行 —— `setParent()` 会让整棵控件树重算继承的样式和调色板
            // （实例设置那一页实测这一段 75 ms，见 docs/dev/PERFORMANCE.md「实例设置按页懒建」），
            // 而那份树几毫秒之后就要被删掉，这一趟纯属白花。
            widget->hide();
            if (!widget->property(kDisposesOnCloseProperty).toBool()) {
                widget->setParent(m_parking);
            }
        }
        delete item;
    }
}

QWidget *OverlayHost::openPage(const QString &title, int ownerTab) {
    if (m_mode != Mode::None) {
        // 一层只放得下一个页面（见头文件那段）。返回 nullptr，调用方当"没开成"。
        // NOTE: 页面**收着**的时候（用户在别的选项卡上逛）也算"有人占着这一层"，
        // 照样是 nullptr —— 那一位没被释放，收起来的页面只是看不见而已。
        return nullptr;
    }
    m_mode = Mode::Page;
    // 整页要摆上来了：浮在页上的弹层（实例切换、账号、活动）先让位收掉，不然会盖在新页上。
    GlassPopover::closeAllIn(window());
    // 这一页是**哪一颗选项卡**的：它决定这一页在哪儿露面（见头文件那段）。
    //
    // 调用方说得出口（`ownerTab != -1`）时**以它为准**，不再看当前选项卡是谁 ——
    // 有些页面的"家"是定死的，和用户从哪条路进来无关（「实例设置」永远归「实例」那一颗）。
    // 取当前选项卡的症状见 openPage() 的 doc。
    m_pageOwnerTab = ownerTab >= 0 ? ownerTab : anchorTabIndex();
    m_pageFocus = nullptr;
    m_backOverride = nullptr;
    m_titleLabel->setText(title);
    applyVisibility();
    if (m_backButton != nullptr) {
        m_backButton->setFocus();
    }
    return m_pageContent;
}

QWidget *OverlayHost::openPrompt(bool dismissOnOutsideClick, bool fullSize) {
    if (m_mode == Mode::Prompt) {
        // 提示叠提示：不做（外面那一层正卡在自己的事件循环里，把它顶掉就是死等）。
        // 这一条在外面会被 InlineMessage::ask() 当成"没问成"（返回 -1）。
        return nullptr;
    }
    const bool overPage = (m_mode == Mode::Page);
    m_overPage = overPage;
    m_dismissOnOutsideClick = dismissOnOutsideClick;
    m_promptFullSize = fullSize;
    if (fullSize) {
        Anim::clearOpacityEffect(this);
        Anim::clearOpacityEffect(m_promptCard);
    }
    const int padding = fullSize ? kOverlayPageMargin : promptPadding();
    m_promptLayout->setContentsMargins(padding, padding, padding, padding);

    // "四周变暗"要能**看见**下面那一层 —— 而覆盖层和下面那一页是**兄弟**：在这上面
    // 半透明地画一层，合的是 `centralWidget` 的底，不是页面的像素（Qt 重绘一个子控件
    // 时不会连带重绘它的兄弟）。所以这里把下面那一块**拍下来**当底图，画的时候先铺底图
    // 再压暗。一次开只拍一张，代价就是一次页面渲染。
    //
    // NOTE: 拍的是"开之前"的样子，所以底下那些会自己动的东西（下载进度）在提示开着的
    // 期间是**冻住的**。提示期间下面本来也点不动，冻住不算问题；反过来"抓完再禁用"
    // 这个顺序要紧 —— 颠倒过来拍到的就是变灰之后的样子了。
    //
    // NOTE: 底下压着一页、而那一页**正收在别的选项卡上**的时候（用户切走之后又冒出来一条
    // 提示），拍的是**现在这一页**：那一页此刻不露面，要变暗的是用户正看着的这块。
    // `m_overPage` 照旧是 true —— 提示收掉之后还是回到那一页（它只是收着）。
    // 换页过渡还在半路时先收到终点：底图要的是那一页本来的样子，不是半透明的中间帧。
    Anim::finishPageAppear(m_pageFrame);
    Anim::finishPageAppear(m_anchor);
    m_backdrop = captureBackdrop();

    m_mode = Mode::Prompt;
    // 整页那一层就此收起来（不是"藏起来就行" —— 藏起来之后底下那些控件还能收到键盘焦点
    // 和 Tab）。收的是**显示**，`m_mode` 说"现在最上面是提示"。
    applyVisibility();
    if (m_promptContent != nullptr) {
        m_promptContent->setFocus();
    }
    return m_promptContent;
}

void OverlayHost::closeOverlay() {
    if (m_mode == Mode::Prompt) {
        // 收最上面那一层：提示。底下压着一页的话，那页**还在**。
        releaseContent(m_promptContentLayout);
        m_dismissOnOutsideClick = false;
        m_promptFullSize = false;
        m_backdrop = QPixmap();
        if (m_overPage) {
            m_overPage = false;
            m_mode = Mode::Page;
            // 回到那一页（它可能是收在别的选项卡上的 —— 那就还是收着，见 applyVisibility）。
            applyVisibility();
            // 焦点：优先摆回"提示盖上来之前那一格"（applyVisibility 已经做过一遍），
            // 没有记录在案的就摆到返回箭头上（键盘一进来就归页眉管）。
            if (m_pageFocus == nullptr && m_backButton != nullptr && !parked()) {
                m_backButton->setFocus();
            }
            // 提示收掉了、底下那一页还在：排着的那几条纯报信提示照样该出来。
            scheduleNotificationDrain();
            return;
        }
        m_mode = Mode::None;
        applyVisibility();
        // 这一条提示收掉了：排着的那几条纯报信提示该出来了。
        scheduleNotificationDrain();
        // 这一层整个空出来了：后台等着的那几页也轮到它们了。
        schedulePageDrain();
        return;
    }
    if (m_mode == Mode::Page) {
        // 这一层刚才是哪一页（`pageClosed()` 要带上它）：`showPage()` 那一档直接记着；
        // `execPage()` 那一档记的是内容区里那**唯一**一个子控件 —— 页面对象活到这次收页
        // 结束（收页信号在下面发，之后才轮到调用方决定删不删）。
        QElapsedTimer closeClock;
        closeClock.start();
        QWidget *closedPage = m_pageWidget;
        if (closedPage == nullptr && m_pageContentLayout != nullptr &&
            m_pageContentLayout->count() > 0) {
            if (QLayoutItem *item = m_pageContentLayout->itemAt(0)) {
                closedPage = item->widget();
            }
        }
        setPageHeaderWidget(nullptr);
        m_backOverride = nullptr;
        if (m_escWatchedDialog != nullptr) {
            m_escWatchedDialog->removeEventFilter(this);
            m_escWatchedDialog = nullptr;
        }
        // `showPage()` 摆着的那一页：两条收页接线先断掉（这一页的调用方不是卡在循环里
        // 等我们的，收尾就在这儿）。`execPage()` 那一档这两个本来就是空的。
        disconnect(m_pageWidgetBack);
        disconnect(m_pageWidgetFinished);
        m_pageWidget = nullptr;
        releaseContent(m_pageContentLayout);
        // 收页的钱花在哪一段：还回去（隐藏 + 换父容器）／藏这一层／底下那一页淡进来
        // （`pageAppear()` 要先照一张整窗快照）。超 10 ms 才记一行。
        const qint64 releaseMs = closeClock.restart();
        m_mode = Mode::None;
        m_pageOwnerTab = -1;
        m_pageFocus = nullptr;
        const bool pageWasShown = m_pageFrame != nullptr && m_pageFrame->isVisible();
        applyVisibility();
        const qint64 hideMs = closeClock.restart();
        // 返回：底下那一页同样淡进来（所有收页路都汇到这里，入口不用各补一次）。
        if (pageWasShown && m_anchor != nullptr && m_anchor->isVisible()) Anim::pageAppear(m_anchor);
        const qint64 appearMs = closeClock.elapsed();
        if (releaseMs + hideMs + appearMs > 10) {
            qInfo() << "[UI timing] overlay close page" << releaseMs << "release" << hideMs << "hide"
                    << appearMs << "appear";
        }
        // 所有收页路都汇到这里，所以收页钩子只发这一处（见头文件那段）。
        emit pageClosed(closedPage);
        schedulePageDrain();
        return;
    }
}

bool OverlayHost::queueNotification(std::function<void()> show) {
    if (!show) {
        return false;
    }
    // 只有**已经有提示开着**的时候才排队。没开着的时候调用方该直接摆 —— 排一轮反而
    // 把"现在就弹"变成了"下一轮才弹"，那是没人要的顺序变化。
    if (!promptOpen()) {
        return false;
    }
    m_queuedNotifications.append(std::move(show));
    return true;
}

bool OverlayHost::queuePageOpen(std::function<void()> show) {
    if (!show) {
        return false;
    }
    if (!isOpen()) {
        // 空着：就是现在。调用方拿到的是"已经开了"，不用再做什么。
        show();
        return false;
    }
    // 被占着 —— **不驱逐**（见头文件那段：这是后台弄完的事，不是用户刚点的）。
    // 收着的页也算占着：它看不见，可用户切回去就指望它原样还在。
    if (m_queuedPageOpens.size() >= kMaxQueuedPageOpens) {
        qWarning() << "Overlay: dropping the oldest queued page, more than"
                   << kMaxQueuedPageOpens << "background pages are waiting";
        m_queuedPageOpens.removeFirst();
    }
    m_queuedPageOpens.append(std::move(show));
    return true;
}

void OverlayHost::schedulePageDrain() {
    if (m_queuedPageOpens.isEmpty() || m_pageDrainPending) {
        return;
    }
    m_pageDrainPending = true;
    // 和提示那条同一个理由（见 scheduleNotificationDrain）：这一下发生在收页的收尾里，
    // 调用栈还属于那一页的调用方，不能把它按在我们的活儿里。
    QTimer::singleShot(0, this, [this] {
        m_pageDrainPending = false;
        // 这中间又有人占上了（或者本来就没收干净）：留着，等下一次收干净再排。
        if (m_queuedPageOpens.isEmpty() || isOpen()) {
            return;
        }
        const auto show = m_queuedPageOpens.takeFirst();
        // **先排下一次、再摆这一发**：`show()` 可能开出新的一页（那剩下的几条就该等它
        // 收掉），也可能什么都没开（比如那个实例已经不在了）—— 两种都不能把后面的堵死。
        schedulePageDrain();
        show();
    });
}

void OverlayHost::scheduleNotificationDrain() {
    if (m_queuedNotifications.isEmpty() || m_notificationDrainPending) {
        return;
    }
    m_notificationDrainPending = true;
    // 下一轮事件循环：这一下多半发生在 `execEmbedded()` 的收尾里，那一刻的调用栈属于
    // **上一条提示的调用方**。当场摆下一条会把它按在嵌套事件循环里，等用户点完下一条
    // 才返回 —— 收尾提示晚一步弹出来是能接受的，收尾本身被拖住不行。
    QTimer::singleShot(0, this, [this] {
        m_notificationDrainPending = false;
        if (m_queuedNotifications.isEmpty()) {
            return;
        }
        // 这中间又有人直接开了一条提示：留着，等它收掉时再排一次。
        if (promptOpen()) {
            return;
        }
        const auto show = m_queuedNotifications.takeFirst();
        // **先排下一次、再摆这一条**：`show()` 摆出来的那条不一定走 `closeOverlay()`
        // （比如宿主已经没了、开不出来，什么都没发生），先排好才不会把后面的堵死。
        // 这一下排出来的定时器如果落进 `show()` 的嵌套事件循环里，会因为 `promptOpen()`
        // 直接返回；那一条收掉时 `closeOverlay()` 会再排一次 —— 两条路都收得住。
        scheduleNotificationDrain();
        show();
    });
}

bool OverlayHost::dismiss() {
    return closeTopLayer(/*evenWhenParked=*/false);
}

bool OverlayHost::evict() {
    return closeTopLayer(/*evenWhenParked=*/true);
}

bool OverlayHost::closeTopLayer(bool evenWhenParked) {
    if (m_mode == Mode::Prompt) {
        // 提示那一档：和"按 Esc / 点框外面"**同一发**（`execEmbedded` 接它去 reject，
        // 那些自己搭提示的调用方也接它）。
        // NOTE: 提示**不跟着选项卡收**（见头文件那段），所以"收着"这一档对它不存在 ——
        // `evenWhenParked` 在这儿没有第二个分支。
        emit promptDismissed();
        return true;
    }
    if (m_mode == Mode::Page) {
        if (!evenWhenParked && !onOwnersTab()) {
            // 收在别的选项卡上：用户看不见它，这一发不该落在它头上（不然就是"我在主页
            // 点了一下主页，下载页那个详情页自己没了"）。
            // NOTE: `evict()` 要的**正是**这一档（把"用户在别的选项卡上逛、这一页还占着
            // 那一位"这件事一起收掉）—— 它走的就是下面那两行。
            return false;
        }
        // 页面那一档：和"点返回箭头"**同一发**。★ 这里**绝不能改成 `closeOverlay()`** ——
        // 它只拆外壳、不经过页面自己的 accept/reject：`showPage()` 那条 `backPressed ⇒
        // reject` 不跑（页面在 reject 里做的收尾全被跳过），`execPage()` 的调用方更会一直
        // 等下去。
        emit backPressed();
        return true;
    }
    return false;
}

void OverlayHost::relayout() {
    setGeometry(contentRect());
    if (m_veil != nullptr && m_veil->isVisible()) {
        m_veil->setGeometry(parentWidget()->rect());
    }
    if (m_pageFrame != nullptr) {
        m_pageFrame->setGeometry(rect());
    }
    if (m_promptCard == nullptr) {
        return;
    }
    if (m_promptFullSize) {
        // 任务中心这类"整页叠在页上"的提示：卡片铺满内容区，看起来就是一层整页，
        // 而不是一张小弹窗；底下的设置页仍然原样保留着。
        if (m_promptCard->width() != this->width() || m_promptCard->height() != this->height()) {
            m_promptCard->setFixedWidth(this->width());
            m_promptCard->setFixedHeight(this->height());
        }
        m_promptCard->move(0, 0);
        return;
    }
    // 宽度**恒定**、只夹高度：所有提示一样宽，不允许出现尺寸不一样的弹窗。
    // 窄一点就成了另一种弹窗，矮一点只是内容滚。
    int preferredWidth = promptWidth();
    if (m_promptContentLayout && m_promptContentLayout->count() > 0) {
        if (QWidget *content = m_promptContentLayout->itemAt(0)->widget())
            preferredWidth = qBound(promptWidth(), content->property("wtfmlPromptWidth").toInt(), 1000);
    }
    const int width = qMin(preferredWidth, qMax(0, this->width() - 2 * promptMargin()));
    const int available = qMax(0, height() - 2 * promptMargin());
    const int wanted = qMin(m_promptCard->sizeHint().height(), available);
    // 一样就不动：这一条是**防循环**的 —— 改尺寸会让卡片里那套布局重算，而重算又会
    // 以 LayoutRequest 的形状回到上面 eventFilter 里（那边还会再叫一次 relayout）。
    if (m_promptCard->width() != width || m_promptCard->height() != wanted) {
        m_promptCard->setFixedWidth(width);
        m_promptCard->setFixedHeight(wanted);
    }
    m_promptCard->move((this->width() - m_promptCard->width()) / 2,
                       (this->height() - m_promptCard->height()) / 2);
}

int OverlayHost::execPage(QDialog *dialog, const QString &title, int ownerTab) {
    return execEmbedded(dialog, title, true, false, ownerTab);
}

bool OverlayHost::showPage(QDialog *dialog, const QString &title, int ownerTab) {
    // 先判"这一层空不空"：不空的时候接着往下走会把**正开着的那一页**（同一个对话框的话）
    // 收进寄存处 —— 一次误触发就把用户眼前的页藏起来了。
    if (dialog == nullptr || m_mode != Mode::None) {
        return false;
    }

    QWidget *area = openPage(title, ownerTab);
    if (area == nullptr || area->layout() == nullptr) {
        return false;
    }
    // 直接挂到最终容器；先寄存再交给布局会连续更换两次父对象，
    // 每次都让整页控件重算继承的样式、调色板和布局。
    dialog->hide();
    if (dialog->parentWidget() != area || dialog->isWindow())
        dialog->setParent(area, Qt::Widget);

    QElapsedTimer showSeg;
    area->layout()->addWidget(dialog);
    dialog->setVisible(true);
    relayout();
    // 开页和导航轨换页走同一发过渡（内容淡入上抬，主页风景同时淡成磨砂）。
    animatePageIn();

    // ③ 收页：返回箭头接成 `reject()`（和 `execPage()` 里那条一模一样），Esc / 这一页
    // 自己 accept/reject 也落到 `finished` —— 两条都在同一处收掉这一层。**没有**卡在
    // 嵌套循环里的调用方要醒，所以 `closeTopLayer()` 那边不用特判。
    //
    // NOTE: 连接只挂到这一次收页上：同一个对象再摆上来时先断掉上一条，否则第二条会落在
    // "现在开着的是别人那一页"这个判断上（`m_pageWidget` 已经不是它了）。
    disconnect(m_pageWidgetBack);
    disconnect(m_pageWidgetFinished);
    m_pageWidget = dialog;
    if (m_escWatchedDialog != nullptr && m_escWatchedDialog != dialog) {
        m_escWatchedDialog->removeEventFilter(this);
    }
    m_escWatchedDialog = dialog;
    dialog->installEventFilter(this);
    m_pageWidgetBack =
        connect(this, &OverlayHost::backPressed, dialog, [dialog] { dialog->reject(); });
    m_pageWidgetFinished = connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (m_pageWidget == dialog) {
            closeOverlay();
        }
    });
    return true;
}

void OverlayHost::setBackOverride(std::function<bool()> handler) {
    m_backOverride = std::move(handler);
}

void OverlayHost::setPageTitle(const QString &title) {
    if (m_titleLabel != nullptr) {
        m_titleLabel->setText(title);
    }
}

QString OverlayHost::pageTitle() const {
    // 页眉上那行字就是"现在开着的是哪一页"的**唯一出处**（`openPage()` / `execPage()`
    // 灌进来、`setPageTitle()` 改的就是它）—— 这儿不另存一份成员，存两份迟早对不上。
    return m_titleLabel != nullptr ? m_titleLabel->text() : QString();
}

int OverlayHost::execPrompt(QDialog *dialog, bool dismissOnOutsideClick) {
    if (auto *modal = QApplication::activeModalWidget(); modal && modal->objectName() == QStringLiteral("instanceUpdateWizard")) {
        if (auto *local = modal->findChild<OverlayHost *>(QStringLiteral("instanceUpdatePromptHost")); local && local != this)
            return local->execPrompt(dialog, dismissOnOutsideClick);
    }
    return execEmbedded(dialog, QString(), false, dismissOnOutsideClick);
}

int OverlayHost::execEmbedded(QDialog *dialog, const QString &title, bool asPage,
                              bool dismissOnOutsideClick, int ownerTab) {
    if (dialog == nullptr) {
        return QDialog::Rejected;
    }

    // ① 先摘掉"窗口"身份。QDialog 只要带着 Qt::Dialog 就是**顶层窗口** —— 构造时给个
    // parent 只是"属于谁"（Windows 上的 owner），不是"画在谁里面"。塞进布局之前必须
    // 先把它降级成普通子控件，否则它照旧另开一个窗，覆盖层就白搭了。
    dialog->hide();
    dialog->setParent(m_parking, Qt::Widget);

    // ② 摆上去。开不出来（已经有页面/已经有提示）时就此打住：`dialog` 只是挂回了寄存处，
    // 调用方看到的是一句"取消了"，和用户自己按了取消是同一件事。
    QWidget *area = asPage ? openPage(title, ownerTab) : openPrompt(dismissOnOutsideClick);
    if (area == nullptr || area->layout() == nullptr) {
        return QDialog::Rejected;
    }
    area->layout()->addWidget(dialog);
    dialog->setVisible(true);
    if (asPage) {
        if (m_escWatchedDialog != nullptr && m_escWatchedDialog != dialog) {
            m_escWatchedDialog->removeEventFilter(this);
        }
        m_escWatchedDialog = dialog;
        dialog->installEventFilter(this);
    }
    // 内容到位了，卡片按真实内容再量一次（`openPrompt()` 那一次量的时候里面还是空的）。
    relayout();
    if (asPage) animatePageIn();

    // ③ 卡住调用方，形状和 `dialog.exec()` 一模一样（语义一个字不变：QMessageBox::exec()
    // 本来就是嵌套事件循环，可重入性的风险面也一样）。
    QEventLoop loop;
    const QPointer<OverlayHost> hostGuard(this);
    const QPointer<QDialog> dialogGuard(dialog);
    int result = QDialog::Rejected;
    QMetaObject::Connection backConnection;
    if (asPage) {
        // 返回箭头 = **reject**（不保存离开）。对话框自己那对「保存 / 取消」照旧在它身上，
        // 点它们走的是它自己的 accept()/reject()。
        backConnection = connect(this, &OverlayHost::backPressed, &loop,
                                 [dialogGuard] { if (dialogGuard) dialogGuard->reject(); });
    }
    // 这一发**只有提示那一层接**（`asPage == false`）。信号的名字和它的 doc 说的都是
    // "最上面那条提示被点外面/按 Esc 关掉了"，而 `mousePressEvent()` / `keyPressEvent()`
    // 也只在 `m_mode == Mode::Prompt` 时才发它 —— 页面那层永远等不到这一发，接了就是接了个
    // 不该有的东西。
    //
    // 两层都接的症状是"点一下提示框外面，底下那一页跟着一起没了"：
    // 提示叠在页上的时候，页那一次的 `execEmbedded` 还卡在 `loop.exec()` 里、它那条连接
    // 还活着 ⇒ 这一发 `emit` 让两条 lambda 同时跑，页的对话框被 `reject()`。
    // `QDialog::done()` 里**没有可见性判断**（只有一层 `QPointer` 保护），所以哪怕
    // 那一页此刻正被提示盖着、`m_pageFrame` 是收着的，照样 `finished(Rejected)` ⇒ 页那次
    // `loop.quit()` ⇒ `execEmbedded` 返回 Rejected。用户看到的是"我只点关了一条提示，
    // 刚编辑了半天的分组设置全丢了"（GroupDialog 那条重名警告就叠在它自己这一页上）。
    //
    // NOTE: 返回箭头那条（`backConnection`）**没有**这个问题：`backPressed` 只有那六颗
    // 按钮会发，而页面被提示盖住时页眉整个是收着的，点不到。
    QMetaObject::Connection dismissConnection;
    if (!asPage) {
        dismissConnection =
            connect(this, &OverlayHost::promptDismissed, &loop,
                    [dialogGuard] { if (dialogGuard) dialogGuard->reject(); });
    }
    const QMetaObject::Connection finishedConnection =
        connect(dialog, &QDialog::finished, &loop, [&result, &loop](int code) {
            result = code;
            loop.quit();
        });

    // QObject 销毁不会发 QDialog::finished。宿主/对话框销毁时也必须释放调用栈。
    const auto dialogDestroyed = connect(dialog, &QObject::destroyed, &loop, &QEventLoop::quit);
    const auto hostDestroyed = connect(this, &QObject::destroyed, &loop, &QEventLoop::quit);
    loop.exec();

    disconnect(backConnection);
    // 页面那一层压根没接（见上面那段），空连接交给 disconnect 是白白一句。
    if (dismissConnection) {
        disconnect(dismissConnection);
    }
    disconnect(finishedConnection);
    disconnect(dialogDestroyed);
    disconnect(hostDestroyed);
    // 收**这一层**：提示收掉之后，底下压着的那一页还在（那是它的调用方在等的）。
    if (hostGuard) {
        hostGuard->closeOverlay();
    }
    return result;
}

bool OverlayHost::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::KeyPress &&
        (watched == m_escWatchedDialog || watched == m_watchedWindow)) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const Qt::KeyboardModifiers mods =
            keyEvent->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier |
                                     Qt::AltModifier | Qt::MetaModifier);
        if (keyEvent->key() == Qt::Key_Escape && mods == Qt::NoModifier &&
            isVisible() && !shouldDeferEscape()) {
            if (watched == m_escWatchedDialog && m_mode == Mode::Page && !parked()) {
                triggerBack();
                keyEvent->accept();
                return true;
            }
            if (watched == m_watchedWindow) {
                if (m_mode == Mode::Prompt) {
                    emit promptDismissed();
                    keyEvent->accept();
                    return true;
                }
                if (m_mode == Mode::Page && !parked()) {
                    triggerBack();
                    keyEvent->accept();
                    return true;
                }
            }
        }
    }
    // 被盖住的那一页不再 setEnabled(false)：应用级过滤器替它吞掉键盘与焦点。
    if (m_disabledPage != nullptr) {
        QWidget *widget = qobject_cast<QWidget *>(watched);
        bool inside = false;
        for (QWidget *p = widget; p != nullptr; p = p->parentWidget()) {
            if (p == m_disabledPage) { inside = true; break; }
        }
        if (inside) {
            switch (event->type()) {
            case QEvent::KeyPress:
            case QEvent::KeyRelease:
            case QEvent::ShortcutOverride:
                return true;
            case QEvent::FocusIn:
                if (m_backButton != nullptr) m_backButton->setFocus();
                return true;
            default:
                break;
            }
        }
    }
    if (watched == m_anchor) {
        switch (event->type()) {
        case QEvent::Resize:
            // 过滤器先于 QTabWidget 自己的 resizeEvent 拿到这一发：当前页那会儿还是旧尺寸，
            // contentRect() 量出来的是上一次的宽高（拖窗口边时覆盖层一边漏出底下的页、一边伸出窗外）。
            // 排一次到页面摆完之后再量。
            QMetaObject::invokeMethod(this, &OverlayHost::relayout, Qt::QueuedConnection);
            relayout();
            break;
        case QEvent::Move:
        case QEvent::Show:
            relayout();
            break;
        default:
            break;
        }
    } else if (watched == m_pageContent) {
        // 整页那一档的内容长高/变矮了，滚动条要跟着重算。
        //
        // NOTE: 这一条**必须自己发**。QScrollArea 只在 `QEvent::LayoutRequest` **送到它
        // 自己身上**的时候才重算滚动条（`QScrollArea::event`），而内容容器的
        // `updateGeometry()` 是发给它**父亲**的（视口）—— 那一步之后就没有下文了。
        // 不补这一发的话，症状是"设置页比内容区高，页脚那排按钮永远滚不到"。
        if (m_pageScroll != nullptr && !m_pageScroll->isVisible()) {
            return QWidget::eventFilter(watched, event);  // 没在显示，不用算
        }
        switch (event->type()) {
        case QEvent::ChildAdded:
            // 塞进来的东西**必须能看见**。Qt 的规矩是"显式 hide 过的会一直是藏着的"，
            // 而调用方给的多半正是"上次关掉时被 hide 过"的那个对话框（「账号与外观」
            // 就是长驻的那一个）。不翻出来的症状是**页面开了、里面是空的，而且不报错** ——
            // 更阴的是布局里那个 `QWidgetItem::isEmpty()` 会因此返回 true（Qt 认的就是
            // `isHidden()` 这一位），于是这个控件连"有多高"都不算数。
            //
            // 必须**排队**再 show()，不能当场调 —— 这一发 ChildAdded 是在
            // `QWidget::setParent()` 肚子里发出来的，setParent 收尾时会把"调用方显式
            // hide 过"的那一位重新按回去，当场那一发 show() 会被它吃掉（量到的样子是
            // `isVisible()==1 而 isHidden()==1`，两个都真）。排队到下一轮事件循环，
            // setParent 已经走完了，这一发才作数。
            //
            // NOTE: 只管**直接子控件** —— 直系子控件就是"这一页的内容"，而页面内部自己
            // 藏起来的那些（实例设置里默认折叠的「高级选项」）是孙子，碰不到。
            if (auto *child = qobject_cast<QWidget *>(static_cast<QChildEvent *>(event)->child())) {
                QMetaObject::invokeMethod(child, [child]() { child->show(); }, Qt::QueuedConnection);
            }
            Q_FALLTHROUGH();
        case QEvent::ChildRemoved:
        case QEvent::LayoutRequest:
            if (m_pageScroll != nullptr) {
                // 排队发（不是当场调）：内容和布局还在拼，此刻算出来的尺寸是半成品。
                QApplication::postEvent(m_pageScroll, new QEvent(QEvent::LayoutRequest));
            }
            break;
        default:
            break;
        }
    } else if (watched == m_promptContent || watched == m_promptCard) {
        // 内容变了、卡片里那套布局要重算了 —— 提示那一档的高矮必须跟着走（见构造里那段
        // NOTE：`openPrompt()` 返回之后内容才进去）。`relayout()` 只在尺寸真的变了的时候
        // 才动控件，所以这一条不会自己叫自己。
        switch (event->type()) {
        case QEvent::ChildAdded:
        case QEvent::ChildRemoved:
        case QEvent::LayoutRequest:
            if (m_mode == Mode::Prompt) {
                relayout();
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void OverlayHost::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const QPaintDevice *redirect = redirected(nullptr);
    const bool toPixmap = redirect != nullptr && redirect->devType() == QInternal::Pixmap;
    const auto paintBase = [&]() {
        // 整页要"盖住"下面那一页，同时和别的页一样直接站在整窗磨砂上：铺那一块整窗底。
        if (auto *backdrop = Glass::Backdrop::of(this)) backdrop->paintBase(painter, this, toPixmap, /*withHero=*/true);
        else painter.fillRect(rect(), palette().color(QPalette::Window));
    };
    if (m_mode == Mode::Page) {
        paintBase();
        return;
    }
    if (m_mode != Mode::Prompt) {
        return;
    }
    // 提示：先铺底图（开之前拍下来的那一层），再压一层半透明黑。用户要的是"四周变暗"，
    // 不是"四周消失"。
    // 没拍到底图时什么都不铺：父控件那张整窗磨砂本来就画在下面。
    // 叠在整页上：底图是那一页（不带底），下面露着的却是选项卡那一页 —— 先铺整页那块底，
    // 不然两页叠着画（体验反馈2 第 2 条）。
    if (promptOverShownPage()) paintBase();
    if (!m_backdrop.isNull()) {
        painter.drawPixmap(rect(), m_backdrop);
    }
    painter.fillRect(rect(), scrimColor());
    if (m_promptCard == nullptr || !m_promptCard->isVisible()) {
        return;
    }
    if (m_promptFullSize) {
        paintBase();
        return;
    }
    // 对话框（tokens-extra §6）：玻璃面、圆角 16；不描亮边，靠卡面压在遮罩上分出来。
    const QRectF card = QRectF(m_promptCard->geometry()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    QColor face = Theme::tokens().card;
    face.setAlphaF(0.96);
    painter.setBrush(face);
    painter.drawRoundedRect(card, 16, 16);
}

void OverlayHost::mousePressEvent(QMouseEvent *event) {
    // 落在"这一层自己"身上的按下 = 点了卡片外面（卡片是子控件，点在它上面根本到不了这儿）。
    if (m_mode == Mode::Prompt && m_dismissOnOutsideClick) {
        emit promptDismissed();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void OverlayHost::triggerBack() {
    if (m_backOverride && m_backOverride()) {
        return;
    }
    emit backPressed();
}

bool OverlayHost::shouldDeferEscape() const {
    if (QApplication::activePopupWidget() != nullptr) {
        return true;
    }
    QWidget *win = window();
    if (QWidget *modal = QApplication::activeModalWidget(); modal != nullptr && modal != win) {
        return true;
    }
    if (win != nullptr) {
        for (QFrame *frame : win->findChildren<QFrame *>()) {
            if (auto *pop = dynamic_cast<GlassPopover *>(frame); pop != nullptr && pop->isVisible()) {
                return true;
            }
        }
    }
    return false;
}

void OverlayHost::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        const Qt::KeyboardModifiers mods =
            event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier |
                                  Qt::AltModifier | Qt::MetaModifier);
        if (mods == Qt::NoModifier && !shouldDeferEscape()) {
            if (m_mode == Mode::Prompt) {
                // Esc 是"离开"的通用键，**不问 m_dismissOnOutsideClick** —— 那一格管的是
                // "点外面"，而按 Esc 是明确表态（各对话框本来就是这么接的：Esc = reject）。
                emit promptDismissed();
                event->accept();
                return;
            }
            if (m_mode == Mode::Page && !parked()) {
                triggerBack();
                event->accept();
                return;
            }
        }
    }
    QWidget::keyPressEvent(event);
}
