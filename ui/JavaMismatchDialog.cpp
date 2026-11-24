/**
 * @file JavaMismatchDialog.cpp
 * @brief 实现实例 Java 与版本要求不匹配时的三岔口对话框。
 * @note 正文必须同时显示实例名和两个 Java 版本。
 */

#include "JavaMismatchDialog.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

// 正文那个宽度常量（`promptContentWidth()`）是那一层给的 —— 这个框现在开在它里面。
#include "OverlayHost.hpp"
#include "InlineMessage.hpp"  // 和别的提示卡同一个头（图标块 + 手写体标题）
#include "Theme.hpp"   // 三颗按钮的身份

namespace {

/// 实际那个 Java 的可读写法。版本串可能是空的（release 快路径不回填 versionOutput），
/// 所以这里必须兜住"只有主版本号"这一种，否则正文里会出现「Java 」。
QString describeActual(int actualMajor, const QString &actualVersionText) {
    const QString text = actualVersionText.trimmed();
    if (text.isEmpty()) {
        return QStringLiteral("Java %1").arg(actualMajor);
    }
    return JavaMismatchDialog::tr("Java %1（%2）").arg(actualMajor).arg(text);
}

}  // namespace

QString JavaMismatchDialog::messageText(const QString &instanceName, int expectedMajor,
                                        int actualMajor, const QString &actualVersionText,
                                        const QString &javaPath) {
    return tr("实例「%1」配置的 Java 为 %2，此版本需要 Java %3。\n\n版本不兼容可能导致启动失败。建议切换到支持的 Java 版本。\n\n%4")
        .arg(instanceName, describeActual(actualMajor, actualVersionText),
             QString::number(expectedMajor), javaPath);
}

JavaMismatchDialog::JavaMismatchDialog(const QString &instanceName, int expectedMajor,
                                       int actualMajor, const QString &actualVersionText,
                                       const QString &javaPath, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Java 版本不兼容"));

    auto *layout = new QVBoxLayout(this);
    // 留白由卡片自己给（`promptPadding()`），这里再给一层就成了双层留白。
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // 设计稿 dialogs.html：Java 图标块（警示浅黄底）+ 手写体标题，和别的提示卡同一个头。
    layout->addLayout(InlineMessageBox::makeHeader(this, new QLabel(windowTitle(), this),
                                                   MessageKind::Warning, QStringLiteral("java")));

    m_textLabel = new QLabel(
        messageText(instanceName, expectedMajor, actualMajor, actualVersionText, javaPath), this);
    m_textLabel->setObjectName(textLabelName());
    m_textLabel->setWordWrap(true);
    // 那条路径会很长（`C:/.../.minecraft/runtime/.../bin/java.exe`），允许选中复制：
    // 用户要按正文里那句话去改配置时，手抄一条路径是不现实的。
    m_textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_textLabel->setFixedWidth(OverlayHost::promptContentWidth());
    QFont bodyFont = m_textLabel->font();
    bodyFont.setPixelSize(14);
    m_textLabel->setFont(bodyFont);
    QPalette bodyPalette = m_textLabel->palette();
    bodyPalette.setColor(QPalette::WindowText, bodyPalette.color(QPalette::PlaceholderText));
    m_textLabel->setPalette(bodyPalette);
    layout->addWidget(m_textLabel);

    m_dontRemindCheck = new QCheckBox(tr("以后不再提醒这个实例"), this);
    m_dontRemindCheck->setObjectName(dontRemindCheckName());
    m_dontRemindCheck->setToolTip(
        tr("此设置仅对当前实例生效，可在「高级选项 → 版本提醒」中修改。"));
    layout->addWidget(m_dontRemindCheck);
    // 勾选框下那行说明直接摆出来（设计稿），缩进对齐勾选框的字。
    auto *remindHelp = new QLabel(m_dontRemindCheck->toolTip(), this);
    remindHelp->setWordWrap(true);
    remindHelp->setFont(Theme::textFont(remindHelp->font(), Theme::TextRole::Caption));
    remindHelp->setForegroundRole(QPalette::PlaceholderText);
    remindHelp->setContentsMargins(26, 0, 0, 0);
    remindHelp->setFixedWidth(OverlayHost::promptContentWidth());
    layout->addWidget(remindHelp);

    layout->addStretch(1);

    auto *buttons = new QHBoxLayout;
    m_continueButton = new QPushButton(tr("仍然用这个 Java 启动"), this);
    m_continueButton->setObjectName(continueButtonName());
    // 默认按钮 = "什么都不改，照我钉的那个启动"。理由见 .hpp：一次误按回车不该改配置。
    m_continueButton->setDefault(true);
    m_useMatchingButton = new QPushButton(tr("改用匹配的 Java 启动"), this);
    m_useMatchingButton->setObjectName(useMatchingButtonName());
    m_cancelButton = new QPushButton(tr("取消"), this);
    m_cancelButton->setObjectName(cancelButtonName());
    // 设计稿：取消（文字）在最左；「仍然用这个」中性；「改用匹配的」是推荐那条路，实心摆最右。
    // 回车照旧落在「仍然用这个」上（上面 setDefault 那句的理由不变）。
    Theme::markButton(m_cancelButton, Theme::ButtonKind::Ghost);
    Theme::markButton(m_continueButton, Theme::ButtonKind::Neutral);
    Theme::markButton(m_useMatchingButton, Theme::ButtonKind::Primary);
    m_cancelButton->setToolTip(tr("暂不启动，可先安装该 Java 或修改实例设置。"));
    buttons->setSpacing(10);
    buttons->addWidget(m_cancelButton);
    buttons->addStretch(1);
    buttons->addWidget(m_continueButton);
    buttons->addWidget(m_useMatchingButton);
    setProperty("wtfmlDialogActions", true);  // 三颗一样高（Theme 里那条规则）
    layout->addLayout(buttons);

    connect(m_continueButton, &QPushButton::clicked, this, [this] {
        m_choice = Choice::UseInstanceJava;
        accept();
    });
    connect(m_useMatchingButton, &QPushButton::clicked, this, [this] {
        m_choice = Choice::UseMatchingJava;
        accept();
    });
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    // 关窗 / Esc 走 reject()，**不动 m_choice** —— 它初值就是 Abort，那正是想要的。
}

bool JavaMismatchDialog::dontRemindAgain() const {
    // 构造失败（理论上到不了）时按"不勾"算：宁可下次再问一句，也不要静默地
    // 替用户把一个"以后别问了"的表态记下来。
    return m_dontRemindCheck != nullptr && m_dontRemindCheck->isChecked();
}
