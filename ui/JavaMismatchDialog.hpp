/**
 * @file JavaMismatchDialog.hpp
 * @brief 「实例指定的 Java 与版本要求对不上」时的三岔口对话框。
 * @note 进参出参都是**值**，本文件不 include 任何 src/ 下的头 —— 和 GroupDialog /
 * InstanceSettingsDialog 同一条约定（ui/ 不依赖业务层）。所以这里没有任何
 * "去挑一个匹配的 Java"的动作：它只负责把用户的选择**问出来**，改配置和挑路径
 * 都在 MainWindow 里（那两步各要碰不同的作用域：实例 meta 与全局设置）。
 *
 * 【为什么不是 QMessageBox】
 *
 * 要问的东西不是一个"是/否"：除了两个动作，还有一个"以后别再问了"的勾选框，而且
 * 勾选框**只对其中一条路有意义**。用 QMessageBox 就得自己 addButton + setCheckBox，
 * 拼出来的东西在离屏的验证工程里驱动不了（模态 exec() 在 offscreen 下会一直等），
 * 于是"按钮点下去到底落到哪个分支"这段判断就永远没有断言盯着。独立一个 QDialog 之后
 * 可以**直接构造**它、直接点它的按钮。
 *
 * 【三个选择的收场】
 *
 * 仍然用这个 Java 启动 —— 用实例钉的那条路径启动，一个字节的配置都不改；
 * 改用匹配的 Java 启动 —— 把该实例的 javaPath 覆盖换成一个匹配的路径，再启动；
 * 取消 / 关窗 / Esc —— 不启动。**不逼用户二选一**（他可能只是想先去把那个 Java
 * 装好再回来）。
 */
#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QPushButton;

class JavaMismatchDialog : public QDialog {
    Q_OBJECT

public:
    /// 用户的三岔口。默认 Abort —— 关窗、按 Esc、点取消都落到这里。
    enum class Choice {
        Abort,
        UseInstanceJava,
        UseMatchingJava,
    };

    /**
     * @brief 构造对话框。
     * @param instanceName 显示在正文里的实例名（用户认得出说的是哪一个）。
     * @param expectedMajor 这个 Minecraft 版本**要求**的 Java 主版本。
     * @param actualMajor 实例指定的那个 Java **实际**的主版本（一定 > 0 —— 探不出
     * 主版本的路径根本走不到这个框，那条路仍然是现有的硬错误）。
     * @param actualVersionText 实际那个 Java 的完整版本串（如 "1.8.0_301"）。**可能是空的**：
     * GlobalJavaSettings::probeJavaMajorVersion() 读 release 文件那条快路径
     * **不回填** versionOutput，所以正文得兜住"只有主版本号"这一种，
     * 别拼出「Java 」这种句子。
     * @param javaPath 实例指定的那条路径。只在正文里露出来，让用户知道改的是哪一个文件。
     * @param parent 父窗口。
     */
    JavaMismatchDialog(const QString &instanceName, int expectedMajor, int actualMajor,
                       const QString &actualVersionText, const QString &javaPath,
                       QWidget *parent = nullptr);

    /// 用户点的哪个按钮。没点就关了窗口 = Abort。
    Choice choice() const { return m_choice; }

    /// 「以后不再提醒这个实例」。默认不勾 —— 一个回车不该顺手改掉一个长期偏好。
    ///
    /// NOTE: 它是一句**独立于选了哪条路**的表态（勾选框的字面意思就是这样），所以
    /// 两条动作路都会把它交给调用方，落盘与否由调用方决定。
    bool dontRemindAgain() const;

    /// 正文那段话。抽成静态函数是为了让**文案本身**能被验证工程按字面核对
    /// （"实例名和两个版本号都在里面"这种事，写成断言比肉眼看图可靠）。
    static QString messageText(const QString &instanceName, int expectedMajor, int actualMajor,
                               const QString &actualVersionText, const QString &javaPath);

    // ---------- 控件的 objectName ----------
    // 验证工程靠这些名字认控件，别改回按文字去认。
    static QString textLabelName() { return QStringLiteral("javaMismatchTextLabel"); }
    static QString continueButtonName() { return QStringLiteral("javaMismatchContinueButton"); }
    static QString useMatchingButtonName() {
        return QStringLiteral("javaMismatchUseMatchingButton");
    }
    static QString cancelButtonName() { return QStringLiteral("javaMismatchCancelButton"); }
    static QString dontRemindCheckName() {
        return QStringLiteral("javaMismatchDontRemindCheck");
    }

private:
    Choice m_choice = Choice::Abort;

    QLabel *m_textLabel = nullptr;
    QCheckBox *m_dontRemindCheck = nullptr;
    /// 「仍然用这个 Java 启动」。**默认按钮是它**，不是"改用匹配的"：一次误按回车
    /// 只该继续启动，不该顺手把用户的配置改掉 —— 改写配置那条路必须是刻意点的。
    QPushButton *m_continueButton = nullptr;
    QPushButton *m_useMatchingButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
};
