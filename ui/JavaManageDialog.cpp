/**
 * @file JavaManageDialog.cpp
 * @brief 实现「Java 管理」页：主版本列表与自动选择。
 * @note 版本列表由 managedMajors() 提供，下载/选用动作只发信号。
 */

#include "JavaManageDialog.hpp"
#include <QCoreApplication>
#include "Theme.hpp"   // 「重新检测」那一颗的身份（一般性）

#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

/// 独立开出来时的大小。**内嵌到覆盖层里时它不作数**（那一层给的是整个内容区的宽度，
/// 见 OverlayHost::showPage）—— 和 InstanceSettingsDialog 同一个道理：这一句只为
/// 了验证工程能把这个框单独摆出来看。
constexpr int kDialogWidth = 780;
constexpr int kDialogHeight = 440;

/// 那一行说明的字号/配色。和 MainWindowUI 的 addDynamicHint() **同一个形状**
/// （小一号 + PlaceholderText 那档墨色），但那一个是那个文件的匿名 namespace 里的，
/// 跨不过来 —— 与其为了三个属性去动它的可见性，不如在这儿照抄一遍。
void applyHintStyle(QLabel *hint) {
    hint->setWordWrap(true);
    QFont hintFont = hint->font();
    hintFont = Theme::textFont(hintFont, Theme::TextRole::Caption);
    hint->setFont(hintFont);
    QPalette hintPalette = hint->palette();
    hintPalette.setColor(QPalette::WindowText, hintPalette.color(QPalette::PlaceholderText));
    hint->setPalette(hintPalette);
}

}  // namespace

QList<int> JavaManageDialog::managedMajors() {
    return {25, 21, 17, 11, 8};
}

QString JavaManageDialog::rowLabelText(int major) {
    return QCoreApplication::translate("JavaManageDialog", "Java %1：").arg(major);
}

QString JavaManageDialog::pathText(const QString &javaPath) {
    const QString trimmed = javaPath.trimmed();
    return trimmed.isEmpty() ? QCoreApplication::translate("JavaManageDialog", "缺失") : trimmed;
}

QString JavaManageDialog::rowText(int major, const QString &javaPath) {
    return rowLabelText(major) + pathText(javaPath);
}

QString JavaManageDialog::rowButtonText(bool installed) {
    return installed ? QCoreApplication::translate("JavaManageDialog", "重新下载") : QCoreApplication::translate("JavaManageDialog", "下载");
}

QString JavaManageDialog::downloadAllText() {
    return QCoreApplication::translate("JavaManageDialog", "下载所有缺少的 Java");
}

QString JavaManageDialog::detectText() {
    return QCoreApplication::translate("JavaManageDialog", "检测已有 Java");
}

QString JavaManageDialog::manualText() {
    return QCoreApplication::translate("JavaManageDialog", "手动指定 java.exe…");
}

QString JavaManageDialog::autoRowText() {
    return QCoreApplication::translate("JavaManageDialog", "自动（按版本挑选）");
}

QString JavaManageDialog::useButtonText() {
    return QCoreApplication::translate("JavaManageDialog", "选用");
}

QString JavaManageDialog::usedMarkText() {
    return QCoreApplication::translate("JavaManageDialog", "（当前）");
}

QString JavaManageDialog::hintText() {
    return QCoreApplication::translate("JavaManageDialog", "下载 Adoptium 运行时至 java/<版本> 目录，不影响系统环境。");
}

QString JavaManageDialog::titleText() const {
    return QCoreApplication::translate("JavaManageDialog", "Java 管理");
}

JavaManageDialog::JavaManageDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle(titleText());
    // 「确定」都没有：这一页不是"填一张表然后提交"，每一颗按钮点下去就各自去做事，
    // 左上角那颗返回箭头只是关掉这一页（OverlayHost::showPage 接的就是它）。
    resize(kDialogWidth, kDialogHeight);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(12, 12, 12, 12);
    outer->setSpacing(10);

    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    // 行距比列距大一点：路径折行之后两行会挨得很近，五行会糊成一片。
    grid->setVerticalSpacing(8);
    // 文字那一列吃掉所有多余的宽度，右边两列保持自己的大小 —— 反过来的话
    // 长路径会把两颗按钮挤成一条，而"路径太长"正是这一页的常态。
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 0);
    grid->setColumnStretch(2, 0);

    // ---------- 顶上那一行：自动（按版本挑选） ----------
    // 它没有"下载"（自动那一档不是一份运行时，没什么可下的），所以只有「选用」。
    // NOTE: 它占的是同一个网格，下面各行的两颗按钮和它在一条竖线上。
    // "每一行的『选用』都在同一列"是这一页看起来整齐的全部原因。
    m_autoLabel = new QLabel(this);
    m_autoLabel->setObjectName(autoLabelName());
    m_autoLabel->setWordWrap(true);
    grid->addWidget(m_autoLabel, 0, 0);

    m_autoUse = new QPushButton(useButtonText(), this);
    m_autoUse->setObjectName(autoUseButtonName());
    m_autoUse->setAutoDefault(false);
    m_autoUse->setToolTip(QCoreApplication::translate("JavaManageDialog", "不钉死任何一份，启动时按版本要求自己挑。"));
    grid->addWidget(m_autoUse, 0, 1, Qt::AlignTop);
    connect(m_autoUse, &QPushButton::clicked, this, &JavaManageDialog::useAutoRequested);

    int row = 1;
    for (int major : managedMajors()) {
        Row entry;
        entry.label = new QLabel(this);
        entry.label->setObjectName(rowLabelName(major));
        entry.label->setWordWrap(true);
        // 路径要能选中复制：这一页的用途之一就是"照这条路径去看看那个 Java 怎么了"。
        entry.label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(entry.label, row, 0);

        entry.use = new QPushButton(useButtonText(), this);
        entry.use->setObjectName(useButtonName(major));
        entry.use->setAutoDefault(false);
        entry.use->setToolTip(
            QCoreApplication::translate("JavaManageDialog", "把「全局」这一层钉死成 Java %1 这一份。").arg(major));
        grid->addWidget(entry.use, row, 1, Qt::AlignTop);
        // 按**值**接主版本号（不是按下标去翻 m_rows）：行序将来再怎么挪，
        // 这一颗点下去说的还是它自己那一行。
        connect(entry.use, &QPushButton::clicked, this,
                [this, major] { emit useRuntimeRequested(major); });

        entry.button = new QPushButton(this);
        entry.button->setObjectName(rowButtonName(major));
        entry.button->setAutoDefault(false);
        grid->addWidget(entry.button, row, 2, Qt::AlignTop);
        connect(entry.button, &QPushButton::clicked, this,
                [this, major] { emit downloadRequested(major); });

        m_rows.insert(major, entry);
        ++row;
    }
    outer->addLayout(grid);

    auto *hint = new QLabel(hintText(), this);
    hint->setObjectName(hintLabelName());
    applyHintStyle(hint);
    outer->addWidget(hint);

    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    m_downloadAll = new QPushButton(downloadAllText(), this);
    m_downloadAll->setObjectName(downloadAllButtonName());
    m_downloadAll->setAutoDefault(false);
    m_downloadAll->setToolTip(
        QCoreApplication::translate("JavaManageDialog", "把上面写着「缺失」的那些一次下完。已经有的那些不动。"));
    m_detect = new QPushButton(detectText(), this);
    m_detect->setObjectName(detectButtonName());
    // 「重新检测」= 重扫一遍（一般性）；上面那些「选用」「下载」都是确认性。
    Theme::markButton(m_detect, Theme::ButtonKind::Neutral);
    m_detect->setAutoDefault(false);
    m_detect->setToolTip(QCoreApplication::translate("JavaManageDialog", "重新扫描本机已安装的 Java。"));
    // 「手动指定」和「检测」一样是一般性动作：它不改上面那几行，只是换一条外面不认识的
    // 路径。装在别处的那一份（另一个盘上的 JDK、GraalVM）只有这一条路进来。
    m_manual = new QPushButton(manualText(), this);
    m_manual->setObjectName(manualButtonName());
    Theme::markButton(m_manual, Theme::ButtonKind::Neutral);
    m_manual->setAutoDefault(false);
    m_manual->setToolTip(QCoreApplication::translate("JavaManageDialog", "手动选择本地 java.exe 文件。"));
    actions->addWidget(m_downloadAll);
    actions->addWidget(m_detect);
    actions->addWidget(m_manual);
    actions->addStretch(1);
    outer->addLayout(actions);

    // 收尾的弹簧：多出来的高度全归它（见上面那段 ★）。
    outer->addStretch(1);

    connect(m_downloadAll, &QPushButton::clicked, this,
            &JavaManageDialog::downloadAllMissingRequested);
    connect(m_detect, &QPushButton::clicked, this, &JavaManageDialog::detectRequested);
    connect(m_manual, &QPushButton::clicked, this, &JavaManageDialog::manualPathRequested);

    // 各行先按“没装”显示，控制器打开页面后会灌入扫描结果。
    //
    // 这一轮**必须排在所有控件都建出来之后**：`setRuntime()` 会顺着
    // `refreshDownloadAllState()` 去够底下那一排按钮，而那一排是**行循环之后**才摆上来的
    // —— 循环里调它，够到的是个空指针。
    for (int major : managedMajors()) {
        setRuntime(major, QString());
    }

    refreshDownloadAllState();
    refreshUsedMarks();
}

void JavaManageDialog::setRuntime(int major, const QString &javaPath) {
    auto it = m_rows.find(major);
    if (it == m_rows.end()) {
        // 不在固定管理列表中，忽略而不是临时加行。
        return;
    }
    Row &entry = it.value();
    entry.path = javaPath.trimmed();
    entry.installed = !entry.path.isEmpty();
    entry.button->setText(rowButtonText(entry.installed));
    // 说明里那句"点下去会发生什么"得跟着状态走：已经装好了的那行，用户点之前
    // 有权利知道这一下是要**覆盖**掉现有的那份。
    entry.button->setToolTip(
        entry.installed
            ? QCoreApplication::translate("JavaManageDialog", "重新下载 Java %1 并替换已安装的版本。").arg(major)
            : QCoreApplication::translate("JavaManageDialog", "下载并安装 Java %1。").arg(major));
    // 「选用」在没装的那几行上是**藏起来**的，不是画灰 —— 灰按钮的位置上写着一个
    // "现在不能按"，而这一行压根没有可选的路径，那不是"暂时不行"，是"还没有"。
    // （这也是这一层别处的取向：挪不动的那一侧不画。）
    entry.use->setVisible(entry.installed);
    refreshUsedMarks();
    refreshDownloadAllState();
}

void JavaManageDialog::setSelection(bool autoSelect, const QString &javaPath) {
    m_autoSelect = autoSelect;
    m_selectedPath = javaPath.trimmed();
    refreshUsedMarks();
}

bool JavaManageDialog::isInstalled(int major) const {
    const auto it = m_rows.constFind(major);
    return it != m_rows.constEnd() && it.value().installed;
}

bool JavaManageDialog::hasMissing() const {
    for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
        if (!it.value().installed) {
            return true;
        }
    }
    return false;
}

void JavaManageDialog::setBusy(bool busy) {
    m_busy = busy;
    for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
        it.value().button->setEnabled(!busy);
        it.value().use->setEnabled(!busy);
    }
    m_detect->setEnabled(!busy);
    if (m_manual != nullptr) m_manual->setEnabled(!busy);
    refreshUsedMarks();
    refreshDownloadAllState();
}

void JavaManageDialog::refreshDownloadAllState() {
    // 忙的时候一律禁掉（理由见 .hpp）；闲的时候只在**真有缺的**才亮 ——
    // 一个都不缺时那颗按钮点下去什么都不会发生，留着它可点就是让用户白点一下。
    m_downloadAll->setEnabled(!m_busy && hasMissing());
}

void JavaManageDialog::refreshUsedMarks() {
    // 手动档按**路径**认行（见 .hpp：钉的可能是这一页压根不管的一份 Java，那时
    // 一条都不带记号，这是实话）。
    const bool autoUsed = m_autoSelect;
    m_autoLabel->setText(autoRowText() + (autoUsed ? usedMarkText() : QString()));
    // 正在用的那一条，它那颗「选用」是灰的：再点一次什么都不会变。**画灰而不是藏**
    // —— 这一条确实存在、也确实选得动，只是现在已经是它了（和"没装"那种不一样）。
    m_autoUse->setEnabled(!m_busy && !autoUsed);

    for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
        Row &entry = it.value();
        const bool used = !m_autoSelect && entry.installed && entry.path == m_selectedPath;
        entry.label->setText(rowText(it.key(), entry.path) + (used ? usedMarkText() : QString()));
        entry.use->setEnabled(!m_busy && entry.installed && !used);
    }
}
