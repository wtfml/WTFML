/**
 * @file AccountSkinDialog.cpp
 * @brief 「账号与皮肤」对话框（见 .hpp 里那几段说明）。
 */

#include "AccountSkinDialog.hpp"
#include "AutoHeight.hpp"
#include "ElidedLabel.hpp"
#include "InputSizing.hpp"

// WheelGuard 住在 MainWindowUI.hpp 里（和 InstanceSettingsDialog.cpp 一样，直接引那一份，
// 不另外抄一个防护类）。它护的是下面那个「模型」下拉框。
#include "MainWindowUI.hpp"
#include "RoundAvatar.hpp"
#include "SkinModel.hpp"
#include "Theme.hpp"
#include "SkinViewer.hpp"
#include "OverlayHost.hpp"
#include "VersionIcons.hpp"
#include <QHideEvent>
#include <QShowEvent>
#include <algorithm>
#include <QResizeEvent>

#include <QAction>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QGuiApplication>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kInitialWidth = 1040;
constexpr int kInitialHeight = 560;
constexpr int kAccountRowHeight = 58;   // 设计稿 .acct-row：头像 32 + 上下 13
constexpr int kAccountsWidth = 372;
constexpr int kControlsWidth = 280;
constexpr int kCapeCell = 44;            // 设计稿 .cape：44 × 60
constexpr int kCapeCellHeight = 60;
constexpr int kCapeGap = 8;
constexpr int kPreviewNoteMaxWidth = 420;  // 预览说明一行的宽度上限

// 行里显示用的拆开那几样（条目 text() 仍是「名字（离线）（当前）」那一整句，测试和无障碍读它）。
constexpr int kNameRole = Qt::UserRole + 2;
constexpr int kOfflineRole = Qt::UserRole + 3;
constexpr int kSavedRole = Qt::UserRole + 4;
constexpr int kFaceRole = Qt::UserRole + 5;
constexpr int kReloginRole = Qt::UserRole + 6;
// 来源那一行小字（「微软账号 · 登录取自 …」/「离线账号」）放在无障碍描述里：测试和读屏读它，行里画的也是它。
constexpr int kSourceRole = Qt::AccessibleDescriptionRole;

/// 去掉「（当前）」两头的括号，拿来做徽标（同一份文案，不另起一句）。
QString bare(const QString &mark) {
    const QString trimmed = mark.trimmed();
    return trimmed.mid(1, trimmed.size() - 2);
}

QFont pixelFont(QFont font, int px, QFont::Weight weight = QFont::Normal) {
    font.setPixelSize(px);
    font.setWeight(weight);
    return font;
}

/// 账号行（设计稿 .acct-row）：头像 32 圆角 8、名字 14 + 「当前」小标、下面一行账号来源。
class AccountRowDelegate : public QStyledItemDelegate {
public:
    AccountRowDelegate(QObject *parent, const QImage *currentSkin)
        : QStyledItemDelegate(parent), m_currentSkin(currentSkin) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if (index.data(kNameRole).isNull()) return QStyledItemDelegate::sizeHint(option, index);
        return QSize(option.rect.width(), kAccountRowHeight);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        if (index.data(kNameRole).isNull()) {  // 「还没有保存的账号。」那一行
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const Theme::Tokens t = Theme::tokens();
        const QString name = index.data(kNameRole).toString();
        const bool current = index.data(Qt::UserRole + 1).toBool();
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const QRectF r = QRectF(option.rect).adjusted(0, 1, 0, -1);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        if (selected || hovered) {
            painter->setBrush(selected ? t.selected : t.hover);
            painter->drawRoundedRect(r, 12, 12);
        }

        const QRect avatar(int(r.left()) + 12, int(r.center().y()) - 16, 32, 32);
        // 当前账号跟着 3D 预览的皮肤走（贴图晚到时头像跟着变）；其余账号用调用方给的头像，没有才画首字母。
        const QImage rowFace = index.data(kFaceRole).value<QImage>();
        const QImage face = current && !m_currentSkin->isNull() ? SkinModel::headAvatar(*m_currentSkin, 64)
                            : !rowFace.isNull()                 ? rowFace
                                                                : SkinModel::letterAvatar(name, 64);
        // 圆头像：和主页右上那颗、账号下拉面板里那几行同一种形状。
        RoundAvatar::paint(*painter, QRectF(avatar), face);

        const int textLeft = avatar.right() + 13;
        // 选中那一行行尾压着「切换 / 删除」两颗（placeRowActions()），字给它们让位。
        const int actions = selected ? option.widget->property("wtfmlRowActionsWidth").toInt() : 0;
        // 需要重新登录的行：行尾有一颗「重新登录」（真按钮，见 placeReloginButtons()），字再让出它的宽。
        const bool relogin = index.data(kReloginRole).toBool();
        const int reloginRoom =
            relogin ? option.widget->property("wtfmlReloginWidth").toInt() + (selected ? 4 : 8) : 0;
        const int textRight = int(r.right()) - (current && !selected && !relogin ? 40 : 16) - actions - reloginRoom;
        const QFont nameFont = pixelFont(option.font, 14, QFont::DemiBold);
        const QFont subFont = pixelFont(option.font, Theme::textSize(Theme::TextRole::Caption));
        const QFontMetrics nm(nameFont);
        const QFontMetrics sm(subFont);
        const int blockTop = int(r.center().y()) - (nm.height() + 2 + sm.height()) / 2;

        painter->setFont(nameFont);
        painter->setPen(t.text);
        const QFont badgeFont = pixelFont(option.font, Theme::textSize(Theme::TextRole::Caption), QFont::DemiBold);
        const QString badge = current ? bare(AccountSkinDialog::currentMark()) : QString();
        const int badgeWidth = current ? QFontMetrics(badgeFont).horizontalAdvance(badge) + 16 : 0;
        const int badgeRoom = current ? qMax(52, badgeWidth + 8) : 0;
        const QString shownName = nm.elidedText(name, Qt::ElideRight, qMax(0, textRight - textLeft - badgeRoom));
        painter->drawText(QRect(textLeft, blockTop, textRight - textLeft, nm.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, shownName);
        if (current) {
            const QRectF pill(textLeft + nm.horizontalAdvance(shownName) + 8,
                              blockTop + (nm.height() - 18) / 2.0, badgeWidth, 18);
            painter->setPen(Qt::NoPen);
            painter->setBrush(t.accentSoft2);
            painter->drawRoundedRect(pill, 9, 9);
            painter->setFont(badgeFont);
            painter->setPen(Theme::textTone(t.accentInk, t.primary));
            painter->drawText(pill, Qt::AlignCenter, badge);
        }

        const QString sub = index.data(kSourceRole).toString();
        painter->setFont(subFont);
        painter->setPen(relogin ? Theme::textTone(t.dangerInk, t.danger) : t.secondary);
        painter->drawText(QRect(textLeft, blockTop + nm.height() + 2, textRight - textLeft, sm.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, sm.elidedText(sub, Qt::ElideRight, textRight - textLeft));

        if (current && !selected && !relogin) {
            VersionIcons::paintLine(*painter, QStringLiteral("check"),
                                    QRectF(r.right() - 32, r.center().y() - 8, 16, 16), t.secondary);
        }
        painter->restore();
    }

private:
    const QImage *m_currentSkin;
};

/// 披风格（设计稿 .cape）：44 × 60 圆角 10，中间一块按 id 稳定配色的竖条；选中那格描主题色。
class CapeCellDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return QSize(kCapeCell + kCapeGap, kCapeCellHeight + kCapeGap);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        const Theme::Tokens t = Theme::tokens();
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const QRectF cell(option.rect.left() + kCapeGap / 2.0, option.rect.top() + kCapeGap / 2.0,
                          kCapeCell, kCapeCellHeight);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QColor fill = t.text;
        fill.setAlpha(hovered ? 26 : 16);
        painter->setPen(selected ? QPen(t.primary, 2) : QPen(t.glassEdge, 1));
        painter->setBrush(fill);
        painter->drawRoundedRect(cell.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
        const QImage texture = index.data(Qt::UserRole + 3).value<QImage>();
        painter->setPen(Qt::NoPen);
        if (index.data(Qt::UserRole + 4).toBool()) {
            // 「无披风」：一支斜杠，和真实披风格区分开。
            painter->setPen(QPen(t.text, 2));
            painter->drawLine(QPointF(cell.left() + 12, cell.bottom() - 12),
                              QPointF(cell.right() - 12, cell.top() + 12));
        } else if (!texture.isNull() && texture.width() >= 64) {
            // 披风贴图正面在 (1,1) 起 10×16（按 64 宽的比例换算高清贴图），放大 2 倍、最近邻不糊。
            const int unit = texture.width() / 64;
            const QRectF front(cell.center().x() - 10, cell.center().y() - 16, 20, 32);
            painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter->drawImage(front, texture, QRect(unit, unit, 10 * unit, 16 * unit));
        } else {
            // 贴图还没到（或没地址）：按 id 取一个稳定色块占位。
            QColor swatch = SkinModel::avatarFallbackColor(index.data(Qt::UserRole).toString());
            painter->setBrush(swatch);
            painter->drawRoundedRect(QRectF(cell.center().x() - 8, cell.top() + 14, 16, 32), 3, 3);
        }
        painter->restore();
    }
};

/// 预览下面那行说明：理想宽度 = 文字一行放下的宽度（封顶 kPreviewNoteMaxWidth，可以比 3D 预览宽），
/// 放不下才自然折行；最小宽度很小，窄窗口里布局能把它压到预览那么宽。
class PreviewNoteLabel : public QLabel {
public:
    using QLabel::QLabel;

    QSize sizeHint() const override {
        const int line = fontMetrics().horizontalAdvance(text()) + 2;
        const int width = qMin(line, kPreviewNoteMaxWidth);
        return QSize(width, heightForWidth(width));
    }
    QSize minimumSizeHint() const override { return QSize(120, QLabel::minimumSizeHint().height()); }
};

/// 侧栏式列表：无框、透明底，行自己画底色。
void makeListTransparent(QListWidget *list) {
    list->setFrameShape(QFrame::NoFrame);
    list->setMouseTracking(true);
    list->setAutoFillBackground(false);
    list->viewport()->setAutoFillBackground(false);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

QLabel *makeSectionTitle(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setFont(Theme::textFont(label->font(), Theme::TextRole::Heading));
    return label;
}

}  // namespace

AccountSkinDialog::AccountSkinDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(windowTitle());
    setObjectName(QStringLiteral("accountSkinDialog"));
    // 尺寸**不**跟着内容长：这个窗里的东西是固定的三块，摆好之后让用户自己拉。
    resize(kInitialWidth, kInitialHeight);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(12);
    root->setAlignment(Qt::AlignTop);

    // ---------- 顶部（设计稿 .page-head）：大标题 + 一句身份副标题，「刷新外观」在右 ----------
    m_headerBar = new QWidget(this);
    m_headerBar->setObjectName(QStringLiteral("accountHeaderBar"));
    m_headerBar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *statusRow = new QHBoxLayout(m_headerBar);
    statusRow->setContentsMargins(0, 0, 0, 0);
    statusRow->setSpacing(12);
    auto *titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(2);
    auto *title = new QLabel(windowTitle(), m_headerBar);
    title->setObjectName(QStringLiteral("accountPageTitle"));
    Theme::markDisplay(title, Theme::textFont(title->font(), Theme::TextRole::Display));
    m_statusLabel = new QLabel(notLoggedInNote(), m_headerBar);
    m_statusLabel->setObjectName(statusLabelName());
    m_statusLabel->setWordWrap(false);
    m_statusLabel->setMaximumWidth(480);
    m_statusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_statusLabel->setFont(Theme::textFont(m_statusLabel->font(), Theme::TextRole::Caption));
    const auto tintStatus = [label = m_statusLabel] {
        label->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::tokens().secondary.name()));
    };
    tintStatus();
    Theme::follow(m_statusLabel, tintStatus);
    titleColumn->addWidget(title);
    titleColumn->addWidget(m_statusLabel);
    // 请求失败的原因另起一行（身份那一句不动）：次要文字色，后面跟「重试」。
    m_statusErrorRow = new QWidget(m_headerBar);
    m_statusErrorRow->setObjectName(QStringLiteral("accountStatusErrorRow"));
    auto *errorLayout = new QHBoxLayout(m_statusErrorRow);
    errorLayout->setContentsMargins(0, 0, 0, 0);
    errorLayout->setSpacing(6);
    m_statusErrorLabel = new ElidedLabel(m_statusErrorRow);
    m_statusErrorLabel->setObjectName(statusErrorLabelName());
    m_statusErrorLabel->setMinimumWidth(1);
    m_statusErrorLabel->setFont(Theme::textFont(m_statusErrorLabel->font(), Theme::TextRole::Caption));
    const auto tintError = [label = m_statusErrorLabel] {
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText, Theme::tokens().secondary);
        label->setPalette(palette);
    };
    tintError();
    Theme::follow(m_statusErrorLabel, tintError);
    m_retryButton = new QPushButton(retryButtonText(), m_statusErrorRow);
    m_retryButton->setObjectName(retryButtonName());
    m_retryButton->setCursor(Qt::PointingHandCursor);
    Theme::markButton(m_retryButton, Theme::ButtonKind::Ghost);
    errorLayout->addWidget(m_statusErrorLabel, 0);
    errorLayout->addWidget(m_retryButton, 0);
    errorLayout->addStretch(1);
    m_statusErrorLabel->setMaximumWidth(400);
    m_statusErrorRow->hide();
    titleColumn->addWidget(m_statusErrorRow);
    statusRow->addLayout(titleColumn, 1);
    root->addWidget(m_headerBar);

    auto *body = new QWidget(this);
    body->setObjectName(QStringLiteral("accountBody"));
    body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    QHBoxLayout *main = new QHBoxLayout(body);
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(28);
    m_bodyLayout = main;
    root->addWidget(body);

    // ---------- 左：账号（设计稿 .acct-col）----------
    // 标题 → 账号列表 → 「添加账号」（菜单选微软账号 / 离线账号；离线：名字 +「添加」就地展开）。
    m_accountsPanel = new QWidget(this);
    m_accountsPanel->setObjectName(QStringLiteral("accountManagementPane"));
    m_accountsPanel->setMaximumWidth(kAccountsWidth);
    // 和立绘两边那两段 stretch 抢宽度时要占上风（Preferred 会被 stretch 压到最小宽）。
    m_accountsPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QVBoxLayout *left = new QVBoxLayout(m_accountsPanel);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(8);  // 控件间距（全站三档里的 8）：标题 → 账号列表 → 入口
    left->setAlignment(Qt::AlignTop);
    // 伸缩权重 100 对立绘两边那两段 stretch（各 1）：余宽先给账号栏长到 kAccountsWidth，
    // 两段留白只分零头。原来 8:1:1 时 1280 窗下账号栏只分到约 280，正版说明再被行尾 stretch
    // 对半分，只剩 160 上下、折成四行。不用最小宽度：那会把整窗最小宽顶过窄窗阈值、再也叠不成上下。
    main->addWidget(m_accountsPanel, 100);

    left->addWidget(makeSectionTitle(accountSectionTitle(), this));

    m_accountList = new QListWidget(this);
    m_accountList->setObjectName(accountListName());
    makeListTransparent(m_accountList);
    m_accountList->setItemDelegate(new AccountRowDelegate(m_accountList, &m_currentSkin));
    AutoHeight::install(m_accountList);  // 账号多少就多高，由整页滚动
    m_accountList->viewport()->installEventFilter(this);
    left->addWidget(m_accountList);
    m_accountList->hide();  // 没有账号时不占位：标题下面直接接入口（见 setAccounts()）

    // 「切换到所选」「删除所选」：不再占一整排，跟着选中那一行摆在行尾（placeRowActions()）。
    m_rowActions = new QWidget(m_accountList->viewport());
    auto *rowActionsLayout = new QHBoxLayout(m_rowActions);
    rowActionsLayout->setContentsMargins(0, 0, 0, 0);
    rowActionsLayout->setSpacing(4);
    m_switchButton = new QPushButton(switchButtonText(), m_rowActions);
    m_switchButton->setObjectName(switchButtonName());
    // 「用这个账号」= 换个身份（一般性那一档）。
    Theme::markButton(m_switchButton, Theme::ButtonKind::Ghost);
    // 「删除所选」：只在选中那一行的行尾露一颗垃圾桶（红的只有图标，不在高亮底上压一块红字）。
    // 字留在 tooltip / 读屏名里。
    m_removeButton = new QPushButton(m_rowActions);
    m_removeButton->setObjectName(removeButtonName());
    m_removeButton->setToolTip(removeButtonText());
    m_removeButton->setAccessibleName(removeButtonText());
    Theme::markButton(m_removeButton, Theme::ButtonKind::Ghost);
    Theme::markIconButton(m_removeButton);
    m_removeButton->setFixedSize(30, 30);
    m_removeButton->setIconSize(QSize(16, 16));
    {
        const auto tint = [b = m_removeButton] {
            const Theme::Tokens t = Theme::tokens();
            b->setIcon(VersionIcons::line(QStringLiteral("trash"), 16, Theme::textTone(t.dangerInk, t.danger)));
        };
        tint();
        Theme::follow(m_removeButton, tint);
    }
    // 「复制 UUID」：行尾第二颗图标钮（切换和删除之间），样式同删除钮。
    m_copyUuidButton = new QPushButton(m_rowActions);
    m_copyUuidButton->setObjectName(copyUuidButtonName());
    m_copyUuidButton->setToolTip(copyUuidText());
    m_copyUuidButton->setAccessibleName(copyUuidText());
    Theme::markButton(m_copyUuidButton, Theme::ButtonKind::Ghost);
    Theme::markIconButton(m_copyUuidButton);
    m_copyUuidButton->setFixedSize(30, 30);
    m_copyUuidButton->setIconSize(QSize(16, 16));
    {
        const auto tint = [b = m_copyUuidButton] {
            b->setIcon(VersionIcons::line(QStringLiteral("copy"), 16, Theme::tokens().text));
        };
        tint();
        Theme::follow(m_copyUuidButton, tint);
    }
    rowActionsLayout->addWidget(m_switchButton);
    rowActionsLayout->addWidget(m_copyUuidButton);
    rowActionsLayout->addWidget(m_removeButton);
    connect(m_accountList->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &AccountSkinDialog::placeRowActions);

    // 「添加账号」：点开弹出菜单选「微软账号」或「离线账号」（菜单走全局的玻璃菜单样式）。
    m_addAccountButton = new QPushButton(addAccountButtonText(), this);
    m_addAccountButton->setObjectName(addAccountButtonName());
    m_addAccountButton->setToolTip(loginHintNote());
    Theme::markButton(m_addAccountButton, Theme::ButtonKind::Primary);
    m_addAccountButton->setProperty("wtfmlDialogActions", true);
    m_addAccountMenu = new QMenu(m_addAccountButton);
    m_addAccountMenu->setObjectName(addAccountMenuName());
    m_addAccountMenu->setToolTipsVisible(true);
    m_addMicrosoftAction = m_addAccountMenu->addAction(microsoftAccountText());
    m_addMicrosoftAction->setObjectName(addMicrosoftActionName());
    m_addOfflineAction = m_addAccountMenu->addAction(offlineAccountText());
    m_addOfflineAction->setObjectName(addOfflineActionName());
    {
        auto *addRow = new QHBoxLayout;
        addRow->setContentsMargins(0, 0, 0, 0);
        addRow->addWidget(m_addAccountButton, 0, Qt::AlignLeft);
        addRow->addStretch(1);
        left->addLayout(addRow);
    }

    // 离线账号那一路：「添加账号」里选了「离线账号」才展开，名字 +「添加」。
    m_offlineRow = new QWidget(this);
    m_offlineRow->setObjectName(offlineRowName());
    QHBoxLayout *offlineRow = new QHBoxLayout(m_offlineRow);
    offlineRow->setContentsMargins(0, 0, 0, 0);
    offlineRow->setSpacing(8);
    left->addWidget(m_offlineRow);
    // 离线行默认收起后左列比立绘矮：余高落在列底，别分进标题和列表之间的缝里。
    left->addStretch(1);
    m_offlineNameEdit = new QLineEdit(m_offlineRow);
    m_offlineNameEdit->setObjectName(offlineNameEditName());
    m_offlineNameEdit->setPlaceholderText(offlineNamePlaceholder());
    // 名字最多 16 个字符（AccountStore::validateOfflineName），框只留这么长，「添加」紧跟在后面。
    InputSizing::shortValue(m_offlineNameEdit, 16, InputSizing::Script::Han);
    m_addOfflineButton = new QPushButton(tr("添加"), m_offlineRow);
    m_addOfflineButton->setObjectName(addOfflineButtonName());
    m_addOfflineButton->setToolTip(addOfflineButtonText());
    m_addOfflineButton->setAccessibleName(addOfflineButtonText());
    Theme::markButton(m_addOfflineButton, Theme::ButtonKind::Neutral);
    offlineRow->addWidget(m_offlineNameEdit, 100);  // 拉伸比 100:1，框先长到上限，余宽才落到行尾
    offlineRow->addWidget(m_addOfflineButton);
    offlineRow->addStretch(1);
    m_offlineRow->hide();

    m_refreshButton = new QPushButton(refreshButtonText(), this);
    m_refreshButton->setObjectName(refreshButtonName());
    Theme::markRefreshButton(m_refreshButton);  // F5 认它
    // 「刷新外观」刷的是**这一整个窗**（账号列表 + 皮肤 + 披风），所以放页头，不放某一栏里。
    Theme::markButton(m_refreshButton, Theme::ButtonKind::Neutral);
    {  // 图标颜色跟主题走（构造时上色的话换主题后还是上一套的字色）
        const auto tint = [b = m_refreshButton] { b->setIcon(VersionIcons::line(QStringLiteral("refresh"), 16, Theme::tokens().text)); };
        tint();
        Theme::follow(m_refreshButton, tint);
    }
    statusRow->addStretch(1);  // 标题列有最大宽，余下的空白留在中间，别让按钮吃成一整条
    statusRow->addWidget(m_refreshButton, 0, Qt::AlignTop);

    // ---------- 中：立绘（设计稿 .stage）----------
    m_viewer = new SkinViewer(this);
    m_viewer->setObjectName(viewerName());
    m_viewer->setFixedSize(288, 432);
    m_viewer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // 一进来就有东西可看：没登录、还没取到、取不到，看到的都是这套默认外观。
    m_viewer->setSkin(SkinModel::defaultSteveSkin(), QImage(), false);
    // 预览下面留一行说明（离线账号的皮肤和游戏里不一致时才有字，见 setPreviewNote()）。
    auto *stage = new QWidget(this);
    stage->setObjectName(QStringLiteral("accountStage"));
    auto *stageLayout = new QVBoxLayout(stage);
    stageLayout->setContentsMargins(0, 0, 0, 0);
    stageLayout->setSpacing(8);
    stageLayout->addWidget(m_viewer, 0, Qt::AlignHCenter);
    m_previewNote = new PreviewNoteLabel(stage);
    m_previewNote->setWordWrap(true);
    m_previewNote->setFont(Theme::textFont(m_previewNote->font(), Theme::TextRole::Caption));
    {
        const auto tint = [label = m_previewNote] {
            QPalette palette = label->palette();
            palette.setColor(QPalette::WindowText, Theme::tokens().secondary);
            label->setPalette(palette);
        };
        tint();
        Theme::follow(m_previewNote, tint);
    }
    m_previewNote->setObjectName(previewNoteName());
    m_previewNote->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_previewNote->hide();
    stageLayout->addWidget(m_previewNote);
    main->addStretch(1);
    main->addWidget(stage, 0, Qt::AlignTop);
    main->addStretch(1);

    // ---------- 右：皮肤 / 披风（设计稿 .looks）----------
    auto *controlsPanel = new QWidget(this);
    m_looksPanel = controlsPanel;
    controlsPanel->setObjectName(QStringLiteral("accountLooksPane"));
    controlsPanel->setFixedWidth(kControlsWidth);
    auto *controls = new QVBoxLayout(controlsPanel);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(10);
    controls->setAlignment(Qt::AlignTop);
    main->addWidget(controlsPanel, 0, Qt::AlignTop);

    controls->addWidget(makeSectionTitle(tr("皮肤"), this));
    m_variantCombo = new QComboBox(this);
    m_variantCombo->setObjectName(variantComboName());
    m_variantCombo->addItem(variantClassicText(), variantClassicValue());
    m_variantCombo->addItem(variantSlimText(), variantSlimValue());
    // 滚轮防护：鼠标划过会静默换成另一种模型，接着点「上传」传上去的就是错的那种。
    WheelGuard::install(m_variantCombo);
    controls->addWidget(m_variantCombo);

    auto *skinButtons = new QHBoxLayout();
    skinButtons->setContentsMargins(0, 0, 0, 0);
    skinButtons->setSpacing(8);
    m_uploadButton = new QPushButton(uploadButtonText(), this);
    m_uploadButton->setObjectName(uploadButtonName());
    {  // 图标颜色跟主题走（构造时上色的话换主题后还是上一套的字色）
        const auto tint = [b = m_uploadButton] { b->setIcon(VersionIcons::line(QStringLiteral("upload"), 16, Theme::tokens().text)); };
        tint();
        Theme::follow(m_uploadButton, tint);
    }
    Theme::markButton(m_uploadButton, Theme::ButtonKind::Neutral);
    skinButtons->addWidget(m_uploadButton);

    // ---------- 离线账号那一档：换一张 / 恢复默认 ----------
    // 和「上传皮肤…」互斥（同一行里只露一档，见 setOfflineSkinState()）：离线账号没有令牌，
    // 上传按下去只会失败；它要的是"从盘上挑一张"。默认藏着。
    m_offlineSkinButton = new QPushButton(offlineSkinButtonText(), this);
    m_offlineSkinButton->setObjectName(offlineSkinButtonName());
    m_offlineSkinButton->setAutoDefault(false);
    m_offlineSkinButton->setToolTip(offlineSkinButtonToolTip());
    {  // 图标颜色跟主题走（构造时上色的话换主题后还是上一套的字色）
        const auto tint = [b = m_offlineSkinButton] { b->setIcon(VersionIcons::line(QStringLiteral("upload"), 16, Theme::tokens().text)); };
        tint();
        Theme::follow(m_offlineSkinButton, tint);
    }
    Theme::markButton(m_offlineSkinButton, Theme::ButtonKind::Neutral);
    m_offlineSkinResetButton = new QPushButton(offlineSkinResetButtonText(), this);
    m_offlineSkinResetButton->setObjectName(offlineSkinResetButtonName());
    m_offlineSkinResetButton->setAutoDefault(false);
    m_offlineSkinResetButton->setToolTip(offlineSkinResetButtonToolTip());
    // 「恢复默认外观」= 撤掉一层（和「不显示披风」同一档）。
    Theme::markButton(m_offlineSkinResetButton, Theme::ButtonKind::Ghost);
    skinButtons->addWidget(m_offlineSkinButton);
    skinButtons->addWidget(m_offlineSkinResetButton);
    skinButtons->addStretch(1);
    controls->addLayout(skinButtons);
    m_offlineSkinButton->hide();
    m_offlineSkinResetButton->hide();

    controls->addSpacing(10);
    {
        auto *capeHeader = new QHBoxLayout();
        capeHeader->setContentsMargins(0, 0, 0, 0);
        capeHeader->setSpacing(8);
        capeHeader->addWidget(makeSectionTitle(capeSectionTitle(), this));
        capeHeader->addStretch(1);
        m_applyCapeButton = new QPushButton(tr("应用"), this);
        m_applyCapeButton->setObjectName(applyCapeButtonName());
        m_applyCapeButton->setToolTip(applyCapeButtonText());
        Theme::markButton(m_applyCapeButton, Theme::ButtonKind::Neutral);
        capeHeader->addWidget(m_applyCapeButton);
        controls->addLayout(capeHeader);
    }

    m_capeLoadingLabel = new QLabel(this);
    m_capeLoadingLabel->setObjectName(QStringLiteral("accountCapeLoading"));
    m_capeLoadingLabel->setFont(Theme::textFont(m_capeLoadingLabel->font(), Theme::TextRole::Caption));
    m_capeLoadingLabel->setWordWrap(true);
    m_capeLoadingLabel->hide();
    controls->addWidget(m_capeLoadingLabel);

    m_capeList = new QListWidget(this);
    m_capeList->setObjectName(capeListName());
    makeListTransparent(m_capeList);
    m_capeList->setViewMode(QListView::IconMode);
    m_capeList->setFlow(QListView::LeftToRight);
    m_capeList->setWrapping(true);
    m_capeList->setResizeMode(QListView::Adjust);
    m_capeList->setMovement(QListView::Static);
    m_capeList->setSpacing(0);
    m_capeList->setItemDelegate(new CapeCellDelegate(m_capeList));
    m_capeList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    controls->addWidget(m_capeList);
    m_capeList->hide();  // 没有披风时不留一块空格子（setCapes() 按有没有再露出来）

    // ---------- 底：关闭 ----------
    // NOTE: 只有「关闭」，没有「确定」—— 这个窗里的每一件事（切换、上传、换披风）**当场
    // 就生效**，没有"确定之后才写盘"这一层。
    QHBoxLayout *bottom = new QHBoxLayout();
    root->addLayout(bottom);
    bottom->addStretch(1);
    m_closeButton = new QPushButton(closeButtonText(), this);
    m_closeButton->setObjectName(closeButtonName());
    Theme::markButton(m_closeButton, Theme::ButtonKind::Neutral);
    bottom->addWidget(m_closeButton);

    // ---------- 接线 ----------
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::close);
    connect(m_addAccountButton, &QPushButton::clicked, this, [this] {
        m_addAccountMenu->popup(m_addAccountButton->mapToGlobal(QPoint(0, m_addAccountButton->height() + 4)));
    });
    connect(m_addMicrosoftAction, &QAction::triggered, this, &AccountSkinDialog::loginRequested);
    connect(m_addOfflineAction, &QAction::triggered, this, &AccountSkinDialog::showOfflineAddRow);
    connect(m_retryButton, &QPushButton::clicked, this, &AccountSkinDialog::refreshRequested);
    connect(m_copyUuidButton, &QPushButton::clicked, this, [this] {
        const QString uuid = selectedUuid();
        if (uuid.isEmpty()) return;
        QGuiApplication::clipboard()->setText(uuid);
        // 没有别的反馈：图标换成对勾停一会儿，告诉用户复制上了。
        m_copyUuidButton->setIcon(VersionIcons::line(QStringLiteral("check"), 16, Theme::tokens().ok));
        QTimer::singleShot(1200, m_copyUuidButton, [b = m_copyUuidButton] {
            b->setIcon(VersionIcons::line(QStringLiteral("copy"), 16, Theme::tokens().text));
        });
    });
    connect(m_addOfflineButton, &QPushButton::clicked, this,
            &AccountSkinDialog::submitOfflineName);
    // 在框里直接回车 = 点那个按钮。
    connect(m_offlineNameEdit, &QLineEdit::returnPressed, this,
            &AccountSkinDialog::submitOfflineName);
    connect(m_refreshButton, &QPushButton::clicked, this, &AccountSkinDialog::refreshRequested);
    connect(m_uploadButton, &QPushButton::clicked, this, &AccountSkinDialog::pickSkinFile);
    connect(m_offlineSkinButton, &QPushButton::clicked, this,
            &AccountSkinDialog::pickOfflineSkinFile);
    connect(m_offlineSkinResetButton, &QPushButton::clicked, this,
            &AccountSkinDialog::offlineSkinResetRequested);
    connect(m_accountList, &QListWidget::itemSelectionChanged, this,
            &AccountSkinDialog::refreshButtonStates);
    connect(m_accountList, &QListWidget::currentRowChanged, this,
            &AccountSkinDialog::placeRowActions);
    connect(m_capeList, &QListWidget::itemSelectionChanged, this,
            &AccountSkinDialog::refreshButtonStates);
    connect(m_switchButton, &QPushButton::clicked, this, [this] {
        const QString uuid = selectedUuid();
        if (!uuid.isEmpty()) {
            emit switchRequested(uuid);
        }
    });
    connect(m_removeButton, &QPushButton::clicked, this, [this] {
        const QString uuid = selectedUuid();
        if (!uuid.isEmpty()) {
            emit removeRequested(uuid);
        }
    });
    connect(m_applyCapeButton, &QPushButton::clicked, this, [this] {
        const QListWidgetItem *item = m_capeList->currentItem();
        if (item == nullptr) return;
        const QString id = item->data(Qt::UserRole).toString();
        if (id.isEmpty()) {  // 「无披风」那一项
            m_previewCapeId.clear();
            m_noCapeApplied = true;
            updatePreviewCape(false, true);
            emit hideCapeRequested();
            return;
        }
        m_previewCapeId = id;
        m_noCapeApplied = false;
        updatePreviewCape(true, true);
        emit capeSelectRequested(id);
    });

    refreshButtonStates();
}

void AccountSkinDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    OverlayHost *host = nullptr;
    for (auto *ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        if ((host = qobject_cast<OverlayHost *>(ancestor))) break;
    }
    // 标题行两种形态都留在本页顶上（设计稿：面包屑下面一行大标题）；内嵌页已有返回，不占页脚。
    m_closeButton->setVisible(!(host && host->isOpen()));
    placeRowActions();
}

void AccountSkinDialog::hideEvent(QHideEvent *event) {
    QDialog::hideEvent(event);
    // 离开这一页（或页面被收起）后再回来，离线名字框是收起、清空的。
    if (!event->spontaneous() && !m_offlineRow->isHidden()) clearOfflineNameDraft();
}

void AccountSkinDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    // 三档：宽（≥860）账号 | 立绘 | 皮肤披风 三栏；中宽（最小窗 900 逻辑宽就落在这一档）两栏，
    // 皮肤 / 披风挪进账号栏下面，立绘独占右栏、随右栏宽放大；再窄（实际到不了，窗口最小宽 900）
    // 才整列上下堆叠。中宽不再整列堆叠：那样右半边空着、披风被挤出窗口要滚动。
    const int w = width();
    const bool wide = w >= 860;
    const bool stacked = !wide && w < kAccountsWidth + 28 + 240;
    auto *left = static_cast<QBoxLayout *>(m_accountsPanel->layout());
    const bool looksInLeft = left->indexOf(m_looksPanel) >= 0;
    if (wide == looksInLeft) {
        (looksInLeft ? left : m_bodyLayout)->removeWidget(m_looksPanel);
        if (wide) {
            m_bodyLayout->addWidget(m_looksPanel, 0, Qt::AlignTop);
        } else {
            left->insertWidget(left->count() - 1, m_looksPanel);  // 列底那段 stretch 之前
        }
        // 放进账号栏时上面再补 8：和栏内控件间距 8 合起来是区块间距 16。
        m_looksPanel->layout()->setContentsMargins(0, wide ? 0 : 8, 0, 0);
    }
    m_bodyLayout->setDirection(stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_accountsPanel->setMaximumWidth(stacked ? QWIDGETSIZE_MAX : kAccountsWidth);
    // 立绘 2:3。中宽时右栏 = 页宽 - 账号栏 - 栏距，左右各留 24 后撑满，封顶 288（再大就盖过左栏高度）。
    // 立绘给足面积：披风是挂在人物背后的，缩到 208 宽时只看得见一小条。
    const int viewerW = (wide || stacked) ? 288 : qBound(208, w - kAccountsWidth - 28 - 48, 288);
    m_viewer->setFixedSize(viewerW, viewerW * 3 / 2);
}

bool AccountSkinDialog::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_accountList->viewport() && event->type() == QEvent::Resize) {
        // 行矩形要等列表重新布局之后才是新宽度，排到下一轮再摆（窗口拉窄时行尾按钮不会留在旧位置）。
        QMetaObject::invokeMethod(this, &AccountSkinDialog::placeRowActions, Qt::QueuedConnection);
    }
    return QDialog::eventFilter(watched, event);
}

void AccountSkinDialog::placeRowActions() {
    QListWidgetItem *item = m_accountList->currentItem();
    if (item == nullptr || selectedUuid().isEmpty()) {
        m_rowActions->hide();
        placeReloginButtons();
        return;
    }
    const QRect row = m_accountList->visualItemRect(item);
    // 「切换到所选」对正在用的那个账号是空操作：那一行只留「删除所选」。
    // 需要重新登录的账号凭据已失效，切过去也启动不了，且「重新登录」成功后自然成为当前账号：同样不给切换。
    m_switchButton->setVisible(!item->data(Qt::UserRole + 1).toBool() && !item->data(kReloginRole).toBool());
    m_rowActions->adjustSize();
    const QSize size = m_rowActions->sizeHint();
    m_rowActions->setGeometry(row.right() - size.width() - 10, row.center().y() - size.height() / 2,
                              size.width(), size.height());
    m_rowActions->show();
    m_rowActions->raise();
    if (m_accountList->property("wtfmlRowActionsWidth").toInt() != size.width() + 10) {
        m_accountList->setProperty("wtfmlRowActionsWidth", size.width() + 10);
        m_accountList->viewport()->update();
    }
    placeReloginButtons();
}

void AccountSkinDialog::placeReloginButtons() {
    const QRect viewport = m_accountList->viewport()->rect();
    for (QPushButton *button : std::as_const(m_reloginButtons)) {
        QListWidgetItem *item = nullptr;
        const QString uuid = button->property("wtfmlReloginUuid").toString();
        for (int i = 0; i < m_accountList->count() && item == nullptr; ++i) {
            if (m_accountList->item(i)->data(Qt::UserRole).toString() == uuid) item = m_accountList->item(i);
        }
        const QRect row = item != nullptr ? m_accountList->visualItemRect(item) : QRect();
        if (row.isNull() || !viewport.intersects(row)) {
            button->hide();
            continue;
        }
        // 选中那一行：贴在「复制 UUID」左边（间距 4，同行尾那几颗）；没选中：贴行尾（右留 10）。
        const bool selectedRow = item == m_accountList->currentItem() && m_rowActions->isVisible();
        const int right = selectedRow ? m_rowActions->geometry().left() - 4 : row.right() - 10;
        const QSize size = button->sizeHint();
        button->setGeometry(right - size.width(), row.center().y() - size.height() / 2, size.width(), size.height());
        button->show();
        button->raise();
    }
}

void AccountSkinDialog::setAccounts(const QList<AccountRow> &rows) {
    // 按钮可能正是点击它的那一刻触发了重建：摘下来、排到下一轮再删。
    for (QPushButton *button : std::as_const(m_reloginButtons)) {
        button->hide();
        button->setParent(nullptr);
        button->deleteLater();
    }
    m_reloginButtons.clear();
    m_accountList->setProperty("wtfmlReloginWidth", 0);
    m_accountList->clear();
    for (const AccountRow &row : rows) {
        addAccountRow(row);
    }

    // 一个账号都没有时整块收起，页签下面直接接入口；「未登录」由页头副题说一次（notLoggedInNote()）。
    m_accountList->setVisible(m_accountList->count() > 0);
    if (m_accountList->count() > 0) {
        // 选中"当前"那一条；没有当前账号（凭据被清了）就选第一条。
        int index = 0;
        for (int i = 0; i < m_accountList->count(); ++i) {
            if (m_accountList->item(i)->data(Qt::UserRole + 1).toBool()) {
                index = i;
                break;
            }
        }
        m_accountList->setCurrentRow(index);
    }
    refreshButtonStates();
    placeRowActions();  // 末尾会顺带摆「重新登录」
}

void AccountSkinDialog::addAccountRow(const AccountRow &row) {
    QListWidgetItem *item = new QListWidgetItem(m_accountList);
    // 两个标记可能同时出现（一个离线账号正被用着）：离线那句在前 —— 它说的是"这是个什么
    // 账号"，"当前"是它现在的状态。
    QString text = row.name;
    if (row.offline) {
        text += offlineMark();
    }
    if (row.current) {
        text += currentMark();
    }
    item->setText(text);
    item->setData(Qt::UserRole, row.uuid);
    item->setData(Qt::UserRole + 1, row.current);
    item->setData(kNameRole, row.name);
    item->setData(kOfflineRole, row.offline);
    item->setData(kSavedRole, row.savedAt);
    item->setData(kFaceRole, row.face);
    const bool relogin = row.needsRelogin && !row.offline;
    item->setData(kReloginRole, relogin);
    // 名字下面那行小字：账号来源；微软账号后面照旧带「登录取自 …」。
    QString source = row.offline ? offlineAccountText() : microsoftAccountText();
    const QDateTime saved = QDateTime::fromString(row.savedAt, Qt::ISODate);
    if (relogin) {
        source = needsReloginText();
    } else if (!row.offline && saved.isValid()) {
        source += QStringLiteral(" · ") + tr("登录于 %1").arg(saved.toLocalTime().toString(tr("M 月 d 日")));
    }
    item->setData(kSourceRole, source);
    if (relogin) {
        // 和「添加账号 → 微软账号」同一条浏览器登录流程；成功后标记被清掉、列表刷新，这一行恢复原样。
        auto *button = new QPushButton(reloginButtonText(), m_accountList->viewport());
        button->setObjectName(reloginButtonName());
        button->setProperty("wtfmlReloginUuid", row.uuid);
        button->setEnabled(m_loginUnavailable.isEmpty());
        button->setToolTip(m_loginUnavailable);
        Theme::markButton(button, Theme::ButtonKind::Ghost);
        button->hide();
        button->ensurePolished();  // 量宽要等样式表套上
        connect(button, &QPushButton::clicked, this, &AccountSkinDialog::loginRequested);
        m_reloginButtons.append(button);
        m_accountList->setProperty("wtfmlReloginWidth", button->sizeHint().width());
    }
    if (row.offline) {
        // 离线账号没有"登录取自什么时候"这回事 —— 摆一个空的 tooltip 还不如说清它是哪一种。
        item->setToolTip(offlineRowTooltip());
    } else if (!row.savedAt.isEmpty()) {
        const QDateTime savedAt = QDateTime::fromString(row.savedAt, Qt::ISODate);
        if (savedAt.isValid()) {
            item->setToolTip(tr("登录于 %1")
                                 .arg(savedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        }
    }
    if (relogin) {
        // 选中时行尾按钮多，来源那句可能被截断：全文放进悬停提示。
        item->setToolTip(item->toolTip().isEmpty() ? needsReloginText() : needsReloginText() + QLatin1Char('\n') + item->toolTip());
    }
}

void AccountSkinDialog::clearOfflineNameDraft() {
    m_offlineNameEdit->clear();
    m_offlineRow->hide();  // 添加成功了：输入行收回去，下次要加再从「添加账号」里选
}

void AccountSkinDialog::showOfflineAddRow() {
    m_offlineRow->show();
    if (isVisible()) m_offlineNameEdit->setFocus();
}

bool AccountSkinDialog::offlineAddRowShown() const { return !m_offlineRow->isHidden(); }

void AccountSkinDialog::setOnlineLoginUnavailable(const QString &reason) {
    m_addMicrosoftAction->setEnabled(reason.isEmpty());
    m_addMicrosoftAction->setToolTip(reason);
    m_loginUnavailable = reason;
    for (QPushButton *button : std::as_const(m_reloginButtons)) {
        button->setEnabled(reason.isEmpty());
        button->setToolTip(reason);
    }
}

void AccountSkinDialog::setStatusText(const QString &text) {
    m_statusLabel->setText(text);
    m_statusLabel->setToolTip(text);
}

void AccountSkinDialog::setStatusError(const QString &reason) {
    m_statusErrorLabel->setText(reason);
    m_statusErrorLabel->setToolTip(reason);
    m_statusErrorRow->setVisible(!reason.isEmpty());
}

void AccountSkinDialog::setPreviewNote(const QString &text) {
    m_previewNote->setText(text);
    m_previewNote->updateGeometry();
    m_previewNote->setVisible(!text.isEmpty());
}

void AccountSkinDialog::setLooks(const QImage &skin, const QImage &cape, bool slim) {
    Q_UNUSED(cape);
    QImage effectiveCape;
    if (m_capesLoaded && !m_noCapeApplied) {
        // 只认披风列表里「应用过的那件」/当前披风。资料接口常常带回旧图
        // （你这里永远是 15 周年），打开动画播到一半更不能先闪它一下。
        for (int i = 0; i < m_capeList->count(); ++i) {
            QListWidgetItem *row = m_capeList->item(i);
            const bool wanted = !m_previewCapeId.isEmpty()
                ? row->data(Qt::UserRole).toString() == m_previewCapeId
                : row->data(Qt::UserRole + 1).toBool();
            if (wanted) {
                effectiveCape = row->data(Qt::UserRole + 3).value<QImage>();
                break;
            }
        }
    }
    m_viewer->setSkin(skin.isNull() ? SkinModel::defaultSteveSkin() : skin, effectiveCape, slim);
    m_currentSkin = skin.isNull() ? SkinModel::defaultSteveSkin() : skin;
    m_accountList->viewport()->update();
    // 下拉框跟着**当前皮肤**走：用户多半是"在这个模型上调一下"，而不是每次都从头选。
    const QString variant = slim ? variantSlimValue() : variantClassicValue();
    const int index = m_variantCombo->findData(variant);
    if (index >= 0) {
        m_variantCombo->setCurrentIndex(index);
    }
}

void AccountSkinDialog::setCapeLoading(const QString &text) {
    m_capeLoadingLabel->setText(text);
    m_capeLoadingLabel->setVisible(!text.isEmpty());
}

void AccountSkinDialog::clearCapes() {
    m_capesOwner.clear();
    setCapes({});
    m_capeList->hide();
}

void AccountSkinDialog::setCapes(const QList<CapeRow> &capes, bool keepSelection) {
    const bool hadSelection = keepSelection && m_capeList->currentItem() != nullptr;
    const QString selectedId = selectedCapeId();
    const QSignalBlocker blocker(m_capeList);
    if (capes.isEmpty()) {
        m_previewCapeId.clear();
        m_noCapeApplied = false;
    }
    m_capesLoaded = !capes.isEmpty();
    const bool wearing = std::any_of(capes.cbegin(), capes.cend(), [](const CapeRow &cape) { return cape.active; });
    m_capeList->clear();
    QListWidgetItem *none = new QListWidgetItem(m_capeList);
    none->setData(Qt::AccessibleTextRole, noCapeText());
    none->setData(Qt::UserRole, QString());
    none->setData(Qt::UserRole + 1, !wearing);
    none->setData(Qt::UserRole + 4, true);
    for (const CapeRow &cape : capes) {
        QListWidgetItem *item = new QListWidgetItem(m_capeList);
        item->setData(Qt::AccessibleTextRole, cape.alias);
        item->setData(Qt::UserRole, cape.id);
        item->setData(Qt::UserRole + 1, cape.active);
        item->setData(Qt::UserRole + 2, cape.url);
        if (m_capeTextures.contains(cape.url)) {
            item->setData(Qt::UserRole + 3, m_capeTextures.value(cape.url));
        }
    }
    // 高度按实际件数算，不再固定两排：有几排就露几排，超长时由外层滚动。
    const int columns = qMax(1, (kControlsWidth - 8) / (kCapeCell + kCapeGap));
    const int rows = qMax(1, (m_capeList->count() + columns - 1) / columns);
    m_capeList->setFixedHeight(rows * (kCapeCellHeight + kCapeGap) + 4);
    int selectedRow = -1;
    if (hadSelection) {
        for (int i = 0; i < m_capeList->count(); ++i) {
            if (m_capeList->item(i)->data(Qt::UserRole).toString() == selectedId) {
                selectedRow = i;
                break;
            }
        }
    }
    m_capeList->setCurrentRow(selectedRow);
    m_capeList->setVisible(true);
    updatePreviewCape(!m_noCapeApplied && (wearing || !m_previewCapeId.isEmpty()));
    refreshButtonStates();
}

void AccountSkinDialog::setCapeTexture(const QString &url, const QImage &texture) {
    if (url.isEmpty() || texture.isNull()) return;
    for (int i = 0; i < m_capeList->count(); ++i) {
        QListWidgetItem *item = m_capeList->item(i);
        if (item->data(Qt::UserRole + 2).toString() == url) {
            m_capeTextures.insert(url, texture);
            item->setData(Qt::UserRole + 3, texture);
            const bool active = item->data(Qt::UserRole + 1).toBool();
            const bool preview = !m_previewCapeId.isEmpty() &&
                                 item->data(Qt::UserRole).toString() == m_previewCapeId;
            if (!m_noCapeApplied && (preview || (m_previewCapeId.isEmpty() && active))) {
                updatePreviewCape(true);
            }
        }
    }
}

void AccountSkinDialog::updatePreviewCape(bool wearing, bool turnToBack) {
    QImage cape;
    if (wearing) {
        for (int i = 0; i < m_capeList->count(); ++i) {
            const auto *row = m_capeList->item(i);
            const bool wanted = m_previewCapeId.isEmpty()
                ? row->data(Qt::UserRole + 1).toBool()
                : row->data(Qt::UserRole).toString() == m_previewCapeId;
            if (wanted) {
                cape = row->data(Qt::UserRole + 3).value<QImage>();
                break;
            }
        }
    }
    const bool slim = m_variantCombo != nullptr &&
                      m_variantCombo->currentData().toString() == variantSlimValue();
    m_viewer->setSkin(m_currentSkin.isNull() ? SkinModel::defaultSteveSkin() : m_currentSkin,
                      cape, slim);
    // 只有用户主动点「显示 / 隐藏」才转视角；打开页面时保持默认正面。
    if (turnToBack) m_viewer->setBackView(wearing);
}

void AccountSkinDialog::setBusy(bool busy, const QString &what) {
    m_busy = busy;
    m_addAccountButton->setEnabled(!busy);
    m_refreshButton->setEnabled(!busy);
    m_uploadButton->setEnabled(!busy);
    // 离线那一档的两颗一起灰（忙的时候下面 refreshButtonStates() 也会按 idle 重算一遍）。
    m_offlineSkinButton->setEnabled(!busy);
    m_offlineSkinResetButton->setEnabled(!busy);
    // 登录将更换活动账号，期间停用两个列表的操作。
    m_accountList->setEnabled(!busy);
    m_capeList->setEnabled(!busy);
    if (busy) {
        m_statusLabel->setText(what);
    }
    refreshButtonStates();
}

void AccountSkinDialog::setOfflineSkinState(bool offline, bool hasCustomSkin) {
    m_offlineSkin = offline;
    m_hasCustomSkin = hasCustomSkin;
    m_uploadButton->setVisible(!offline);
    m_offlineSkinButton->setVisible(offline);
    m_offlineSkinResetButton->setVisible(offline);
    refreshButtonStates();
}

QString AccountSkinDialog::selectedUuid() const {
    const QListWidgetItem *item = m_accountList->currentItem();
    if (item == nullptr || !item->flags().testFlag(Qt::ItemIsSelectable)) {
        return QString();
    }
    return item->data(Qt::UserRole).toString();
}

QString AccountSkinDialog::selectedCapeId() const {
    const QListWidgetItem *item = m_capeList->currentItem();
    return item == nullptr ? QString() : item->data(Qt::UserRole).toString();
}

QString AccountSkinDialog::capeUrl(const QString &capeId) const {
    if (capeId.isEmpty()) return QString();
    for (int i = 0; i < m_capeList->count(); ++i) {
        const QListWidgetItem *item = m_capeList->item(i);
        if (item->data(Qt::UserRole).toString() == capeId) {
            return item->data(Qt::UserRole + 2).toString();
        }
    }
    return QString();
}

QString AccountSkinDialog::selectedVariant() const {
    const QString value = m_variantCombo->currentData().toString();
    return value.isEmpty() ? variantClassicValue() : value;
}

void AccountSkinDialog::pickSkinFile() {
    // NOTE: 这一处也是独立窗（Windows 自己的文件选择框）—— 理由和取舍见
    // MainWindow::onAddFolderClicked() 上面那一段。
    const QString path = QFileDialog::getOpenFileName(this, tr("选择皮肤图片"), QString(),
                                                      tr("PNG 图片 (*.png)"));
    if (path.isEmpty()) {
        return;  // 用户取消了，不是错误
    }
    // NOTE: 这里**只**把路径交出去，不自己校验尺寸 —— 尺寸那条规矩在
    // SkinApi::isAcceptableSkinSize() 里（唯一出处），校验失败由 MainWindow 弹窗说。
    emit skinUploadRequested(path, selectedVariant());
}

void AccountSkinDialog::pickOfflineSkinFile() {
    // 和上面那颗同一个选择框；区别只在**交出去的那件事**是什么（见头文件那两个信号）。
    const QString path = QFileDialog::getOpenFileName(this, tr("选择皮肤图片"), QString(),
                                                      tr("PNG 图片 (*.png)"));
    if (path.isEmpty()) {
        return;  // 用户取消了，不是错误
    }
    // 尺寸那条规矩同样不在这里判（唯一出处 SkinApi::isAcceptableSkinSize()，由 MainWindow 弹窗说）。
    emit offlineSkinPickRequested(path, selectedVariant());
}

void AccountSkinDialog::submitOfflineName() {
    // 名字合不合法**不在这里判**（空、有空白、太长都在 AccountStore::addOffline() 里，
    // 唯一出处）。这里只管把用户填的东西交出去。
    emit offlineAccountRequested(m_offlineNameEdit->text().trimmed());
}

void AccountSkinDialog::refreshButtonStates() {
    // 「切换到所选」「删除所选」「显示所选披风」三件事都**要有一个选中的东西**才做得了。
    // NOTE: 三个都要**同时**看「有没有选中」和「忙不忙」。只看选中那一半的话，
    // setBusy(true) 里末尾那次调用会把它们重新点亮（见 .hpp 里 m_busy 那段）。
    const bool idle = !m_busy;
    const bool haveAccount = !selectedUuid().isEmpty();
    m_switchButton->setEnabled(idle && haveAccount);
    m_removeButton->setEnabled(idle && haveAccount);
    m_applyCapeButton->setEnabled(idle && m_capeList->currentItem() != nullptr);
    // 加离线账号要跟着"忙不忙"灰：它一旦成功就会把活动槽清空（切到离线身份），而正飞着的
    // 那次登录回来还要往活动槽里写 —— 两个人会打架。
    m_offlineNameEdit->setEnabled(idle);
    m_addOfflineButton->setEnabled(idle);
    // 离线那一档：露着的时候才按 idle 判；「恢复默认外观」还要**确实有东西可撤**。
    m_offlineSkinButton->setEnabled(idle && m_offlineSkin);
    m_offlineSkinResetButton->setEnabled(idle && m_offlineSkin && m_hasCustomSkin);
}





