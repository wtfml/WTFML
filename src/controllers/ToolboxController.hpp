/**
 * @file ToolboxController.hpp
 * @brief 工具箱那几件小工具的脑子（页面在 `ui/ToolboxPage`，纯算术在 `src/tools/`；
 * 「高速下载」不是纯算术，它自己一份 `DirectDownloadController`）。
 * @note 要求：「不要再往 mainwindow 里堆超级屎山了，用 controller 等等方式
 * 合理分散功能」。所以这一件**一行逻辑都不往 MainWindow 放**：那边只有三件事 ——
 * 建它、把导航轨底部的「工具箱」接到 `open()` 上、析构时 `shutdown()`。
 *
 * 【这个 controller 管什么、不管什么】
 *
 * 管：
 * - **服务器在线检测**（`ServerPing::Pinger`）：异步那一套本来就在 `ServerPing` 里
 * （服务器列表「测速」用的是同一个），这里只是拿它问一台；
 * - **当场就能算的纯函数**（附魔顺序、文本生成器、坐标换算、信标、酿造、旗帜、纹饰）：
 * 这里只做"把页面上的控件读成参数 → 调 `src/tools/` → 把结果翻成页面认的那几个字符串"。
 *
 * 不改现有游戏文件。「高速下载」由 DirectDownloadController 写用户指定目标；
 * 纹饰预览资源由 ArmorTrimPreviewController 写独立 cache/preview-resources，不安装或执行客户端。
 *
 * 【为什么那几张表由 controller 喂给页面】
 *
 * `ui/` 不 include `src/`（全仓的约定）。所以"十六色是哪十六个""维度有哪几个""附魔表
 * 长什么样"这几张表都由这里读出来、翻成**字符串**再交给页面 —— 表本身各自只有一处
 * 出处（`TextFormat` / `CoordConvert` / `Anvil`）。
 */

#pragma once

#include <QObject>
#include <QString>

#include <QHash>
#include <QImage>
#include <QStringList>

#include "ToolboxPage.hpp"

#include <functional>

class OverlayHost;
class QWidget;
class ToolboxPage;
class DirectDownloadController;
class ActivityCenterController;
class ArmorTrimPreviewController;

namespace ServerPing {
class Pinger;
}

class ToolboxController final : public QObject {
    Q_OBJECT

public:
    struct Host {
        /// 写一行日志（这一层自己只记"问了什么、算出了什么"这种一行）。
        std::function<void(const QString &)> log;
        ActivityCenterController *activity = nullptr;
    };

    /// @param pageHost 工具箱选项卡的页面容器（页首次打开时挂进它的布局）。
    ToolboxController(OverlayHost *overlay, QWidget *pageHost, Host host,
                      QObject *parent = nullptr);
    ~ToolboxController() override;

    /**
     * @brief 工具箱选项卡露出来之后摆哪一层：@p toolKey 是 `ToolboxPage::toolKey()` 的键 →
     * 进那件工具；`"catalog"` → 回目录；空串 → 只确保页已建好，停在上次那一层。
     *
     * 切选项卡归 `MainWindowUI::openToolboxTab()`（它再调到这里）；认不出来的键什么都不做。
     * 页只在首次打开时创建，避免未使用的控件参与主窗口首次布局。
     */
    void open(const QString &toolKey);

    /// 收线程（析构会调一次；`MainWindow` 收尾时也调一次）。
    void shutdown();
    void setPlayerSkin(const QImage &skin, const QImage &cape, bool slim);
    bool ownsDownloadTask(quint64 id) const;
    bool canCancelDownloadTask(quint64 id) const;
    bool cancelDownloadTask(quint64 id);
    bool pauseDownloadTask(quint64 id);
    bool resumeDownloadTask(quint64 id);

private:
    /// 工具箱页只在首次打开时创建，避免未使用的控件参与主窗口首次布局。只搭目录页，接页面级信号。
    void ensurePage();
    /// 某件工具那一片刚搭出来（`ToolboxPage::toolBuilt`）：灌它的下拉表、摆回记住的输入、算初值。
    void setupTool(ToolboxPage::Tool tool);
    QHash<QString, QImage> m_bannerTextures;
    bool m_bannerTextureLoading = false;
    // ---------- 附魔顺序助手 ----------
    void runAnvilPlan();
    // ---------- 文本生成器 ----------
    void refreshText();
    void refreshTextFromPreview();
    void loadTextFromJson(const QString &json);
    // ---------- 坐标换算 ----------
    void convertCoordinates(int dimensionIndex, double x, double y, double z);
    void parseTeleport(const QString &text);
    void makeTeleportCommand(int fromDimensionIndex, int toDimensionIndex, double x, double y,
                             double z);
    // ---------- 服务器在线检测 ----------
    void checkServer(const QString &address);
    void cancelServerCheck();
    // ---------- 信标计算 / 酿造助手 ----------
    void refreshBeacon(int fromLevel, int level, int materialIndex, int beaconCount);
    void refreshBrewing(int potionIndex, bool extended, bool level2, int formIndex, int bottles);
    // ---------- 旗帜设计 / 纹饰搭配 ----------
    void refreshBanner(int baseColor, const QStringList &layers);
    /// 按四列里每一件的选择重算结果（材料账 + 判据层给的提示）。
    void refreshTrim();

    /// 那一页认的那几个维度（`setDimensionChoices` 喂的就是它）。**下标 = 页面上下拉框的
    /// 下标**，所以"第 i 项是哪个维度"只有这一处说得出口。
    QStringList dimensionNames() const;
    /// 第 @p index 项对应的维度（越界就回"认不出来"那一档）。
    int dimensionAt(int index) const;

    OverlayHost *m_overlay = nullptr;
    QWidget *m_pageHost = nullptr;
    Host m_host;
    ToolboxPage *m_page = nullptr;
    QImage m_playerSkin;
    QImage m_playerCape;
    bool m_playerSlim = false;
    bool m_hasPlayerSkin = false;
    DirectDownloadController *m_directDownload = nullptr;
    ArmorTrimPreviewController *m_trimPreview = nullptr;
    ServerPing::Pinger *m_pinger = nullptr;

    /// 这一趟问的是哪个地址（`Pinger::oneFinished` 只给下标、不给地址，所以自己记着）。
    QString m_serverAddress;
};
