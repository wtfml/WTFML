/**
 * @file NewServerPage.cpp
 * @brief 「新建服务端」那一页的实现（形状与取舍见头文件）。
 */

#include "NewServerPage.hpp"
#include "InputSizing.hpp"
#include "LoadingState.hpp"

#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "MainWindowPageLayout.hpp"
#include "MainWindowUI.hpp"
#include "Theme.hpp"
#include "VersionIcons.hpp"

#include <QApplication>
#include <QComboBox>

using namespace MainWindowPageLayout;

namespace {

/// 端类别那几行的行高 / 图标边长（安装页那张「模组加载器」卡片是同一档）。
constexpr int kFlavorRowHeight = 44;
constexpr int kFlavorIconSize = 26;
/// 行内左边距：高亮框左缘仍和名称框对齐，图标退到名称框文字那条线上（名称框文字距框左缘 12 px，
/// 图标自己在格里已经靠右 3 px，这里补 10）。做在图标左侧的透明留白里，不碰列表的样式。
constexpr int kFlavorRowPad = 10;

/// 一档的头像：原版是草方块，Fabric / Forge / NeoForge 用各自的标志，
/// 认不出来的（Quilt / Paper / Purpur / Spigot）用拼图 —— 和列表里"这一份是什么"
/// 同一个记号（见 VersionIcons::puzzleMark 那段）。
QIcon flavorMark(const NewServerPage::FlavorOption &option) {
    QPixmap mark;
    if (option.token == QLatin1String("vanilla")) {
        mark = VersionIcons::grassBlock(kFlavorIconSize);
    } else {
        mark = VersionIcons::iconMarkForKey(option.token, kFlavorIconSize);
        if (mark.isNull()) {
            mark = VersionIcons::puzzleMark(kFlavorIconSize, QApplication::palette().color(QPalette::WindowText));
        }
    }
    // 左边补透明留白（kFlavorRowPad），图标按设备像素比整张重画。
    const qreal dpr = mark.devicePixelRatio();
    QPixmap padded(QSize(kFlavorIconSize + kFlavorRowPad, kFlavorIconSize) * dpr);
    padded.setDevicePixelRatio(dpr);
    padded.fill(Qt::transparent);
    QPainter painter(&padded);
    painter.drawPixmap(kFlavorRowPad, 0, mark);
    return QIcon(padded);
}

/// 一行小字（说明/计划）。和更新中心那一页的 `makeMutedLabel()` 是同一套做法。
QLabel *makeMutedLabel(QWidget *parent, const QString &objectName) {
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    QPalette palette = label->palette();
    QColor muted = palette.color(QPalette::WindowText);
    muted.setAlpha(150);
    palette.setColor(QPalette::WindowText, muted);
    label->setPalette(palette);
    return label;
}

/// 一个"标签 + 输入"的行（**整行是一个控件**，好一起露/收）。返回那一行的容器，
/// `*editOut` 里回那个输入框。
QWidget *makeRow(QWidget *parent, const QString &labelText, QLineEdit **editOut,
                 const QString &objectName) {
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto *label = new QLabel(labelText, row);
    label->setFixedWidth(96);
    layout->addWidget(label);
    auto *edit = new QLineEdit(row);
    edit->setObjectName(objectName);
    layout->addWidget(edit, 100);
    layout->addStretch(1);  // 输入框是短值（调用方 InputSizing::shortValue），多出的宽度留在行尾
    *editOut = edit;
    return row;
}

/// 一个"标签 + 任意控件"的行（下拉框 / 数字框那一类）。
QWidget *makeControlRow(QWidget *parent, const QString &labelText, QWidget *control,
                        bool stretchControl) {
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto *label = new QLabel(labelText, row);
    label->setFixedWidth(96);
    layout->addWidget(label);
    control->setParent(row);
    layout->addWidget(control, stretchControl ? 100 : 0);
    if (!stretchControl) {
        layout->addStretch(1);
    }
    return row;
}

}  // namespace

NewServerPage::NewServerPage(QWidget *parent) : QDialog(parent) {
    setObjectName(MainWindowUI::newServerPageName());
    setWindowTitle(windowTitle());
    // 只影响"单独拿出来用"（验证工程那几处）：嵌进覆盖层时这一页拿的是内容区的整幅宽。
    resize(720, 520);

    // 形状照搬下载页那张安装页（用户点名「服务端下载要和游戏下载相一致」）：
    // 中间是滚动区里几张卡片，底下「创建」那颗主动作浮在正中间。
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *content = new QWidget(this);
    content->setObjectName(QStringLiteral("newServerContent"));
    auto *contentLayout = new QGridLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(content, 1);

    // 底下那颗浮着的按钮会盖住最后一行：留出它的高度（和安装页同一个算式）。
    QVBoxLayout *bodyLayout = nullptr;
    auto *scroll = makePageScroll(content, 88, &bodyLayout);
    scroll->setObjectName(QStringLiteral("newServerScroll"));
    contentLayout->addWidget(scroll, 0, 0);
    auto *body = bodyLayout->parentWidget();
    bodyLayout->setSpacing(10);

    // ---------- 名称：直接摆在页面上 ----------
    //
    // 形状照搬安装页那个名字块（用户点名「服务端下载要和游戏下载相一致」）：
    // 一条小标题 + 一整幅输入框 + 底下一行淡色说明，**不套卡片**
    // （那一页的「版本名称」也是这么摆的）。
    auto *identity = new QWidget(body);
    identity->setObjectName(QStringLiteral("newServerIdentity"));
    auto *identityLayout = new QVBoxLayout(identity);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(6);
    identityLayout->addWidget(new QLabel(tr("名称"), identity));
    m_nameEdit = new QLineEdit(identity);
    m_nameEdit->setObjectName(MainWindowUI::newServerNameEditName());
    m_nameEdit->setPlaceholderText(tr("会当成这个服务端的目录名"));
    InputSizing::shortValue(m_nameEdit, InputSizing::kNameChars, InputSizing::Script::Han);
    identityLayout->addWidget(m_nameEdit);
    m_targetDirLabel = makeMutedLabel(identity, MainWindowUI::newServerTargetDirName());
    m_targetDirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // 路径可能很长：**不许**参与横向度量（同一条规矩，和摘要那一句一样）。
    m_targetDirLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_targetDirLabel->setToolTip(tr("服务端根目录下面以它的名字命名的那个目录"));
    // 没有路径可写（名字还空着）时整行不占位：空着的一行会把名称框和下面的「服务端」撑开 20 多 px。
    m_targetDirLabel->hide();
    identityLayout->addWidget(m_targetDirLabel);
    bodyLayout->addWidget(identity);

    // ---------- 计划说明（由 MainWindow 现算现喂）----------
    // 位置和安装页名字框底下那行淡字一样：紧跟着名字块，读下来才知道这一趟会做什么。
    m_summaryLabel = makeMutedLabel(body, MainWindowUI::newServerSummaryLabelName());
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // 折行 QLabel 的 sizeHint 是"整句排成一行"的宽度 —— 纯说明不许参与横向度量（同一条规矩）。
    m_summaryLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_summaryLabel->hide();
    bodyLayout->addWidget(m_summaryLabel);

    // ---------- 服务端：整页唯一的一张卡片 ----------
    QVBoxLayout *serverCardLayout = nullptr;
    auto *serverCard = makeSectionCard(tr("服务端"), body, &serverCardLayout);
    // 卡片本身没有底色，左右不留内边距：小标题、端类别、各行与上面的名称框、顶上的返回行同一条左缘。
    serverCardLayout->setContentsMargins(0, 16, 0, 18);
    // 端类别是**一排行**（不是下拉框）：和安装页那张「模组加载器」卡片同一个形状 ——
    // 一眼看得见有哪几档、每行点一下就选它。
    m_flavorList = new QListWidget(serverCard);
    m_flavorList->setObjectName(MainWindowUI::newServerFlavorListName());
    m_flavorList->setFrameShape(QFrame::NoFrame);
    m_flavorList->setStyleSheet(QStringLiteral("QListWidget{background:transparent;border:none;}"));
    m_flavorList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_flavorList->setIconSize(QSize(kFlavorIconSize + kFlavorRowPad, kFlavorIconSize));
    m_flavorList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_flavorList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    serverCardLayout->addWidget(m_flavorList);
    serverCardLayout->addWidget(makeRow(serverCard, tr("游戏版本"), &m_gameVersionEdit,
                                        MainWindowUI::newServerGameVersionEditName()));
    InputSizing::shortValue(m_gameVersionEdit, 20);  // 1.21.4、24w14potato 这一类版本号
    auto versionRow = [this, serverCard](const QString &title, const QString &name,
                                          QComboBox **comboOut, QLineEdit **editOut) {
        auto *combo = new QComboBox(serverCard);
        combo->setObjectName(name + "Combo");
        combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert);
        combo->lineEdit()->setReadOnly(true);
        combo->lineEdit()->setObjectName(name);
        // 建框时不写「正在获取…」：不发请求的档位（原版等）不该挂着一句假的加载话。
        // 真在等的时候由 setVersionsLoading() 画加载态，回来后 setVersionChoices() 写占位。
        InputSizing::shortValue(combo, 24);  // 加载器 / 安装器版本号，如 0.16.9、1.0.1-build.123
        *comboOut = combo; *editOut = combo->lineEdit();
        auto *row = makeControlRow(serverCard, title, combo, true);
        auto *retry = new QPushButton(tr("刷新"), row);
        retry->setAutoDefault(false);
        Theme::markButton(retry, Theme::ButtonKind::Neutral);
        row->layout()->addWidget(retry);
        static_cast<QHBoxLayout *>(row->layout())->addStretch(1);  // 下拉框封顶后，「刷新」跟在它后面
        connect(retry, &QPushButton::clicked, this, &NewServerPage::versionsRetryRequested);
        return row;
    };
    m_loaderRow = versionRow(tr("加载器版本"), MainWindowUI::newServerLoaderEditName(),
                             &m_loaderCombo, &m_loaderEdit);
    serverCardLayout->addWidget(m_loaderRow);
    m_installerRow = versionRow(tr("安装器版本"), MainWindowUI::newServerInstallerEditName(),
                                &m_installerCombo, &m_installerEdit);
    serverCardLayout->addWidget(m_installerRow);
    m_buildRow = makeRow(serverCard, tr("构建号"), &m_buildEdit,
                         MainWindowUI::newServerBuildEditName());
    InputSizing::shortValue(m_buildEdit, 12);  // 构建号是几位数字
    serverCardLayout->addWidget(m_buildRow);
    {
        m_memorySpin = new QSpinBox(serverCard);
        m_memorySpin->setObjectName(MainWindowUI::newServerMemorySpinName());
        m_memorySpin->setRange(512, 65536);
        m_memorySpin->setSingleStep(512);
        m_memorySpin->setValue(2048);
        m_memorySpin->setSuffix(QStringLiteral(" MB"));
        serverCardLayout->addWidget(
            makeControlRow(serverCard, tr("内存上限"), m_memorySpin, false));
    }
    bodyLayout->addWidget(serverCard);

    // 内容装不满时**上对齐**（和安装页那条收尾同一个写法）。
    bodyLayout->addStretch(1);

    // ---------- 底下那颗「创建」（浮在正中间）----------
    m_createButton = new QPushButton(tr("创建"), content);
    m_createButton->setObjectName(MainWindowUI::newServerCreateButtonName());
    // 这一页只有一个输入表单 + 一个主动作：回车就是「创建」（「刷新版本」那颗仍是
    // autoDefault(false)，不会因为抢到焦点变成默认按钮）。
    m_createButton->setAutoDefault(false);
    m_createButton->setDefault(true);
    m_createButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_createButton->setMinimumWidth(180);
    Theme::markButton(m_createButton, Theme::ButtonKind::Primary);
    QFont createFont = m_createButton->font();
    createFont = Theme::textFont(createFont, Theme::TextRole::Body);  // 按钮一律正文 14 系统字（tokens-r2 §1）
    createFont.setWeight(QFont::Medium);
    m_createButton->setFont(createFont);
    m_createButton->setMinimumHeight(48);
    auto *footer = new QFrame(content);
    footer->setObjectName(QStringLiteral("newServerFooter"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(0, 12, 0, 0);
    footerLayout->addWidget(m_createButton);
    contentLayout->addWidget(footer, 0, 0, Qt::AlignHCenter | Qt::AlignBottom);
    footer->raise();

    // 退回上一层（「← 返回版本列表」/ 实例页那颗）就是"取消"：这一层不另摆一颗，
    // 和安装页同一条规矩（页上只有一颗主动作）。
    connect(m_createButton, &QPushButton::clicked, this, &QDialog::accept);

    // 用户改任何一格 → 告诉翻译点重算计划说明。
    const auto emitChanged = [this] { emit fieldsChanged(); };
    connect(m_nameEdit, &QLineEdit::textChanged, this, emitChanged);
    connect(m_gameVersionEdit, &QLineEdit::textChanged, this, emitChanged);
    connect(m_loaderEdit, &QLineEdit::textChanged, this, emitChanged);
    connect(m_installerEdit, &QLineEdit::textChanged, this, emitChanged);
    connect(m_buildEdit, &QLineEdit::textChanged, this, emitChanged);
    connect(m_memorySpin, &QSpinBox::valueChanged, this, emitChanged);
    connect(m_flavorList, &QListWidget::currentRowChanged, this, [this](int) {
        applyFlavorVisibility();
        emit fieldsChanged();
    });
}

void NewServerPage::setFlavors(const QList<FlavorOption> &flavors, const QString &defaultToken) {
    m_flavors = flavors;
    const QSignalBlocker blocker(m_flavorList);  // 灌数据这一步不算"用户改了"
    m_flavorList->clear();
    int defaultRow = 0;
    for (int i = 0; i < flavors.size(); ++i) {
        const FlavorOption &option = flavors.at(i);
        auto *item = new QListWidgetItem(flavorMark(option), option.name, m_flavorList);
        item->setData(Qt::UserRole, option.token);
        item->setToolTip(option.note);
        item->setSizeHint(QSize(0, kFlavorRowHeight));
        if (option.token == defaultToken) {
            defaultRow = i;
        }
    }
    m_flavorList->setCurrentRow(defaultRow);
    // 行数变了高度跟着变（不留半行空着、也不出滚动条）。
    m_flavorList->setFixedHeight(qMax(1, flavors.size()) * kFlavorRowHeight + 4);
    applyFlavorVisibility();
}

void NewServerPage::setDefaults(const QString &name, const QString &gameVersion) {
    const QSignalBlocker b1(m_nameEdit);
    const QSignalBlocker b2(m_gameVersionEdit);
    if (m_gameVersionEdit->text() != gameVersion) {
        const QSignalBlocker b3(m_loaderEdit), b4(m_installerEdit), b5(m_buildEdit);
        m_loaderEdit->clear(); m_installerEdit->clear(); m_buildEdit->clear();
    }
    m_nameEdit->setText(name);
    m_gameVersionEdit->setText(gameVersion);
}

void NewServerPage::setTargetDir(const QString &dir) {
    if (m_targetDirLabel->text() == dir) {
        return;
    }
    m_targetDirLabel->setText(dir);
    m_targetDirLabel->setVisible(!dir.isEmpty());
    m_targetDirLabel->setToolTip(dir.isEmpty()
                                     ? tr("服务端名称将用作目录名称。")
                                     : dir);
}

void NewServerPage::setVersionChoices(bool installer, const QStringList &versions, const QString &status) {
    auto *combo = installer ? m_installerCombo : m_loaderCombo;
    auto *edit = installer ? m_installerEdit : m_loaderEdit;
    const QString previous = edit->text();
    LoadingState::finish(combo);
    {
        const QSignalBlocker block(combo), lineBlock(edit);
        combo->clear();
        combo->addItems(versions);
        combo->setCurrentIndex(versions.isEmpty() ? -1 : qMax(0, versions.indexOf(previous)));
        const QString placeholder = status.isEmpty() ? tr("暂无可用版本") : status;
        combo->setPlaceholderText(placeholder);
        edit->setPlaceholderText(placeholder);
        if (versions.isEmpty()) edit->clear();
        combo->setEnabled(!versions.isEmpty());
        combo->setToolTip(status);
    }
    emit fieldsChanged();
}

void NewServerPage::setVersionsLoading(bool installer) {
    auto *combo = installer ? m_installerCombo : m_loaderCombo;
    auto *edit = installer ? m_installerEdit : m_loaderEdit;
    {
        const QSignalBlocker block(combo), lineBlock(edit);
        combo->clear();
        combo->setPlaceholderText(QString());
        edit->setPlaceholderText(QString());
        edit->clear();
        combo->setEnabled(false);
        combo->setToolTip(QString());
    }
    LoadingState::begin(combo, tr("正在获取版本…"));
    emit fieldsChanged();
}

void NewServerPage::setSummary(const QString &text, const QString &problem) {
    // 有问题就先说问题（那是用户现在要解决的事），没有才说计划。两句都是翻译点给的。
    const QString shown = problem.isEmpty() ? text : problem;
    m_summaryLabel->setVisible(!shown.trimmed().isEmpty());
    if (m_summaryLabel->text() != shown)
        m_summaryLabel->setText(shown);
    // 有问题时「创建」点不动（填法不成立就点下去，只会建一个半残的服务端）。
    m_createButton->setEnabled(problem.isEmpty());
}

void NewServerPage::applyFlavorVisibility() {
    const int row = m_flavorList != nullptr ? m_flavorList->currentRow() : -1;
    const bool valid = row >= 0 && row < m_flavors.size();
    const FlavorOption option = valid ? m_flavors.at(row) : FlavorOption{};
    m_loaderRow->setVisible(option.needsLoader);
    m_installerRow->setVisible(option.needsInstaller);
    m_buildRow->setVisible(option.needsBuild);
    // 换到不要这一格的档：那一发的答复会被丢掉，加载态也一起撤（别让共享时钟为藏着的框空转）。
    if (!option.needsLoader) LoadingState::finish(m_loaderCombo);
    if (!option.needsInstaller) LoadingState::finish(m_installerCombo);
}

QString NewServerPage::serverName() const { return m_nameEdit->text().trimmed(); }

QString NewServerPage::flavorToken() const {
    const QListWidgetItem *item = m_flavorList->currentItem();
    return item != nullptr ? item->data(Qt::UserRole).toString() : QString();
}

QString NewServerPage::gameVersion() const { return m_gameVersionEdit->text().trimmed(); }

QString NewServerPage::loaderVersion() const {
    // 收着的那一行不算数（换档之后旧的字还留在框里，读回去就成了"这一档也有加载器版本"）。
    return !m_loaderRow->isHidden() ? m_loaderEdit->text().trimmed() : QString();
}

QString NewServerPage::installerVersion() const {
    return !m_installerRow->isHidden() ? m_installerEdit->text().trimmed() : QString();
}

QString NewServerPage::buildId() const {
    return !m_buildRow->isHidden() ? m_buildEdit->text().trimmed() : QString();
}

int NewServerPage::memoryMb() const { return m_memorySpin->value(); }
