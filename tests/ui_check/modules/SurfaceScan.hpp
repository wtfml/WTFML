// 扫一棵控件树里「还长着 Qt 默认样子」的控件：列表代理、视口/边框、原生表头、
// 分组框、单选钮、页签、带边框的文本框、没走 markSwitch 的复选框，以及换页没有动效的
// QStackedWidget。
//
// 判据只报「看起来没接设计系统」。计入的每一条都让 `wtfml.surface-sweep` 失败（守卫，见
// UiSurfaceSweepCheck.cpp 末尾）；真该保留原样的地方不另开白名单，只在 checkBoxException()
// 里登记，并写一句理由。
//
// 检测「换页动效」的办法：打开 Anim，切一页，看有没有 `wtfmlPageCurtain`
// （`Anim::pageAppear()` 挂上去的那块幕布）露出来 —— 有就是接了，没有就是硬切。
//
// scanTextFit() 是「文字放不下」的开发期巡检：只写 textfit.tsv，**不参与退出码**。
// 量宽靠字体度量、省略靠 elidedText 复算，是启发式的，给人挑页面用，别当守卫。
#pragma once

#include "Animations.hpp"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHash>
#include <QHeaderView>
#include <QItemDelegate>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTextEdit>
#include <QTreeView>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <typeinfo>
#if defined(__GNUC__)
#include <cxxabi.h>
#endif

struct SurfaceFinding {
    QString kind;    ///< 类别（汇总时按它计数）
    QString cls;     ///< 真实类名
    QString name;    ///< objectName，空的话写父控件链上的线索
    QString detail;  ///< 看到的样子
};

namespace SurfaceScan {

inline QString classNameOf(const QObject *object) {
    return object == nullptr ? QString() : QString::fromLatin1(object->metaObject()->className());
}

/// 认人用的标签：objectName → 父控件类名 + 父控件 objectName → 可见文字。
inline QString labelOf(QWidget *widget) {
    if (widget == nullptr) return QString();
    if (!widget->objectName().isEmpty()) return widget->objectName();
    QWidget *parent = widget->parentWidget();
    QString parentName = classNameOf(parent);
    if (parent != nullptr && !parent->objectName().isEmpty()) parentName += QLatin1Char('#') + parent->objectName();
    return parentName;
}

inline bool isDefaultDelegate(const QAbstractItemDelegate *delegate) {
    // 认「是不是 Qt 自带那一个」要靠 typeid：本仓不少自家代理没有 Q_OBJECT
    // （它们不发信号），metaObject()->className() 会报成基类名 QStyledItemDelegate，
    // 照名字判会把它们全当成没挂代理。
    if (delegate == nullptr) return false;
    return typeid(*delegate) == typeid(QStyledItemDelegate) || typeid(*delegate) == typeid(QItemDelegate);
}

/// 给人看的类名：typeid 的原文比 metaObject 准（见 isDefaultDelegate()），GCC 下再拆一次名字。
inline QString runtimeClassName(const QObject *object) {
    if (object == nullptr) return QString();
#if defined(__GNUC__)
    int status = 0;
    char *demangled = abi::__cxa_demangle(typeid(*object).name(), nullptr, nullptr, &status);
    QString name = status == 0 && demangled != nullptr ? QString::fromLatin1(demangled)
                                                       : QString::fromLatin1(typeid(*object).name());
    std::free(demangled);
    return name;
#else
    return QString::fromLatin1(typeid(*object).name());
#endif
}

/// 单独成行、但确实该留复选框的那几颗（不改开关）。每条写一句理由；只许人看过再加。
inline QString checkBoxException(const QString &objectName) {
    static const QHash<QString, QString> reasons = {
        {QStringLiteral("javaMismatchDontRemindCheck"),
         QStringLiteral("「不再提醒」是随这一次确认提交的一次性勾选，不是一直生效的状态")},
    };
    return reasons.value(objectName);
}

/// 父布局里只摆了这一件（「单独成行」的复选框多半是想当开关用）。
inline bool aloneInItsRow(QWidget *widget) {
    QWidget *parent = widget != nullptr ? widget->parentWidget() : nullptr;
    QLayout *layout = parent != nullptr ? parent->layout() : nullptr;
    if (layout == nullptr) return false;
    int visible = 0;
    for (int i = 0; i < layout->count(); ++i) {
        QLayoutItem *item = layout->itemAt(i);
        QWidget *w = item != nullptr ? item->widget() : nullptr;
        if (w == nullptr || qobject_cast<QCheckBox *>(w) == nullptr) continue;
        if (w->isVisible()) ++visible;
    }
    return visible == 1;
}

inline QList<SurfaceFinding> scanWidgets(QWidget *root) {
    QList<SurfaceFinding> out;
    if (root == nullptr) return out;
    const bool dumpViews = qEnvironmentVariableIsSet("WTFML_SURFACE_DEBUG_VIEWS");
    const auto add = [&out](const QString &kind, QWidget *widget, const QString &detail) {
        SurfaceFinding finding;
        finding.kind = kind;
        finding.cls = classNameOf(widget);
        finding.name = labelOf(widget);
        finding.detail = detail;
        out.append(finding);
    };
    for (QWidget *w : root->findChildren<QWidget *>()) {
        if (!w->isVisible() || w->width() <= 0) continue;
        // 下拉框自己那套内部件（弹层列表、编辑框）不算一页的表面。
        if (qobject_cast<QComboBox *>(w->parentWidget()) != nullptr) continue;

        if (auto *view = qobject_cast<QAbstractItemView *>(w)) {
            if (dumpViews)
                std::printf("     VIEW %-24s %-28s delegate=%s\n", classNameOf(view).toUtf8().constData(),
                            qPrintable(view->objectName()),
                            runtimeClassName(view->itemDelegate()).toUtf8().constData());
            if (isDefaultDelegate(view->itemDelegate())) {
                add(QStringLiteral("default-delegate"), view,
                    QStringLiteral("delegate=%1（没挂自家代理：整行选中底、圆角、悬停动效都没有）")
                        .arg(runtimeClassName(view->itemDelegate())));
            }
            const QFrame::Shape shape = view->frameShape();
            const bool autoFill = view->viewport() != nullptr && view->viewport()->autoFillBackground();
            if (autoFill) {
                add(QStringLiteral("opaque-viewport"), view,
                    QStringLiteral("frame=%1 + 视口 autoFillBackground=1（自家列表是 NoFrame + 视口透明）")
                        .arg(shape));
            } else if (shape != QFrame::NoFrame) {
                add(QStringLiteral("native-frame"), view,
                    QStringLiteral("frame=%1（QSS 里可能已经 border:none 盖掉，待人看）").arg(shape));
            }
            // 表头的底和字色 QSS 已经统一接管（透明底、辅助色）；这里只报还剩的三种原生毛病：
            // 表格左上角那颗白色角按钮、表头字居中（格里的字靠左）、某一列窄到表头字被截。
            QHeaderView *header = nullptr;
            QStringList headerIssues;
            if (auto *tree = qobject_cast<QTreeView *>(view)) header = tree->header();
            if (auto *table = qobject_cast<QTableView *>(view)) {
                header = table->horizontalHeader();
                if (table->isCornerButtonEnabled() && table->verticalHeader()->isVisible())
                    headerIssues.append(QStringLiteral("左上角角按钮"));
                if (header->defaultAlignment().testFlag(Qt::AlignHCenter))
                    headerIssues.append(QStringLiteral("表头字居中"));
            }
            if (header != nullptr && header->isVisible() && header->model() != nullptr) {
                const QFontMetrics fm(header->font());
                for (int i = 0; i < header->count(); ++i) {
                    if (header->isSectionHidden(i)) continue;
                    const QString label = header->model()->headerData(i, header->orientation()).toString();
                    const int need = fm.horizontalAdvance(label) + 12;  // QSS 里表头左右各 6px
                    if (!label.isEmpty() && header->sectionSize(i) < need)
                        headerIssues.append(QStringLiteral("「%1」列宽 %2 < %3").arg(label).arg(header->sectionSize(i)).arg(need));
                }
            }
            if (header != nullptr && header->isVisible() && !headerIssues.isEmpty())
                add(QStringLiteral("native-header"), view, headerIssues.join(QStringLiteral("；")));
            continue;
        }
        if (auto *edit = qobject_cast<QPlainTextEdit *>(w)) {
            if (edit->frameShape() != QFrame::NoFrame)
                add(QStringLiteral("framed-textedit"), edit,
                    QStringLiteral("frame=%1（裸 QPlainTextEdit 的立体边框）").arg(edit->frameShape()));
            continue;
        }
        if (auto *edit = qobject_cast<QTextEdit *>(w)) {
            if (edit->frameShape() != QFrame::NoFrame)
                add(QStringLiteral("framed-textedit"), edit,
                    QStringLiteral("frame=%1（裸 QTextEdit 的立体边框）").arg(edit->frameShape()));
            continue;
        }
        if (auto *scroll = qobject_cast<QScrollArea *>(w)) {
            if (scroll->frameShape() != QFrame::NoFrame)
                add(QStringLiteral("native-scroll-frame"), scroll,
                    QStringLiteral("frame=%1").arg(scroll->frameShape()));
            continue;
        }
        if (auto *box = qobject_cast<QGroupBox *>(w)) {
            add(QStringLiteral("groupbox"), box, box->title());
            continue;
        }
        if (qobject_cast<QRadioButton *>(w) != nullptr) {
            add(QStringLiteral("radiobutton"), w, static_cast<QRadioButton *>(w)->text());
            continue;
        }
        if (auto *check = qobject_cast<QCheckBox *>(w)) {
            // 复选框的勾选框本身是全局代理画的（Theme.cpp 的 PE_IndicatorCheckBox），
            // 所以只挑「单独成行、想当开关用」的：这些才该走 markSwitch() / ToggleSwitch。
            if (check->property("wtfmlSwitch").toBool() || check->text().trimmed().isEmpty()) continue;
            for (QWidget *parent = check->parentWidget(); parent != nullptr; parent = parent->parentWidget()) {
                if (qobject_cast<QAbstractItemView *>(parent) != nullptr) {
                    check = nullptr;  // 表格/列表行里的勾：归代理管，不算开关
                    break;
                }
            }
            if (check == nullptr) continue;
            if (!aloneInItsRow(check)) continue;
            if (const QString reason = checkBoxException(check->objectName()); !reason.isEmpty())
                add(QStringLiteral("exempt"), check, QStringLiteral("switch-like-checkbox 例外：%1").arg(reason));
            else
                add(QStringLiteral("switch-like-checkbox"), check, check->text());
            continue;
        }
        if (auto *tabs = qobject_cast<QTabWidget *>(w)) {
            add(QStringLiteral("tab-widget"), tabs,
                QStringLiteral("%1 页，tabBar 原生类=%2").arg(tabs->count()).arg(classNameOf(tabs->tabBar())));
            continue;
        }
        if (qobject_cast<QTabBar *>(w) != nullptr) {
            add(QStringLiteral("tab-bar"), w, QStringLiteral("%1 页").arg(static_cast<QTabBar *>(w)->count()));
            continue;
        }
    }
    return out;
}

/// 一个一个 QStackedWidget 试：切到下一页，看有没有换页幕布。
inline QList<SurfaceFinding> probePageTransitions(QWidget *root, QApplication &app) {
    QList<SurfaceFinding> out;
    if (root == nullptr) return out;
    const bool wasEnabled = Anim::enabled();
    Anim::setEnabled(true);
    for (QStackedWidget *stack : root->findChildren<QStackedWidget *>()) {
        if (!stack->isVisible() || stack->count() < 2) continue;
        const int old = stack->currentIndex();
        const int target = (old + 1) % stack->count();
        stack->setCurrentIndex(target);
        for (int i = 0; i < 4; ++i) app.processEvents();
        bool curtainUp = false;
        for (QWidget *curtain : root->findChildren<QWidget *>(QStringLiteral("wtfmlPageCurtain")))
            if (curtain->isVisible()) {
                curtainUp = true;
                break;
            }
        stack->setCurrentIndex(old);
        for (int i = 0; i < 2; ++i) app.processEvents();
        // 换回原页那一下同样起了一发（栈默认就有）：幕布长在页上，逐块收掉，截图不截到半路。
        for (QWidget *curtain : root->findChildren<QWidget *>(QStringLiteral("wtfmlPageCurtain")))
            Anim::finishPageAppear(curtain->parentWidget());
        if (!curtainUp) {
            SurfaceFinding finding;
            finding.kind = QStringLiteral("stacked-no-anim");
            finding.cls = QStringLiteral("QStackedWidget");
            finding.name = stack->objectName().isEmpty() ? labelOf(stack) : stack->objectName();
            finding.detail =
                QStringLiteral("%1 页，setCurrentIndex 硬切（没看到 pageAppear 的幕布；待人看）").arg(stack->count());
            out.append(finding);
        }
    }
    Anim::setEnabled(wasEnabled);
    return out;
}

// ---------------------------------------------------------------- 文字放不下

/// 一条「文字放不下」。source 是控件上设的整段原文，shown 是实际画出来的样子
/// （省略过的话带 …，和原文不同）；need/avail 只在量过宽高时有值。
struct TextFitFinding {
    QString kind;    ///< elided / clipped / wrap-clipped / header / item / han
    QString cls;
    QString name;
    QString source;
    QString shown;
    int need = -1;
    int avail = -1;
};

inline bool hasHan(const QString &text) {
    static const QRegularExpression re(QStringLiteral("\\p{Han}"));
    return re.match(text).hasMatch();
}

/// 逐类量「设上去的字」和「这块地方给得下多少」。只在 WTFML_SURFACE_SHOTS 下跑。
/// @param english 英文巡检时额外把可见文字里的汉字单独记一类（中文巡检时满屏都是，不记）。
inline QList<TextFitFinding> scanTextFit(QWidget *root, bool english) {
    QList<TextFitFinding> out;
    if (root == nullptr) return out;
    const auto add = [&out](const QString &kind, QWidget *w, const QString &source, const QString &shown,
                            int need, int avail) {
        TextFitFinding f;
        f.kind = kind;
        f.cls = runtimeClassName(w);
        f.name = labelOf(w);
        f.source = source;
        f.shown = shown;
        f.need = need;
        f.avail = avail;
        out.append(f);
    };

    // 换行标签：heightForWidth 比实际高度高就是末行被裁。
    const auto checkWrapped = [&add, english](QWidget *w, const QString &text) {
        auto *label = qobject_cast<QLabel *>(w);
        if (label == nullptr || text.trimmed().isEmpty()) return;
        const int want = label->heightForWidth(label->width());
        if (want > label->height()) add(QStringLiteral("wrap-clipped"), w, text, text, want, label->height());
        if (english && hasHan(text)) add(QStringLiteral("han"), w, text, text, -1, -1);
    };

    // 单行：量宽度，装不下分「会省略」和「硬裁」两种（靠 ElidedLabel 写的 elideMode 属性认）。
    const auto checkSingle = [&add, english](QWidget *w, const QString &text, const QFont &font, int avail) {
        // 富文本（拼接用的 <span>）和开关复选框（markSwitch 把文字挪到旁边一行，自己不画字）不算。
        if (text.trimmed().isEmpty() || avail <= 0 || Qt::mightBeRichText(text)) return;
        if (w->property("wtfmlSwitch").toBool()) return;
        const QFontMetrics fm(font);
        const int need = fm.horizontalAdvance(text);
        if (need > avail) {
            const bool elideCapable = w->property("elideMode").isValid();
            const Qt::TextElideMode mode = static_cast<Qt::TextElideMode>(
                elideCapable ? w->property("elideMode").toInt() : int(Qt::ElideRight));
            const QString shown = elideCapable ? fm.elidedText(text, mode, avail) : text;
            add(elideCapable ? QStringLiteral("elided") : QStringLiteral("clipped"), w, text, shown, need, avail);
        }
        if (english && hasHan(text)) add(QStringLiteral("han"), w, text, text, -1, -1);
    };

    for (QWidget *w : root->findChildren<QWidget *>()) {
        if (!w->isVisible() || w->width() <= 0) continue;
        if (qobject_cast<QComboBox *>(w->parentWidget()) != nullptr) continue;

        if (auto *view = qobject_cast<QAbstractItemView *>(w)) {
            // 列表/表格项：代理画的字，按可视矩形量。
            if (QAbstractItemModel *model = view->model()) {
                const QFontMetrics fm(view->font());
                std::function<void(const QModelIndex &, int)> walk;
                walk = [&](const QModelIndex &parent, int depth) {
                        const int rows = qMin(model->rowCount(parent), 40);
                        const int cols = qMin(model->columnCount(parent), 8);
                        for (int r = 0; r < rows; ++r) {
                            for (int c = 0; c < cols; ++c) {
                                const QModelIndex idx = model->index(r, c, parent);
                                const QString text = model->data(idx, Qt::DisplayRole).toString();
                                if (text.trimmed().isEmpty()) continue;
                                const QRect rect = view->visualRect(idx);
                                if (rect.width() <= 0) continue;
                                const int need = fm.horizontalAdvance(text);
                                if (need > rect.width() - 4)
                                    add(QStringLiteral("item"), w, text, text, need, rect.width());
                                if (english && hasHan(text)) add(QStringLiteral("han"), w, text, text, -1, -1);
                            }
                            const QModelIndex child = model->index(r, 0, parent);
                            if (depth > 0 && model->hasChildren(child)) walk(child, depth - 1);
                        }
                    };
                walk(QModelIndex(), 1);
            }
            // 表头：段宽装不下就是硬裁（表头不省略）。
            QHeaderView *header = nullptr;
            if (auto *tree = qobject_cast<QTreeView *>(view)) header = tree->header();
            if (auto *table = qobject_cast<QTableView *>(view)) header = table->horizontalHeader();
            if (header != nullptr && header->isVisible() && header->model() != nullptr) {
                const QFontMetrics fm(header->font());
                for (int i = 0; i < header->count(); ++i) {
                    if (header->isSectionHidden(i)) continue;
                    const QString label = header->model()->headerData(i, header->orientation()).toString();
                    if (label.trimmed().isEmpty()) continue;
                    const int need = fm.horizontalAdvance(label) + 12;
                    const int avail = header->sectionSize(i);
                    if (need > avail) add(QStringLiteral("header"), header, label, label, need, avail);
                    if (english && hasHan(label)) add(QStringLiteral("han"), header, label, label, -1, -1);
                }
            }
            continue;
        }
        if (auto *label = qobject_cast<QLabel *>(w)) {
            const QString text = label->text();
            if (label->wordWrap()) {
                checkWrapped(label, text);
            } else {
                const QMargins m = label->contentsMargins();
                checkSingle(label, text, label->font(),
                            label->contentsRect().width() - m.left() - m.right());
            }
            continue;
        }
        if (auto *button = qobject_cast<QAbstractButton *>(w)) {
            const QString text = button->text();
            if (text.trimmed().isEmpty()) continue;  // 纯图标按钮不量
            int extra = 0;
            if (!button->icon().isNull()) extra = button->iconSize().width() + 4;
            checkSingle(button, text, button->font(), button->contentsRect().width() - extra);
            continue;
        }
        if (auto *edit = qobject_cast<QLineEdit *>(w)) {
            checkSingle(edit, edit->placeholderText(), edit->font(), edit->contentsRect().width());
            continue;
        }
        if (auto *tabs = qobject_cast<QTabBar *>(w)) {
            for (int i = 0; i < tabs->count(); ++i)
                checkSingle(tabs, tabs->tabText(i), tabs->font(), tabs->tabRect(i).width());
            continue;
        }
    }
    return out;
}

inline QString findingSummary(const QList<SurfaceFinding> &findings) {
    QMap<QString, int> counts;
    for (const SurfaceFinding &finding : findings) ++counts[finding.kind];
    QStringList parts;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        parts.append(QStringLiteral("%1×%2").arg(it.key()).arg(it.value()));
    return parts.join(QLatin1Char(' '));
}

}  // namespace SurfaceScan
