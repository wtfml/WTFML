/**
 * @file WheelFocusFilter.hpp
 * @brief 全局滚轮规矩：下拉框、数字框、滑条没有获得焦点时不认滚轮，滚轮交给外层滚动区。
 *
 * 挂在 `QApplication` 上的一个事件过滤器，全仓每个 `QComboBox` / `QAbstractSpinBox` /
 * `QSlider`（含以后新建的）都不用逐个处理。
 *
 * 两件事缺一不可：
 * - Qt 对真滚轮事件是「先按焦点策略给焦点、再派发」，而这三种控件的默认策略
 *   `Qt::WheelFocus` 会让滚轮自己抢到焦点，`hasFocus()` 随之永远为真。所以控件第一次
 *   `Polish` 时把 `WheelFocus` 那一位摘掉（其余位照旧）。
 * - 没有焦点时把 `Wheel` 事件转给最近的外层滚动区（`WheelForward`）并吃掉。
 *
 * 只护具体控件，不护基类：滑条按 `QSlider` 认，**不用** `QAbstractSlider` —— 滚动条
 * 也是它的子类，而那一位必须继续吃滚轮（滚动条本来就是拿来滚的）。滚动条不在护的范围里。
 */
#pragma once

#include "WheelForward.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QObject>
#include <QSlider>

class WheelFocusFilter : public QObject {
public:
    /// 往 @p app 上挂一次；重复调用不会挂第二个。
    static void install(QApplication &app) {
        const QString name = QStringLiteral("wtfmlWheelFocusFilter");
        if (app.findChild<QObject *>(name, Qt::FindDirectChildrenOnly) != nullptr) {
            return;
        }
        auto *filter = new WheelFocusFilter(&app);
        filter->setObjectName(name);
        app.installEventFilter(filter);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget == nullptr || !isGuarded(widget)) {
            return false;
        }
        if (event->type() == QEvent::Polish) {
            if ((widget->focusPolicy() & Qt::WheelFocus) == Qt::WheelFocus) {
                widget->setFocusPolicy(Qt::FocusPolicy(widget->focusPolicy() & ~(Qt::WheelFocus ^ Qt::StrongFocus)));
            }
        } else if (event->type() == QEvent::Wheel && !widget->hasFocus()) {
            WheelForward::toScrollArea(widget, static_cast<QWheelEvent *>(event));
            return true;
        }
        return false;
    }

private:
    explicit WheelFocusFilter(QObject *parent) : QObject(parent) {}

    static bool isGuarded(const QWidget *widget) {
        return qobject_cast<const QComboBox *>(widget) != nullptr ||
               qobject_cast<const QAbstractSpinBox *>(widget) != nullptr ||
               qobject_cast<const QSlider *>(widget) != nullptr;
    }
};
