/**
 * @file VersionDisplay.hpp
 * @brief 版本显示名的拼装规则：把一个版本的各个组成部分拼成界面上那一串。
 * @note 纯函数，不碰磁盘、不碰 widget。磁盘上的元数据由
 * VersionManager::displayParts() 读。拼法是固定的一份（UI 问题 #14 删掉了「版本名格式」开关）。
 */
#pragma once

#include <QString>
#include <QStringList>

/**
 * @brief 一个版本参与显示名的各个部分。
 *
 * customName / loaderName / loaderVersion 都是"有就显示、没有就跳过"的可选项，
 * 它们的来源见 VersionManager::displayParts()。
 */
struct VersionDisplayParts {
    /// 版本目录名，也就是真正进 --version 和文件路径的那个 id。永远非空。
    QString id;
    /// 这个实例实际是哪个版本，例如 "1.18.2"。**为空时退化成 id**。
    ///
    /// 为什么不能拿 id 当实际版本：用户的目录名早就把加载器揉进去了，真机上是
    /// `1.18.2-Forge_40.3.12`、`1.21.1-NeoForge_21.1.210`、`26.2-Fabric 0.19.5`
    /// 这种。往 loaderName 里填 "Forge" 就会拼出
    /// `我的整合包-1.18.2-Forge_40.3.12-Forge` —— 去重只对**完全相同**的串生效，
    /// 救不了 `Forge` 和 `Forge_40.3.12`。PCL 用 Setup.ini 里的 VersionVanillaName
    /// 这个独立字段解决同一个问题（`海波篮子` 的真实版本是 `26.2`）。
    ///
    /// 来源是 wtfml.meta 里可选的 "realVersion"。**读侧已经全部打通**：displayParts() 读它、
    /// 这里拿它当"实际版本"段，所以用户手写这个字段会**立刻**反映到界面上的显示名。
    /// 缺的是**写侧** —— 还没有任何界面或代码会写它（那个入口排在"版本重命名 / 实例管理"里），
    /// 所以今天的常态是这一项为空、由上面的 id 兜底。
    QString realVersion;
    /// 重命名后的自定义名。没重命名过就是空串。
    QString customName;
    /// 模组加载器名，例如 "Forge"。没检测到就是空串。
    QString loaderName;
    /// 模组加载器版本，例如 "47.2.0"。没检测到就是空串。
    QString loaderVersion;
    /// 这个实例自定义的图标，对应 wtfml.meta 里可选的 "icon"。为空表示"用默认那张"
    /// （按版本分类 / 加载器算出来 —— 见 MainWindow::instanceRowMark()）。
    ///
    /// 取值是带前缀的串，三种形态（唯一出处见 VersionManager::iconField()）：
    /// - 空串 → 默认
    /// - `builtin:<候选键>` → 内置候选里那一张（键见 VersionIcons::iconCandidateKeys()）
    /// - `local` → 用实例目录下那份 icon.png（AppPaths::versionIconPath()）
    ///
    /// NOTE: 放在这个结构里而不是让每个调用方自己去 versionMeta() 里掏：`displayParts()`
    /// 已经把这个文件读过一遍了（它就在读 name / realVersion / loaderName），再有
    /// 一个地方单独去读一次，实例树每一行就多一次文件打开。图标和显示名是同一次
    /// 读取的两半。
    QString icon;
};

/**
 * @brief 把一个版本拼成显示名。
 * @param parts 版本的各个组成部分。
 * @return 显示名，`-` 连接的若干段。
 *
 * 顺序固定是：自定义名槽 → 实际版本 → 加载器名。规则：
 * - 名称槽在没有自定义名时**退化成实际版本**（没重命名过就显示版本号本身）；
 * - "实际版本"指的是 `parts.realVersion`，为空时退化成 `parts.id`（目录名）。
 * `id` 不会作为**独立的一段**出现，它只在上面这处兜底时被顶上来当实际版本用；
 * 而它真正的身份是进 --version 和文件路径的那个值，可以长得完全不像版本号
 * （真机上有 `原版ATT2地图（无模组）` 这种）；
 * - 文本重复的段跳过，所以"名称槽退化成版本号"不会拼出 `1.20.1-1.20.1`；
 * - 空串段跳过，所以没加载器时不会留下多余的分隔符。
 *
 * NOTE: **不再有「版本名格式」开关**（UI 问题 #14 删掉了那一组设置）。拼法就是上面这一份，
 * 也就是改之前那四个开关的出厂默认值（自定义名✓ 实际版本✓ 加载器名✓ 加载器版本✗）——
 * 没改过设置的用户看到的显示名一个字都不变。加载器版本那一段刻意不出现：出厂默认关着它。
 *
 * NOTE: 输入按不可信数据清洗（去首尾空白、内部换行/制表符折成空格、长度封顶）。wtfml.meta
 * 的意义就是让用户手写，而一个含 '\n' 的名字会把"启动游戏"按钮的两行文字撑成三行。
 */
QString formatVersionName(const VersionDisplayParts &parts);

/**
 * @brief 底栏左下角「启动中」那句里的实例名 —— **不受显示名拼法影响**的写法。
 * @param parts 版本的各个组成部分（和 formatVersionName() 同一份来源）。
 * @return 有自定义名就是**它本身**；没有就是 `实际版本-加载器名`（加载器名没有就只留版本号）。
 *
 * 要求：「启动的时候…文本为版本名（没有就版本号-加载器名称，**不受名字
 * 显示设置影响**）启动中...」。
 *
 * 为什么不能拿 formatVersionName() 凑出来（试过，凑不出这个语义）：
 * - 有自定义名时**只要名字**：formatVersionName() 会拼成
 * `我的整合包-1.20.1-Forge`，而这句话要说的是"哪个实例在启动"——名字就是它的名字，
 * 后面那两段是把同一件事再说两遍；
 * - 没有自定义名时**要"版本号-加载器名"**：这一档和 formatVersionName() 恰好重合，
 * 但上面那一档不同，所以它仍是一条独立的规则，单独立在这里。
 *
 * NOTE: 输入照样过 sanitizeComponent()（同一份清洗：去首尾空白、内部换行折成空格、
 * 长度封顶）—— 这一串是要摆进底栏那一格里的，界面上那块地方的宽度是钉死的。
 */
QString launchStatusName(const VersionDisplayParts &parts);

/**
 * @brief 清单里那个 `releaseTime` 变成界面上"精确到日"的那行小字。
 * @param releaseTime 清单里的原始值，例如 `T11:23:02+00:00`。
 * @return ``；认不出来就返回**空串**。
 *
 * 两个刻意的选择：
 * - **按清单里那个日期原样取，不换算成本机时区**。清单给的是 UTC 时刻（`+00:00`），
 * 换算到 UTC+8 会把当天 16:00 之后发布的版本显示成第二天 —— 而所有人口口相传的
 * 是清单上那个日期（"26.3 是 9 月 15 号发的"）。
 * - 认不出来就回空串，让那行小字**整行不画**。宁可少一行，也不要把一串 ISO
 * 原样糊到界面上（那行是给"这是什么时候发的"用的，不是给机器看的）。
 *
 * NOTE: 调用方是版本树（`MainWindow::refreshAvailableVersions()` 往条目上写、
 * `VersionTreeDelegate` 画出来）。纯函数，验证工程可以直接喂真清单那 915 条。
 */
QString releaseDateText(const QString &releaseTime);

/**
 * 「当前实例」该是哪一个。规则：**有实例就一定有当前实例**，只有一个都没有时才返回空串。
 *
 * - 原来的当前实例还在 → 不动（重命名由调用方先把 previousId 换成新 id 再来问）；
 * - 原来的被删了 → 顺延到同一位置上的那一个（删的是末尾就落到新的末尾）；
 * - 原来就没有（刚启动、刚从零个装出第一个）→ 上次启动的 → 上次选中的 → 第一个。
 *
 * 启动失败不写 lastLaunched（LaunchController 只在成功时写），所以失败不会改当前实例。
 * `ids` 是能启动的那一份（正在安装的不算）。纯函数，测试在 VersionNameTests 的 I8。
 */
QString resolveCurrentInstance(const QStringList &ids, const QString &previousId, int previousIndex,
                               const QString &lastLaunched, const QString &lastSelected);
