/**
 * @file TaskProgressBar.hpp
 * @brief 自绘的进度条（统一自绘任务进度组件）。
 *
 * 任务进度条（底栏中间那格的摘要、任务中心浮动面板与整页、任务详情页）**共用这一条**，
 * 而不是各自就地 new 一颗原生 `QProgressBar`。
 *
 * 【为什么是 `QProgressBar` 的子类，而不是另起一个 `QWidget`】
 *
 * 因为"进度"这件事的**模型**（`range` / `value` / "总量未知 ⇒ 不确定态"）原生那一位已经
 * 说得很清楚了，而且两样东西已经钉在它身上：
 *
 * 另起一个 `QWidget` 的话，上面两处全得改（连带 `smoothProgress` 的签名）。子类化之后
 * **只把"画"换掉**，模型、动画、断言一个字节都不动 —— 这正是"换一层皮"该有的样子。
 *
 * 【画什么】
 *
 * 一根圆角细轨道：底是 `Theme::progressTrackColor()`、往前走的那一截是
 * `Theme::progressFillColor()`（就是主题色）。**不画百分比文字** —— 数在标题行 / 右边
 * 那一格（`MainWindowUI` 那边挪的），条里再写一遍是重复；连带上游把文字打开的那一位
 * 也一并关掉了（`ui_modernize.py audit` 里那条 "progress text enabled" 就是它）。
 *
 * 总量未知（`minimum() == maximum()`，底栏"有东西在跑、说不清到哪"那一档）画一截**来回
 * 滚的带子**；相位按单调时钟算（条被重建也接着滚）。关掉动效、以及 ui_shot 那种"像素每次
 * 都要一样"的场合 ⇒ 一条**不动**的带子，定住相位 0。
 */

#pragma once

#include <QProgressBar>

class QTimer;

class TaskProgressBar : public QProgressBar {
    Q_OBJECT

public:
    explicit TaskProgressBar(QWidget *parent = nullptr);

    /// 画成导航轨底部那枚 28 的进度环（设计稿：line-2 底圈 3 + accent 弧 2.5，从 12 点顺时针）。
    void setRing(bool ring) { m_ring = ring; update(); }
    bool isRing() const { return m_ring; }

    /// 走满时仍用主题色（默认走满换成功绿）。只有主页启动卡片打开：它的成功和失败另由眉题与 W 区分。
    void setFullUsesAccent(bool on) { m_fullAccent = on; update(); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    /// 这一格现在**该不该**有个定时器在催重画（确定态 / 藏起来 / 关了动效 ⇒ 都不该）。
    /// 幂等：状态没变就什么都不做。paint 一趟、显隐各一趟都会问它。
    void syncBusyTimer();

    bool m_ring = false;
    bool m_fullAccent = false;
    QTimer *m_busyTimer = nullptr;
};
