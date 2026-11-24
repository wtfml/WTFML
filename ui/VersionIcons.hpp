/**
 * @file VersionIcons.hpp
 * @brief 版本树和加载器那几行前面的小图标：草方块、四个加载器的标志、最右边那个 >/V。
 * @note 本文件**不 include 任何 src/ 下的头、不碰网络**；除 `fileMark()` 外也**不读磁盘**
 * （`fileMark()` 是唯一的例外 —— 它按调用方给的路径读那张图，因此那一支依赖盘）。
 * （四个加载器的标志是从**编进 exe 的资源**里读的 —— 那也是
 * `:/assets/…`，不是磁盘上的文件。）
 *
 * 【哪些是画的、哪些是真图】
 *
 * 四个加载器（Fabric / Forge / NeoForge / OptiFine）读的是各家自己那张真图：四张
 * 64×64 的成品 PNG 放在 `assets/loader/` 下、由 `assets/icons.qrc` 编进 exe，
 * 来路见那份 README。界面这一侧只剩"读一张图、缩到要的边长"。
 */

#pragma once

#include <QColor>
#include <QRectF>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

class QPainter;

/// 版本树 / 加载器那几行前面的小图标。
namespace VersionIcons {

/// 原版版本号前面那个草方块（MC 那块贴图的侧面：上面一层绿、底下一层土，草的下沿是锯齿状）。
/// @param size 边长（像素）。画法是 16×16 的像素画再按整数放大，所以大小取 16 的倍数最清楚。
QPixmap grassBlock(int size);

/// 覆雪草块（上面那层是白的，下面还是土）—— 快照版那一栏版本号前面的图标。
/// 和 grassBlock() 同一份底图，只有"上面那层是哪三个颜色"不同（下沿那张锯齿表共用）。
QPixmap snowyGrassBlock(int size);

/// 土块（一点草都没有，整块都是土）—— 远古版那一栏版本号前面的图标。
QPixmap dirtBlock(int size);

/// 幸运方块（金底 + 深一档的边 + 中间一个奶油色的问号）—— 愚人节版那一栏的图标。
/// 那个问号是**写死的 5×7 位图**，不是画的字：字号一缩放就不是这个形状了，
/// 而且这一格要能对着像素断言。
QPixmap luckyBlock(int size);

/// 版本树上那四栏各自的小图标（快照版用覆雪草块、远古版用土块、愚人节版用幸运方块、
/// 原版不变）。
/// @param channelIndex 走的是 `VersionCategory::channelAt()` 那个顺序
/// （0 正式版 / 1 快照版 / 2 远古版 / 3 愚人节版）。**越界给草方块** ——
/// 和 loaderMark() 一个路数：认不出来的时候画"原版"，那条路装出来的就是原版。
/// @param size 边长（像素）。四张和 grassBlock() 一样都是 16×16 的像素画再放大。
///
/// NOTE: 这里收的是**下标**、不是 `VersionCategory::Channel`；本文件不许 include
/// `src/` 下的头。分类 → 下标那一步由 `MainWindow.cpp` 调
/// `VersionCategory::channelIndex()` 完成，顺序的唯一出处因此仍然是那个枚举。
QPixmap versionChannelMark(int channelIndex, int size);

/// 加载器标志 —— 各家自己那张图（见文件头【哪些是画的、哪些是真图】）。
/// @param index 走的是 MainWindowUI::modLoaderOptionNames() 那张表的顺序
/// （0 Fabric / 1 Forge / 2 NeoForge / 3 OptiFine / 4 Quilt），第 5 个（「不装（纯原版）」）
/// 和越界的下标都给草方块 —— 那条路装的就是原版。
/// @param size 边长（像素）。出图是 64×64 的成品等比缩到这么大（放大走最近邻、缩小走
/// 平滑，见 .cpp）。**资源读不出来时也给草方块** —— 和"认不出下标"一条规矩。
QPixmap loaderMark(int index, int size);

// ---------- 实例图标----------
//
// 「给一个实例挑图标」这件事有两半：**八个内置候选**（就是
// 上面那两排字形）和**用户自己挑的一张图**（那一半在 MainWindow 里，落成实例目录下的
// icon.png）。
//
// NOTE: 这几个键**要写进 wtfml.meta**（见 VersionManager::iconField()），所以它们是稳定
// 身份 —— 界面上的名字（"草方块""Fabric"）会随文案改，键不会。用户挑过的实例
// 在下次改文案时必须还认得出自己挑的是哪一张。
// NOTE: 后半截（Fabric / Forge / NeoForge / OptiFine）读的是 assets/loader/ 下的真图，
// **变的只是 iconCandidateMark() 底下那一层**（键和候选表一个字都没动），盘上已经写好的
// wtfml.meta 因此一个字节都不用改。前四个方块还是画的（见文件头）。

/// 八个内置候选的键，**顺序就是界面上从左到右、从上到下的顺序**。
/// 前四个是版本分类那四张（正式版草方块 / 快照版覆雪草块 / 远古版土块 / 愚人节版幸运
/// 方块），后四个是四个加载器的标志 —— 和实例树上那两处用的是同一批图。
QStringList iconCandidateKeys();

/// 按下标取候选图。越界 = 草方块（和 loaderMark() 越界那条一个规矩：认不出来给原版）。
QPixmap iconCandidateMark(int index, int size);

/// 按**键**取候选图。认不出来的键（包括空串）给一张**空** pixmap —— 调用方要把"认不出来"
/// 和"认出来是草方块"分开（前者要退回默认那张，后者就是草方块本身）。
QPixmap iconMarkForKey(const QString &key, int size);

/// 把 wtfml.meta 里那个 icon 值里的内置键剥出来（`builtin:<键>` → `<键>`）。
/// 不是内置那一档（空串 / `local` / 别的什么）给空串。
///
/// NOTE: 前缀这一句话只有这一处 —— 拼它的地方（实例设置对话框）和认它的地方（实例树）
/// 各写一遍字面量的话，写错的形态是**静默失效**：树上的图标安安静静地不换。
QString iconKeyFromValue(const QString &value);

/// 把候选键拼成写进 wtfml.meta 的那个值（`builtin:<键>`）。
QString iconValueForKey(const QString &key);

/// 把盘上的一张图读成图标那一格要的样子：**保持长宽比**缩到 @p size 见方，再补成正方形
/// （四周透明）。读不动（文件不在 / 不是图 / 半截文件）给一张空图，调用方自己决定退回什么。
///
/// NOTE: 这一层**按 (路径, 修改时间, 边长) 缓存**，是必需的而不是优化 —— 实例树每一行都
/// 要问一次、每次刷新整棵树重建一遍，用户挑的那张图会被解成百上千遍。带上修改时间
/// 是为了"同一个文件名换了内容"这一档：只按路径键住的话，用户第二次挑一张同名的图
/// 会一直看到第一次那张。
QPixmap fileMark(const QString &path, int size);

/// 最右边那个箭头：收起时朝右（>）、展开时朝下（V）。
/// @param expanded 这一条现在是不是展开的。
/// @param color 线的颜色（树里按"分类 / 版本号"两种深浅给）。
QPixmap chevron(bool expanded, int size, const QColor &color);

/// 朝上那个箭头（^）：底栏那两颗"点开向上展开"的按钮右边缀的就是它。
/// 和 chevron() 同一份笔画（同一个笔宽比例、同一套缓存），只是换了朝向。
/// @param size 边长（像素）。
/// @param color 线的颜色。
QPixmap chevronUp(int size, const QColor &color);

/// 一支**有杆的上箭头**（↑）：一条竖直的杆 + 顶上一个实心三角箭头。实例页每一条实例行
/// 行尾那颗「置顶」按钮画的就是它。
///
/// 为什么**不**复用现成的那两支箭头（这是挑形状时唯一要想清楚的事）：
/// * 不是 chevronUp() —— 那个只有两笔、是个 `^`，没有杆。16 px 的一行里它和行尾
/// 那颗展开箭头（chevron 的 `>`）长得太像，"置顶"会读成"展开/收起"；
/// * 也不是 triangleUp() —— 那个是实心的、一点杆都没有，读起来是滚动条上那种"翻页"
/// 三角，不是"把这条挪到最上面"这层意思。带杆的 ↑ 是"置顶/回到最上面"的通用长相
/// （浏览器"回到顶部"那颗就是它），一行里三个字形一眼分得开。
///
/// NOTE: 四边都留了 2 px 以上的余量（**最外一圈一个有色像素都没有**）—— 这个仓库有一条
/// 既有规矩：字形不许碰到画布边缘，抗锯齿那一圈会被裁掉、看着像缺了个角
/// （refresh() 那段把这条规矩的账算得最细）。杆因此也**没画满整格**，上下都收着。
/// @param size 边长（像素）。
/// @param color 填充的颜色（三角和杆同色）。
QPixmap arrowUp(int size, const QColor &color);

/// 同一格的**反面**那颗：下箭头（↓）。
///
/// 它和 arrowUp() 是**同一个字形上下翻过来**（同一套比例：箭头底边 0.34、杆半宽 0.10、
/// 杆长到 0.85，只是上下对调），所以两颗在那同一格里轮流出现时，看起来是"同一颗箭头
/// 掉了个头"，而不是"换了另一族的图案"—— 这正是"同一个动作的反面"该有的样子。
///
/// NOTE: 别拿 `triangleDown()` 顶替 —— 那是个不带杆的实心三角（滚动条上那种"翻页"），
/// 和 ↑ 摆在一起会读成"两颗不一样的按钮"（理由和 arrowUp() 那段逐字相同）。
/// @param size 边长（像素）。
/// @param color 填充的颜色（三角和杆同色）。
QPixmap arrowDown(int size, const QColor &color);

/// 组行行尾那两颗「上移 / 下移」箭头：**实心三角**（▲ / ▼）。
///
/// NOTE: 特意**不**用 chevron/chevronUp —— 组行行尾本来就有一个 chevron（展开/收起），
/// 一行里三个同一支笔画的 V 分不出谁是谁。实心三角是另一族的形状（滚动条上那两颗
/// 就是它），一眼分得开。纯代码画的、带缓存、颜色由调用方给（和别的字形一样）。
/// @param size 边长（像素）。
/// @param color 填充的颜色。
QPixmap triangleUp(int size, const QColor &color);
QPixmap triangleDown(int size, const QColor &color);

/// 选中那条加载器右边那个**小叉**（在箭头左边）：点它 = 取消装这个加载器、退回纯原版。
/// 两条对角线画的，
/// 和 chevron 同一个路数（全是代码画的，仓库里没有图片文件）。
/// @param size 边长（像素）。
/// @param color 线的颜色（比正文浅一档 —— 它是"次要操作"，别和版本号抢眼睛）。
QPixmap cross(int size, const QColor &color);

/// 顶上那条自己画的标题栏里，**最小化**那一格里的那条横杠（—）。
/// 画法和 cross 同一支笔、同一套边距（左右各 22%、笔宽 size/7）—— 这两颗按钮挨着摆，
/// 笔画粗细和留白必须一样，不然一颗看着比另一颗大一号。
/// @param size 边长（像素）。
/// @param color 线的颜色。
QPixmap windowMinimize(int size, const QColor &color);

/// 那一圈**刷新箭头**（↻）：下载页左栏每个分类条目右边挂一个，点它 = 这一类重新问一遍。
/// 和 cross / chevron 同一个路数 ——
/// 纯代码画的、带缓存、颜色由调用方给。
/// @param size 边长（像素）。
/// @param color 线的颜色（和 cross 一样用次要那一档：它是行里的一个动作，不是正文）。
QPixmap refresh(int size, const QColor &color);
/// 一只**眼睛**（杏仁形眼眶 + 瞳孔）：密钥框右侧「显示 / 遮挡」。@p crossed 为真时多一道斜杠（= 现在看得见，点了遮回去）。
/// 和 refresh 同一支 24 格线稿笔，纯代码画、带缓存、颜色由调用方给（emoji 👁 在不少系统字体里缺字形，画成豆腐块）。
QPixmap eye(int size, const QColor &color, bool crossed);
/// Theme-aware action icons resolve their colour when painted, not when the widget is created.
/// `QuietDelete`：行尾那颗低调的垃圾桶 —— 平时次要墨色，悬停（QIcon::Active）才换危险色。
enum class Action { Refresh, Settings, Edit, Folder, Details, Delete, QuietDelete };
QIcon actionIcon(Action action);

/// 那个**电源**符号（⏻）：左面板每一行右边第一颗按钮，「关闭这个实例」。
/// 画的是一圈留了缺口的环 + 从缺口穿上去的一竖（就是机箱上那个开关的长相）。
/// 和别的字形同一个路数：纯代码画的、带缓存、颜色由调用方给。
/// @param size 边长（像素）。
/// @param color 线的颜色。
QPixmap power(int size, const QColor &color);

/// 那个**齿轮**（⚙）：左面板每一行右边第二颗按钮，「进这个实例的设置」。
/// 六齿描边 + 中心圆孔，与刷新图标共用轻量线稿风格。
/// @param size 边长（像素）。
/// @param color 填充的颜色。
QPixmap gear(int size, const QColor &color);

/// 一支**斜着的笔**：实例页左栏每个游戏文件夹行尾那颗齿轮弹出来的菜单里，「重命名」那一行。
/// 斜 45°（笔尖朝左下），杆上留一道透明的箍把"笔头 / 笔身"分开。
/// @param size 边长（像素）。
/// @param color 笔的颜色。
QPixmap pen(int size, const QColor &color);

/// 一个**文件夹**：同一个菜单里「打开」那一行。
/// 标签页 + 开口轮廓，与其余动作图标采用相同线宽和圆角端点。
/// @param size 边长（像素）。
/// @param color 文件夹的颜色。
QPixmap folderMark(int size, const QColor &color);

/// 一个**垃圾桶**：同一个菜单里「删除」那一行（红色那一行）。
/// 细线桶盖、提手与桶身，内侧两条线表示桶身，不是厚重实心块。
/// @param size 边长（像素）。
/// @param color 垃圾桶的颜色。
QPixmap trash(int size, const QColor &color);

/// 一个**信息圈**（ⓘ）：实例设置「文件管理」那四页里，行尾那颗「详情」按钮
/// （行尾**三格里的第一格**，即 `[详情][开关][删除]`）。
///
/// 画的是一个描边圆与「i」，和齿轮/垃圾桶采用同一套轻量线稿，
/// 尺寸也取同一档（`MainWindowUI::instanceFileIconSize()`），一列下来行尾三格左右对得齐。
/// @param size 边长（像素）。
/// @param color 线的颜色（圈和那个 i 都用它）。
QPixmap infoMark(int size, const QColor &color);

/// 一块**拼图**（右边凸一个圆钮、上边挖掉一个同样大的圆）：实例设置「文件管理」那几页里
/// 某一条**没有图标**时的占位。
///
/// 真图标是资源作者
/// 传的那一张（反查哈希 → 走 ResourceStore 的图标缓存拿，见 MainWindow 里那条路），
/// 而它**认不出来是常态**：自己改过的 jar、作者没传过图标的、断网的那一次都会走到这儿。
/// 那时候画这个 —— 不是空白：一列下来有的有图有的没图，没图那一格若是空的，看着像
/// "这一行漏画了"，而不是"这一条查不到图标"。
///
/// NOTE: 拼图是启动器里"模组 / 资源包"这一族的通用长相（它和"文件"不一样：一个普通
/// 文档的字形在这一列里只会让人以为这是份说明）。六个页共用这一个字形，
/// **不为每一类各画一个** —— 那是六个字形换来的"更精确"，而这几页的差别在
/// 标题和路径上已经写着了。
/// @param size 边长（像素）。
/// @param color 拼图的颜色（用次要那一档：这一列是配角，别和名字抢眼睛）。
QPixmap puzzleMark(int size, const QColor &color);

/// 一颗**实心圆点**：轨底"实例运行状态"那一格的状态点（颜色由调用方按状态给）。
/// 画图而不是摆一个 ● 字，像素确定、能对着圆心断言颜色。
QPixmap runStateMark(int size, const QColor &color);

/// 设计稿实例封面那颗等距方块的配色数（shell.js 的 BLOCKS：草、钻石、红石、紫晶、沙、樱花、石、铜）。
inline constexpr int kIsoBlockKinds = 8;
/// 第 @p kind 种配色的等距方块（设计稿 cube()，透明底）。kind 取模。
QPixmap isoBlock(int kind, int size);
/// 实例没挑图标时的默认封面：按实例名**稳定**挑一种配色（跨进程不变，不用 qHash 的随机种子）。
int isoBlockKindFor(const QString &name);

/// 设计稿那套线性图标（24 网格、圆头圆角）：home / manage / download / settings / agent /
/// toolbox / fold / unfold / min / max / restore / close / play / swap / gear / chevR / chevD /
/// chevU / plus / user / list。路径照抄设计稿 shell.js 的 SVG 数据；名字不认识时什么都不画。
void paintLine(QPainter &painter, const QString &name, const QRectF &box, const QColor &color, qreal stroke = 1.6);
/// 同上，画成一张 `size` 见方的透明图（按钮 setIcon 用）。
QPixmap line(const QString &name, int size, const QColor &color, qreal stroke = 1.6);

}  // namespace VersionIcons
