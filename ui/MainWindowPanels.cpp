// Bottom bar, transient activity/account/toolbox panels and folder/group menus.
#include "MainWindowUI.hpp"

#include "ActivityPopover.hpp"
#include "Animations.hpp"  // 换页 / 展开折叠 / 上拉面板那几段动效
#include "ElidedLabel.hpp"
#include "Glass.hpp"
#include "InstanceGroupDelegate.hpp"
#include "MemorySlider.hpp"
#include "OverlayHost.hpp"
#include "PopupPlacement.hpp"
#include "RoundAvatar.hpp"
#include "SkinModel.hpp"
#include "TaskProgressBar.hpp"
#include "Theme.hpp"
#include "ThemeAdvancedEditor.hpp"
#include "ThemeBackground.hpp"
#include "TitleBar.hpp"
#include "VersionIcons.hpp"
#include "VersionTreeDelegate.hpp"
#include "WindowChrome.hpp"

#include <QColor>
#include <QEvent>
#include <QFont>
#include <QGridLayout>
#include <QPainterPath>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSettings>
#include <QStyle>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "MainWindowPageLayout.hpp"
#include "Popups.hpp"
using namespace MainWindowPageLayout;

namespace {

/// 底栏那颗按钮点开之后向上弹出的面板有多大（边距 / 行距 / 和按钮之间的空隙）。
constexpr int kPanelPadding = 10;
constexpr int kPanelRowSpacing = 6;
constexpr int kPanelGap = 6;
/// 面板四角的圆角：行首头像 24 + 边距 10，半径 14 让头像那一圈和面板的角大致同心。
constexpr qreal kPanelRadius = 14.0;

/// 面板里一行内部的两处间距：行首图标和那列字之间、那列字和行尾第一颗按钮之间。
/// 后者取小一点（4）：行尾那几颗按钮是一组，得看着"贴着"那列字，别像两个各管各的。
constexpr int kPanelRowIconSpacing = 8;
constexpr int kPanelRowActionSpacing = 4;

/// 行尾那个**文字**按钮的最小宽度（「选择」「登录」「登出」都是两个字）。
/// 钉死是为了让同一个面板里几行的按钮**竖着对齐** —— 跟着字宽变的话，三行的「登录」
/// 和「选择」就会错开几个像素，看着像没对齐的表格。
constexpr int kPanelActionTextWidth = 56;

/**
 * @brief 面板里行尾那几颗动作按钮的外观。
 *
 * 和底栏那两颗按钮同一个路数：不画自己的底（面板本来就有底色）、只在鼠标划过/按下时
 * 淡淡蒙一层，颜色一律从调色板取（**不写死**），浅色深色主题下都成立。
 *
 * NOTE: hover / pressed 那两条都带 `:enabled` —— 万一有哪一颗是灰掉的，它划过时**不许**
 * 亮：亮一下就说明"能点"，点下去却什么都不发生。（`:enabled` 留着是护这条规矩本身。）
 *
 * NOTE: 文字按钮要留左右各 6 px 内边距（两三个字贴着边看着像被裁过）；图标按钮不留
 * （它的大小已经由 kPanelActionSize 定死了，留了反而把图形挤小）。
 */
void applyPanelActionStyle(QPushButton *button, const QPalette &panelPalette, bool hasText) {
    QColor hover = panelPalette.color(QPalette::Highlight);
    hover.setAlpha(48);
    QColor pressed = panelPalette.color(QPalette::Highlight);
    pressed.setAlpha(96);
    // 图标那一档**要把钉死的边长再说一遍**。应用级样式表里那条
    // `QPushButton[wtfmlButtonSize="icon"] { min-height: 0px; }` 每次 polish 都会把
    // 控件的最小高度复位成 0，而 `setFixedSize()` 钉的"最大"那一半救了它：
    //   布局拿到的是 sizeHint（= 图标那么大，16），于是 24 的齿轮上屏是 24×16。
    // 控件级规则优先级更高，在这颗按钮自己的样式表里把最小高度写回钉死的那个数
    // （边长是调用方 `setFixedSize()` 时给的，这里现问，不另立第二个数）。
    const bool pinnedSize = !hasText && button->minimumHeight() > 0 &&
                            button->minimumHeight() == button->maximumHeight();
    const QString sizeRule =
        pinnedSize ? QStringLiteral(" min-height: %1px;").arg(button->minimumHeight())
                   : QString();
    button->setStyleSheet(
        QStringLiteral("QPushButton { border: none; background: transparent; padding: %1;%2 }"
                       "QPushButton:hover:enabled { background-color: %3; }"
                       "QPushButton:pressed:enabled { background-color: %4; }")
            .arg(hasText ? QStringLiteral("0 6px") : QStringLiteral("0"))
            .arg(sizeRule, hover.name(QColor::HexArgb), pressed.name(QColor::HexArgb)));
}

/**
 * @brief 面板里那一行：行首图标 + 一列字（主行/次行）+ 行尾几个动作按钮。
 *
 * NOTE: 行尾那几颗用的是**真按钮**（QPushButton），不是自绘 + 事件过滤器 —— 和实例树里
 * 那个"取消小叉"正好相反（那里只能自绘，因为那是 item view 的一行，`editorEvent`
 * 压不住 `itemClicked`）。面板不是列表控件，这里放真控件白得 hover / 按下 /
 * disabled 三态，验证工程也能按 objectName 把每一颗找回来点它。
 */
class BottomBarPanelRowWidget : public QWidget {
public:
    /// @param row 这一行要画什么（数据由 MainWindow 翻好递进来）。
    /// @param index 第几行（**只用来生成 objectName**）。
    /// @param onAction 行尾第 a 颗按钮被点了（参数是"哪一行是谁、第几颗"）。
    BottomBarPanelRowWidget(const MainWindowUI::PanelRow &row, int index,
                            const std::function<void(const QString &, int)> &onAction,
                            bool roundIcon, QWidget *parent)
        : QWidget(parent) {
        setObjectName(MainWindowUI::bottomBarPanelRowName(index));

        QHBoxLayout *line = new QHBoxLayout(this);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(kPanelRowIconSpacing);

        const int textWidth = MainWindowUI::bottomBarPanelTextWidth();
        QVBoxLayout *texts = new QVBoxLayout;
        texts->setContentsMargins(0, 0, 0, 0);
        texts->setSpacing(0);

        if (!row.icon.isNull()) {
            // 账号面板那几行是头像：跟主页那颗一样画成圆（pixmap 照旧是方图）。
            QLabel *icon = roundIcon ? new RoundAvatar::Label(this) : new QLabel(this);
            icon->setObjectName(MainWindowUI::bottomBarPanelRowIconName(index));
            icon->setPixmap(row.icon);
            icon->setFixedSize(MainWindowUI::bottomBarPanelIconSize(),
                               MainWindowUI::bottomBarPanelIconSize());
            // 图标**对着两行字那一整块居中**（对第一行会显得偏高）。
            line->addWidget(icon, 0, Qt::AlignVCenter);
        } else if (roundIcon && !row.placeholder) {
            // 账号面板里没头像的那一行（「账号与外观…」）：空出头像那一格，字和上面几行的名字对齐。
            // 占位格两边 Qt 不补布局间距，那 8 得自己加上。
            line->addSpacing(MainWindowUI::bottomBarPanelIconSize() + kPanelRowIconSpacing);
        }

        QLabel *title = new ElidedLabel(this);
        title->setObjectName(MainWindowUI::bottomBarPanelRowTitleName(index));
        title->setText(row.title);
        // 宽度**钉死**（不是只封顶）：面板的宽度是"内容多宽就多宽"，钉死之后同一个面板
        // 在任何数据下都是同一个宽度，几行的按钮也竖着对齐。超出去的截成省略号。
        title->setFixedWidth(textWidth);
        if (row.placeholder) {
            // "什么都没有"那句话用**次要颜色**：它是一句说明，不是一行内容。
            QPalette titlePalette = title->palette();
            titlePalette.setColor(QPalette::WindowText,
                                  titlePalette.color(QPalette::PlaceholderText));
            title->setPalette(titlePalette);
        }
        texts->addWidget(title);

        if (!row.subtitle.isEmpty()) {
            QLabel *subtitle = new ElidedLabel(this);
            subtitle->setObjectName(MainWindowUI::bottomBarPanelRowSubtitleName(index));
            subtitle->setText(row.subtitle);
            subtitle->setFixedWidth(textWidth);
            // 次行一律次要颜色（已运行时长 / 账号类型都是"说明"那一档）。
            QPalette subtitlePalette = subtitle->palette();
            subtitlePalette.setColor(QPalette::WindowText,
                                     subtitlePalette.color(QPalette::PlaceholderText));
            subtitle->setPalette(subtitlePalette);
            if (row.liveSubtitle) {
                subtitle->setToolTip(row.subtitle);
                auto *timer = new QTimer(subtitle);
                connect(timer, &QTimer::timeout, subtitle, [subtitle, provider = row.liveSubtitle] {
                    if (!subtitle->isVisible()) return;
                    const QString text = provider();
                    subtitle->setText(text);
                    subtitle->setToolTip(text);
                });
                timer->start(1000);
            }
            texts->addWidget(subtitle);
        }

        // 份数 0：这一列字是钉死的，富余宽度都给后面那个空档 —— 行尾那几颗按钮因此
        // 贴在**最右边**（不这么做的话它们会紧跟着短名字，几行的按钮就对不齐了）。
        line->addLayout(texts, 0);
        line->addStretch(1);

        for (int a = 0; a < row.actions.size(); ++a) {
            const MainWindowUI::PanelAction &action = row.actions.at(a);
            QPushButton *button = new QPushButton(this);
            button->setObjectName(MainWindowUI::bottomBarPanelRowActionName(index, a));
            const bool hasText = action.icon.isNull();
            if (hasText) {
                button->setText(action.text);
                button->setMinimumWidth(kPanelActionTextWidth);
            } else {
                button->setIcon(QIcon(action.icon));
                button->setIconSize(action.icon.size());
                button->setFixedSize(MainWindowUI::bottomBarPanelActionSize(),
                                     MainWindowUI::bottomBarPanelActionSize());
                // 只有图标那几颗是**自己的边长**（22）—— 得报一句，不然会被
                // `actionHeight()` 那条 `min-height` 顶到 32（见 `markIconButton()`）。
                // NOTE: 上面那个 `hasText` 那一支**不报**：那几颗本来就是一行里的动作按钮。
                Theme::markIconButton(button);
            }
            button->setEnabled(action.enabled);
            button->setToolTip(action.tooltip);
            applyPanelActionStyle(button, palette(), hasText);
            // 点的是**哪一颗**靠下标认（不认按钮上的字：那两个字会随语言/状态变，
            // 拿显示内容当身份标识的接线在产品改文案时会以看不懂的方式挂掉）。
            // 行号**不进这个回调**：见 PanelRow::key 那段 NOTE（行号是"哪一行"，
            // 外面要的是"哪一个人/哪一份会话"）。
            const QString key = row.key;
            QObject::connect(button, &QPushButton::clicked, button,
                             [onAction, key, a]() { onAction(key, a); });
            if (a > 0) {
                line->addSpacing(kPanelRowActionSpacing);
            }
            line->addWidget(button, 0, Qt::AlignVCenter);
        }
    }
};

/**
 * @brief 底栏两头那两颗按钮点开之后**向上**弹出来的那块面板（像官启/PCL 那样）。
 *
 * 用 Qt::Popup 而不是 QMenu：这里要的是"面板"这么一块地方（以后左边放实例列表、
 * 右边放账号列表），菜单那种"一行一个 action"的形状套不上。而 Qt::Popup 该白给的一样
 * 白给：点面板外面自动关、Esc 关、不抢主窗口的焦点。
 *
 * NOTE: 它是个**窗口**（Qt::Popup 一定是顶层窗口），但 parent 照旧挂着 —— 这样
 * findChild 找得到它（验证工程靠这个量几何），生命周期也不用单独管。
 */
class BottomBarPanel : public QWidget {
public:
    /// 行尾某个按钮被点了：参数是"那一行是谁（PanelRow::key）、第几颗"。
    using ActionCallback = std::function<void(const QString &, int)>;
    /// "点在面板外面"那一下要问外面两件事（两个都按**全局坐标**说话 —— 面板自己认不出
    /// "外面是哪儿"，它只知道用户在框外点了一下）。两问的顺序不能换，理由见
    /// handleOutsidePress() 的第二段 NOTE：
    /// · claimed —— 收回去**之前**问：这一下落在底栏那两颗按钮上吗？（是 ⇒ 把 Qt 那记
    /// 重放压掉）
    /// · dismissed —— 收回去**之后**叫：去开该开的那一块（点的要是同一颗就什么都不开）。
    struct OutsidePressHandler {
        std::function<bool(const QPoint &globalPos)> claimed;
        std::function<void(const QPoint &globalPos)> dismissed;
    };

    /// @param anchor 贴着哪颗按钮弹（也决定"至少和按钮一样宽"）。
    /// @param alignRight true = 右边缘和按钮右边缘对齐（右端那颗按钮用），
    /// false = 左边缘对齐（左端那颗用）。
    /// @param dropDown true = 面板挂在锚点**下面**（主页右上那张账号卡片用），
    /// false = 挂在锚点**上面**（底栏那几颗按钮用 —— 底下就是窗沿，
    /// 往上弹才不会压住内容区）。
    /// @param rows 面板里的几行（数据由 MainWindow 翻好，见 setBottomBarPanelContent()）。
    /// @param onAction 行尾的按钮被点了该告诉谁（可以给空的 = 这些按钮点了没反应）。
    /// @param onOutsidePress 点在外面那一下该告诉谁（两个回调都可以给空的 = 只知道"要收回去"）。
    BottomBarPanel(QWidget *anchor, bool alignRight, bool dropDown,
                   const QList<MainWindowUI::PanelRow> &rows, const ActionCallback &onAction,
                   const OutsidePressHandler &onOutsidePress, QWidget *parent)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), m_anchor(anchor),
          m_alignRight(alignRight), m_dropDown(dropDown), m_onOutsidePress(onOutsidePress) {
        setObjectName(MainWindowUI::bottomBarPanelName());
        // 底色比底栏再深一档：它是压在内容区上面的一块，得从底下那堆东西里分出来。
        // **不写死颜色**：从当前调色板推出来，浅色/深色主题下都成立（和底栏同一个路数）。
        // 那一档由 `Theme::partBackground(Part::Panel)` 说了算（用户可以把这一族的
        // 浮层单独换一个色）。
        // NOTE: 这一块**每次现建**（Qt::Popup + WA_DeleteOnClose）⇒ 不用挂 follow()：
        // 主题换了之后下一次打开自然就是新的（`Theme::follow()` 的 NOTE 那一段）。
        // 圆角面：窗口本身透明，面在 paintEvent 里画（四角才是圆的）。
        Popups::preparePopup(this);
        QPalette panelPalette = QApplication::palette();
        panelPalette.setColor(QPalette::Window, Theme::partBackground(Theme::Part::Panel));
        setPalette(panelPalette);

        // 点行尾那几颗按钮要**先把自己收回去**，再把动作交给外面 —— 见下面那段 lambda。
        // context 用 parent（底栏）而**不是** this：这个面板是 WA_DeleteOnClose，
        // close() 之后它随时会被删掉，捕它等于捕一个随时没了的对象；底栏和主窗口同寿。
        QWidget *const deferContext = parent;
        const auto dispatch = [this, deferContext, onAction](const QString &key, int action) {
            // **先把面板收回去**：这些动作里有两个要弹模态窗（进实例设置、关实例那个
            // "游戏不会存档"的确认框），而这块面板是个 Qt::Popup —— 它按着鼠标键盘的
            // grab。模态窗开在一块抓着手柄的东西底下，Windows 上就是"窗出来了但点不动"。
            close();
            if (!onAction) {
                return;
            }
            // 再排到**下一轮事件循环**去做事：close() 只是发了个关闭事件，面板要等这一轮
            // 走完才真的没了 —— 当轮就弹模态窗等于没关（上面那条毛病一样在）。
            QTimer::singleShot(0, deferContext,
                               [onAction, key, action]() { onAction(key, action); });
        };

        QVBoxLayout *box = new QVBoxLayout(this);
        box->setContentsMargins(kPanelPadding, kPanelPadding, kPanelPadding, kPanelPadding);
        box->setSpacing(kPanelRowSpacing);
        if (rows.isEmpty()) {
            // 兜底：没装回调（或回调给了空列表）时面板里不留一块空白。
            MainWindowUI::PanelRow empty;
            empty.title = QCoreApplication::translate("MainWindowPanels", "（没有可显示的内容）");
            empty.placeholder = true;
            box->addWidget(new BottomBarPanelRowWidget(empty, 0, dispatch, alignRight, this));
            return;
        }
        for (int i = 0; i < rows.size(); ++i) {
            box->addWidget(new BottomBarPanelRowWidget(rows.at(i), i, dispatch, alignRight, this));
        }
    }

    /// 贴的是哪颗按钮（"再点一次收回"要按它认人：点的是同一颗才算收回）。
    QWidget *anchor() const { return m_anchor; }

    /// 摆到 anchor 的正上方（`dropDown` 时是正下方）并显示出来。
    void popUp() {
        adjustSize();
        // 至少和那颗按钮一样宽：以后装实例列表/账号列表，比按钮还窄就没法看了。
        if (m_anchor != nullptr && width() < m_anchor->width()) {
            resize(m_anchor->width(), height());
        }
        if (m_anchor != nullptr) {
            const QPoint anchorTopLeft = m_anchor->mapToGlobal(QPoint(0, 0));
            int x = m_alignRight ? anchorTopLeft.x() + m_anchor->width() - width()
                                 : anchorTopLeft.x();
            // 两个方向各有一处讲究：
            // · 底栏那几颗（`dropDown = false`）往上弹 —— 它们底下就是窗沿；
            // · 主页右上那张账号卡片（`dropDown = true`）往下弹 —— 它贴着头顶，往上弹
            // 连"从哪儿滑上来"都没有地方（会被下面那段夹回屏幕里、压在卡片上）。
            int y = m_dropDown ? anchorTopLeft.y() + m_anchor->height() + kPanelGap
                               : anchorTopLeft.y() - height() - kPanelGap;
            // 窄窗口避让只实现这一处：把它**夹进那一块
            // 屏的可用区域**。面板是个顶层窗口，按锚点算出来的落点会**跑到屏幕外面**去 ——
            // 窗口贴着屏幕右边那一块（或者面板比窗口还宽）时，右端那块面板有半截在屏外，
            // 屏幕上就是"点了没反应"。夹的是**屏幕**（不是启动器窗口）那一块：面板可以
            // 压在启动器外面，只要人还看得见它。
            // NOTE: 只夹到边界、**不留边距** —— 现有那两条几何判据钉的是"左边缘贴按钮
            // 左边缘"/"右边缘贴按钮右边缘"（ui_check I4），多留一格边距就是在正常
            // 情形下也把那两条判据顶红。零边距的夹法在正常情形下**一个像素都不动**。
            // NOTE: 面板比可用区域还宽/还高时（极端窄的屏）夹不动，那就不夹（`qBound`
            // 要求下界 ≤ 上界，反过来的话它在 debug 里会直接断言）。
            const QScreen *screen = m_anchor->screen() != nullptr ? m_anchor->screen()
                                                                 : QGuiApplication::primaryScreen();
            if (screen != nullptr) {
                const QRect avail = screen->availableGeometry();
                const int xMax = avail.right() - width() + 1;
                const int yMax = avail.bottom() - height() + 1;
                if (xMax >= avail.left()) {
                    x = qBound(avail.left(), x, xMax);
                }
                if (yMax >= avail.top()) {
                    y = qBound(avail.top(), y, yMax);
                }
            }
            if (m_dropDown) {
                Anim::slideDown(this, QPoint(x, y));
            } else {
                Anim::slideUp(this, QPoint(x, y));
            }
        }
        show();
        raise();
        // 拿焦点只为一件事：Esc 能关（外面点一下 Qt 自己会关，那条不用管）。
        setFocus(Qt::PopupFocusReason);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            close();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    /// 点在面板**外面**的那一下（面板开着的时侯，外面那一下是**投递给面板自己**的 ——
    /// 局部坐标在框外，底下那颗按钮一个事件都收不到；见文件头那段）。
    /// 返回 true = 这一下已经办完了（不要再去走基类那套）。
    ///
    /// NOTE: 【为什么要在这里把坐标交出去】外面那一下是"面板该收了"，而"换到另一块面板"
    /// 只有在这里才办得到（那颗按钮收不到任何事件）。所以认"外面是哪儿"这件事由
    /// 回调办（它才知道那两颗按钮在哪），面板只管收自己。
    /// NOTE: 【为什么要压 Qt 的重放】Qt 在关一块 Qt::Popup 的时候，如果光标不在它几何里，
    /// 会把这同一个 press **重放**给光标底下那个控件（`QApplicationPrivate::closePopup`
    /// → `replayMousePress` → `QWidgetWindow::handleMouseEvent` 尾巴上那段；
    /// Windows 上那个 style hint 是 true）。那一下落在底栏那颗按钮上就是**又一次点击**：
    /// · 点的是**另一颗** → 我们刚替用户把那一块开出来，重放这一下又把它收回去
    /// （看上去就是"点了没反应，得点两下"）；
    /// · 点的是**同一颗** → 我们刚收回去，重放这一下又开出来（看上去就是"收不回去"）。
    /// `Qt::WA_NoMouseReplay` 就是干这个的（QMenu 对它自己那颗按钮也是这么办的，
    /// 见 `QMenuPrivate::noReplayFor`）。**必须在 close() 之前钉上** —— Qt 是关窗
    /// （`hide_helper`）那一刻读这个属性。属性会被 Qt 在"这个控件又收到鼠标事件"时
    /// 清掉，所以挂在构造里没用，只能在这儿按需要钉。
    bool handleOutsidePress(QMouseEvent *event) {
        const QPoint local = event->position().toPoint();
        if (rect().contains(local)) {
            return false;  // 落在面板自己身上：照旧往下走
        }
        const QPoint global = event->globalPosition().toPoint();
        const bool claimed = m_onOutsidePress.claimed && m_onOutsidePress.claimed(global);
        if (claimed) {
            // **必须赶在这里**（close() 之前）：Qt 是关窗那一刻读这个属性的。
            setAttribute(Qt::WA_NoMouseReplay, true);
        }
        close();
        // 收回去之后才轮到"要不要接着开另一块"：那一下要开的那一块，是**这一块已经收掉
        // 之后**才该出现的（顺序反了的话，新面板会被底下那段"收旧"再关一次）。
        if (claimed && m_onOutsidePress.dismissed) {
            m_onOutsidePress.dismissed(global);
        }
        event->accept();
        return true;
    }

    /**
     * @brief 点在面板外面的那一下 = "面板该收回了"。落点是不是底栏那两颗按钮、要不要换一块，
     * 由回调说了算（见 handleOutsidePress()）。
     */
    void mousePressEvent(QMouseEvent *event) override {
        if (handleOutsidePress(event)) {
            return;
        }
        QWidget::mousePressEvent(event);
    }

    /// 点得**快**的时候，这"外面的一下"到的是双击事件（Windows 上第二发就是它，见
    /// ui/RowActivation.hpp 那段）—— 同一个处理，不接的话那一下还是白点。
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (handleOutsidePress(event)) {
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

    /// 和其他浮层同一张磨砂面（`Popups::paintSurface()`），调色板只给行取色。
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        Popups::paintSurface(painter, QRectF(rect()), kPanelRadius);
    }

private:
    QWidget *m_anchor = nullptr;
    bool m_alignRight = false;
    /// 挂在锚点下面（往下弹）还是上面（往上弹），见构造函数那段。
    bool m_dropDown = false;
    OutsidePressHandler m_onOutsidePress;
};

/// 底栏整条那一块布的调色板：底比窗口底色深一档（用户单独改过就是那个）。
///
/// 基色调色板一律取 `QApplication::palette()`，**不能**取底栏自己的 —— 底栏身上就写着
/// 算好的那一份，第二次进来取到的 Window 已经是压暗过一档的，再压一次就深两档
/// （`TitleBar::applyColors()` 那边同一个坑， 在那儿踩过一次）。
QPalette bottomBarChromePalette() {
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Window, Theme::partBackground(Theme::Part::BottomBar));
    return palette;
}

/// 导航轨底部「工具箱」那颗：和轨上导航项一个画法（20 的线性图标 + 霞鹜文楷 16，图文间距 14），
/// 窄到图标轨时只剩图标居中。悬停中性灰、按下 fill-press。
class RailNavButton : public QPushButton {
public:
    using QPushButton::QPushButton;

protected:
    /// 勾选态 = "工具箱页正开着"，由主窗口按当前选项卡摆，点击不自己翻面。
    void nextCheckState() override {}
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme::Tokens t = Theme::tokens();
        const bool hot = underMouse() && isEnabled();
        const QColor base = isDown() ? t.fillPress : (hot || isChecked()) ? t.hover : QColor(Qt::transparent);
        if (base.alpha() > 0) {
            p.setPen(Qt::NoPen);
            p.setBrush(base);
            p.drawRoundedRect(QRectF(rect()), 10, 10);
        }
        const QColor ink = hot || isDown() ? t.text : t.secondary;
        const bool compact = width() < 80;
        const QRectF iconBox(compact ? (width() - 20) / 2.0 : 14, (height() - 20) / 2.0, 20, 20);
        VersionIcons::paintLine(p, QStringLiteral("toolbox"), iconBox, ink);
        if (compact) return;
        p.setFont(font());
        p.setPen(ink);
        p.drawText(QRect(int(iconBox.right()) + 14, 0, width() - int(iconBox.right()) - 14, height()),
                   Qt::AlignVCenter | Qt::AlignLeft, text());
    }
};

/// 轨底「运行状态」那颗按钮。展开轨里是整格（状态方块 + 两行字，装在 `runStateText` 里）；
/// 图标轨里（宽度 < 80，判据和工具箱那颗一致）文字收起，只画一枚运行图标和进程数徽标。
/// 两种样子只看**此刻的宽度**，所以轨在 64 和 208 之间做宽度动画时，交接是连续的、没有切换那一帧。
/// 这一格常驻（issue #85）：进程起停时轨上不增减元素、几何不变；空闲时图标轨画弱化的同一枚图标、不带数字。
class RunStateButton : public QPushButton {
public:
    using QPushButton::QPushButton;
    static QString textBoxName() { return QStringLiteral("runStateText"); }
    int count() const { return m_count; }
    /// 在跑的进程数（客户端、服务端合计）。0 = 空闲。
    void setCount(int count) {
        if (m_count == count) return;
        m_count = count;
        // 图标轨里没有文字，读屏靠这一句。
        setAccessibleName(count > 0 ? MainWindowUI::runStateRunningCountText(count)
                                    : MainWindowUI::runStateNotRunningText());
        update();
    }

protected:
    void showEvent(QShowEvent *event) override {
        QPushButton::showEvent(event);
        syncText();
    }
    void resizeEvent(QResizeEvent *event) override {
        QPushButton::resizeEvent(event);
        syncText();
    }
    /// 回车也算一次点击（空格 QAbstractButton 自己认）。
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            click();
            return;
        }
        QPushButton::keyPressEvent(event);
    }
    void paintEvent(QPaintEvent *event) override {
        QPushButton::paintEvent(event);  // 悬停 / 按下的底（样式表）
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        if (compact()) {
            // 取主题的 ok 色（浅色主题是加深的 #2E875A）：图标和徽标没有字那样的容错，浅色底上要 3:1。
            const QColor ok = m_count > 0 ? Theme::tokens().ok : Theme::tokens().disabled;
            const QRectF icon((width() - 20) / 2.0, (height() - 20) / 2.0, 20, 20);
            VersionIcons::paintLine(p, QStringLiteral("play"), icon, ok);
            const int count = m_count;
            if (count > 0) {
                const QString text = count > 99 ? QStringLiteral("99+") : QString::number(count);
                QFont badgeFont = font();
                badgeFont.setPixelSize(10);
                badgeFont.setWeight(QFont::DemiBold);
                const int badgeHeight = 14;
                const int badgeWidth = qMax(badgeHeight, QFontMetrics(badgeFont).horizontalAdvance(text) + 8);
                const QRectF badge(icon.right() + 7 - badgeWidth, icon.top() - 5, badgeWidth, badgeHeight);
                p.setPen(Qt::NoPen);
                p.setBrush(ok);
                p.drawRoundedRect(badge, badgeHeight / 2.0, badgeHeight / 2.0);
                p.setFont(badgeFont);
                p.setPen(Theme::tokens().onPrimary);
                p.drawText(badge, Qt::AlignCenter, text);
            }
        }
        if (hasFocus() && property("wtfmlKeyboardFocus").toBool()) {
            QPen ring(Theme::focusRingColor(palette()));
            ring.setWidthF(2.0);
            p.setPen(ring);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), Theme::controlRadius(), Theme::controlRadius());
        }
    }

private:
    int m_count = 0;
    bool compact() const { return width() < 80; }
    void syncText() {
        if (QWidget *text = findChild<QWidget *>(textBoxName(), Qt::FindDirectChildrenOnly)) {
            text->setVisible(!compact());
        }
    }
};

/// 底栏按钮身上那个"跟随登记只做过一次"的印记（见 `applyBottomBarButtonStyle()` 里的闸门）。
constexpr const char kBottomBarButtonFollowedProperty[] = "wtfmlThemeFollowed";

/// 底栏那两颗按钮的**同一套外观**：不画按钮自己的底（它占的是底栏那一格），
/// 只在鼠标划过/按下时淡淡蒙一层 —— 不然用户根本看不出那一格能点。
/// 蒙的那层取自调色板的 Highlight（**不写死颜色**），浅色/深色主题下都成立。
void applyBottomBarButtonStyle(QPushButton *button, const QPalette &barPalette) {
    Q_UNUSED(barPalette);
    // 和导航轨同一套悬停：正文墨色的一点点 alpha（扁平，不压主题色块）。
    const QColor hover = Theme::tokens().hover;
    const QColor pressed = Theme::tokens().fillPress;
    // 工具箱那颗是一个独立的小按钮：两边留 14 px，焦点框和悬停底都不压字；圆角和别的按钮一套。
    // 任务摘要那颗是铺满中间那一格的区域按钮，照旧不留边。
    const bool compact = button->objectName() == MainWindowUI::toolboxButtonName();
    // 运行状态那格里是两行字（状态 + 实例名），按钮 sizeHint 只算一行 ⇒ 和任务摘要同时出现时
    // 被压扁叠字。最小高只能写在这里：全局 QSS 的 min-height 会盖掉 C++ 的 setMinimumHeight。
    // 只钉高度 —— 宽度归导航轨管。先 polish：按钮字（正文 14）是 polish 时才给的，
    // 不先 polish 量到的是应用默认字，两行各矮一像素。
    button->ensurePolished();
    // 行高按「带中文的一行」量：主字体缺中文时回落字体那一行会比 fm.height() 高。
    const auto lineHeight = [](const QFont &font) {
        const QFontMetrics fm(font);
        return qMax(fm.height(), fm.boundingRect(QRect(), Qt::AlignLeft, QStringLiteral("实例 Ag")).height()); // i18n-ignore
    };
    const int minHeight =
        button->objectName() == MainWindowUI::runStateButtonName()
            ? lineHeight(button->font()) +
                  lineHeight(Theme::textFont(button->font(), Theme::TextRole::Caption)) + 8
            : 0;
    button->setStyleSheet(
        QStringLiteral("QPushButton { border: none; background: transparent; text-align: left; "
                       "padding: %3; border-radius: %4px; min-height: %5px; }"
                       "QPushButton:hover { background-color: %1; }"
                       "QPushButton:pressed { background-color: %2; }"
                       // 运行状态入口空闲时：不高亮、不按下去（见 setRunPanelAvailable()）
                       "QPushButton[wtfmlIdle=\"true\"]:hover, QPushButton[wtfmlIdle=\"true\"]:pressed "
                       "{ background-color: transparent; }")
            .arg(hover.name(QColor::HexArgb), pressed.name(QColor::HexArgb),
                 compact ? QStringLiteral("0 12px") : QStringLiteral("0"))
            .arg(Theme::controlRadius())
            .arg(minHeight));
    // 登记**只能登记一次**，所以这里用属性当闸门 —— 这个函数本身就在"主题变了"的回调
    // 那一支上（下面那条 lambda），里面那句 `setStyleSheet()` 又发一次 `StyleChange`。
    // 把 `Theme::follow()` 直接摆在这个函数里 ⇒ 每一趟都**再生一个**跟随者，新跟随者
    // 又同步回调进这个函数、又生一个…… 挂上主题之后进程起来直接栈溢出（`0xC00000FD`，
    // 一行输出都没有）。`PaletteFollower` 里那个 `m_running` 只拦得住**同一个**跟随者自咬，
    // 拦不住"生新的"。闸门放在这里，是因为两个调用点之外谁也够不到这一支。
    if (button->property(kBottomBarButtonFollowedProperty).toBool()) {
        return;
    }
    button->setProperty(kBottomBarButtonFollowedProperty, true);
    // 源调色板取**应用那一份**：这一条只用到 `Highlight`，而底栏身上那份只改了 `Window`
    // （所以取谁都一样）—— 取应用那份就不必操心"底栏和按钮谁先收到事件"。
    Theme::follow(button, [button]() { applyBottomBarButtonStyle(button, QApplication::palette()); });
}

// ---------------------------------------------------------------------------
// 实例页左栏「游戏文件夹」列表：一行 = 上面一个名字、下面那条路径、行尾一颗齿轮。
// ---------------------------------------------------------------------------

/// 行里两圈边距：外面那圈（行自己的内边距）比组框那圈小一点，里面两行字之间不留缝
/// （它们是一段话的两行，不是两个控件）。
constexpr int kFolderRowSideMargin = 8;
/// 文件夹浮层列表最多露几行（再多在列表里滚；5 × 56 和下拉弹层露 8 项差不多高）。
constexpr int kFolderListMaxRows = 5;
constexpr int kFolderRowTopMargin = 6;

/// 行尾那颗齿轮到自己那两行字的间距。
constexpr int kFolderRowGearSpacing = 6;

/// 齿轮菜单那块浮层自己的内边距与行距。**不共用 `kPanelPadding`**：那几个数是底栏那两块
/// 上拉面板的，菜单比它们紧一档（菜单是压在内容上的一小块，四周留白跟面板一样多就显得
/// 糊在底下那行上了）。
///
/// 取值按"左右与上下都缩到 ≈80%，上下与左右匹配"来：
/// 文字左边 8 + 6 = 14 px（框内边距 + 按钮内边距），上边 8 + **6** = 14 px
/// （按钮的上下内边距不动）—— 于是**上下与左右都是 14 px**。行距 4：
/// 行与行之间 6 + 4 + 6 = 16 px。
constexpr int kFolderMenuPadding = 8;
constexpr int kFolderMenuRowSpacing = 4;

/**
 * @brief 「游戏文件夹」列表里的一行。
 *
 * NOTE: 这两行字和行尾那颗齿轮全是**真控件**，没有一行是自绘的 —— 和底栏面板那几行
 * 同一个取法（理由也一样：真按钮白得 hover / 按下 / disabled 三态，验证工程还
 * 能按 objectName 把每一颗找回来点它）。
 *
 * NOTE: 这一行里**只有齿轮吃鼠标那一下**。那两行字是普通 QLabel（不吃鼠标事件），
 * 事件会一路冒泡到列表的 viewport，于是"点行 = 选中这一份文件夹"照旧生效 ——
 * 这和"取消小叉"那个自绘的格子正好相反（那里非拦不可，因为拦晚了会变成"点进了
 * 安装页"）。这里不拦是有意的：选中那一份文件夹是这个列表最主要的用途。
 */
class FolderRowWidget : public QWidget {
public:
    /// @param row 这一行要画什么（数据由 MainWindow 翻好递进来）。
    /// @param index 第几行（**只用来生成 objectName**）。
    /// @param onGear 行尾那颗齿轮被点了，参数是"哪一行、那颗按钮"。
    FolderRowWidget(const MainWindowUI::FolderRow &row, int index,
                    const std::function<void(const MainWindowUI::FolderRow &, QWidget *)> &onGear,
                    QWidget *parent)
        : QWidget(parent) {
        setObjectName(MainWindowUI::folderRowName(index));

        QHBoxLayout *line = new QHBoxLayout(this);
        line->setContentsMargins(kFolderRowSideMargin, kFolderRowTopMargin, kFolderRowSideMargin,
                                 kFolderRowTopMargin);
        line->setSpacing(kFolderRowGearSpacing);

        QVBoxLayout *texts = new QVBoxLayout;
        texts->setContentsMargins(0, 0, 0, 0);
        texts->setSpacing(0);

        // 上面那行：名字。正常字色、正常字号 —— 它是这一行的正文。
        QLabel *title = new ElidedLabel(this);
        title->setObjectName(MainWindowUI::folderRowTitleName(index));
        title->setText(row.title);
        texts->addWidget(title);

        // 下面那行：路径。小一号、次要颜色（和版本树那几行的第二行一个路数）。
        ElidedLabel *subtitle = new ElidedLabel(this);
        subtitle->setObjectName(MainWindowUI::folderRowSubtitleName(index));
        // **从中间省略**：路径的两头都有用（盘符 + 最后一级目录名），从尾巴截掉的恰好是
        // "这一份是哪个文件夹"唯一认得出的那截 —— 几个文件夹前缀一样时（D:/MC/a、D:/MC/b）
        // 尾巴省略会让两行看起来一模一样。见 ElidedLabel::setElideMode()。
        subtitle->setElideMode(MainWindowUI::folderRowPathElideMode());
        subtitle->setText(row.subtitle);
        QFont subtitleFont = subtitle->font();
        subtitleFont = Theme::textFont(subtitleFont, Theme::TextRole::Caption);
        subtitle->setFont(subtitleFont);
        QPalette subtitlePalette = subtitle->palette();
        subtitlePalette.setColor(QPalette::WindowText,
                                 subtitlePalette.color(QPalette::PlaceholderText));
        subtitle->setPalette(subtitlePalette);
        texts->addWidget(subtitle);

        // 份数 0：这两行字多宽就占多宽，富余的宽度都给齿轮前面那个空档 —— 齿轮因此
        // 贴在**最右边**（几行的齿轮竖着对齐）。不这么做的话它紧跟着路径，而路径一行
        // 长一行短，几颗齿轮就会错开。
        line->addLayout(texts, 0);
        line->addStretch(1);

        QPushButton *gear = new QPushButton(this);
        m_gear = gear;
        gear->setObjectName(MainWindowUI::folderRowGearName(index));
        const int iconSize = MainWindowUI::folderMenuIconSize();
        gear->setIcon(VersionIcons::actionIcon(VersionIcons::Action::Settings));
        gear->setIconSize(QSize(iconSize, iconSize));
        gear->setFixedSize(MainWindowUI::folderRowGearSize(), MainWindowUI::folderRowGearSize());
        // 行尾那颗齿轮按自己的 24 来（报一句见 `Theme::markIconButton()`：`min-height`
        // 连 `setFixedSize()` 都拦不住）。
        Theme::markIconButton(gear);
        gear->setCursor(Qt::PointingHandCursor);
        gear->setToolTip(row.assistant
                             ? QCoreApplication::translate("MainWindowUI", "更多操作。内置条目无法移除或删除。")
                             : QCoreApplication::translate("MainWindowUI", "这一份的更多操作"));
        applyPanelActionStyle(gear, palette(), false);
        // 点的是**哪一行**靠这里捕下来的值认（不认"第几行"）。菜单弹出来到用户点某一项
        // 之间隔着看两眼的时间，这中间列表是会变的（见 FolderRow::path 那段 NOTE）。
        QObject::connect(gear, &QPushButton::clicked, gear,
                         [onGear, row, gear]() { onGear(row, gear); });
        line->addWidget(gear, 0, Qt::AlignVCenter);

        // 整行的悬停提示：两行字都可能被省略号截掉（左栏只有窗口宽度的 3/13），
        // 把没截的那份原文挂在这儿。
        setToolTip(row.title + QLatin1Char('\n') + row.subtitle);
    }

    /// 行尾那颗齿轮。**只有 `setFolderRows()` 串焦点链的时候要** —— 那几颗按钮是运行时
    /// 才建的，得在它们建出来之后当场把顺序接上（见那儿那段）。
    QPushButton *gearButton() const { return m_gear; }

protected:
    // 行底（悬停那块圆角）是列表代理画在 viewport 上的，按本行 underMouse() 认：进出时让它重画。
    void enterEvent(QEnterEvent *event) override {
        QWidget::enterEvent(event);
        if (parentWidget() != nullptr) parentWidget()->update(geometry());
    }
    void leaveEvent(QEvent *event) override {
        QWidget::leaveEvent(event);
        if (parentWidget() != nullptr) parentWidget()->update(geometry());
    }

private:
    QPushButton *m_gear = nullptr;
};

/**
 * @brief 行尾那颗齿轮点出来的那个框（刷新 / 重命名 / 打开 / 移除 / 删除）。
 *
 * 用 Qt::Popup 而不是 QMenu：**不是**为了好看，是为了验证工程驱动得了 —— QMenu::exec()
 * 会阻塞在事件循环里（offscreen 下尤其没法驱动），而 Qt::Popup + show() 该白给的一样
 * 白给：点框外面自动关、Esc 关、不抢主窗口的焦点，而且**不阻塞**。
 *
 * NOTE: 它是个窗口（Qt::Popup 一定是顶层窗口），但 parent 照旧挂着 —— 这样 findChild
 * 找得到它（验证工程靠这个把它捞回来逐项点），生命周期也不用单独管。
 */
class FolderMenu : public QWidget {
public:
    /// 某一项被点了：参数是"哪一份文件夹、第几项"。
    using ActionCallback = std::function<void(const QString &, int)>;

    /// @param row 给谁弹的（决定哪几项是灰的）。
    /// @param anchor 贴着哪颗齿轮弹（也用来认"再点一次是收回"）。
    /// @param onAction 某一项被点了该告诉谁（可以给空的 = 点了没反应）。
    FolderMenu(const MainWindowUI::FolderRow &row, QWidget *anchor,
               const ActionCallback &onAction, QWidget *parent)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), m_anchor(anchor), m_path(row.path) {
        setObjectName(MainWindowUI::folderMenuName());
        // 底色比内容区深一档：它是压在列表上面的一块，得从底下那堆东西里分出来。
        // **不写死颜色**：从当前调色板推出来，浅色/深色主题下都成立（和底栏、面板同一个路数）。
        // 这一族（浮层）那一档由 `Theme::partBackground(Part::Panel)` 说了算。
        // NOTE: 每次现建 ⇒ 不用挂 follow()（和底栏面板同一条）。
        // 17 号反馈之后浮层只有一种面：磨砂玻璃（窗外挂系统瞬态亚克力），不再铺不透明底。
        Popups::preparePopup(this);
        setAutoFillBackground(false);
        const QPalette menuPalette = palette();  // 里面那几项的外观仍按调色板取色

        // 点某一项要**先把自己收回去**，再把动作交给外面 —— 那五项里有两项要弹模态窗
        // （重命名那个输入框、删除那个确认框），而这块框是个 Qt::Popup，按着鼠标键盘的
        // grab。模态窗开在一块抓着手柄的东西底下，Windows 上就是"窗出来了但点不动"。
        //
        // NOTE: context 用 parent（列表那一头）而**不是** this：这个框是 WA_DeleteOnClose，
        // close() 之后它随时会被删掉，捕它等于捕一个随时没了的对象。
        QWidget *const deferContext = parent;
        const auto dispatch = [this, deferContext, onAction](int action) {
            const QString path = m_path;
            close();
            if (!onAction) {
                return;
            }
            // 再排到**下一轮事件循环**：close() 只是发了个关闭事件，要等这一轮走完才真的
            // 没了 —— 当轮就弹模态窗等于没关（上面那条毛病一样在）。
            QTimer::singleShot(0, deferContext, [onAction, path, action]() {
                onAction(path, action);
            });
        };

        const int iconSize = MainWindowUI::folderMenuIconSize();
        QVBoxLayout *box = new QVBoxLayout(this);
        box->setContentsMargins(kFolderMenuPadding, kFolderMenuPadding, kFolderMenuPadding,
                                kFolderMenuPadding);
        box->setSpacing(kFolderMenuRowSpacing);
        for (int a = 0; a < MainWindowUI::folderActionCount(); ++a) {
            // 「移除」和「删除」对自带那一份是灰的：它移不掉（FolderManager::removeFolder）
            // 也删不掉（deleteFolder）。灰掉必须**配一句为什么** —— 灰着不说话，用户只会
            // 以为界面坏了。
            //
            // NOTE: 这个判断要在**画图标之前**算出来（下面那条彩色规则要用它）。
            const bool protectedFolder = row.assistant &&
                                         (a == MainWindowUI::FolderActionRemove ||
                                          a == MainWindowUI::FolderActionDelete);
            QPushButton *button = new QPushButton(this);
            button->setObjectName(MainWindowUI::folderMenuActionName(a));
            button->setText(MainWindowUI::folderActionText(a));
            // 字形颜色：「删除」那一项的垃圾桶跟着它的字一起红。
            // 灰掉的那一档仍走正文色 —— 和字那边是同一条规矩（见 folderActionStyle 里
            // 的 `:enabled`）：红图标落在灰按钮上会被读成"这一项现在能点"。
            button->setIcon(QIcon(MainWindowUI::folderActionIcon(
                a, iconSize,
                (a == MainWindowUI::FolderActionDelete && !protectedFolder)
                    ? MainWindowUI::folderDangerColor(menuPalette)
                    : menuPalette.color(QPalette::WindowText))));
            if (a == MainWindowUI::FolderActionRefresh)
                button->setIcon(VersionIcons::actionIcon(VersionIcons::Action::Refresh));
            else if (a == MainWindowUI::FolderActionRename)
                button->setIcon(VersionIcons::actionIcon(VersionIcons::Action::Edit));
            else if (a == MainWindowUI::FolderActionOpen)
                button->setIcon(VersionIcons::actionIcon(VersionIcons::Action::Folder));
            else if (a == MainWindowUI::FolderActionDelete)
                button->setIcon(VersionIcons::actionIcon(VersionIcons::Action::Delete));
            button->setIconSize(QSize(iconSize, iconSize));
            // 图标放在文字**前面**：QPushButton 本来就是这个摆法（图标在左、文字在右），
            // 前提是别把 text-align 设成居中 —— 下面那行 QSS 设的是 left，于是这一整块
            // （图标 + 文字）从左边起排，看着才是"菜单"而不是"一排居中按钮"。
            button->setStyleSheet(folderActionStyle(a, menuPalette));
            button->setEnabled(!protectedFolder);
            button->setToolTip(protectedFolder
                                   ? QCoreApplication::translate("MainWindowUI", "默认游戏目录，此项操作不可用")
                                   : MainWindowUI::folderActionTooltip(a));
            button->setCursor(Qt::PointingHandCursor);
            QObject::connect(button, &QPushButton::clicked, button,
                             [dispatch, a]() { dispatch(a); });
            box->addWidget(button);
        }
    }

    /// 贴的是哪颗齿轮（"再点一次收回"要按它认人：点的是同一颗才算收回）。
    QWidget *anchor() const { return m_anchor; }

    /// 摆到 @p globalTopLeft 这个位置并显示出来（超出屏幕时往里收）。
    void popUpAt(const QPoint &globalTopLeft) {
        adjustSize();
        QPoint target = globalTopLeft;
        // 贴着行尾那颗齿轮弹，左栏又在窗口靠左的位置，所以整块框是往**右**展开的 ——
        // 窗口窄的时候它会伸到屏幕外，得往回拉一下（拉的是整块框，不是它里面某一项）。
        if (const QScreen *screen = QGuiApplication::screenAt(globalTopLeft)) {
            const QRect available = screen->availableGeometry();
            target.setX(qBound(available.left(), target.x(),
                               qMax(available.left(), available.right() - width() + 1)));
            target.setY(qBound(available.top(), target.y(),
                               qMax(available.top(), available.bottom() - height() + 1)));
        }
        move(target);
        show();
        raise();
        // 拿焦点只为一件事：Esc 能关（外面点一下 Qt 自己会关，那条不用管）。
        setFocus(Qt::PopupFocusReason);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            close();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    /// 点在**自己那颗齿轮**上 = "再点一次收回"，这里自己把框关掉（和底栏那块面板同一个
    /// 理由：Qt::Popup 有把这一下重放给下面控件那条路数，两条路都只关不开，所以怎么点
    /// 都不会再冒出一块新的）。
    void mousePressEvent(QMouseEvent *event) override {
        if (m_anchor != nullptr &&
            m_anchor->rect().contains(m_anchor->mapFromGlobal(event->globalPosition().toPoint()))) {
            close();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    /// 一圈细边：同一档深浅在浅色主题下几乎看不出边界，分不出"框到哪为止"。
    void paintEvent(QPaintEvent *) override {
        // 磨砂玻璃那一层（圆角 14 + 1px 亮边），和其他浮层同一套；不再画那圈 Mid 细边。
        QPainter painter(this);
        Popups::paintSurface(painter, QRectF(rect()), 14);
    }

private:
    /// 菜单里一项的外观：不画自己的底、只在鼠标划过时淡淡蒙一层（颜色从调色板取，
    /// **不写死**），文字一律左对齐，**字一律加粗**（一块压在上面的
    /// 浮层，字不沉一点读起来像被底下的内容冲淡了）。「删除」那一项的字色是全仓唯一
    /// 一处红字。
    static QString folderActionStyle(int action, const QPalette &menuPalette) {
        QColor hover = menuPalette.color(QPalette::Highlight);
        hover.setAlpha(48);
        QColor pressed = menuPalette.color(QPalette::Highlight);
        pressed.setAlpha(96);
        // 左右各留 6 px：图标贴着框边看着像被裁过，但再多就显得这块框糊在底下的行上了
        // （8，跟着 kFolderMenuPadding 一起收紧 —— 两个数怎么来的都写在
        // kFolderMenuPadding 上面那段）。
        // NOTE: 加粗走 QSS 的 `font-weight`（别去 setFont() 改控件的字体）—— 这条样式
        // 表的**每一档状态**都是这里出的，字重跟别的属性待在同一个地方才不会漏。
        // NOTE: 那句 `min-height: 0px` 是**复位**，不是"这一项要更矮"。应用级
        // `QPushButton { min-height: 20px }` 会把这一项的内容格撑到 20，图标（16）在
        // 那一格里居中 ⇒ 上下的留白变成 6+2，用户要的"上下和左右一样宽"就差了 2 px。
        // 复位之后内容格由那一行字自己定，图标离边正好 6。
        QString sheet = QStringLiteral("QPushButton { border: none; background: transparent;"
                                       " font-weight: bold;"
                                       " text-align: left; padding: 6px 6px; min-height: 0px; }"
                                       "QPushButton:hover:enabled { background-color: %1; }"
                                       "QPushButton:pressed:enabled { background-color: %2; }")
                            .arg(hover.name(QColor::HexArgb), pressed.name(QColor::HexArgb));
        if (action == MainWindowUI::FolderActionDelete) {
            // 带 `:enabled` 是必须的：「删除」对自带那一份是**灰的**，红字落在灰按钮上
            // 会读成"这一项现在能点"（而它点不动）。灰的时候一律跟着样式走（和别的项一样）。
            sheet += QStringLiteral("QPushButton:enabled { color: %1; }")
                         .arg(MainWindowUI::folderDangerColor(menuPalette).name());
        }
        return sheet;
    }

    QWidget *m_anchor = nullptr;
    QString m_path;
};

/**
 * @brief 实例页右栏那张树里**组行行尾那颗齿轮**点出来的框。
 *
 * 两套构造，同一个类：
 * * **动作菜单**（默认）：这一组能做什么（移入分组… / 都移出分组 / 重命名 / 默认展开 /
 * 删除）；
 * * **挑目标**：把**选中的那个实例**移进哪一组（列出全部组，含一个实例都没有的
 * 那些 —— 空组在树里是藏着的，这是唯一能把第一个实例放进去的入口）。
 *
 * NOTE: 骨架是从 FolderMenu 抄的（Qt::Popup + 点框外/Esc 自己关 + "再点同一颗齿轮
 * 收回" + 按屏幕收边 + 点某项先关窗、排到下一轮事件循环再交出去；那边每一条的
 * **理由**都写在它的注释里，这里不重复）。两块真正的差别只有两点：
 * · 认"点的是不是同一颗齿轮"用的是一个**锚点矩形**（行是代理画的，压根没有
 * 控件可指）；于是"收回"那一下还得压 Qt 的重放 —— 见 handleAnchorPress()；
 * · 动作菜单里有一项（移入分组…）要**再弹第二个框**，所以这一块的父窗口
 * 底下可能同时存在两"代"。
 * **要改这套骨架时两块一起改**（文件夹那一块叫 FolderMenu，就在上面）。
 */
class GroupMenu : public QWidget {
public:
    /// 动作菜单里某一项被点了：参数是"哪一组、第几项"。
    using ActionCallback = std::function<void(const QString &, int)>;
    /// 挑目标那个框里某一组被点了：参数是目标组 id（空串 = 未分组）。
    using PickCallback = std::function<void(const QString &)>;

    /// @param row 给哪一组弹的（决定哪几项显示、哪几项是灰的）。
    /// @param anchorGlobal 那颗齿轮占的**全局**矩形（贴它弹，也用来认"再点一次是收回"）。
    /// @param replayHost 接得住 Qt 那记重放的控件（树的 viewport，见 handleAnchorPress()）。
    GroupMenu(const MainWindowUI::GroupRow &row, const QRect &anchorGlobal,
              const ActionCallback &onAction, QWidget *replayHost, QWidget *parent)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint),
          m_anchorGlobal(anchorGlobal),
          m_groupId(row.groupId),
          m_replayHost(replayHost) {
        setObjectName(MainWindowUI::groupMenuName());
        applyMenuPalette();

        QWidget *const deferContext = parent;
        const auto dispatch = [this, deferContext, onAction](int action) {
            const QString groupId = m_groupId;
            close();
            if (!onAction) {
                return;
            }
            // 再排到**下一轮事件循环**：close() 只是发了个关闭事件，要等这一轮走完才真的
            // 没了 —— 当轮就弹模态窗等于没关（和文件夹那块一模一样，那里有完整一段）。
            QTimer::singleShot(0, deferContext, [onAction, groupId, action]() {
                onAction(groupId, action);
            });
        };

        QVBoxLayout *box = new QVBoxLayout(this);
        box->setContentsMargins(kFolderMenuPadding, kFolderMenuPadding, kFolderMenuPadding,
                                kFolderMenuPadding);
        box->setSpacing(kFolderMenuRowSpacing);
        const QPalette menuPalette = palette();
        for (int a = 0; a < MainWindowUI::groupActionCount(); ++a) {
            // 虚拟的「未分组」只剩第一项（见 groupActionVisible 那段）。
            if (!MainWindowUI::groupActionVisible(a, row)) {
                continue;
            }
            QPushButton *button = new QPushButton(this);
            button->setObjectName(MainWindowUI::groupMenuActionName(a));
            button->setText(MainWindowUI::groupActionText(a, row));
            button->setStyleSheet(entryStyle(a == MainWindowUI::GroupActionDelete, menuPalette));
            // 灰掉的那几项必须**配一句为什么**（灰着不说话用户只会以为界面坏了）——
            // 提示那一句由 MainWindowUI::groupActionTooltip() 给，它那边已经分好了
            // "能点时说什么 / 不能点时说什么"。
            button->setEnabled(MainWindowUI::groupActionEnabled(a, row));
            button->setToolTip(MainWindowUI::groupActionTooltip(a, row));
            button->setCursor(Qt::PointingHandCursor);
            QObject::connect(button, &QPushButton::clicked, button, [dispatch, a]() { dispatch(a); });
            box->addWidget(button);
        }
    }

    /// 挑目标：@p rows 是**全部**分组（含一个实例都没有的、含虚拟的「未分组」），
    /// 顺序就是显示顺序。
    GroupMenu(const QList<MainWindowUI::GroupRow> &rows, const QRect &anchorGlobal,
              const PickCallback &onPick, QWidget *replayHost, QWidget *parent)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint),
          m_anchorGlobal(anchorGlobal),
          m_pick(true),
          m_replayHost(replayHost) {
        setObjectName(MainWindowUI::groupMenuName());
        applyMenuPalette();

        QWidget *const deferContext = parent;
        const auto dispatch = [this, deferContext, onPick](const QString &groupId) {
            close();
            if (!onPick) {
                return;
            }
            QTimer::singleShot(0, deferContext, [onPick, groupId]() { onPick(groupId); });
        };

        QVBoxLayout *box = new QVBoxLayout(this);
        box->setContentsMargins(kFolderMenuPadding, kFolderMenuPadding, kFolderMenuPadding,
                                kFolderMenuPadding);
        box->setSpacing(kFolderMenuRowSpacing);
        const QPalette menuPalette = palette();
        for (int i = 0; i < rows.size(); ++i) {
            const MainWindowUI::GroupRow &row = rows.at(i);
            QPushButton *button = new QPushButton(this);
            // 名字按下标给（0 起，就是递进来的顺序）：验证工程按它取控件，而组名是用户
            // 起的、随时会变，拿文字当身份标识必错（和文件夹那几行同一条）。
            button->setObjectName(MainWindowUI::groupPickRowName(i));
            // 文字带上实例数（和树上那个组头一模一样）：挑的时候"这一组里已经有几个了"
            // 是个有用的信息，而空组在这张表里是**看得见的**（树上它藏着）。
            button->setText(MainWindowUI::groupItemText(row.name, row.memberCount));
            button->setStyleSheet(entryStyle(false, menuPalette));
            // 选中的那个实例**已经在这一组里**的那一条灰掉：点它等于什么都没做，
            // 而"什么都没做"和"点了没反应"在用户眼里是一回事。
            button->setEnabled(!row.containsSelected);
            button->setToolTip(row.containsSelected
                                   ? QCoreApplication::translate("MainWindowUI", "选中的实例已经在这一组里了")
                                   : QCoreApplication::translate("MainWindowUI", "把这个实例放到这一组里"));
            button->setCursor(Qt::PointingHandCursor);
            const QString groupId = row.groupId;
            QObject::connect(button, &QPushButton::clicked, button,
                             [dispatch, groupId]() { dispatch(groupId); });
            box->addWidget(button);
        }
    }

    /// 这块框是"挑目标"那一套吗（真假两块共用一个 objectName，验证工程靠它分开）。
    bool pickMode() const { return m_pick; }

    /// "再点一次同一颗齿轮 = 收回"认的人：动作菜单是它伺候的那一组，挑目标那个框是空串
    /// （它不认人 —— 同一颗齿轮下第二次点进来时，上面那块动作菜单已经不在了）。
    QString groupId() const { return m_groupId; }

    /// 摆到 @p globalTopLeft 这个位置并显示出来（超出屏幕时往里收）。理由和 FolderMenu
    /// 那个 ::popUpAt() 一模一样：贴着行尾那颗齿轮弹，而它在窗口靠右的位置。
    void popUpAt(const QPoint &globalTopLeft) {
        adjustSize();
        QPoint target = globalTopLeft;
        if (const QScreen *screen = QGuiApplication::screenAt(globalTopLeft)) {
            const QRect available = screen->availableGeometry();
            target.setX(qBound(available.left(), target.x(),
                               qMax(available.left(), available.right() - width() + 1)));
            target.setY(qBound(available.top(), target.y(),
                               qMax(available.top(), available.bottom() - height() + 1)));
        }
        move(target);
        show();
        raise();
        // 拿焦点只为一件事：Esc 能关（外面点一下 Qt 自己会关，那条不用管）。
        setFocus(Qt::PopupFocusReason);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            close();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (handleAnchorPress(event)) {
            return;
        }
        QWidget::mousePressEvent(event);
    }

    /// 点得**快**的时候，第二发及以后到的是双击事件（Windows 上就是它，见
    /// ui/RowActivation.hpp 那段）—— 同一个处理，不接的话"再点一下收回"就成了白点。
    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (handleAnchorPress(event)) {
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

    /// 一圈细边（和 FolderMenu 那条同一个理由）：同一档深浅在浅色主题下几乎看不出边界，
    /// 分不出"框到哪为止"。
    void paintEvent(QPaintEvent *) override {
        // 磨砂玻璃那一层（圆角 14 + 1px 亮边），和其他浮层同一套；不再画那圈 Mid 细边。
        QPainter painter(this);
        Popups::paintSurface(painter, QRectF(rect()), 14);
    }

private:
    /// 底色比内容区深一档（从调色板推，**不写死颜色**）—— 和 FolderMenu 那条一样。
    /// 那一档由 `Theme::partBackground(Part::Panel)` 说了算（这一族浮层共用一个色）。
    void applyMenuPalette() {
        // 不再铺一块不透明面板色：浮层的面由 `Popups::paintSurface()` 画（磨砂玻璃）。
        Popups::preparePopup(this);
        setAutoFillBackground(false);
    }

    /// 菜单里一项的外观。**和 FolderMenu::folderActionStyle() 逐字同形** —— 两块框是
    /// 同一族浮层（都是行尾齿轮弹出来的一列按钮），字重/对齐/留白/红字那几条理由写在
    /// 那边，这里不重复。差别只有"哪一项是红的"由参数给（那边写死是「删除」）。
    static QString entryStyle(bool danger, const QPalette &menuPalette) {
        QColor hover = menuPalette.color(QPalette::Highlight);
        hover.setAlpha(48);
        QColor pressed = menuPalette.color(QPalette::Highlight);
        pressed.setAlpha(96);
        QString sheet = QStringLiteral("QPushButton { border: none; background: transparent;"
                                       " font-weight: bold;"
                                       " text-align: left; padding: 6px 6px; min-height: 0px; }"
                                       "QPushButton:hover:enabled { background-color: %1; }"
                                       "QPushButton:pressed:enabled { background-color: %2; }")
                            .arg(hover.name(QColor::HexArgb), pressed.name(QColor::HexArgb));
        if (danger) {
            // 带 `:enabled` 是必须的：红字落在灰按钮上会被读成"这一项现在能点"（和
            // folderActionStyle() 里那条注释同一个理由）。
            sheet += QStringLiteral("QPushButton:enabled { color: %1; }")
                         .arg(MainWindowUI::groupDangerColor(menuPalette).name());
        }
        return sheet;
    }

    /// 落在**自己那颗齿轮**上 = "再点一次收回"。
    ///
    /// NOTE: 【为什么要压 Qt 的重放】和底栏那两块面板同一条：Qt 关一块 Qt::Popup 的时候，
    /// 如果光标不在它几何里，会把这同一个 press **重放**给光标底下那个控件。那颗
    /// 齿轮在**树的行里**，重放那一下落到 viewport 上 —— 而那一格正是"点齿轮"的
    /// 命中格（见 MainWindow::eventFilter），于是菜单刚收回去又会被自己弹开，看着
    /// 就是"收不回去"。`WA_NoMouseReplay` 干的就是这个，**必须在 close() 之前钉**
    /// （Qt 是关窗那一刻读它；属性会被 Qt 在"这个控件又收到鼠标事件"时清掉，所以
    /// 挂在构造里没用）。popup 这一侧和**接重放那一侧**都钉：文档说这个属性是给
    /// popup 用的，但重放是"投递给下面那个控件"的，两处都钉才两种路径都盖住。
    /// @return true = 这一下已经办完了（不要再去走基类那套）。
    bool handleAnchorPress(QMouseEvent *event) {
        if (!m_anchorGlobal.contains(event->globalPosition().toPoint())) {
            return false;  // 落在框自己身上（或者别处）：照旧往下走
        }
        setAttribute(Qt::WA_NoMouseReplay, true);
        if (m_replayHost != nullptr) {
            m_replayHost->setAttribute(Qt::WA_NoMouseReplay, true);
        }
        close();
        event->accept();
        return true;
    }

    /// 那颗齿轮占的全局矩形（贴它弹、也按它认"再点一次"）。
    QRect m_anchorGlobal;
    /// 动作菜单伺候的是哪一组（挑目标那个框不用它，留空串）。
    QString m_groupId;
    bool m_pick = false;
    QWidget *m_replayHost = nullptr;
};

}  // namespace

void MainWindowUI::setupBottomBar(QWidget *parent) {
    // ---------- 底部通栏状态条（Steam 风格） ----------
    // 三个格子：左 = 实例运行状态，中 = 启动/下载进度，右 = 头像与用户名。
    // 这里只**搭**它：显示什么由 MainWindow::updateBottomBar() 决定，
    // 界面这边不持有任何业务状态。
    //
    // NOTE: 两头是实例状态在左、头像在右。底下那几个 stretch 围绕中间那格配，
    // 两头对调时中间那格的份数原样不动，进度条仍然居中。
    bottomBar = new QFrame(parent);
    bottomBar->setObjectName(bottomBarName());
    bottomBar->setFrameShape(QFrame::NoFrame);
    // 现在它是左导航轨底部那一块（竖排三格），和轨同一层底：不铺色块。
    bottomBar->setAutoFillBackground(false);
    bottomBar->setPalette(bottomBarChromePalette());
    // 底栏是**常驻**的（不像浮层那样每次现建）⇒ 主题一换它得自己重算一遍。
    Theme::follow(bottomBar, [this]() { bottomBar->setPalette(bottomBarChromePalette()); });

    QVBoxLayout *barLayout = new QVBoxLayout(bottomBar);
    barLayout->setContentsMargins(0, 8, 0, 0);
    barLayout->setSpacing(4);

    // ---------- 左：当前实例运行状态 ----------
    // 第一行是状态正文，第二行是实例名。**在主页且选中的就是正在跑的那个实例**时
    // 只显示第一行 —— 那种情况下用户眼前就是那个实例，再说一遍名字是废话。
    // 最左边那颗**状态方块**（红/黄/绿， 加的）。它是**第一行的一部分**，
    // 所以下面把第一行做成"方块 + 文字"的一个横排，而不是把方块摆在整格的最左边 ——
    // 摆整格最左边的话，显示第二行时方块会停在两行的中间（VB 居中那一档），
    // 看着不属于任何一行。
    //
    // NOTE: 这里**只是把图摆出来**，颜色由 MainWindow 按状态来要（runStateMarkPixmap()）。
    // 方块自己不知道现在是哪个状态 —— 那件事的判据在 MainWindow::updateBottomBar()
    // 里（"有没有正在启动的会话、有没有正在跑的进程"），这儿是 ui/ 层，看不到那些。
    runStateMarkLabel = new QLabel(bottomBar);
    runStateMarkLabel->setObjectName(runStateMarkName());
    runStateMarkLabel->setFixedSize(MainWindowUI::runStateMarkSize(),
                                    MainWindowUI::runStateMarkSize());

    runStateLabel = new ElidedLabel(bottomBar);
    runStateLabel->setObjectName(runStateLabelName());
    // 贴在底栏最左边，所以文字左对齐
    runStateLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // 宽度封顶（理由见头文件那段）：多开之后这几行字会随"跑着几个、都叫什么名字"变长，
    // 宽度跟着变就会把中间那根进度条挤得长短不一。
    runStateLabel->setMaximumWidth(MainWindowUI::bottomBarRunStateTextWidth());

    runStateDetailLabel = new ElidedLabel(bottomBar);
    runStateDetailLabel->setObjectName(runStateDetailLabelName());
    runStateDetailLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // 这一格的宽度上限 = **第一行那个数 + 方块那一段**。两行的"少掉 16 px"不是一回事：
    // 第一行那个方块是布局里**单独的控件**（它占的是上限**之外**的位置，所以那 136 全是
    // 给字的），第二行的缩进走的是 contentsMargins —— 那 16 px 是**从上限里面**扣的。
    // 加回一段之后：两行留给字的宽度都是 136，整格的总宽也都是 152。
    runStateDetailLabel->setMaximumWidth(MainWindowUI::bottomBarRunStateTextWidth() +
                                         MainWindowUI::bottomBarRunStateMarkWidth());
    // 第二行**缩进方块那一段**，两行文字的左边才对得齐。不缩进的话第一行比第二行多出
    // 16 px，两行会看着像各说各的。
    runStateDetailLabel->setContentsMargins(MainWindowUI::bottomBarRunStateMarkWidth(), 0, 0, 0);
    // 第二行小一号、灰一点，跟第一行区分开。字号按当前字号减 1（不用点值，
    // 免得不支持点值的平台出现负字号）；颜色用调色板里的 PlaceholderText。
    QFont detailFont = runStateDetailLabel->font();
    detailFont = Theme::textFont(detailFont, Theme::TextRole::Caption);
    runStateDetailLabel->setFont(detailFont);
    QPalette detailPalette = runStateDetailLabel->palette();
    detailPalette.setColor(QPalette::WindowText, detailPalette.color(QPalette::PlaceholderText));
    runStateDetailLabel->setPalette(detailPalette);

    // 第一行 = 方块 + 文字。方块那一格**不带 stretch**（它是固定的 10 px），文字那格带 1。
    // 竖直方向上文字是 VCenter、方块也是 VCenter，两者因此居中对齐 —— 方块比文字矮一号，
    // 居中之后看着就跟这一行是一体的。
    QHBoxLayout *runStateTitleRow = new QHBoxLayout();
    runStateTitleRow->setContentsMargins(0, 0, 0, 0);
    runStateTitleRow->setSpacing(MainWindowUI::runStateMarkSpacing());
    runStateTitleRow->addWidget(runStateMarkLabel, 0, Qt::AlignVCenter);
    runStateTitleRow->addWidget(runStateLabel, 1);

    QVBoxLayout *runStateLayout = new QVBoxLayout();
    runStateLayout->setSpacing(0);
    runStateLayout->addLayout(runStateTitleRow);
    runStateLayout->addWidget(runStateDetailLabel);
    runStateLayout->setContentsMargins(0, 0, 0, 0);

    // 这一整格是一颗**按钮**：点它向上弹出实例那块面板。
    // 按钮的宽度是**钉死**的，所以这一格的宽度跟"里面有没有字、字有多长"彻底没关系。
    //
    // NOTE: 宽度钉在**按钮**上、不钉在里面那两行字上，是量出来的结论：那两行在没有实例
    // 在跑时是藏起来的，而 **Qt 的布局把藏起来的格子当成宽度 0** —— 钉在字上的话，
    // 「下载中」到「启动起来」这一步就是 0 → 200，中间那根条当场从 582 缩到 440
    // （**142 px**），"实例运行中一出来条就变长变短"量到的就是这个数。
    // 钉在按钮上，这一格就一直在。
    // 按钮里那个 layout 的边距要显式清成 0：**套在控件里的 layout 默认会拿样式给的
    // 边距**（套在 layout 里的不会），不清的话这格是 200 加左右两条边距。
    runStateButton = new RunStateButton(bottomBar);
    runStateButton->setObjectName(runStateButtonName());
    // 展开轨里整格 184，图标轨里跟着轨走（轨宽动画途中也连续），所以只封顶、不钉死。
    runStateButton->setMinimumWidth(0);
    runStateButton->setMaximumWidth(MainWindowUI::bottomBarRunStateColumnWidth());
    // NOTE: **竖直方向必须自己说要铺满**。QPushButton 的竖直 sizePolicy 默认是 Fixed，
    // 而它这会儿的高度提示只有一行字那么点（QSS 把 padding 和 border 都清零了，
    // sizeHint 高 12 px）—— 结果这一格只有 12 px 高，里面那两行字被压成各 6 px
    // （"文字和箭头挤在一起、显示不全"），鼠标划过时那块
    // 高亮框也成了一长条。宽度钉死 + 高度铺满，这一格才真的是"占住底栏那一块"。
    runStateButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    runStateButton->setCursor(Qt::PointingHandCursor);
    runStateButton->setToolTip(MainWindowUI::bottomBarRunPanelTip());
    applyBottomBarButtonStyle(runStateButton, bottomBar->palette());
    QHBoxLayout *runStateRow = new QHBoxLayout(runStateButton);
    runStateRow->setContentsMargins(12, 4, 8, 4);
    runStateRow->setSpacing(MainWindowUI::bottomBarChevronSpacing());
    // 文字那一块装在一个透明格子里：图标轨里整块收起（见 RunStateButton），只画图标和进程数。
    auto *runStateTextBox = new QWidget(runStateButton);
    runStateTextBox->setObjectName(RunStateButton::textBoxName());
    runStateTextBox->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    runStateTextBox->setLayout(runStateLayout);
    runStateRow->addWidget(runStateTextBox, 1);
    // 右侧不挂朝上的小箭头（体验反馈 8）：点得开由悬停底色、手型指针和悬停提示说明。

    // 没有实例在跑时这几样藏着。**藏的是字和那颗方块，不是格子** —— 格子（上面那颗
    // 按钮）永远占着那 170 px（宽度是 bottomBarRunStateColumnWidth()）。
    // NOTE: 方块和那两行一起 hide() 是初值：产品里 MainWindow 每次重画底栏都会摆一个状态
    // （三个状态各有一句话，见 updateBottomBar()），所以这之后它一直是亮的。
    runStateMarkLabel->hide();
    runStateLabel->hide();
    runStateDetailLabel->hide();

    // ---------- 中：任务摘要（点一下展开任务中心） ----------
    // 这一格**也是一整颗按钮**（整块可点击），做法跟两头那两颗一样：
    // 悬停高亮走 applyBottomBarButtonStyle()、整格都是一个点击区。
    // 宽度**不钉**（两头那两颗才钉）—— 中间这格本来就是"吃掉富余宽度"的那一份。
    taskSummaryButton = new QPushButton(bottomBar);
    taskSummaryButton->setObjectName(taskSummaryButtonName());
    // 竖直方向铺满，理由同另外两颗（QPushButton 默认 Fixed，会让这一格只有一行字那么高）。
    taskSummaryButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    taskSummaryButton->setCursor(Qt::PointingHandCursor);
    taskSummaryButton->setToolTip(MainWindowUI::taskSummaryTip());
    applyBottomBarButtonStyle(taskSummaryButton, bottomBar->palette());

    // 里面一列两样：上面"标题 + 右边那句小字 + 箭头"，下面那根进度条。
    // 设计稿：左边 28 的进度环，右边两行（任务名 13 / 「还有 N 项」+ 百分比 12）。
    auto *taskCard = new QHBoxLayout(taskSummaryButton);
    taskCard->setContentsMargins(10, 8, 10, 8);
    // QPushButton 的 sizeHint 只算自己那行字（22 高），里面这套布局不算进去 ⇒ 导航轨底部
    // 只给它 22，环和两行字被裁成半截。让按钮的最小尺寸跟着这套布局走。
    taskCard->setSizeConstraint(QLayout::SetMinimumSize);
    taskCard->setSpacing(10);
    // 两行字装在一个透明格子里：导航轨折成图标轨时整格收起，只剩那枚环。
    auto *taskTextBox = new QWidget(taskSummaryButton);
    taskTextBox->setObjectName(QStringLiteral("taskSummaryText"));
    taskTextBox->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    QVBoxLayout *taskLayout = new QVBoxLayout(taskTextBox);
    taskLayout->setContentsMargins(0, 0, 0, 0);
    taskLayout->setSpacing(0);
    QHBoxLayout *taskTextRow = new QHBoxLayout();
    taskTextRow->setContentsMargins(0, 0, 0, 0);
    taskTextRow->setSpacing(MainWindowUI::bottomBarChevronSpacing());

    // 「还没看过的一次失败」那颗红点：**收起来的**（布局里宽度是 0，所以不显示的时候
    // 右边那几样不会跟着挪）。它是个真控件，不是拼进文字里的字符 —— 图的颜色得能跟着
    // 主题走（画的，见 setBottomBarTaskSummary()）。
    taskFailureDot = new QLabel(taskSummaryButton);
    taskFailureDot->setObjectName(MainWindowUI::taskSummaryDotName());
    taskFailureDot->setFixedSize(MainWindowUI::taskCenterPanelFailureDotSize(),
                                 MainWindowUI::taskCenterPanelFailureDotSize());
    taskFailureDot->hide();
    taskTextRow->addWidget(taskFailureDot, 0, Qt::AlignVCenter);

    statusLabel = new ElidedLabel(taskSummaryButton);
    statusLabel->setObjectName(statusLabelName());
    // 固定一个最小宽度：这一格的字长长短短时，右边那几样不会再跳来跳去
    statusLabel->setMinimumWidth(1);  // 1 不是 0：0 = 交回 minimumSizeHint（整串字宽），会把轨的最小宽顶上去
    // **上限也得有**。ElidedLabel 的 sizeHint() 是把宽度封在上限上，
    // 而上限的来源就是 maximumWidth() —— 不设上限时，一句长文件名会把这一格的 sizeHint
    // 顶到 3912、把底栏顶到 4338、把**主窗口**顶到 4338（默认窗口只有 900 宽，见
    // ui_check 的 E5 段）。理由全文（含"最小宽度那一半其实不受影响"）见头文件那段。
    statusLabel->setMaximumWidth(MainWindowUI::bottomBarStatusTextWidth());
    // NOTE: 它是 ElidedLabel（**不是**普通 QLabel）：中间那格比这句长的时候（任务标题长、
    // 窗口窄）普通 QLabel 是**硬裁**的 —— 最后一个字被切掉一半就杵在那儿。
    // 超出去的部分由它自己截成省略号。上下两个宽度一夹，布局就钉死了。
    taskTextRow->addWidget(statusLabel, 0, Qt::AlignVCenter);

    // 弹簧摆在**标题和小字之间**（不是小字右边）：要的是「在**右侧**写'另有 2 项'」，
    // 而标题那一格有自己的上限（320），弹簧在中间才把那句小字推到右边去。
    taskTextRow->addStretch(1);

    // 「另有 N 项」那句小字。宽度封顶（`taskExtraTextWidth()`）—— 它要是能被一句长话顶宽，
    // 中间那格就会把旁边那颗按钮挤走（E5 那一段量的就是这件事）。
    taskExtraLabel = new ElidedLabel(taskSummaryButton);
    taskExtraLabel->setObjectName(taskExtraLabelName());
    taskExtraLabel->setMaximumWidth(MainWindowUI::taskExtraTextWidth());
    taskExtraLabel->setMinimumWidth(0);
    {
        QFont extraFont = taskExtraLabel->font();
        extraFont = Theme::textFont(extraFont, Theme::TextRole::Caption);
        taskExtraLabel->setFont(extraFont);
        QPalette extraPalette = taskExtraLabel->palette();
        extraPalette.setColor(QPalette::WindowText,
                              extraPalette.color(QPalette::PlaceholderText));
        taskExtraLabel->setPalette(extraPalette);
    }
    // 这一格不再挂朝上的小箭头：它离那句状态隔了大半条底栏，看上去是一个不知所指的「^」。
    // 整格本身就是按钮（悬停有底色、有提示），点开任务中心不靠那颗箭头提示。
    taskLayout->addLayout(taskTextRow);

    // 下面那一行：自绘的细轨道 + 右边一格百分比（底栏 / 面板 / 详情页三处统一到
    // 同一条自绘组件上）。条**不自己画数**（`TaskProgressBar` 只管画轨道）—— 所以数挪到
    // 它右边这一格里，和任务详情页那一行（条在左、数在右）同构。
    auto *taskProgressRow = new QHBoxLayout();
    taskProgressRow->setContentsMargins(0, 0, 0, 0);
    taskProgressRow->setSpacing(8);
    taskProgressRow->addWidget(taskExtraLabel, 0, Qt::AlignVCenter);
    taskProgressRow->addStretch(1);
    progressBar = new TaskProgressBar(taskSummaryButton);
    progressBar->setObjectName(progressBarName());
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setRing(true);
    progressBar->setFixedSize(28, 28);
    // 导航收起时任务卡只剩这枚环、标题前那颗红点跟着字藏了：未读失败改挂到环的右上角。
    // 它是环的子控件、落在环那 28×28 之内 —— 裁不掉，也不进布局（不会把图标轨撑宽）。
    taskFailureBadge = new QLabel(progressBar);
    taskFailureBadge->setObjectName(MainWindowUI::taskSummaryBadgeName());
    taskFailureBadge->setFixedSize(MainWindowUI::taskCenterPanelFailureDotSize(),
                                   MainWindowUI::taskCenterPanelFailureDotSize());
    taskFailureBadge->move(progressBar->width() - taskFailureBadge->width(), 0);
    taskFailureBadge->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    taskFailureBadge->hide();
    taskCard->addWidget(progressBar, 0, Qt::AlignVCenter);
    taskCard->addWidget(taskTextBox, 1);
    taskPercentLabel = new QLabel(taskSummaryButton);
    taskPercentLabel->setObjectName(taskPercentLabelName());
    {
        QFont pctFont = taskPercentLabel->font();
        pctFont = Theme::textFont(pctFont, Theme::TextRole::Caption);
        taskPercentLabel->setFont(pctFont);
    }
    taskProgressRow->addWidget(taskPercentLabel, 0, Qt::AlignVCenter);
    taskLayout->addLayout(taskProgressRow);

    // 一开始一项任务都没有：那一句低强调度的占位话 + **不显示空进度条**。
    // 产品里 MainWindow 每次重画底栏都会摆一份真的进来（updateBottomBar()），
    // 这里这个初值管的是**还没被谁摆过**的时候（独立构造 MainWindowUI 的地方吃它）。
    taskExtraLabel->hide();
    progressBar->hide();
    taskPercentLabel->hide();
    setBottomBarTaskSummary(BottomBarTaskSummary());
    Theme::follow(statusLabel, [this]() { setBottomBarTaskSummary(m_lastTaskSummary); });

    // ---------- 右：头像 + 用户名 ----------
    // 真正的手绘皮肤头像要等 Minecraft profile API（登录流程现在只存了令牌，没取
    // 玩家档案），这里先画一块"首字母方块"。颜色同样取自调色板，不写死。
    // NOTE: 刻意**不加 border-radius** —— 这块是方的。真皮肤头像是 8x8 的方块贴图，
    // 方形底比圆形底更接近它以后的样子。
    avatarLabel = new QLabel(QStringLiteral("?"), bottomBar);
    avatarLabel->setObjectName(avatarLabelName());
    avatarLabel->setFixedSize(kAvatarSize, kAvatarSize);
    avatarLabel->setAlignment(Qt::AlignCenter);
    // 底和字都取自调色板的两个角色（选中那一块 / 它上面的字）——**不写死颜色**。
    // NOTE: QSS 里嵌的是**色名** ⇒ 主题一换它不会自己变，所以这两句包成一个动作、
    // 除了现在跑一遍，还挂在 `Theme::follow()` 上（下面那一条）。
    const auto paintAvatar = [this]() {
        const QPalette appPalette = QApplication::palette();
        avatarLabel->setStyleSheet(
            QStringLiteral("QLabel { background-color: %1; color: %2; }")
                .arg(appPalette.color(QPalette::Highlight).name(),
                     appPalette.color(QPalette::HighlightedText).name()));
    };
    paintAvatar();
    Theme::follow(avatarLabel, [paintAvatar]() { paintAvatar(); });

    accountLabel = new ElidedLabel(bottomBar);
    accountLabel->setObjectName(accountLabelName());
    accountLabel->setText(QStringLiteral("?"));
    // 用户名给一个宽度上限：正版名最长 16 个字符，长度一变右边那格就带着进度条一起动。
    // 这里**只封顶、不钉死**（和左边那格不一样，理由见头文件那段）：这格右对齐贴着窗边，
    // 钉死会让短名字在窗口最右边留出一块空白。
    // 上限之外的名字截成省略号，不硬裁（见 ElidedLabel）。
    accountLabel->setMaximumWidth(MainWindowUI::bottomBarAccountColumnWidth());

    // 第二行：这次启动会用哪种身份（正版 / 离线 / 还没登录）。做法**照抄左上角那格**
    // （小一号 + PlaceholderText 颜色）—— 底栏左右两格在视觉上是对称的两条两行文字。
    // 文字由 MainWindow::updateBottomBar() 刷，界面这边不持有业务状态。
    accountTypeLabel = new QLabel(accountTypeNotLoggedInText(), bottomBar);
    accountTypeLabel->setObjectName(accountTypeLabelName());
    accountTypeLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QFont accountTypeFont = accountTypeLabel->font();
    accountTypeFont = Theme::textFont(accountTypeFont, Theme::TextRole::Caption);
    accountTypeLabel->setFont(accountTypeFont);
    // `QLabel` 不吃 `PlaceholderText` 这个角色（那是输入框占位用的），要把那一支**抄进
    // `WindowText`**。抄这一下不能只在构造里做一次：局部调色板里没标的角色在装上去之后
    // 会退回应用那一支，**换主题时 `PlaceholderText` 跟着变、抄进去的 `WindowText` 不动**，
    // 两行字从此一个旧色一个新色（量出来 `#64776d` 与 `#607c70` 各说各的）。所以和上面
    // 那颗头像一样包成一个动作、挂 `Theme::follow()`。
    const auto paintAccountType = [this]() {
        QPalette typePalette = accountTypeLabel->palette();
        typePalette.setColor(QPalette::WindowText,
                             typePalette.color(QPalette::PlaceholderText));
        accountTypeLabel->setPalette(typePalette);
    };
    paintAccountType();
    Theme::follow(accountTypeLabel, [paintAccountType]() { paintAccountType(); });

    QVBoxLayout *accountTextLayout = new QVBoxLayout();
    accountTextLayout->setSpacing(0);
    accountTextLayout->addWidget(accountLabel);
    accountTextLayout->addWidget(accountTypeLabel);
    accountTextLayout->setContentsMargins(0, 0, 0, 0);

    // 这一格也是**一整颗按钮**：点它向上弹出账号那块面板（和左边那颗对称）。
    // 宽度同样**钉死**，理由见头文件 bottomBarAccountButtonWidth()：里面那一组靠右摆，
    // 所以钉死之后短名字看着照样贴着窗边，而中间那根条不再受"用户名多长"影响。
    accountButton = new QPushButton(bottomBar);
    accountButton->setObjectName(accountButtonName());
    accountButton->setFixedWidth(MainWindowUI::bottomBarAccountButtonWidth());
    // 竖直方向铺满，理由同左边那颗（默认 Fixed 会让这一格只有一行字那么高）。
    accountButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    accountButton->setCursor(Qt::PointingHandCursor);
    accountButton->setToolTip(QCoreApplication::translate("MainWindowUI", "查看账号信息"));
    applyBottomBarButtonStyle(accountButton, bottomBar->palette());
    QHBoxLayout *accountRow = new QHBoxLayout(accountButton);
    accountRow->setContentsMargins(0, 0, 0, 0);
    accountRow->setSpacing(MainWindowUI::bottomBarAccountRowSpacing());
    // 头像**贴着按钮左边缘**摆（不是靠右摆）：这一格宽 208，而头像 + 名字常态只有
    // 九十来 px，靠右摆就会在头像左边留出一大段空白 ——
    // 「头像那里的左侧有一段空白」就是它（空 115 px：头像 x=115、按钮宽 208）。
    // 弹簧因此摆在名字右边：剩下的那一截落在姓名格里（姓名格自己就是这么宽的），
    // 中间那根条的位置一点都不动。
    accountRow->addWidget(avatarLabel);
    // 头像和那两行文字按竖直居中对齐（QHBoxLayout 默认就是这个），所以名字多一行之后
    // 那块方头像不会往上看。
    accountRow->addLayout(accountTextLayout);
    accountRow->addStretch(1);
    // 账号身份与入口在主页右上区域；底栏保留这组兼容控件给业务层更新数据，
    // 但不放进可见布局。工具箱入口在导航轨底部。
    accountButton->hide();

    toolboxButton = new RailNavButton(QCoreApplication::translate("MainWindowPanels", "工具箱"), bottomBar);
    toolboxButton->setObjectName(toolboxButtonName());
    toolboxButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    toolboxButton->setCursor(Qt::PointingHandCursor);
    toolboxButton->setToolTip(QCoreApplication::translate("MainWindowPanels", "工具箱"));
    toolboxButton->setCheckable(true);
    toolboxButton->setAttribute(Qt::WA_Hover, true);
    {
        QFont navFont = toolboxButton->font();
        navFont.setPixelSize(TitleBar::navFontPixelSize());
        navFont.setWeight(QFont::Medium);
        toolboxButton->setFont(navFont);
    }

    // 那两颗按钮点开都是**向上**弹一块面板（像官启那样）。面板里画什么由 MainWindow 灌
    // （每次点开现问一遍，见 setBottomBarPanelContent()），这里只留"怎么弹、弹在哪、
    // 点了哪一行该告诉谁"。左边那块（account = false）列**正在跑的实例**，右边那块
    // （account = true，也就是右对齐的那块）列**最近用过的账号**。
    // 开关的三种情形都归 toggleBottomBarPanel() 一处：点同一颗 = 收回、点另一颗 = 收旧开新、
    // 都没开着 = 开。**每一次点击都算数**，点另一颗要一条路走到位（见
    // BottomBarPanel::handleOutsidePress() 那两段 NOTE）。
    //
    // NOTE: 行里的按钮点下去不是"立刻做事"，而是"面板先收、动作排到下一轮"（见
    // BottomBarPanel 里 dispatch 那段）—— 那两个动作都要弹模态窗，压在 Qt::Popup
    // 底下会点不动。所以**别**在这里顺手补一句"点完也 close 一下"：那是同一件事
    // 做两遍，而且顺序反了就白做。
    QObject::connect(runStateButton, &QPushButton::clicked, runStateButton, [this]() {
        toggleBottomBarPanel({runStateButton, BottomBarAnchorKind::Instances});
    });
    QObject::connect(toolboxButton, &QPushButton::clicked, toolboxButton, [this]() {
        toggleBottomBarPanel({toolboxButton, BottomBarAnchorKind::Tools});
    });
    // 中间那格也是**一颗按钮**：点它向上展开任务中心。三格挂了同一个
    // toggle，所以"点另一颗 = 收旧开新"在三个方向上都成立（那一件事全靠 toggle）。
    QObject::connect(taskSummaryButton, &QPushButton::clicked, taskSummaryButton, [this]() {
        toggleBottomBarPanel({taskSummaryButton, BottomBarAnchorKind::Tasks});
    });
    // 老的那一格账号（accountButton）**没有接线**、一直 `hide()`：账号入口是主页右上那颗
    // `accountSkinButton`（见 setupHomePage()、以及 toggleAccountsPanel()）。导航轨底部只剩任务 / 运行状态 / 工具箱。

    // 三格从左到右摆：实例状态（0）· 空档（1）· 任务摘要（6）· 空档（1）· 账户（0）。
    // 两头那两颗按钮的宽度都是钉死的，份数写 0；富余宽度全在中间三份里分。
    // NOTE: 中间那份是 6：两头那两格吃掉 458 px 时，900 px 的窗口里富余只有 370 px，
    // 中间拿 4/6 就只有 247 px（条 262 px），中间进度条奇小无比。缩了两头
    // （200+258 → 170+208）之后再让中间拿 6/8，条 356 px（900 的窗口），两边各留
    // 一份空档所以还居中。
    // NOTE: 中间那一份是一颗按钮（任务摘要），份数照旧是 6 —— 按钮里面那个 layout 的
    // 边距显式清成了 0，所以宽度还是 356 px。
    // 竖排：任务摘要 · 实例状态 · 工具箱。每格一个最小高，轨窄所以都是单列。
    taskSummaryButton->setMinimumHeight(44);
    runStateButton->setMinimumHeight(TitleBar::navItemHeight());  // 4+4 边距 + 两行字
    toolboxButton->setFixedHeight(TitleBar::navItemHeight());
    toolboxButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    barLayout->addWidget(taskSummaryButton);
    barLayout->addWidget(runStateButton);
    barLayout->addWidget(toolboxButton);
    // Tab 顺序和竖排一致：任务卡 → 运行状态 → 工具箱（运行状态是先建的，默认会排到任务卡前面）。
    QWidget::setTabOrder(taskSummaryButton, runStateButton);
    QWidget::setTabOrder(runStateButton, toolboxButton);
}

QString MainWindowUI::folderActionText(int action) {
    // 上下文写死 "MainWindowUI"：这个类不是 QObject（没有 tr()），和 launchingStatusText()
    // 那几句同一个写法。
    switch (action) {
    case FolderActionRefresh:
        return QCoreApplication::translate("MainWindowUI", "刷新");
    case FolderActionRename:
        return QCoreApplication::translate("MainWindowUI", "重命名");
    case FolderActionOpen:
        return QCoreApplication::translate("MainWindowUI", "打开");
    case FolderActionRemove:
        return QCoreApplication::translate("MainWindowUI", "移除");
    case FolderActionDelete:
        return QCoreApplication::translate("MainWindowUI", "删除");
    }
    // 越界（理论上到不了：菜单是照 folderActionCount() 循环摆的）。给空串而不是随便
    // 返回一项 —— 空串在画面上是个空白按钮，一眼看得出不对。
    return QString();
}

QString MainWindowUI::folderActionTooltip(int action) {
    switch (action) {
    case FolderActionRefresh:
        return QCoreApplication::translate("MainWindowUI", "重新扫描游戏目录以刷新实例列表");
    case FolderActionRename:
        // 这句是用户最容易误会的一步：「重命名」听起来像要把磁盘上的目录改个名，
        // 而它改的只是一个**显示用的名字**。所以正文里把"目录名不动"写死。
        return QCoreApplication::translate("MainWindowUI", "仅更改列表显示名称");
    case FolderActionOpen:
        return QCoreApplication::translate("MainWindowUI", "用文件资源管理器打开这个文件夹");
    case FolderActionRemove:
        return QCoreApplication::translate("MainWindowUI", "仅从列表中移除，保留本地文件。");
    case FolderActionDelete:
        return QCoreApplication::translate("MainWindowUI", "从磁盘永久删除文件夹及所有内容。");
    }
    return QString();
}

QPixmap MainWindowUI::folderActionIcon(int action, int size, const QColor &color) {
    // 刷新和 X 用的是树上早就有的那两个字形（同一个"转一圈回来"、同一个"×"），
    // 另外三个是本轮新画的（见 VersionIcons 里那三个函数的说明）。
    switch (action) {
    case FolderActionRefresh:
        return VersionIcons::refresh(size, color);
    case FolderActionRename:
        return VersionIcons::pen(size, color);
    case FolderActionOpen:
        return VersionIcons::folderMark(size, color);
    case FolderActionRemove:
        return VersionIcons::cross(size, color);
    case FolderActionDelete:
        return VersionIcons::trash(size, color);
    }
    return QPixmap();
}

bool MainWindowUI::paletteIsDark(const QPalette &palette) {
    // 起判据只有一处（`Theme::isDarkPalette()`）—— 主题那一层要知道同一件事
    // （语义色按深浅挑），而"两处各写一遍 lightness() < 128"正是这个改动要收掉的东西。
    return Theme::isDarkPalette(palette);
}

QColor MainWindowUI::folderDangerColor(const QPalette &palette) {
    // 出厂的深浅两档（#ff6b6b / #c02828）、以及红这一档，都在 Theme 里。
    return Theme::dangerColor(palette);
}

QColor MainWindowUI::installNameErrorColor(const QPalette &palette) {
    // NOTE: 这一处红和上面那个**故意不是同一个色**：浅底 #c0392b 对窗口底色 #f0f0f0
    // 4.77:1（够 WCAG 正文那条 4.5:1），#c02828 是另一条来历。深底那一档没法
    // 照搬同一个 #c0392b（只有 2.26:1），所以借了同一个浅红。理由整段挪进了
    // `Theme::errorTextColor()`。
    return Theme::errorTextColor(palette);
}

QString MainWindowUI::groupActionText(int action, const GroupRow &row) {
    // 上下文写死 "MainWindowUI"：这个类不是 QObject（没有 tr()），和上面那几张
    // 文件夹的表同一个写法。
    switch (action) {
    case GroupActionMoveInto:
        // 带省略号：点了之后还要弹一个"移进哪一组"的小框问点什么（和「重命名…」同一条
        // 规矩）。那一项**必须**有 —— 一个实例都没有的组在树里是藏着的，把它放进第一个
        // 实例这件事只有这一个入口（见 openGroupPicker）。
        return QCoreApplication::translate("MainWindowUI", "把选中的实例移入分组…");
    case GroupActionClearMembers:
        return QCoreApplication::translate("MainWindowUI", "把这一组里的实例都移出分组");
    case GroupActionRename:
        return QCoreApplication::translate("MainWindowUI", "重命名这一组…");
    case GroupActionToggleExpand:
        // 状态**写在字里**（而不是摆一个勾字形）：菜单里只有这一项有状态，摆勾会变成
        // "有的项前面有东西、有的没有"，右边那列文字全对不齐。
        return row.expanded ? QCoreApplication::translate("MainWindowUI", "默认展开：开")
                            : QCoreApplication::translate("MainWindowUI", "默认展开：关");
    case GroupActionDelete:
        return QCoreApplication::translate("MainWindowUI", "删除分组…");
    }
    // 越界（理论上到不了：菜单是照 groupActionCount() 循环摆的）。给空串而不是随便
    // 返回一项 —— 空串在画面上是个空白按钮，一眼看得出不对。
    return QString();
}

bool MainWindowUI::groupActionEnabled(int action, const GroupRow &row) {
    switch (action) {
    case GroupActionMoveInto:
        // 没有选中的实例就没什么可移的。
        return row.hasSelection;
    case GroupActionClearMembers:
        // 空组**在树里是看不见的**，所以这一条真正拦的是"菜单开着的时候实例被别处
        // 搬走了"那种过期快照。
        return row.memberCount > 0;
    case GroupActionRename:
    case GroupActionToggleExpand:
    case GroupActionDelete:
        // 这三项什么时候都办得到（删一个有实例的组也只是把那些实例放回未分组，
        // 不删任何东西 —— 见下面那句提示）。
        return true;
    }
    return false;
}

QString MainWindowUI::groupActionTooltip(int action, const GroupRow &row) {
    // 点不动的那几项要在**这里**说清为什么（菜单那边照着这个把话挂上去）。
    switch (action) {
    case GroupActionMoveInto:
        if (!row.hasSelection) {
            return QCoreApplication::translate("MainWindowUI", "请先在列表中选中实例，再选择分组。");
        }
        return QCoreApplication::translate("MainWindowUI", "把选中的实例放进一个组（会离开原来的组）");
    case GroupActionClearMembers:
        if (row.memberCount <= 0) {
            return QCoreApplication::translate("MainWindowUI", "分组内暂无实例");
        }
        return QCoreApplication::translate("MainWindowUI", "这一组里的 %1 个实例会回到未分组（组留着，实例一个都不删）")
            .arg(row.memberCount);
    case GroupActionRename:
        return QCoreApplication::translate("MainWindowUI", "给这一组换个名字（只改名字，组里的实例不动）");
    case GroupActionToggleExpand:
        // 这一步同时是"默认"和"当前"：树每次重灌都按它画（所以点完那一下，收着的组
        // 不会在下次刷新时自己弹开）。
        return row.expanded ? QCoreApplication::translate("MainWindowUI", "这一组现在画成展开的。点一下改成默认收起")
                            : QCoreApplication::translate("MainWindowUI", "这一组现在画成收着的。点一下改成默认展开");
    case GroupActionDelete:
        return QCoreApplication::translate("MainWindowUI", "删除分组后，其中 %1 个实例将移至未分组，实例本身不会被删除。")
            .arg(row.memberCount);
    }
    return QString();
}

QColor MainWindowUI::runStateMarkColor(RunStateMark state, const QPalette &palette) {
    // 三个色是**语义色**（Theme 那一层）：红 = 出过事、黄 = 警告/启动中、绿 = 正常/在跑。
    // 深浅两档的判据、以及语义色那一档，都在 Theme 里 —— 这里只剩下"哪一档
    // 状态是哪一个语义"（那是界面的说法，不是主题的说法）。灰那一档不走 Theme：调色板
    // 自己就有一位现成的"灰掉"，照它的深浅给，比我们再挑一个数更贴。
    switch (state) {
    case RunStateMark::Error:
        // 红这一档**直接复用**「删除」那一项的红（同一个界面上的两种红各调各的没有道理，
        // 调歪了就是"一处红看着干净、另一处发脏"这种没人查得出的毛病）。
        return folderDangerColor(palette);
    case RunStateMark::Launching:
        return Theme::warningColor(palette);
    case RunStateMark::Running:
        return Theme::okColor(palette);
    case RunStateMark::Idle:
        // 灰：**调色板 Disabled 档的正文色**。选它而不是 disabledInkColor() 那两个自己挑的
        // 数，理由有两条：
        // · 这一格画在底栏那块布上，而底栏的调色板是**传进来的这一份**（见
        // MainWindow::updateBottomBar() 里为什么取底栏的 palette）—— 调色板自己那一档
        // 灰就是"同一块布上灰掉的东西"，不会撞色；
        // · 深浅两套各有一位现成的灰（浅底给中灰、深底给另一档），主题换过去它跟着走，
        // 不用我们再维护一对数。
        return palette.color(QPalette::Disabled, QPalette::WindowText);
    }
    return palette.color(QPalette::WindowText);
}

QPixmap MainWindowUI::runStateMarkPixmap(RunStateMark state, const QPalette &palette) {
    // 图按 (边长, 颜色) 缓存（VersionIcons 里那套），所以这个函数每次重画底栏都能随便调。
    return VersionIcons::runStateMark(runStateMarkSize(), runStateMarkColor(state, palette));
}

void MainWindowUI::setFolderRows(const QList<FolderRow> &rows) {
    QListWidget *list = folderList;
    if (list == nullptr) {
        return;  // setupUi() 还没跑完（和那几个访问器拿空指针当判据是同一个意思）
    }
    // 重灌不是"用户改选"，别让最后那句 setCurrentItem() 把切换逻辑又跑一遍（换了当前
    // 文件夹等于重扫磁盘、重灌界面）。挡住信号的这一句就在填表这里。
    const QSignalBlocker blocker(list);
    list->clear();
    for (int i = 0; i < rows.size(); ++i) {
        const FolderRow row = rows.at(i);
        auto *item = new QListWidgetItem(list);
        // 身份照旧挂在 UserRole 上（updateFolderButtons() 和点击那条路都按它认人）。
        item->setData(Qt::UserRole, row.path);
        // 行高**必须显式给**：setItemWidget() 摆进去的那个控件多高由这一格决定，
        // 不给的话条目按"没有文字"算高度，那一行会被压成一条缝。
        item->setSizeHint(QSize(0, folderRowHeight()));
        // 回调捕的是 row 本身（不是行号）：菜单弹出来到用户点某一项之间列表是会变的。
        auto *widget = new FolderRowWidget(
            row, i, [this](const FolderRow &clicked, QWidget *gear) { openFolderMenu(clicked, gear); },
            list->viewport());
        list->setItemWidget(item, widget);
        if (row.current) {
            list->setCurrentItem(item);
            // 页头文件夹胶囊显示当前那一份的名字（设计稿 .folder-pill）。
            if (tabWidget != nullptr)
                if (auto *pill = tabWidget->findChild<QPushButton *>(instanceFolderPillName())) pill->setText(row.title);
        }
    }
    // 焦点链：列表本身 → 各行那颗齿轮 → 左栏底下那两颗按钮。
    // 这几颗齿轮是**这一趟才建的**（每次重灌都重建），而 Qt 的默认顺序是"建的时候谁早
    // 谁在前" ⇒ 它们会落在整条焦点链的最末尾，也就是**底栏之后**：按 Tab 走到最右下
    // 那一颗，下一格"唰"地跳回左栏最上面那一行，眼睛看到的就是一个往上窜的乱序。
    // 这里当场串一遍。`setTabOrder` 干的是"把第二格挪到第一格后面"，重灌时重串一遍
    // 是安全的（`clear()` 已经把上一代的行连控件一起删了，不会有残骸留在链上）。
    QWidget *previous = list;
    for (int i = 0; i < list->count(); ++i) {
        auto *row = dynamic_cast<FolderRowWidget *>(list->itemWidget(list->item(i)));
        if (row == nullptr || row->gearButton() == nullptr) {
            continue;
        }
        QWidget::setTabOrder(previous, row->gearButton());
        previous = row->gearButton();
    }
    if (addFolderButton != nullptr) {
        QWidget::setTabOrder(previous, addFolderButton);
    }
    // 高度按行数走（最多露 kFolderListMaxRows 行，再多在列表里滚）；浮层开着时跟着收放。
    list->setFixedHeight(qBound(1, int(list->count()), kFolderListMaxRows) * folderRowHeight());
    if (QWidget *popup = list->window(); popup->windowType() == Qt::Popup && popup->isVisible()) {
        popup->adjustSize();
    }
}

void MainWindowUI::openFolderMenu(const FolderRow &row, QWidget *gear) {
    // 弹出来那一刻现问一遍"这一行长什么样"是做不到的（数据在 MainWindow 手里，
    // 而这里只是个画界面的）—— 所以用的就是点这颗齿轮时捕下来的那一份快照。
    // 它可能已经过时了（比如这一份刚被别处删掉）：那几种情况由 MainWindow 那边
    // 按 path 重新查一遍状态来处理，界面这一层不猜。
    QListWidget *const list = folderList;
    if (list == nullptr) {
        return;  // setupUi() 还没跑完（齿轮是列表里的东西，正常到不了这儿）
    }
    // "现在有没有开着一块"靠 findChild 认（和底栏那两块面板同一个路数）：
    // NOTE: **挂在这个列表底下**，不是挂 MainWindowUI 上面 —— 这个类根本不是 QObject
    // （它没有 tr()），没有 findChild()。列表是这一行那个齿轮的宿主，也是全窗口
    // 寿命的东西，拿它当 popup 的 parent 既找得回来又不会被谁提前删掉。
    if (auto *open = dynamic_cast<FolderMenu *>(list->findChild<QWidget *>(folderMenuName()))) {
        const bool sameAnchor = open->anchor() == gear;
        open->close();
        if (sameAnchor) {
            return;
        }
    }
    const FolderMenu::ActionCallback onAction = [this](const QString &path, int action) {
        if (m_folderActionHandler) {
            m_folderActionHandler(path, action);
        }
    };
    auto *menu = new FolderMenu(row, gear, onAction, list);
    // 这次弹出来的框归它自己管：关掉就删（不然点一次攒一个）。
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // 开口朝右下：贴着齿轮的**右下角**往下展开一点点。菜单弹在行尾那颗按钮的下方
    // 而不是"鼠标点在哪就弹在哪"—— 那样的话同一个齿轮点两次会弹在差几个像素的
    // 地方，而且行高一行只有一次，对齐按钮比对齐鼠标更稳。
    menu->popUpAt(gear->mapToGlobal(QPoint(0, gear->height() + kPanelGap)));
}

void MainWindowUI::openGroupMenu(const GroupRow &row, const QRect &anchorGlobal) {
    // 和文件夹那一块同一个路数：弹出来那一刻**不重新查数据**（数据在 MainWindow 手里），
    // 用的就是点这颗齿轮时捕下来的那一份快照；过期的那几种情况由 MainWindow 按组 id
    // 重新查一遍来处理，界面这一层不猜。
    QTreeWidget *const tree = instanceList;
    if (tree == nullptr) {
        return;  // setupUi() 还没跑完（组行是树里的东西，正常到不了这儿）
    }
    // "现在有没有开着一块"靠 findChild 认，**挂在这棵树底下**（这个类不是 QObject，
    // 没有 findChild；树是那颗齿轮的宿主，也是全窗口寿命的东西）。
    //
    // NOTE: 两块框（动作菜单 / 挑目标）**共用一个 objectName** —— 同一颗齿轮下只可能
    // 开着其中一个，按名字捞回来的就该是当下那一个。捞回来的是"挑目标"那块时
    // groupId() 是空串，于是它永远不认"再点同一颗齿轮"，直接收掉重开动作菜单
    // （那正是"想换个操作"时该发生的事）。
    if (auto *open = dynamic_cast<GroupMenu *>(tree->findChild<QWidget *>(groupMenuName()))) {
        const bool sameAnchor = !open->pickMode() && open->groupId() == row.groupId;
        open->close();
        if (sameAnchor) {
            return;
        }
    }
    const GroupMenu::ActionCallback onAction = [this](const QString &groupId, int action) {
        if (m_groupActionHandler) {
            m_groupActionHandler(groupId, action);
        }
    };
    // 重放那一侧就是这棵树的 viewport（见 GroupMenu::handleAnchorPress()）。
    auto *menu = new GroupMenu(row, anchorGlobal, onAction, tree->viewport(), tree);
    // 这次弹出来的框归它自己管：关掉就删（不然点一次攒一个）。
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popUpAt(QPoint(anchorGlobal.left(), anchorGlobal.bottom() + kPanelGap));
}

void MainWindowUI::openGroupPicker(const QList<GroupRow> &rows, const QRect &anchorGlobal) {
    QTreeWidget *const tree = instanceList;
    if (tree == nullptr) {
        return;
    }
    // 挨个换菜单时（动作菜单 → 移入分组… → 这个框）前一块**已经在关的路上了**
    // （GroupMenu 里是"先 close() 再排到下一轮事件循环交给回调"），正常捞不到它；
    // 捞到了就是"同一颗齿轮下还开着一块"，收掉再开新的。
    if (auto *open = dynamic_cast<GroupMenu *>(tree->findChild<QWidget *>(groupMenuName()))) {
        open->close();
    }
    const GroupMenu::PickCallback onPick = [this](const QString &groupId) {
        if (m_groupPickHandler) {
            m_groupPickHandler(groupId);
        }
    };
    auto *menu = new GroupMenu(rows, anchorGlobal, onPick, tree->viewport(), tree);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // 和动作菜单弹在**同一个位置**：那颗齿轮下面。用户看到的是"刚才那块框换了个内容"，
    // 而不是"又冒出来一块"（这也是"挑目标"不做成二级子菜单的代价换来的好处：
    // 二级子菜单在 Qt::Popup 上要另立一套抓取/收回，验证工程也驱动不动）。
    menu->popUpAt(QPoint(anchorGlobal.left(), anchorGlobal.bottom() + kPanelGap));
}

MainWindowUI::BottomBarAnchor MainWindowUI::bottomBarAnchorAt(const QPoint &globalPos) const {
    // 三颗按钮摆在哪由底栏的布局说了算，所以判据只能是"这个全局坐标落在它自己的矩形里吗"
    // （mapFromGlobal 之后按 rect() 判 —— 和所有"点在不在这个控件上"的写法一致）。
    const auto hits = [&globalPos](QPushButton *button) {
        return button != nullptr && button->isVisible()
               && button->rect().contains(button->mapFromGlobal(globalPos));
    };
    if (hits(runStateButton)) {
        return {runStateButton, BottomBarAnchorKind::Instances};
    }
    if (hits(toolboxButton)) {
        return {toolboxButton, BottomBarAnchorKind::Tools};
    }
    if (hits(taskSummaryButton)) {
        return {taskSummaryButton, BottomBarAnchorKind::Tasks};
    }
    return {};
}

TaskCenterPanel *MainWindowUI::taskCenterPanel() const {
    // 认它靠 objectName + 动态转型（和那两块 BottomBarPanel 一个路数）：面板挂在底栏底下，
    // close() 之后要等 deleteLater 才真删，所以"现在开着哪一块"不用自己记指针 —— 记了
    // 反而要操心它在哪一行被删掉。
    if (overlayHost != nullptr) {
        TaskCenterPanel *parked = nullptr;
        for (QWidget *widget : overlayHost->findChildren<QWidget *>(taskCenterPanelName())) {
            if (auto *page = dynamic_cast<TaskCenterPanel *>(widget)) {
                if (page->isVisible()) return page;
                parked = page;
            }
        }
        if (parked != nullptr) return parked;
    }
    return dynamic_cast<TaskCenterPanel *>(
        bottomBar != nullptr ? bottomBar->findChild<QWidget *>(taskCenterPanelName()) : nullptr);
}

void MainWindowUI::openTaskCenterPanel() {
    if (bottomBar == nullptr || taskSummaryButton == nullptr || overlayHost == nullptr) {
        return;
    }
    // 任务中心是全局覆盖面板；主页实例列表是页面布局状态。两者不同时展开。
    closeHomeInstancePicker();
    if (TaskCenterPanel *panel = taskCenterPanel()) {
        // 「开着」不能只看 findChild 找没找到：`close()` 之后面板要等 deleteLater 才真删
        // —— 中间这一小段里它仍然是个子控件（嵌在覆盖页里，或者已经被还到停放区）。
        // 真开着的两种样子：摊在本选项卡上（可见），或收在别的选项卡上（`parked()`）。
        const bool open = overlayHost->isOpen() && (panel->isVisible() || overlayHost->parked());
        if (open) {
            if (panel->isVisible()) {
                overlayHost->dismiss();
                return;
            }
            if (overlayHost->evict()) {
                QTimer::singleShot(0, taskSummaryButton, [this]() { openTaskCenterPanel(); });
            }
            return;
        }
        // 残影（覆盖层已经关了 / 页面上这块被 close() 藏起来了，对象还没删）。
        // 对 `!overlayHost->isOpen()` 不能调 `dismiss()` 然后 return —— `dismiss()` 在
        // Mode::None 上什么都不做，于是这一按**被整个吞掉**：用户点底栏中间那格，
        // 什么都没发生。残影不能再挡路：直接排进删除队列，下面照常开一页新的。
        // NOTE: 只 `deleteLater()` 不 `delete` —— 它可能正处在自己的信号处理里。
        panel->deleteLater();
    }
    // 点在面板外面的那一下：和那两块面板同一套（理由全文见 BottomBarPanel 里
    // handleOutsidePress() 那两段 NOTE —— 包括"为什么必须在 close() 之前钉
    // WA_NoMouseReplay"）。
    TaskCenterPanel::OutsidePressHandler onOutsidePress;
    onOutsidePress.claimed = [this](const QPoint &globalPos) {
        return bottomBarAnchorAt(globalPos).button != nullptr;
    };
    onOutsidePress.dismissed = [this](const QPoint &globalPos) {
        const BottomBarAnchor hit = bottomBarAnchorAt(globalPos);
        if (hit.button != nullptr) {
            toggleBottomBarPanel(hit);
        }
    };
    if (overlayHost->isOpen()) {
        // 覆盖页开着（实例设置等）时**不挤掉它**：任务中心作为提示层叠在上面，
        // 关掉就回原页，背景不重开、不播换页动画。
        const auto openOverPage = [this, onOutsidePress] {
        QWidget *content = overlayHost->openPrompt(true, /*fullSize=*/true);
        if (content == nullptr) return;
        auto *promptLayout = qobject_cast<QVBoxLayout *>(content->layout());
        if (promptLayout == nullptr) { overlayHost->closeOverlay(); return; }
        auto *workspace = new QWidget(content);
        workspace->setObjectName(QStringLiteral("taskCenterPromptWorkspace"));
        auto *workspaceLayout = new QVBoxLayout(workspace);
        workspaceLayout->setContentsMargins(0, 0, 0, 0);
        workspaceLayout->setSpacing(8);
        promptLayout->addWidget(workspace, 1);
        auto *panel = new TaskCenterPanel(taskSummaryButton, m_taskCenterProvider,
                                          m_taskCenterClearHandler, onOutsidePress,
                                          workspace, /*embedded=*/true);
        panel->setRowActivatedHandler(m_taskCenterRowActivated);
        panel->setRowActionHandler(m_taskCenterRowAction);
        panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        // 提示卡片里的任务中心要自带卡片内边距、标题和关闭按钮：
        // 直接塞进 promptContent 会贴着边、没有标题，看着像一块裸控件。
        promptLayout->setContentsMargins(0, 0, 0, 0);
        promptLayout->setSpacing(8);
        auto *header = new QWidget(workspace);
        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(0, 0, 0, 0);
        headerLayout->setSpacing(8);
        auto *title = new QLabel(QCoreApplication::translate("MainWindowPanels", "任务中心"), header);
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        headerLayout->addWidget(title);
        headerLayout->addStretch(1);
        if (QWidget *actions = panel->headerActions()) headerLayout->addWidget(actions);
        auto *closeButton = new QPushButton(QCoreApplication::translate("MainWindowPanels", "关闭"), header);
        closeButton->setObjectName(QStringLiteral("taskCenterPromptClose"));
        Theme::markButton(closeButton, Theme::ButtonKind::Neutral);
        closeButton->setCursor(Qt::PointingHandCursor);
        QObject::connect(closeButton, &QPushButton::clicked, overlayHost, [this] { overlayHost->dismiss(); });
        headerLayout->addWidget(closeButton);
        workspaceLayout->addWidget(header);
        workspaceLayout->addWidget(panel, 1);
        // 底栏别被当成“提示期禁用”：任务中心要能靠任务按钮再点一次收掉。
        overlayHost->setPromptBlockedWidget(nullptr);
        // 再点一次任务按钮 / 点外面 / Esc 都会走到 promptDismissed：把它收掉并还回原页。
        QObject::connect(overlayHost, &OverlayHost::promptDismissed, workspace, [this, workspace] {
            if (!workspace->isVisible()) return;
            // 先恢复原页面，再采集收起终点；不在快照结束后重建设置页。
            Anim::layoutTransition(overlayHost->parentWidget(), [this, workspace] {
                overlayHost->closeOverlay();
                overlayHost->setPromptBlockedWidget(bottomBar);
                workspace->deleteLater();
            }, false, tabWidget->geometry());
        }, Qt::SingleShotConnection);
        panel->showEmbedded();
        };
        Anim::layoutTransition(overlayHost->parentWidget(), openOverPage, true, tabWidget->geometry());
        return;
    }
    const auto openNow = [this, onOutsidePress]() {
    QWidget *page = overlayHost->openPage(QCoreApplication::translate("MainWindowPanels", "任务中心"));
    if (page == nullptr) {
        return;
    }
    auto *pageLayout = qobject_cast<QVBoxLayout *>(page->layout());
    if (pageLayout == nullptr) {
        overlayHost->closeOverlay();
        return;
    }
    auto *panel = new TaskCenterPanel(taskSummaryButton, m_taskCenterProvider,
                                      m_taskCenterClearHandler, onOutsidePress, page,
                                      /*embedded=*/true);
    // 点一行进详情页。★ **要在 popUp() 之前挂**：`rebuild()`（popUp() 里第一件事）就是按
    // 这个处理器决定那几行"可不可点"的 —— 挂晚了的话这一面板里的行没手型光标、点了也没
    // 反应，得关掉重开一次才行。
    panel->setRowActivatedHandler(m_taskCenterRowActivated);
    panel->setRowActionHandler(m_taskCenterRowAction);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    pageLayout->addWidget(panel);
    QObject::connect(overlayHost, &OverlayHost::backPressed, panel, [this, panel]() {
        const auto closeNow = [this, panel]() {
            overlayHost->closeOverlay();
            panel->deleteLater();
        };
        Anim::layoutTransition(overlayHost->parentWidget(), closeNow, false, tabWidget->geometry());
    });
    overlayHost->setPageHeaderWidget(panel->headerActions());
    panel->showEmbedded();
    };
        Anim::layoutTransition(overlayHost->parentWidget(), openNow, true, tabWidget->geometry());
}

void MainWindowUI::openBottomBarPanel(const BottomBarAnchor &anchor) {
    if (bottomBar == nullptr || anchor.button == nullptr) {
        return;
    }
    // 现问一遍要弹什么（不缓存：实例随时会结束、账号随时会多一个，"上次那几行"
    // 一旦留着就会弹出早就不是那样的一面板）。
    // 账号弹层开着就先收（同一时间只挂一块弹层）。
    if (QWidget *account = bottomBar->window()->findChild<QWidget *>(accountPopoverName()); account != nullptr && account->isVisible()) {
        account->close();
    }
    const QList<PanelRow> rows =
        m_panelRowsProvider ? m_panelRowsProvider(false) : QList<PanelRow>();
    // 面板只知道"第几行第几颗"，"是哪一块面板"由这里补上再转给 MainWindow ——
    // 一块面板只可能属于一边，所以这个得在**建面板的时候**就绑死。
    const BottomBarPanel::ActionCallback onAction = [this](const QString &key, int action) {
        if (m_panelActionHandler) {
            m_panelActionHandler(false, key, action);
        }
    };
    // 点在面板外面的那一下：面板自己认不出"外面是哪儿"，所以这里按全局坐标认那一颗按钮。
    // 认出来了就压掉 Qt 的重放（claimed），收掉之后走**同一个** toggle 换过去（dismissed）——
    // 点另一颗那条路全靠它：那一下面板底下的按钮**一个事件都收不到**，只有面板收到了
    // （见 BottomBarPanel::handleOutsidePress() 的第一段 NOTE）。
    BottomBarPanel::OutsidePressHandler onOutsidePress;
    onOutsidePress.claimed = [this](const QPoint &globalPos) {
        return bottomBarAnchorAt(globalPos).button != nullptr;
    };
    onOutsidePress.dismissed = [this](const QPoint &globalPos) {
        const BottomBarAnchor hit = bottomBarAnchorAt(globalPos);
        if (hit.button != nullptr) {
            // NOTE: 这里让 toggle 自己去 findChild 认"现在开着哪一块"：刚刚被收掉的那一块
            // 还在（close 之后要等 deleteLater 才真删），所以"点的是同一颗"那一路它
            // 认得出来（认出来就什么都不开）；点另一颗则收旧开新。
            toggleBottomBarPanel(hit);
        }
    };
    auto *panel = new BottomBarPanel(anchor.button, /*alignRight=*/false, /*dropDown=*/false, rows, onAction,
                                     onOutsidePress, bottomBar);
    // 这次弹出来的面板归它自己管：关掉就删（不然点一次攒一个）。
    panel->setAttribute(Qt::WA_DeleteOnClose);
    panel->popUp();
}

void MainWindowUI::toggleBottomBarPanel(const BottomBarAnchor &anchor) {
    if (bottomBar == nullptr || anchor.button == nullptr) {
        return;
    }
    // 任务中心正以提示层叠在页上：再点一次任务按钮就收掉它（不依赖 findChild 的可见性判断）。
    if (anchor.kind == BottomBarAnchorKind::Tasks && overlayHost != nullptr &&
        overlayHost->promptOpen() && overlayHost->promptFullSize() && taskCenterPanel() != nullptr) {
        overlayHost->dismiss();
        return;
    }
    // 左下那一格**没有实例可列时点不开**（见 setRunPanelAvailable()）：
    // 这一下只当"收"用。
    //
    // NOTE: 之所以不只靠那颗按钮收不到鼠标（WA_TransparentForMouseEvents）：面板开着的
    // 时候，外面那一下是**投递给面板自己的**，它按坐标认出点在哪一颗按钮上再转给
    // 这里（见 BottomBarPanel::handleOutsidePress()）—— 那条路根本不经过按钮。
    // 而下面 open->close() 那一步照走，所以点在这颗点不动的格子上时，**已经开着的
    // 那一块会被收掉**（点外面本来就该收），只是不再开新的 —— 不然那一下看着像
    // "点了没反应"，而那一块还杵在那儿。
    const bool blocked = anchor.kind == BottomBarAnchorKind::Instances && !m_runPanelAvailable;
    // "现在有没有开着一块"靠 findChild 认（和行尾齿轮菜单一个路数）：面板挂在 bottomBar
    // 底下，close 之后要等 deleteLater 才真删（同一轮事件处理里还找得到），不用自己记指针。
    //
    // NOTE: 中间那格那块是**另一个类、另一个名字**（`TaskCenterPanel`），所以这里要问两次
    // —— 一次问"那两块短行面板开着吗"、一次问"任务中心开着吗"。只问一次的话，
    // "任务中心开着时点左下那格"就会变成两块面板同时在屏幕上。
    bool sameAnchor = false;
    bool anyOpen = false;
    bool reopenTaskCenter = false;
    if (auto *open = dynamic_cast<BottomBarPanel *>(
            bottomBar->findChild<QWidget *>(MainWindowUI::bottomBarPanelName()))) {
        sameAnchor = open->anchor() == anchor.button;
        anyOpen = true;
        open->close();
    }
    if (auto *panel = taskCenterPanel()) {
        // 和 `openTaskCenterPanel()` 同一把尺：findChild 找得到不等于开着（close 之后
        // 还要等 deleteLater）。真开着 = 覆盖层开着，而且这一页摊在本选项卡上或者
        // 收在别的选项卡上。残影不能算「开着」—— 算进去的话这一按会被当成"收回"，
        // 用户看到的就是「点底栏中间那格没反应」。
        const bool open =
            overlayHost != nullptr && overlayHost->isOpen()
            && (panel->isVisible() || overlayHost->parked());
        if (open) {
            const bool same = panel->anchor() == anchor.button;
            sameAnchor = sameAnchor || same;
            anyOpen = true;
            // 任务中心叠成提示层时，它的「可见」在卡片布局落定前可能是 false；
            // 以 promptOpen() 为准直接收掉，别再走 reopen 那一套。
            if (same && overlayHost->promptOpen()) {
                overlayHost->dismiss();
                return;
            }
            if (same && !panel->isVisible()) {
                reopenTaskCenter = true;
            } else {
                overlayHost->dismiss();
            }
        } else {
            panel->deleteLater();
        }
    }
    if (reopenTaskCenter) {
        if (overlayHost->evict()) {
            QTimer::singleShot(0, taskSummaryButton, [this]() { openTaskCenterPanel(); });
        }
        return;
    }
    if (anyOpen && (sameAnchor || blocked)) {
        return;  // 再点一次**同一颗** = 收回去，就这一下
    }
    if (blocked) {
        return;
    }
    if (anchor.kind == BottomBarAnchorKind::Tasks) {
        toggleActivityPopover();
        return;
    }
    if (anchor.kind == BottomBarAnchorKind::Tools) {
        openToolboxTab();
        return;
    }
    openBottomBarPanel(anchor);
}

void MainWindowUI::setBottomBarTaskSummary(const BottomBarTaskSummary &summary) {
    if (statusLabel == nullptr || progressBar == nullptr) {
        return;  // setupUi() 还没跑完（和那几个访问器拿空指针当判据是同一个意思）
    }
    m_lastTaskSummary = summary;
    syncHomeLaunchButton();  // 没实例时主按钮写「安装中 N%」
    const QString tip = MainWindowUI::taskSummaryToolTip(summary);
    if (taskSummaryButton != nullptr) {
        taskSummaryButton->setToolTip(tip);
    }
    progressBar->setToolTip(tip);
    // ---------- 那一行字 ----------
    // 占位那一档（一项任务都没有）用调色板里的 PlaceholderText：要的是**低强调度**
    // 的一句"暂无后台任务"，和"真有一件事在跑"的标题不能长成一个样子。
    //
    // NOTE: 色的底取 `QApplication::palette()`，不取这个标签自己的 —— 它装在一颗挂了
    // QSS 的按钮里（applyBottomBarButtonStyle()），带样式表那句 `border: none;
    // background: transparent` 不影响文字色，但按钮上那份 palette 是从调色板推出来的
    // 一份**副本**，主题换了之后哪个先更新不好说（和 runStateMarkPixmap 那条取
    // "底栏的调色板"是同一个顾虑）。取应用那一份就不必操心谁先收到事件。
    QPalette textPalette = QApplication::palette();
    textPalette.setColor(QPalette::WindowText,
                         textPalette.color(summary.hasTask ? QPalette::WindowText
                                                           : QPalette::PlaceholderText));
    statusLabel->setPalette(textPalette);
    statusLabel->setText(summary.hasTask ? MainWindowUI::taskSummaryText(summary.title, summary.state)
                                         : MainWindowUI::taskSummaryNoTaskText());

    // 「失败」那一档整句染红：这一格只有一行字，"那次没办成"是它最要紧的信息。
    // （面板里那一行才有地方挂红点，见 ui/TaskCenterPanel.cpp。）
    if (summary.state == TaskCenter::State::Failed
        || summary.state == TaskCenter::State::Interrupted) {
        QPalette failedPalette = statusLabel->palette();
        failedPalette.setColor(QPalette::WindowText, folderDangerColor(textPalette));
        statusLabel->setPalette(failedPalette);
    }

    // 那一颗红点（"用户还没看过的一次失败"）：**藏着的时候它也在**，只是宽度 0。
    if (taskFailureDot != nullptr) {
        if (summary.unreadFailure) {
            taskFailureDot->setPixmap(VersionIcons::runStateMark(
                MainWindowUI::taskCenterPanelFailureDotSize(),
                folderDangerColor(textPalette)));
            taskFailureDot->show();
        } else {
            taskFailureDot->hide();
        }
    }
    // 收起时那颗角标：同一个信号、同一颗点，只在图标轨上露（展开时标题前那颗管）。
    // 未读失败在账本里一定占着这一格（优先级 1，不受「刚完成」时间窗限制），所以环一直在。
    if (taskFailureBadge != nullptr) {
        taskFailureBadge->setPixmap(VersionIcons::runStateMark(
            MainWindowUI::taskCenterPanelFailureDotSize(), folderDangerColor(textPalette)));
        taskFailureBadge->setVisible(summary.unreadFailure && m_railCollapsed);
    }

    // ---------- 右边那句小字（「另有 N 项」） ----------
    if (taskExtraLabel != nullptr) {
        if (summary.extraCount > 0) {
            taskExtraLabel->setText(MainWindowUI::taskSummaryMoreText(summary.extraCount));
            taskExtraLabel->show();
        } else {
            taskExtraLabel->clear();
            taskExtraLabel->hide();
        }
    }

    // ---------- 下面那一行（条 + 右边那个数） ----------
    // 右边那格百分比是**低强调度**的（和那句「另有 N 项」一个待遇），色的底取应用那一份
    // （理由同上面 statusLabel 那段：这一格装在一颗挂着 QSS 的按钮里）。
    if (taskPercentLabel != nullptr) {
        QPalette pctPalette = QApplication::palette();
        pctPalette.setColor(QPalette::WindowText, pctPalette.color(QPalette::PlaceholderText));
        taskPercentLabel->setPalette(pctPalette);
    }
    // 「无任务时不显示空进度条」。所以没任务时**这一整行**（条 + 数）都是**藏着的**
    // （不是复位之后留着）。
    if (!summary.hasTask) {
        progressBar->setRange(0, 100);
        Anim::smoothProgress(progressBar, 0);
        // 展开态不摆空进度条；图标轨里这颗环就是任务中心图标，必须留着。
        progressBar->setVisible(m_railCollapsed);
        if (taskPercentLabel != nullptr) {
            taskPercentLabel->clear();
            taskPercentLabel->hide();
        }
        return;
    }
    progressBar->show();
    if (summary.percent < 0) {
        // 测不出总量（同步等待那几段、启动那一段、以及"刚点下载、清单还在路上"）：
        // 不确定态的滚动条 —— 绝不编一个百分比出来。数没有真值 ⇒ 那一格收起。
        progressBar->setRange(0, 0);
        if (taskPercentLabel != nullptr) {
            taskPercentLabel->clear();
            taskPercentLabel->hide();
        }
        return;
    }
    progressBar->setRange(0, 100);
    // 走同一条动画路数（`ui/Animations.hpp` 里那四条"原地落"的规则）：它自己认得出
    // "总量变了 / 退回 0 了"那几档，不用这里再判一遍。
    Anim::smoothProgress(progressBar, summary.percent);
    // 数走和面板那一行**同一个出处**（`taskCenterPercentText()`）；条本身不画数。
    if (taskPercentLabel != nullptr) {
        taskPercentLabel->setText(MainWindowUI::taskCenterPercentText(summary.percent));
        taskPercentLabel->show();
    }
}

void MainWindowUI::refreshTaskCenterPanel() {
    if (TaskCenterPanel *panel = taskCenterPanel()) {
        panel->requestRefresh();
    }
    if (taskSummaryButton != nullptr) {
        if (auto *popover = taskSummaryButton->window()->findChild<QFrame *>(ActivityPopover::objectNameValue());
            popover != nullptr && popover->isVisible())
            if (auto *p = dynamic_cast<ActivityPopover *>(popover)) p->refresh();
    }
}

void MainWindowUI::toggleActivityPopover() {
    if (taskSummaryButton == nullptr) return;
    closeHomeInstancePicker();
    QWidget *window = taskSummaryButton->window();
    if (auto *open = window->findChild<QFrame *>(ActivityPopover::objectNameValue());
        open != nullptr && open->isVisible()) {
        open->setObjectName(QString());
        open->close();
        return;
    }
    ActivityPopover::Handlers handlers;
    handlers.items = m_taskCenterProvider;
    handlers.rowAction = m_taskCenterRowAction;
    handlers.clearFinished = m_taskCenterClearHandler;
    handlers.rowActivated = m_taskCenterRowActivated;
    handlers.openAll = [this] { openTaskCenterPanel(); };
    auto *popover = new ActivityPopover(taskSummaryButton, handlers);
    popover->popUp();
    popover->refresh();
}

void MainWindowUI::setRunStateCount(int count) {
    if (auto *button = dynamic_cast<RunStateButton *>(runStateButton)) {
        button->setCount(count);
    }
}

void MainWindowUI::setRunPanelAvailable(bool available) {
    if (runStateButton == nullptr || m_runPanelAvailable == available) {
        return;
    }
    m_runPanelAvailable = available;
    // 点不动的那一格仍收鼠标：悬停要能显示「未运行」（图标轨里这一格没有字，靠它说明状态）。
    // 点不开由 toggleBottomBarPanel() 里的 m_runPanelAvailable 把住；这里收掉手型指针和悬停 / 按下底色。
    // 不用 setEnabled：那会让里面的字跟着变灰；焦点策略也不随状态变。
    runStateButton->setCursor(available ? Qt::PointingHandCursor : Qt::ArrowCursor);
    runStateButton->setProperty("wtfmlIdle", !available);
    runStateButton->style()->unpolish(runStateButton);
    runStateButton->style()->polish(runStateButton);
    runStateButton->update();
    runStateButton->setToolTip(available ? MainWindowUI::bottomBarRunPanelTip()
                                         : MainWindowUI::runStateNotRunningText());
}



