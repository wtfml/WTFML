/**
 * @file MainWindowLaunchJava.cpp
 * @brief 这次启动用哪个 Java、给多少内存：不匹配时问用户，自动档现算一份。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是"这一次启动的 Java 与内存"：`askAboutJavaMismatch()` 在实例要求的版本
 * 与手头那个 Java 对不上时问一句（用哪颗、还是取消），`resolveMemoryForLaunch()` 决定
 * xms/xmx 从哪儿来，`instanceModCount()` 只给自动档数模组用（见 .hpp 那条 NOTE：只在
 * 那一支数）。从主窗里分出来，"自动档到底写了什么"只在一个文件里翻。
 *
 * 【两条纪律（原来就在注释里，原样跟着搬）】
 *
 * - 实例要"自动"时**现算一份给这次启动用**，全局那份一个字节都不动 —— 实例 A 开着自动，
 *   不能让全局内存配置跟着变（跨作用域污染）。
 * - 算出来的数**不写回 meta**：写回去下次读出来就成了"自定义 + 一个用户没填过的数"。
 *
 * 【判据不在这一层】
 *
 * 建议值在 `GlobalJavaSettings::suggestXmxMb()` / `suggestXmsMb()`，机器内存读数在
 * `SystemMemory::read()`，实例 meta 在 `VersionManager`，对话框在 `ui/JavaMismatchDialog`。
 */

#include "MainWindow.hpp"

#include "..\ui\JavaMismatchDialog.hpp"
#include "..\ui\OverlayHost.hpp"
#include "manage/VersionManager.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "util/SystemMemory.hpp"

#include <QDir>

MainWindow::JavaMismatchAnswer MainWindow::askAboutJavaMismatch(
    const QString &versionId, const JavaSettingsOverride &override, int expectedMajor) {
    // ① 只有"实例钉了 Java"才有这个问题。全局档不匹配时 ensureJavaForVersion() 会
    // 静默换一个匹配的 —— 那是有意的行为（用户没钉任何东西，替他挑一个正确的
    // 运行时不叫"换掉他的选择"），不动它。
    if (!override.javaPath.has_value()) {
        return JavaMismatchAnswer::NotAsked;
    }

    // ② 全局总闸关着，③ 或者这个实例表过态"以后别再问了"。
    //
    // 两条的语义**一样**，都是"别问了，照他钉的那个启动" —— 返回 KeepInstanceJava
    // 正是那个意思（它唯一的作用就是把 allowMismatchedPath 打开）。这里不能返回
    // NotAsked：那条路会落到手动档的硬错误上（路径不匹配就 fail()），也就是说，
    // 用户关了提醒之后反而**再也启动不了**这个实例 —— 一个纯负作用的开关。
    if (!m_javaSettings->warnVersionMismatch() ||
        m_versionManager->skipJavaVersionWarning(versionId)) {
        return JavaMismatchAnswer::KeepInstanceJava;
    }

    // ④ 那条路径到底是不是另一个主版本。探不出主版本（-1）时**不问** —— 那是"路径
    // 不存在/认不出来"，和"版本不符"是两件事，它继续落到现有的「Java 运行时缺失」
    // 硬错误那条路上。主版本相同当然更不用问。
    QString actualVersionText;
    const int actualMajor =
        GlobalJavaSettings::probeJavaMajorVersion(*override.javaPath, &actualVersionText);
    if (actualMajor <= 0 || actualMajor == expectedMajor) {
        return JavaMismatchAnswer::NotAsked;
    }

    // 内容区中间那一块（也就是「像 java 设置这种小弹窗」），不是独立窗口。
    // 关掉 / 按 Esc / 点外面都**不动 `choice`**，它的初值就是 Abort
    // （见那个对话框的 .cpp 末尾）。
    JavaMismatchDialog dialog(instanceDisplayName(versionId), expectedMajor, actualMajor,
                              actualVersionText, *override.javaPath);
    m_ui->overlay()->execPrompt(&dialog, false);
    const JavaMismatchDialog::Choice choice = dialog.choice();

    if (choice == JavaMismatchDialog::Choice::Abort) {
        appendLog(tr("已取消启动：「%1」指定的是 Java %2，而这个版本要求 Java %3。")
                      .arg(instanceDisplayName(versionId))
                      .arg(actualMajor)
                      .arg(expectedMajor));
        return JavaMismatchAnswer::AbortLaunch;
    }

    // 「以后不再提醒这个实例」是用户刚点下的一下**表态**，立刻落盘才对得上他的意思
    // （等他下次想起来去「实例设置…」里关，那个框会一直问下去）。
    //
    // NOTE: 写失败**只记日志、不拦启动**。这是全程唯一一处"错误不弹窗"：存不下一个
    // 偏好和"这次启动能不能跑"是两件事，不该因为它把用户刚授权的启动掐掉；
    // 而它自己的后果（下次还问一句）是无害的、也是看得见的。
    //
    // NOTE: 勾选框是**独立于选了哪条路**的一句表态（它的字面意思就是这样），所以
    // 「改用匹配的 Java」那一次也照写不误 —— 要是以后给这个实例钉了个
    // 对不上的 Java，"别问了"这句话仍然算数。
    if (dialog.dontRemindAgain()) {
        QString error;
        if (!m_versionManager->updateVersionMeta(
                versionId, {{VersionManager::skipJavaVersionWarningField(), QJsonValue(true)}},
                &error)) {
            appendLog(tr("无法保存「不再提醒」设置：%1").arg(error));
        } else {
            appendLog(tr("「%1」以后不再提醒 Java 版本不符。").arg(instanceDisplayName(versionId)));
        }
    }

    return choice == JavaMismatchDialog::Choice::UseMatchingJava
               ? JavaMismatchAnswer::UseMatchingJava
               : JavaMismatchAnswer::KeepInstanceJava;
}

int MainWindow::instanceModCount(const QString &versionId) const {
    const QString modsDir = instanceFileDirs(versionId).value(QStringLiteral("mod"));
    if (modsDir.isEmpty()) {
        return 0;
    }
    return QDir(modsDir).entryList({QStringLiteral("*.jar")}, QDir::Files).size();
}

void MainWindow::resolveMemoryForLaunch(const QString &versionId, EffectiveJavaSettings &io,
                                       const JavaSettingsOverride &override) {
    if (!io.autoMemory) {
        // 手动档：io 里那两个数字是人定的值（可能来自全局、也可能来自这个实例
        // 的覆盖），就是这次要用的值，一个都不许动。
        return;
    }

    // NOTE: 自动档下 xmx/xms 的覆盖**在这里被丢掉**，这是有意的：自动档的定义就是
    // "按系统内存现算"，用户单独钉一个 Xmx 而让 Xms 自动算，只会得到一对
    // 谁也说不清的数字。对话框那边是同一件事的另一半 —— 自动档下那两行整行
    // 禁用，点确定时也不把它们写进 meta。
    if (!override.autoMemory.has_value()) {
        // 自动这一档来自**全局**：refreshSuggestedMemory() 会按当前系统内存
        // 重算并更新全局那份值（落盘由设置页 / 下次 save() 负责）。
        m_javaSettings->refreshSuggestedMemory();
        io.xmx = m_javaSettings->xmx();
        io.xms = m_javaSettings->xms();
        return;
    }

    // 自动这一档是**这个实例**自己要的（全局可能是手动档）。现算一份给这次启动用，
    // 全局一个字节都不动 —— 实例 A 开着自动，不能让全局那份内存配置跟着变，
    // 那是跨作用域污染的同一种错（见 ensureJavaForVersion 的值版那段注释）。
    //
    // NOTE: 算出来的数**不写回 meta**。写回去的话，那个实例的 meta 里就多出一对
    // xmx/xms（它们本来没有），而这对值是死的 —— 自动档每次启动都重算，
    // 下次读出来只会让「实例设置…」里那两行显示成"自定义 + 一个用户没填过的数"。
    // 跨作用域污染的反面不是"多写一份"，而是"只写在这次要用的值上"。
    const SystemMemory::Usage usage = SystemMemory::read();
    // 数一数这个实例装了多少模组（见 .hpp 那条 NOTE：只在这一支数）。
    // 同一个 200 模组的整合包会比原版多要一截内存 —— 自动档要认整合包规模。
    io.xmx = GlobalJavaSettings::suggestXmxMb(usage.totalMb, usage.availableMb,
                                              instanceModCount(versionId));
    io.xms = GlobalJavaSettings::suggestXmsMb(io.xmx);
}
