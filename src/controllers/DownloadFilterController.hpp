/**
 * @file DownloadFilterController.hpp
 * @brief 下载页每一页顶上那一行筛子（来源 / 版本 / 加载器 / 类型）的灌与读。
 *
 * 【为什么单独一层】
 *
 * 这一段的规矩是"界面只搭空框，**内容全在这儿灌**，读也只从下面这几个访问器读"。谁要是
 * 绕开它们自己去 `combo->currentText()`，症状是"界面上筛了、请求里没筛" —— 不报错、
 * 也看不出哪儿不对。所以"谁能读这一格"这件事得有个唯一的门，而不是散在主窗口那几百个
 * 成员里。搬出来之后，写侧（灌选项）与读侧（拼搜索请求）在同一个 .cpp 里，看一遍就够。
 *
 * 【它只管这一行筛子，不管搜索结果】
 *
 * 发起搜索（`MainWindow::startResourceSearch`）、结果回来怎么摆行都留在主窗口 —— 那是
 * 另一件事的生命周期（有请求钥匙、有假外网路由）。这一层只回答"这一页现在筛的是什么"。
 *
 * NOTE: 那一行五个控件的所有权在 `MainWindowUI` 手里（这一页建的是空框），这里只按
 * 分类键去要，拿不到就回"不筛" —— 别的页上某些格本来就不存在（比如加载器只有模组页有）。
 */

#pragma once

#include <QObject>
#include <QString>
#include "download/ResourceCatalog.hpp"

class MainWindow;

class DownloadFilterController final : public QObject {
    Q_OBJECT

public:
    explicit DownloadFilterController(MainWindow *owner);

    /// 搜索请求钥匙 → 是哪一类、哪个来源。拼钥匙和认钥匙读的是同一个函数，所以这里
    /// 一条一条比、不解析字符串；**来源也要比一圈**（CurseForge 那一发回来时一把都对不上
    /// 的症状是"切到 CurseForge 按了搜索，那一页什么都没有、也不报错"）。
    bool kindForSearchRequestKey(const QString &key, ResourceCatalog::Kind *out,
                                 ResourceCatalog::Source *outSource) const;
    /// 这一页还空着就搜一发（已经有东西了就不重搜：来回换分类每次都问一遍既慢、
    /// 又是在拿限流额度换同一份东西）。
    void ensureResourcePageLoaded(ResourceCatalog::Kind kind);
    /// 左栏那一列分类里某一行被点了。
    void refreshDownloadCategory(int row);

    ResourceCatalog::Source downloadFilterSource(ResourceCatalog::Kind kind) const;
    QString downloadFilterGameVersion(ResourceCatalog::Kind kind) const;
    QString downloadFilterLoader(ResourceCatalog::Kind kind) const;
    QString downloadFilterCategory(ResourceCatalog::Kind kind) const;

    void fillDownloadFilterVersionOptions();
    void refreshDownloadFilterOptions(ResourceCatalog::Kind kind);
    void fillDownloadFilters();
    void resetDownloadFilters(ResourceCatalog::Kind kind);

private:
    MainWindow &m_owner;
};
