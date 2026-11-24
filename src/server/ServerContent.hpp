/**
 * @file ServerContent.hpp
 * @brief 服务端「内容」那一页的**判据层**（「内容管理」）。
 *
 * 服务端要装东西只有两个地方：
 *
 * * `mods/` 里的 `.jar` —— 模组和插件（Paper/Spigot 的插件也是往这里扔，只是游戏侧
 * 现在还没把"插件"单独分出来）；
 * * `<世界>/datapacks/` 里的 `.zip` —— 数据包（世界目录 = `server.properties` 的
 * `level-name`）。
 *
 * 这一层**不碰盘**（列目录、改名在 `MainWindow` 那一边），只有名字上的规矩 —— 判据能直接
 * 钉，也不依赖任何一个真服务端目录。
 *
 * 【禁用 = 改后缀，和客户端文件页同一条规矩】
 *
 * `X.jar` → `X.jar.disabled`、`X.zip` → `X.zip.disabled`（内容、大小、mtime 全不动）。
 * 服务端只认 `.jar` / `.zip`，改名之后它就不加载了，用户想恢复时把后缀摘回去就行 ——
 * 比"删了再下"稳，也比"移去别处"好找。认文件时必须先把 `.disabled` 摘掉再判扩展名，
 * 否则禁用过的就"消失"了（同 `InstanceFiles` 那条 `identityPath()` 的坑）。
 *
 * NOTE: 数据包还有一个"压缩包/文件夹"的形态（`datapacks/foo/` 直接是个目录也认）——
 * 这一层只管文件形态，目录形态列出来了也只是"不能禁用"的目录行，改名会动到里面
 * 所有东西，这一版不做。
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace ServerContent {

/// 内容的两类。**下标就是界面上那个下拉框的下标**。
enum class Kind {
    Mods = 0,       ///< 模组（`mods/` 里的 `.jar`）
    Datapacks = 1,  ///< 数据包（世界目录下的 `datapacks/` 里的 `.zip`）
};

/// 有几类（界面下拉框的上限）。
int kindCount();
/// 界面上那一格的标题（「模组」「数据包」）。
QString kindTitle(Kind kind);
/// 这一类落在哪个文件夹名下（`mods` / `datapacks`）。
QString folderName(Kind kind);
/// 这一类认哪个后缀（带点：`.jar` / `.zip`）。
QString fileSuffix(Kind kind);

/// 禁用状态的尾巴。**唯一出处。**
QString disabledSuffix();

/// 名字是不是这一类能加载的（`.jar` / `.zip`，允许带 `.disabled` 尾巴）。
bool isContentFileName(Kind kind, const QString &name);
/// 名字是不是"禁用着"的那一种（只看尾巴，不判扩展名）。
bool isDisabledName(const QString &name);
/// 把 `.disabled` 尾巴摘掉（不是这个尾巴就原样回来）。
QString enabledName(const QString &name);

/// 改名之后叫什么：@p enable 为真摘尾巴、为假补尾巴。**名字不是这一类内容文件时回空串**
/// （调用方拿空串说明"这一行动不了"，不猜）。
QString toggledName(Kind kind, const QString &name, bool enable);

/// 数据包该在哪个文件夹里（世界目录 + `/datapacks`；世界目录为空回空串）。
QString datapacksDir(const QString &worldDir);
/// 模组该在哪个文件夹里（服务端目录 + `/mods`；服务端目录为空回空串）。
QString modsDir(const QString &serverDir);

/// 列出来的一行（**顺序照传进来的名字**）。
struct Entry {
    QString name;      ///< 文件现在的名字（带不带 `.disabled` 都照原样）
    qint64 bytes = 0;  ///< 大小（取不到就是 0）
    bool disabled = false;
};

/**
 * @brief 把一串文件名筛成这一类的行。
 *
 * 认名靠 @p kind，**别的文件（配置、说明、`.jar.bak` 那一类）一行都不出**；禁用着的也
 * 出（`disabled = true`），因为它们还在那个文件夹里、用户要能看见它们。
 */
QList<Entry> entriesFromNames(const QStringList &names, Kind kind);

}  // namespace ServerContent
