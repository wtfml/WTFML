/**
 * @file AccountSkinDialog.hpp
 * @brief 「账号与外观」对话框：左边管多个正版账号（登录/切换/删除），右边管外观
 * （换皮肤、换披风、不显示披风）。
 * @note 和 GroupDialog / InstanceSettingsDialog 同一条约定：**ui/ 不依赖业务层**，本文件
 * 不 include 任何 src/ 下的头；进参出参都是值，对话框只发"用户想做什么"的信号，
 * 真正的干活（登录、拉列表、上传）全在 MainWindow 里。
 *
 * 【为什么这个模块单独开一个对话框，而不是塞进设置页】
 *
 * 设置页里那一格是"给这个启动器选一个账户"，是**一次性**的决定；这里管的是"我有一堆
 * 账号、现在想换成哪个、顺手把这套外观换一下"——它是个**来回切**的地方。两类操作的节奏
 * 不一样，挤在一格里会出现"改完账号想去改皮肤，还得先记住在哪个标签页"。
 *
 * 【为什么披风只给一个列表，不给缩略图】
 *
 * 缩略图要按 url 去下（那是 PlayerSkinStore 的活），而"这件披风长什么样"看 3D 预览更准
 * ——预览里那个小人是**穿上**这一件的效果，比一排小图直观。列表上写名字就够了。
 *
 * 【错误怎么报】
 *
 * 这个对话框自己**不弹窗**（它连 QMessageBox 都不 include）：正文由 MainWindow 那边
 * 决定（用户主动发起的动作失败 → 弹窗；背景刷新失败 → 写进这里的 statusLabel，见
 * MainWindow::refreshAccountSkin()）。分开的理由是这两种失败的"该不该打断用户"不一样。
 */

#pragma once

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QList>
#include <QString>

class QAction;
class QComboBox;
class QBoxLayout;
class ElidedLabel;
class QLabel;
class QMenu;
class QLineEdit;
class QListWidget;
class QPushButton;
class SkinViewer;

class AccountSkinDialog : public QDialog {
    Q_OBJECT

public:
    /// 列表里一个账号。**只带显示要用的东西**，凭据字节在 AccountStore 里，不出业务层。
    struct AccountRow {
        QString uuid;
        QString name;
        /// 收进列表的时刻（ISO）。显示成"存于 …"，用户认得出是哪一个。
        QString savedAt;
        bool current = false;
        /// 这是一条**离线账号**（只有名字，盘上没有凭据）。行尾会挂「（离线）」。
        bool offline = false;
        /// 这个账号自己的头像（调用方裁好的方形脸）。空 = 画首字母。
        QImage face = {};
        /// 刷新令牌失效、必须重新登录（微软账号才有）。来源那行改写「需要重新登录」，行尾给「重新登录」。
        bool needsRelogin = false;
    };

    /// 一件披风。
    struct CapeRow {
        QString id;
        QString alias;
        bool active = false;
        QString url;  ///< 披风贴图地址；贴图到了用 setCapeTexture() 补进格子
    };

    explicit AccountSkinDialog(QWidget *parent = nullptr);

    // ---------- 数据进 ----------

    /// 重建账号列表。当前账号那一条会被选中（列表空/没当前账号时选中第一条）。
    /// NOTE: 选中项**故意不跨调用保留**：这一列会被刷新好几遍（登录成功、切换、删除），
    /// 保留一个 uuid 就得处理"它已经不在列表里了"，而用户看的是"当前账号是哪一条"。
    void setAccounts(const QList<AccountRow> &rows);

    /// 当前身份那一句（"已登录为「X」（正版登录）" / "已登录为「X」（离线登录）" /
    /// "还没登录"）。正文由调用方给（它才知道当前账号是哪种），这里只负责显示。
    void setStatusText(const QString &text);
    /// 请求失败的原因：写在身份那一句下面另起一行（次要文字色，后面跟「重试」），不动身份那一句。
    /// 空串 = 收起这一行。
    void setStatusError(const QString &reason);
    /// 3D 预览下面那一行次要文字（离线账号的皮肤说明）。空串 = 不显示。
    void setPreviewNote(const QString &text);

    /// 3D 预览那套外观。**和主页右栏是同一份贴图**（同一份数据两个窗口）。
    /// @param skin 皮肤贴图（64×64 或老格式 64×32）。空图 = 只画背景。
    /// @param cape 披风贴图（64×32）。可空。
    /// @param slim 是不是纤细（Alex）模型。
    void setLooks(const QImage &skin, const QImage &cape, bool slim);

    /// 同一账号后台刷新时保留仍存在的选中项。
    void setCapes(const QList<CapeRow> &capes, bool keepSelection = false);
    void clearCapes();
    void setCapeLoading(const QString &text);
    /// 某件披风的贴图（64×32）到了：那一格改画贴图正面，不再是占位色条。
    void setCapeTexture(const QString &url, const QImage &texture);

    /// 浏览器登录期间暂时锁住会修改当前账号的操作。
    void setBusy(bool busy, const QString &what = QString());

    /**
     * @brief 当前账号是不是离线身份（决定「皮肤」那一行露出哪一颗按钮）。
     *
     * 正版账号走「上传皮肤…」（要令牌、传到官方）；离线账号没有令牌，那一颗按下去只会
     * 失败，所以**藏起来**，换成「换一张…」（从盘上挑一张）。
     * 两档互斥，不是两个都能点。
     *
     * @param offline 当前账号本身是不是离线账号（判据由调用方给，见
     * `PlayerSkinController::offlineIdentity()` —— 全仓唯一出处）。
     * @param hasCustomSkin 这个离线账号现在是不是已经有一张自己挑的了 ——
     * 「恢复默认外观」只在有东西可恢复时可用（没有就灰着，点了没反应比灰着更难懂）。
     */
    void setOfflineSkinState(bool offline, bool hasCustomSkin);

    // ---------- 数据出（现场读控件，没有影子状态） ----------

    /// 账号列表里选中那一条的 uuid（没选中返回空串）。
    QString selectedUuid() const;
    /// 披风列表里选中那一件的 id（没选中返回空串）。
    QString selectedCapeId() const;
    /// 披风列表里这个 id 的贴图地址（列表还没有它时回空串）。
    QString capeUrl(const QString &capeId) const;

    /// 让 3D 预览立即跟着「显示 / 隐藏」走，不等下一次资料刷新。
    void updatePreviewCape(bool wearing, bool turnToBack = false);
    /// 「模型」下拉框里选的 variant（已经是小写形态：classic / slim）。
    QString selectedVariant() const;

    /// 把离线名字那个输入框清空。
    /// NOTE: **只能由调用方在添加成功之后调**，别塞进 setAccounts() —— 那个函数是"刷新
    /// 列表"的意思，用户正打字时来一次刷新就把没提交的名字擦掉了。
    void clearOfflineNameDraft();

    /// 展开离线账号那一行（名字输入框 + 「添加」），输入框拿焦点。「添加账号」菜单里选「离线账号」就是它。
    void showOfflineAddRow();
    bool offlineAddRowShown() const;
    /// 非空 = 这一版微软登录用不了：「添加账号」菜单里「微软账号」置灰，悬停显示这句话。
    void setOnlineLoginUnavailable(const QString &reason);

    /// 披风列表是哪个账号的。请求失败时列表留着上次的内容，但换过账号就不该再留别人的披风，
    /// 调用方拿它判断"留不留"。
    void setCapesOwner(const QString &uuid) { m_capesOwner = uuid; }
    QString capesOwner() const { return m_capesOwner; }

    // ---------- 控件的 objectName ----------
    // 验证工程靠这些名字认控件，别改回按文字去认（文字会随数据变）。
    static QString addAccountButtonName() { return QStringLiteral("accountSkinAddButton"); }
    static QString addAccountMenuName() { return QStringLiteral("accountSkinAddMenu"); }
    static QString addMicrosoftActionName() { return QStringLiteral("accountSkinAddMicrosoft"); }
    static QString addOfflineActionName() { return QStringLiteral("accountSkinAddOffline"); }
    static QString offlineRowName() { return QStringLiteral("accountOfflineRow"); }
    static QString statusLabelName() { return QStringLiteral("accountSkinStatus"); }
    static QString statusErrorLabelName() { return QStringLiteral("accountSkinStatusError"); }
    /// 需要重新登录的行尾按钮（每个这样的行一颗，同名；uuid 在动态属性 `wtfmlReloginUuid` 上）。
    static QString reloginButtonName() { return QStringLiteral("accountSkinReloginButton"); }
    static QString retryButtonName() { return QStringLiteral("accountSkinRetryButton"); }
    static QString previewNoteName() { return QStringLiteral("accountSkinPreviewNote"); }
    static QString copyUuidButtonName() { return QStringLiteral("accountSkinCopyUuidButton"); }
    static QString accountListName() { return QStringLiteral("accountSkinAccountList"); }
    static QString switchButtonName() { return QStringLiteral("accountSkinSwitchButton"); }
    static QString removeButtonName() { return QStringLiteral("accountSkinRemoveButton"); }
    static QString refreshButtonName() { return QStringLiteral("accountSkinRefreshButton"); }
    static QString offlineNameEditName() { return QStringLiteral("accountSkinOfflineNameEdit"); }
    static QString addOfflineButtonName() {
        return QStringLiteral("accountSkinAddOfflineButton");
    }
    static QString viewerName() { return QStringLiteral("accountSkinViewer"); }
    static QString uploadButtonName() { return QStringLiteral("accountSkinUploadButton"); }
    /// 离线账号那一颗「换一张…」（正版账号露的是 uploadButtonName()，两档互斥）。
    static QString offlineSkinButtonName() {
        return QStringLiteral("accountSkinOfflineSkinButton");
    }
    /// 离线账号那一颗「恢复默认外观」（撤掉自己挑的那张）。
    static QString offlineSkinResetButtonName() {
        return QStringLiteral("accountSkinOfflineSkinResetButton");
    }
    static QString variantComboName() { return QStringLiteral("accountSkinVariantCombo"); }
    static QString capeListName() { return QStringLiteral("accountSkinCapeList"); }
    static QString applyCapeButtonName() { return QStringLiteral("accountSkinApplyCapeButton"); }
    static QString closeButtonName() { return QStringLiteral("accountSkinCloseButton"); }

    // ---------- 产品文案 ----------
    static QString windowTitle() { return tr("账号与外观"); }
    static QString accountSectionTitle() { return tr("账号"); }
    static QString looksSectionTitle() { return tr("外观预览"); }
    static QString capeSectionTitle() { return tr("披风"); }
    static QString addAccountButtonText() { return tr("添加账号"); }
    /// 账号来源，行里名字下面那一行小字，也是「添加账号」菜单里的两项。
    static QString microsoftAccountText() { return tr("微软账号"); }
    static QString offlineAccountText() { return tr("离线账号"); }
    static QString copyUuidText() { return tr("复制 UUID"); }
    /// 需要重新登录的微软账号：来源那行小字（危险色）与行尾按钮。
    static QString needsReloginText() { return tr("需要重新登录"); }
    static QString reloginButtonText() { return tr("重新登录"); }
    static QString retryButtonText() { return tr("重试"); }
    /// 离线账号预览下面的说明（预览和游戏一致时不显示）。
    static QString previewNoteLocalSkin() {
        return tr("本地皮肤只在启动器内显示，游戏内按名字分配默认皮肤。");
    }
    static QString previewNoteSteve() { return tr("游戏内按名字分配默认皮肤。"); }
    static QString switchButtonText() { return tr("切换到所选"); }
    static QString removeButtonText() { return tr("删除所选"); }
    static QString refreshButtonText() { return tr("刷新外观"); }
    static QString uploadButtonText() { return tr("上传皮肤…"); }
    /// 离线账号那一颗（见 `setOfflineSkinState()`）。用词和图片选择框/Coding plan 里那句
    /// 「换一张」一致 —— 它换的只是**启动器里**显示的那张，不是游戏里那张脸。
    static QString offlineSkinButtonText() { return tr("换一张…"); }
    static QString offlineSkinResetButtonText() { return tr("恢复默认外观"); }
    static QString offlineSkinButtonToolTip() {
        return tr("选一张 PNG（64×64 或 64×32）作为这个离线账号在启动器里的外观。只影响启动器预览，游戏内皮肤由游戏自己决定。");
    }
    static QString offlineSkinResetButtonToolTip() {
        return tr("移除手动设置的皮肤，恢复此账号的默认皮肤。");
    }
    static QString applyCapeButtonText() { return tr("应用所选披风"); }
    static QString noCapeText() { return tr("无披风"); }
    static QString closeButtonText() { return tr("关闭"); }
    static QString variantClassicText() { return tr("经典（Steve）"); }
    static QString variantSlimText() { return tr("纤细（Alex）"); }
    /// 模型下拉框里那一项对应的 variant 值（**小写**，就是传给官方那个）。
    static QString variantClassicValue() { return QStringLiteral("classic"); }
    static QString variantSlimValue() { return QStringLiteral("slim"); }
    /// 当前账号那一条后面的标记。
    static QString currentMark() { return tr("（当前）"); }
    /// 离线账号那一条后面的标记（和「（当前）」可以同时出现）。
    static QString offlineMark() { return tr("（离线）"); }
    static QString offlineNamePlaceholder() { return tr("离线账号的名字"); }
    static QString addOfflineButtonText() { return tr("添加离线账号"); }
    /// 离线账号那一行的 tooltip —— 它没有"什么时候登录取自"这回事，得说清它是哪一种。
    static QString offlineRowTooltip() {
        return tr("离线账号不校验在线凭据，直接以此名称启动游戏。");
    }
    /// 当前身份那一句，离线那一档。和 identityText() 是**两句不同的话** —— 离线账号
    /// 不是"正版登录"，这两个词在用户那里是完全不同的两件事，不能混用一句。
    /// @param name 离线账号的名字。
    static QString offlineIdentityText(const QString &name) {
        return tr("已登录为「%1」（离线登录）").arg(name);
    }
    /// 未登录时页头副题那一句。账号列表空着时整块收起、不再摆占位行，状态只在这里说一次。
    static QString notLoggedInNote() { return tr("未登录"); }
    static QString loginHintNote() {
        return tr("微软账号通过浏览器登录；离线账号直接输入玩家名称。");
    }
    /// 当前身份那一句。**只在这一种情况下用**：当前账号是个正版（微软）账号。
    /// 另外两种（当前是离线账号 / 没登录）各自有专门那句，见 offlineIdentityText() /
    /// notLoggedInNote()。
    ///
    /// NOTE: 「离线」只有一种意思：**当前账号本身是离线账号**。主页没有「离线模式」
    /// 勾选框；添加账号时选「添加离线账号」，正版账号不会走离线那一档。
    /// 所以 identityText() 只按这一个判据分两句（见 offlineIdentityText() /
    /// notLoggedInNote()）。
    /// @param name 账号名。
    static QString identityText(const QString &name) {
        return tr("已登录为「%1」（正版登录）").arg(name);
    }

signals:
    /// 用户要加一个账号（开始走微软登录）。
    void loginRequested();
    /// 用户填了个离线名字要加进来。
    /// @param name 已经 trim 过的名字（合不合法由接收方判，见文件头那两条约定）。
    void offlineAccountRequested(const QString &name);
    /// 用户要切换到某个账号（列表里那一行的 uuid）。
    void switchRequested(const QString &uuid);
    /// 用户要删掉某个账号。
    void removeRequested(const QString &uuid);
    /// 用户要重新拉一遍外观（皮肤/披风/账号列表）。
    void refreshRequested();
    /// 用户选好了要上传的皮肤文件。
    /// @param path 本地 PNG 路径。@param variant 想用哪个模型（classic / slim）。
    void skinUploadRequested(const QString &path, const QString &variant);
    /// 离线账号：用户挑了一张本地皮肤当"启动器里那张脸"。
    /// @param path 本地图片路径。@param variant 模型（classic / slim）。
    ///
    /// NOTE: 和 `skinUploadRequested()` **分开**：那个是传给 Mojang（要令牌、改的是
    /// 官方档案），这个是只存在本机（不需要令牌、改的是启动器里的预览）。一个信号
    /// 两件事的话，接收方就得按"当前是不是离线"再分一次 —— 那一分迟早分错。
    void offlineSkinPickRequested(const QString &path, const QString &variant);
    /// 离线账号：撤掉自己挑的那张。
    void offlineSkinResetRequested();
    /// 用户要把某一件披风设成当前。
    void capeSelectRequested(const QString &capeId);
    /// 用户要取消披风（不显示）。
    void hideCapeRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    /// 「上传皮肤…」：选文件，成了就发 skinUploadRequested()。
    void pickSkinFile();
    /// 「换一张…」（只有离线账号露这一颗）：选文件，发 offlineSkinPickRequested()。
    void pickOfflineSkinFile();
    /// 「添加离线账号」（或者在那个输入框里回车）：发 offlineAccountRequested()。
    void submitOfflineName();
    /// 列表选择变化时刷那几个按钮的可用性（没选中就点不动）。
    void refreshButtonStates();

private:
    /// 往列表里塞一行（文字 + uuid + 当前标记）。
    void addAccountRow(const AccountRow &row);
    /// 「切换 / 删除」两颗跟着选中那一行摆在行尾（设计稿 .acct-row 悬停操作）。
    void placeRowActions();
    /// 「重新登录」摆在各自那一行的行尾；选中那一行让到「复制 UUID」「删除」左边。
    void placeReloginButtons();
    bool eventFilter(QObject *watched, QEvent *event) override;

    /// 现在是不是有一件请求在走。**它必须参与** refreshButtonStates() 的判断：
    /// 那个函数是按「有没有选中」定可用性的，而 setBusy() 的末尾会调它一次 —— 于是
    /// 忙的时候那几个按钮会被重新点亮，等于登录还在飞用户就能切账号，回来之后两条路
    /// 各写一遍凭据。这个成员就是给那个函数补上「忙的时候一律灰」这一条。
    bool m_busy = false;
    QWidget *m_headerBar = nullptr;
    QWidget *m_accountsPanel = nullptr;
    QWidget *m_looksPanel = nullptr;  // 皮肤 / 披风：宽版在右栏，中宽挪进账号栏下面（见 resizeEvent）
    QBoxLayout *m_bodyLayout = nullptr;

    /// 「皮肤」那一行现在露的是哪一档（见 setOfflineSkinState()）。两个都从外面喂进来，
    /// 这个窗不自己判"当前账号是不是离线" —— 那件事的唯一出处在业务层。
    bool m_offlineSkin = false;
    bool m_hasCustomSkin = false;

    QLabel *m_statusLabel = nullptr;
    QListWidget *m_accountList = nullptr;
    QPushButton *m_switchButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QLineEdit *m_offlineNameEdit = nullptr;
    QPushButton *m_addOfflineButton = nullptr;
    /// 「添加账号」：弹出菜单选微软账号 / 离线账号；选离线才露出下面那一行（输入框 + 「添加」）。
    QPushButton *m_addAccountButton = nullptr;
    QMenu *m_addAccountMenu = nullptr;
    QAction *m_addMicrosoftAction = nullptr;
    QAction *m_addOfflineAction = nullptr;
    QWidget *m_offlineRow = nullptr;
    QWidget *m_statusErrorRow = nullptr;
    ElidedLabel *m_statusErrorLabel = nullptr;
    QPushButton *m_retryButton = nullptr;
    QLabel *m_previewNote = nullptr;
    QPushButton *m_copyUuidButton = nullptr;
    QString m_capesOwner;
    SkinViewer *m_viewer = nullptr;
    QPushButton *m_uploadButton = nullptr;
    /// 离线账号那一档的两颗（默认都藏着，见 setOfflineSkinState()）。
    QPushButton *m_offlineSkinButton = nullptr;
    QPushButton *m_offlineSkinResetButton = nullptr;
    QComboBox *m_variantCombo = nullptr;
    QListWidget *m_capeList = nullptr;
    QLabel *m_capeLoadingLabel = nullptr;
    QPushButton *m_applyCapeButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    QWidget *m_rowActions = nullptr;
    /// 每个需要重新登录的行一颗，挂在列表 viewport 上；setAccounts() 时整批重建。
    QList<QPushButton *> m_reloginButtons;
    /// 非空 = 微软登录用不了（见 setOnlineLoginUnavailable()），「重新登录」跟着置灰。
    QString m_loginUnavailable;
    /// 当前账号那一行的头像取自这张（setLooks() 喂进来的皮肤）。
    QImage m_currentSkin;
    /// 按贴图地址缓存，列表重建时先把已有的贴图塞回去，别等下一次网络回调。
    QHash<QString, QImage> m_capeTextures;
    /// 用户最后点「应用」的那件披风；资料刷新时预览以它为准，别退回旧 active。
    QString m_previewCapeId;
    /// 用户应用了「无披风」：资料刷新带回旧披风也不能再贴回来。
    bool m_noCapeApplied = false;
    /// 披风列表已经喂过（之后资料接口再带旧披风图也不信它）。
    bool m_capesLoaded = false;
};



