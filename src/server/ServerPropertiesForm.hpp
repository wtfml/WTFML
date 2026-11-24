/**
 * @file ServerPropertiesForm.hpp
 * @brief 服务端工作区里「**这十几个旋钮**」那一层：
 * 常用字段是哪几个、每一项现在是什么值、用户敲进来的值收不收、把改过的那几项合回文件。
 *
 * 【它和 `ServerModel::Properties` 的分工】
 *
 * `ServerModel::Properties` 管的是**文件本身**（保序的行、注释、行尾、`toString()` 一个字节
 * 都不许乱动），它**不认识任何一个键** —— 端口在它眼里和 `motd` 一样，都是 `key=value`。
 *
 * 这一层回答的是另外两件事：
 *
 * * 「**常用的那几项**是哪几个、叫什么名字、值域多少、没写时游戏自己用什么」（`commonFields()`）
 * —— 「常用字段表单」就是这个表的落点；
 * * 「用户敲进那一格的东西**能不能收**」（`accepts()`）—— 表单比文本编辑器多出来的那一点
 * 价值全在这儿：文本编辑器让用户把 `server-port` 敲成 `abc` 也能存下去，而服务端下次
 * 启动会**直接崩**在解析上（那是个 java 的 `NumberFormatException`，报错里一个字都不提
 * `server.properties`）。
 *
 * 所以这一层照旧是**纯函数、不碰盘、不看时钟**（和 `ServerModel` 同一档），验证工程喂几个
 * 字符串进来就能把整张表钉死。
 *
 * 【为什么值是字符串，不是 int / bool】
 *
 * 因为**文件里存的就是字符串**（`Properties` 是保序的文本行），而这一页要能**原地往返**：
 * 用户没动的那一项，写回去时必须一个字符都不差（`view-distance = 10` 和 `view-distance=10`
 * 是两个不同的字节串，虽然值一样）。所以这一层里从头到尾只有 `QString`，"这一项是个数字"
 * 这件事只体现在 `Field::kind` 上（决定界面上摆哪种控件、`accepts()` 怎么判）。
 *
 * 【几种值域是从哪来的】
 *
 * 抄的是 Minecraft 自己 `DedicatedServerProperties` 里那几个 clamp / 枚举，**不是我们编的**：
 * 视距和模拟距离 3~32、`max-players` 20 起步但游戏自己也收 1000、`player-idle-timeout` 是
 * 分钟。范围写窄了会让本来能跑的服务端起不来（那比"没帮你拦住"糟得多），所以两头都是游戏
 * 自己的界，不是"我们觉得合适"的界。
 *
 * NOTE: **`commonFields()` 是这一页的规格唯一出处**：界面上摆哪十几行、`ServerCheck` 拿哪
 * 几项去体检、页面上那几行字怎么排，全从这个表来。加一项只改这一处（别在界面里另写
 * 一份"这一行叫什么"）。
 */

#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include "server/ServerModel.hpp"

namespace ServerPropertiesForm {

/// 这一项在界面上摆成什么控件（也是 `accepts()` 怎么判那一路的分岔）。
enum class Kind {
    Text,    ///< 随便填（`motd`、`level-seed`）
    Number,  ///< 整数，`min`~`max`
    Switch,  ///< `true` / `false` 两档（拨动开关）
    Choice,  ///< `choices` 里那几个之一（下拉框）
};

/// 下拉框里的一个候选值：**存进文件的那个词** + 摆给用户看的那句人话。
struct Choice {
    QString token;  ///< 写进 `server.properties` 的那个词（`peaceful`）
    QString name;   ///< 界面上显示的那个词（「和平」）
};

/// 常用字段里的一项。**全是值**（这一层没有 QObject、不碰盘）。
struct Field {
    QString key;    ///< `server.properties` 里的键（身份，界面认人用它）
    QString label;  ///< 行首那个中文名
    QString hint;   ///< 那一行右边/下面那行小字（"为什么默认是这个"）
    Kind kind = Kind::Text;
    /// **文件里没写这一项时，游戏自己用的那个值**。
    ///
    /// 这一格是"表单上显示什么"的唯一出处：盘上那份 `server.properties` 只写游戏**自己
    /// 生成的**那些键，而一个新装的、还没跑过的服务端目录里**根本没有这个文件** —— 那时
    /// 表单上填的就是缺省值（用户按下保存才落盘）。别用空串代替它：空串在 `online-mode`
    /// 那一格上会被写进去，服务端读到空值会当 false —— 一个把正版验证悄悄关掉的"默认值"。
    QString fallback;
    int min = 0;  ///< `Number` 那档的下界（游戏自己的界）
    int max = 0;  ///< `Number` 那档的上界
    /// `Choice` 那档的候选。★ **`fallback` 必须是这中间的一个**（不一定是第一个 ——
    /// `difficulty` 的缺省是 `easy`，而它在候选里排第二：游戏自己的顺序是
    /// 和平 / 简单 / 普通 / 困难，缺省却落在"简单"上）。
    QList<Choice> choices;
    /// 这一项是个**目录名**（`level-name`）：值里不许有路径分隔符和 Windows 不吃的那几个
    /// 字符 —— 它会变成 `<服务端目录>/<值>`，值里带一个 `..` 就等于把世界写到服务端外面去。
    bool dirName = false;
    /// 这一项要在界面**下面挂一条"游戏里长什么样"的预览**（目前只有 `motd`）。
    ///
    /// Why 放在这张表里而不是界面里按 key 写死一句 `if (key == "motd")`：那一句就多出了
    /// 第二个"哪一项是 MOTD"的出处。表是唯一出处 —— 哪天再加一项要预览的（`level-name`
    /// 那种），也只改这一处。
    bool preview = false;
    /// 这一项旁边要摆一颗「选一份本地 zip、算出来填进来」的按钮（目前只有
    /// `resource-pack-sha1`）。选文件、算 SHA-1 在 `MainWindow` 那一边（`ZipUtil` +
    /// `FileHash` 都是 `src/` 的东西，界面这一层不许碰）。
    bool zipPick = false;
};

/// 常用字段表。**顺序就是界面上从上到下的顺序**（基本 → 规则 → 性能，见 .cpp 里那段）。
const QList<Field> &commonFields();

/// 表里那一项（认不出来回 nullptr）。界面据 key 反查那一行的规格时用它。
const Field *fieldFor(const QString &key);

/// 表里全部键（`ServerCheck` 拿它筛"文件里那几项是我们认识的吗"）。
QStringList knownKeys();

// ---------------------------------------------------------------------------
// 读：盘上那一项现在是什么值
// ---------------------------------------------------------------------------

/// 盘上那一项现在该显示成什么。**永远回一个能显示的值**：文件里没有这一项就回 `fallback`
/// （理由见 `Field::fallback` 那段）。值两侧的空白照旧裁掉（`parseProperties()` 已经裁过）。
QString valueOf(const ServerModel::Properties &properties, const Field &field);

/// 盘上那一项**是不是写着**（`false` = 上面那个值是缺省值，不是用户在文件里写的）。
/// 界面上拿它说"这一项还没落盘"（新装的、还没跑过的服务端就是这样）。
bool isInFile(const ServerModel::Properties &properties, const Field &field);

// ---------------------------------------------------------------------------
// 判：用户敲进来的值收不收
// ---------------------------------------------------------------------------

/**
 * @brief 用户敲进那一格的值能不能收。
 *
 * @param raw 界面上那一格现在的内容（字符串形态）。
 * @param normalized 收的时候回**真要写进文件的那个字符串**（数字规范化成十进制、开关一律
 * `true`/`false`、下拉框回表里拼的那个词、前后空白裁掉）。
 * @param problem 不收时那句话（人话，直接摆给用户）。
 * @return 收 = true。
 *
 * 数字这一档**不做"帮你改一改"**：`" 10 "` 收（裁空白是安全的），但 `"1e3"`、
 * `"10.0"`、`"0x10"` 一律**不收**并说清楚要整数 —— `toInt()` 对后面那三种会静默回 0，
 * 而 0 在 `view-distance` 那一格上是个合法的烂值（服务端会把它 clamp 到 3，用户却以为
 * 自己填的生效了）。宁可挡在这儿。
 */
bool accepts(const Field &field, const QString &raw, QString *normalized, QString *problem);

// ---------------------------------------------------------------------------
// 写：把改过的那几项合回一份 Properties
// ---------------------------------------------------------------------------

/// 一项要写的改动（`key` 必须是表里的；值**必须已经过 `accepts()`**）。
struct Edit {
    QString key;
    QString value;
};

/// 某几项现在的值（界面上的表单读回来的那一份）。
/// NOTE: 用 `QList<QPair>` 而不是 `QHash`：这一层**从来不按散列顺序办事**，而"改了哪几项"
/// 这个次序在日志和判据里都要看得出来（同 `ServerModel::Properties` 保序那条理由）。
using Values = QList<QPair<QString, QString>>;

/// 从一份 Properties 里把那几项读出来（没有的项回 `fallback`）。**顺序跟着 `fields` 走**。
Values valuesOf(const ServerModel::Properties &properties, const QList<Field> &fields);

/**
 * @brief 把 @p edits 合进 @p properties（就地改）。
 *
 * @return **真的改了几项**（值原来就是那样、或者那个键在文件里已经有一模一样的值 ⇒ 不算改）。
 *
 * 只碰 `edits` 里点名的键：别的行（注释、空行、用户手写的别的项、键的顺序、行尾）一个
 * 字节都不动 —— 靠的是 `Properties::setValue()` 那条"改第一个出现的同名键、没有就追加"
 * 的路。所以这一层写出来的文件是"用户那份 + 他改的那几项"，不是"我们重新生成的一份"。
 */
int applyEdits(ServerModel::Properties &properties, const Values &edits);

// ---------------------------------------------------------------------------
// 体检：文件里现在有哪几项不对劲（`ServerCheck` 拿它当一条原料）
// ---------------------------------------------------------------------------

/// 文件里某一项的值不合规矩。
struct ValueProblem {
    QString key;
    QString label;    ///< 那一项的中文名（`Field::label`，界面照抄）
    QString value;    ///< 文件里现在写着的那个值（原样）
    QString problem;  ///< 为什么不收（`accepts()` 的那句话）
};

/**
 * @brief 把文件里**表认识的那几项**挨个过一遍 `accepts()`。
 * @return 不合规矩的那些（空 = 都对）。
 *
 * NOTE: 只查**文件里真写着的**项 —— 缺省值来自这张表、它按定义就是合法的，报出来只会
 * 制造噪音（那也正是"新装的、还没跑过的服务端"这一页打开时的样子）。
 */
QList<ValueProblem> problemsIn(const ServerModel::Properties &properties);

/// 文件里写了**两次以上**的键（值还不同的那些才报 —— 值一样的是无意义的重复，服务端自己
/// 也只看最后一个）。Java 的 Properties 语义是"后面盖前面"，用户很可能没意识到自己手写的那
/// 一行被下面那行盖掉了。
QStringList duplicateKeys(const ServerModel::Properties &properties);

/// 文件里没有专用控件的键（`commonFields()` 以外）。用于「其他属性」。
/// 这不是错误：整合包和插件会往里加自己的键，那些键这一页一个都不动（原样留着）。
QStringList unknownKeys(const ServerModel::Properties &properties);

/// `level-name` 指的那个目录在服务端目录底下的相对名（值合法时就是它，不合法回空串）。
/// 备份 / 体检都要拿它算"世界住在哪儿"，所以只留这一处算法。
QString worldDirName(const ServerModel::Properties &properties);

}  // namespace ServerPropertiesForm
