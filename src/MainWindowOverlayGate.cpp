/**
 * @file MainWindowOverlayGate.cpp
 * @brief 覆盖层那一层「现在能不能开新页」：只回答、让位、排到下一轮、以及实在开不了时弹的那一句。
 *
 * 【为什么单独一个翻译单元】
 *
 * 这一片的主语是覆盖层的**占用**：`overlayAcceptsNewPage()` 只回答"空着没有"，一个字节都
 * 不往外弹（"先关闭当前页面再继续"那一类一律静默收掉再继续）；`runWhenOverlayFree()` 是
 * 让位的唯一出处（收掉占着的那一层，把"开"排到下一轮）；`openPageWhenIdle()` 排到下一轮；
 * `warnOverlayBusy()` 是"四颗导航一次只放得下一个页面"那句话。全窗所有开页的调用点都从
 * 这里过 —— 分出来之后，"页到底什么时候才开"只在一个文件里翻。
 *
 * 【纪律】
 *
 * 静默收掉、再排一轮：不要让用户在"按钮点了没反应"和"页被硬关掉"之间二选一。
 *
 * 【判据不在这一层】
 *
 * 页的打开与收尾在 `ui/OverlayHost`（`showPage()` 不等旧页收完就返回，所以才有"排一轮"）。
 */

#include "MainWindow.hpp"

#include "..\ui\InlineMessage.hpp"
#include "..\ui\MainWindowUI.hpp"
#include "..\ui\OverlayHost.hpp"

#include <QTimer>

bool MainWindow::overlayAcceptsNewPage() const {
    // **只回答"空着没有"，一个字节都不往外弹**（要求见 .hpp 那段：
    // 「先关闭当前页面再继续」那一类情况现在一律**静默收掉**再继续）。
    OverlayHost *overlay = m_ui->overlay();
    return overlay == nullptr || !overlay->isOpen();
}

void MainWindow::runWhenOverlayFree(const std::function<void()> &open, int roundsLeft) {
    if (!open) {
        return;
    }
    OverlayHost *overlay = m_ui->overlay();
    if (overlay == nullptr || !overlay->isOpen()) {
        // 绝大多数时候走的是这一支：这一层本来就空着，**同一轮就把页开出来**。
        open();
        return;
    }
    // ---------- 这一层被占着：先让位，再开 ----------
    //
    // 占着它的可能是**提示**，也可能是**整页**；整页那一档还可能是"收着的"（用户在别的
    // 选项卡上逛，而它一直占着这一位）。这里的规矩是**静默收掉，直接继续**。
    if (roundsLeft <= 0 || !overlay->evict()) {
        // 收不掉（那一层不认这一发 / 轮数用完了）。**到这一步才出声**：挡住用户操作的
        // 事必须说清楚，而且得说清怎么办。
        warnOverlayBusy();
        return;
    }
    // 收页那一发对"非阻塞开的那一页"是同步落定的（reject → finished → closeOverlay），
    // 可就当它是**请求**来排：这一趟调用栈随后还要退出，而下一轮里可能又有别的东西
    // 把这一层占上了。∴ 排到下一轮，而且**下一轮再从头上过一遍**。
    QTimer::singleShot(0, this, [this, open, roundsLeft] {
        runWhenOverlayFree(open, roundsLeft - 1);
    });
}

void MainWindow::openPageWhenIdle(const std::function<void()> &open) {
    if (!open) {
        return;
    }
    OverlayHost *overlay = m_ui->overlay();
    if (overlay == nullptr) {
        open();
        return;
    }
    // 空着 / 被占着两条路都在 OverlayHost 里（排队 + 收干净之后下一轮再摆）。
    overlay->queuePageOpen(open);
}

void MainWindow::warnOverlayBusy() {
    OverlayHost *overlay = m_ui->overlay();
    if (overlay == nullptr) {
        return;
    }
    // 挡在这儿的是**已经开着的那一层**（一层只放得下一个页面，见 OverlayHost.hpp 那段）。
    // 它可能是"正铺在眼前"（用户点的是底栏那些没被盖住的按钮），也可能是"收着"。
    //
    // NOTE: **这一条不能不出声。** 开页那一下拿到 nullptr / false 时，调用方把对话框
    // 建了、一个字节没写、原地销毁 —— 用户点了实例行（或者组行那颗齿轮），屏幕上
    // **什么都没发生**，也没有任何提示。
    //
    // NOTE: 起这一句只在**收不掉**的时候才出现（正常那一档是静默收掉 + 直接
    // 继续，见 runWhenOverlayFree）—— 真走到这儿说明那一层连收页那一发都没认
    // （页面早就不卡嵌套循环了，只可能是这一层的状态出了意外），
    // 这时候唯一能做的是把话说清楚。
    const QString title = overlay->pageTitle();
    if (title.isEmpty()) {
        InlineMessage::warning(overlay, tr("请先关闭当前提示"),
                               tr("请确认或关闭当前对话框后再继续。"));
        return;
    }
    InlineMessage::warning(overlay, tr("先关掉现在这一页"),
                           tr("「%1」仍有一个页面打开。切换选项卡会隐藏该页面。\n\n请返回「%1」，选择左上角的返回按钮关闭该页面，再重试。")
                               .arg(title));
}
