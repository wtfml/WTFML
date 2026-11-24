/**
 * @file FolderManager.hpp
 * @brief 「游戏文件夹」列表：启动器用哪一份 .minecraft、以及用户还添加过哪些。
 * @note 列表的来源是 LauncherSettings（落盘在 QSettings 的 Launcher/Folders），
 * 本类负责列表的**语义**：自带那份永远在第一项且移不掉、当前目录不在盘上时
 * 退回自带那份、路径归一化与去重。
 *
 * 这个类和 AppPaths 的分工：本类只说"当前应该用哪个根目录"，**不动 AppPaths**（那是
 * 全局状态，改它是有副作用的一步）。界面（MainWindow）拿到 currentFolderChanged()
 * 之后调 AppPaths::setMinecraftDir() 再重灌界面 —— 副作用留在控制层，本类可以单独测。
 */
#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class LauncherSettings;

/// 递归删掉 @p path 这一棵，但**一个链接都不追进去**（链接只把链接本身摘掉，那一头的东西
/// 一个字节都不动）。@return 整棵都删干净了没有；删到一半失败时返回 false 但仍然尽力删完。
///
/// 【为什么不是 `QDir::removeRecursively()`】
/// 对一个**本身就是 junction** 的目录调用它，junction 指向的那个
/// 真目录里的文件**没了**；目录**里面**含一个 junction 时同样跟着进去把目标删了。
/// 于是"删掉这一份游戏文件夹 / 这一个实例"会牵连到外面那块盘上的东西 —— 把 `saves` 挂到
/// 网盘同步目录、把 `mods` 挂到另一块盘都是常见做法。完整结论写在 .cpp 里那个函数
/// 上面（判据为什么必须带上 `isJunction()` 也在那里）。
///
/// NOTE: 它是自由函数、不属于 FolderManager 这个类：两个调用者（删游戏文件夹、删实例）
/// 都只想要"把一棵目录树删干净"，和文件夹列表没关系。所以声明摆在这里只是给它
/// 一个家 —— 实现和那段最细的注释在 FolderManager.cpp。
bool removeTreeWithoutFollowingLinks(const QString &path);

/// 把 @p path **移进 Windows 回收站**（文件、目录、整棵树都行），移不进去就**失败返回
/// false**，绝不"删不动就干脆永久删掉"。@p error 失败时写一句可以直接弹给用户的话；可以传空。
///
/// 【为什么不是直接删】
/// 删实例、删世界、删文件页里的东西、删服务端、删预设 —— 这些点下去之后盘上少的东西都是
/// 用户自己攒出来的。永久删除一旦点错就找不回来；回收站让"手滑"有退路，而且不需要启动器
/// 自己备份一份（用户的盘不归我们占）。
///
/// 【约定】
/// · 路径不存在 ⇒ 返回 true（和 removeTreeWithoutFollowingLinks 一样：调用方问的是"它还
///   在不在"）；
/// · **一个链接都不追**：要移的是链接本身时只摘掉那个壳子（回收链接没有任何意义，而跟着
///   它走会动到那一头）；一棵树里嵌着的链接在移之前先按"只摘壳子"剥掉，剩下的才进回收站；
/// · 网络位置（UNC / 映射盘）没有回收站 ⇒ 直接拒绝并说清楚，不当作本地删除处理；
/// · 卷根目录（`D:\` 这种）拒绝 —— 那是整块盘，不是"一样东西"；
/// · 动手之前先用 `CreateFileW(path, DELETE, …)` 预检一次（不落地任何改动）：权限不许删
///   就明说「权限不允许」，被别的程序占着就明说「被占着」，而不是等回收站那一趟回来之后
///   用一句笼统的失败让用户自己猜。预检不过 ⇒ **一个字节都不动**（让 `IFileOperation`
///   自己去撞的话，它会先把子树里删得动的搬进回收站、最后才在那一层失败，留下一半在
///   里面一半在外面的现场）。子树里某一件被占着不归它管，照旧由那一趟报。
/// · 树里（含这一项自己）有只读的东西：shell 那一趟会回"源访问被拒绝"（0x80270021）。
///   这时先摘掉只读位再试一次 —— 资源管理器自己也是这么做的（它多一句"确实要删除吗"）。
///   一次就成功的那趟**不动任何属性**，进回收站的那一份还是只读的。
///
/// NOTE: 实现用 `IFileOperation` + `FOFX_RECYCLEONDELETE`（不是旧的 `SHFileOperation`）：
/// 前者明确"必须进回收站"，回收站不可用时返回错误；后者在放不进去时会**悄悄**改成永久删除。
/// 工人在 QtConcurrent 线程里调用时函数自己补一次 COM 初始化。
///
/// NOTE: 它和 removeTreeWithoutFollowingLinks 是**两个**不同用途的入口：前者给"用户点删除"
/// 用，后者留给启动器自己的暂存 / 缓存 / 事务收尾（那些东西没有回收的价值，也不该占用户的
/// 回收站配额）。别拿一个替换另一个。
bool moveToRecycleBin(const QString &path, QString *error = nullptr);

/// 把 @p from 这一棵**原样**拷到 @p to（@p to 必须还不存在），同样**一个链接都不追进去**
/// —— 碰到链接就直接算失败，而不是顺着它把那一头的东西也拷一份出来。
///
/// 【"目标必须不存在"是硬条件，不是省事】
/// 这一族函数只服务于"造一份**新的**出来"：复制实例、导入世界都是先算好一个空目录路径，
/// 再往里灌。允许往已存在的目录里合并的话，`QFile::copy()` 遇到同名文件会**直接失败**
/// （Qt 不肯覆盖），于是"合并"这个语义在这里根本实现不了 —— 与其留个半吊子的合并，不如
/// 一开始就把重名挡在外面，让调用方自己去问用户换个名字。
///
/// 【为什么不用 `QDir::copy()` / `QFile::copy()` 递归自己写循环】
/// `QDir::copy()` 没有（Qt 6 的 QDir 没有整树拷贝），`QFile::copy()` 只认文件。
/// 所以这里只能自己递归；要注意的两个坑：
/// · 目录得**先 mkpath 再进去**（先拷文件再建目录会失败）；
/// · 符号链接 / junction 一律拒掉（理由同上，和删的那一个正好相反：删是"只摘链接"，
/// 拷是"干脆不拷"，两边都不能顺着链接走）。
///
/// @return 整棵都拷过去了没有。任何一步失败就**立刻收手**返回 false，已经拷出来的那半棵
/// **留在盘上**（由调用方决定删不删 —— 复制实例和导入世界都选择删掉它）。
/// `problemOut` 可选：保留首个失败文件/阶段和系统原因；调用方应区分复制失败与后续内容校验失败。
///
/// NOTE: 和 removeTreeWithoutFollowingLinks 一样，它声明在头文件里、实现留在 .cpp（那边
/// 有更细的注释）。两个调用者（VersionManager::copyInstance、WorldFiles 那条路）
/// 都只想要"把一棵目录树拷成新的一份"，和文件夹列表没关系。
bool copyTreeWithoutFollowingLinks(const QString &from, const QString &to,
                                   const std::function<bool()> &cancelled = {},
                                   QString *problemOut = nullptr);

class FolderManager : public QObject {
    Q_OBJECT

public:
    /**
     * @brief 构建文件夹管理器，并从盘上把列表读进来。
     * @param settings 读写设置的对象。**不接管所有权**，必须比本对象活得久。
     * @param parent Parent QObject.
     *
     * 构造完立刻就可以问 folders() / currentFolder()，界面按它摆第一帧。
     */
    explicit FolderManager(LauncherSettings *settings, QObject *parent = nullptr);

    /**
     * @brief 启动器自带的那一份根目录（<exe 目录>/.minecraft）。
     * @return 归一化之后的绝对路径。
     *
     * 它永远在列表第一项、也移不掉：别的目录被挪走、U 盘被拔掉的时候，启动器总得有
     * 一个能落的根目录（AppPaths 的默认值也正是它）。
     */
    static QString defaultFolder();

    /**
     * @brief 判断某个路径是不是自带的那一份。
     * @param path 待判定的路径（内部会先归一化，所以写法不同也能认出来）。
     */
    static bool isDefaultFolder(const QString &path);

    /**
     * @brief 官方启动器（Mojang 那个）默认用的那一份 .minecraft。
     * @return `%APPDATA%\.minecraft`；APPDATA 没定义时返回空串。
     *
     * NOTE: 只看**这一个**约定位置，不看盘上还有没有别的（更不扫盘）。存在与否由
     * `reuseSourceFolders()` 判 —— 这个函数只负责把路径说出来。
     */
    static QString officialLauncherFolder();

    /**
     * @brief 下载时可以"从里面借现成文件"的那些根目录。
     * @return 存在的那些目录：`folders()` 里在盘上的 + `officialLauncherFolder()`（存在时）。
     * 当前在用的那一份也在里面 —— `DownloadManager` 自己会跳过它。
     *
     * 要求：「文件夹的使用仅限用户添加了的和官方启动器默认文件夹（如果存在），
     * 不要全盘扫描」。所以这个函数就是"允许的来源"的**唯一出处**：不在这里的目录，
     * 下载逻辑永远不会去看。
     *
     * NOTE: 结果会去重（不区分大小写）。官方那一份很可能**就是**用户添加过的那一份，
     * 同一个目录出现两次只是让每一次候选检查白做一遍。
     */
    QStringList reuseSourceFolders() const;

    /**
     * @brief 全部可选根目录。
     * @return 自带那份在最前，后面是用户添加的（按添加顺序）。
     */
    QStringList folders() const;

    /**
     * @brief 当前正在用的根目录。
     * @return 一定非空：设定的那一份不存在（U 盘没插、目录被挪走）时退回自带那份。
     */
    QString currentFolder() const;

    /**
     * @brief 添加一个根目录。
     * @param path 目录路径。
     * @return 路径是个存在的目录（或本来就等于自带那份）时返回 true。
     * **已经在列表里时同样返回 true**（不重复追加）—— 返回值表达的是
     * "这条路径现在在列表里"，不是"这次新加进去了"。
     */
    bool addFolder(const QString &path);

    /**
     * @brief 把一个用户添加的根目录**从列表里移除**。
     * @param path 目录路径。
     * @return 真从列表里去掉了返回 true；自带那份、以及不在列表里的路径返回 false。
     *
     * NOTE: 只动列表与设置 —— **盘上的目录和文件一个字节都不碰**。这一条是刻意的（删几百
     * GB 的整合包目录不可逆，而这个入口就贴在存档目录旁边），也正是界面上那个按钮叫
     * 「移除文件夹…」而不是「删除」的原因：名字得说得出这一步到底做了什么。
     *
     * 移除掉的正好是当前用的那一份时，当前目录**退回自带那份**（否则启动器会指向一个
     * 已经不在列表里的目录）。
     */
    bool removeFolder(const QString &path);

    /**
     * @brief 换一个当前根目录。
     * @param path 目录路径，必须在 folders() 里、而且是个存在的目录。
     * @return 换成功了返回 true；不在列表里或目录不存在时返回 false 且什么都不做。
     *
     * 换成当前已经在用的那一份时也返回 true，但**不发信号**（没必要让界面重灌一遍）。
     */
    bool setCurrentFolder(const QString &path);

    // ------------------------------------------------------------------
    // 各个根目录的**自定义名字**（要的是一行浅显的标签，不是去改磁盘上的目录名）
    // ------------------------------------------------------------------
    // 要求：「文件夹允许重命名（一个 customname，**不是直接改**，毕竟
    // .minecraft 改起来容易出事）」。所以这一组只动设置、一个字都不碰盘 —— 和
    // removeFolder() 那条"只动列表"的规矩是同一条。
    //
    // 名字的作用域就是这个类：设定、读回、跟着移除一起清掉，界面只管问它要。
    // ------------------------------------------------------------------

    /// 名字最长多少个字（界面那个输入框和数据层用的是**同一个数**）。
    static int maxDisplayNameLength() { return 40; }

    /**
     * @brief 用户给某个根目录起的名字。
     * @return 起过名字就返回那个名字；没起过、或者路径不在列表里时返回空串。
     *
     * NOTE: 空串**不是**一个可以显示的字 —— "没起过名字时显示什么"是界面的事
     * （自带那份显示「当前文件夹」，别的显示目录名），数据层不替它编一个。
     */
    QString displayName(const QString &path) const;

    /**
     * @brief 校验一个名字合不合法。**界面和数据层共用这一份判断**（和实例改名那条
     * `InstanceFiles::validateRenameTarget` 一个路数）—— 界面绝不自己抄一遍规则。
     * @return 合不合法都是人话：合法返回空串，不合法返回一句可以直接弹给用户的原因。
     *
     * 空名字（或者只有空白）算**合法**：它的意思是"把名字清掉、回到默认那个显示名"，
     * 不是错误 —— 用户想恢复原样时得有路可走。
     */
    static QString validateDisplayName(const QString &name);

    /**
     * @brief 给一个根目录起名字。传空名字 = 把这一项清掉。
     * @param path 目录路径（内部先归一化）。
     * @param name 新名字，调用前应当先过 validateDisplayName()。
     * @return 这条路径在列表里就返回 true（名字没变也返回 true，和 addFolder() 一样，
     * 返回值表达的是"现在它叫什么由我说了算"，不是"这次真改了"）。
     * 名字真变了才发 foldersChanged()。
     */
    bool setDisplayName(const QString &path, const QString &name);

    /**
     * @brief 把一个用户添加的根目录**从列表里拿掉，并且把盘上那个目录整个移进回收站**。
     * @param path 目录路径。
     * @param error 失败时写进一句可以直接弹给用户的原因；可以传空。
     * @return 移干净了返回 true。
     *
     * NOTE: 这正是 removeFolder() 那份文档里写着"刻意不做"的事 —— 所以它是**单独的**
     * 一个入口，界面上也是单独一项（红色那项），名字就叫「删除」。两者的区别
     * 必须一眼看得出来：移除只动列表，删除连盘上的东西一起没。
     *
     * NOTE: 删的是**回收站**（`moveToRecycleBin()`）而不是永久删除：这一颗按钮就贴在存档
     * 目录旁边，误点一次不该把几百 GB 的整合包彻底抹掉。回收站放不进去就失败，不降级成
     * 永久删除。
     *
     * 自带那份删不掉（和移除同一个理由：它是 AppPaths 的默认值、别的目录都不在盘上时
     * 唯一的退路）。盘上目录**不存在**时不算失败：列表里那一条还是要清掉。
     */
    bool deleteFolder(const QString &path, QString *error = nullptr);

signals:
    /// 列表本身变了（加/移除/改名）—— 界面据此重灌左边那张列表。
    void foldersChanged();
    /// 当前根目录变了 —— 界面据此切 AppPaths 并重灌所有跟路径有关的东西。
    void currentFolderChanged(const QString &path);
    void directoryAccessDenied(const QString &problem);

private:
    /// 把用户输入/盘上读来的路径归一化成可比对的形式：绝对路径 + cleanPath；
    /// 目录真的存在时再取一次 canonicalFilePath（顺带统一盘符大小写、解掉符号链接，
    /// 否则同一份目录会以两种写法同时进列表）。
    ///
    /// **例外：这一格本身就是链接（junction / 符号链接）的，不解**——
    /// 解了就变成"用户加的是 `D:\MC\link`、列表里却是 `E:\real`"，接着"删除这一份"
    /// 删掉的就是链接那一头的东西。只归一它的**父目录**，最后那一段原样留着。
    /// 详见 .cpp 里那一格。
    static QString normalized(const QString &path);

    /// 把自定义名字表写回设置（原始形态见 LauncherSettings::folderNames()）。
    void storeNames();

    /// 「这个根目录不归我们管了」的收尾：从列表与名字表里摘掉、落盘、发信号，
    /// 摘掉的正好是当前在用的那一份时退回自带那份。
    ///
    /// NOTE: 摘出来是因为**移除**和**删除**都要走这一段，两边各写一遍的话迟早只剩一边
    /// 记得要发信号、或者只剩一边记得要退回自带那份（那两种漏法在界面上都表现为
    /// "列表变了但右边还是旧的那一份"）。
    void forgetFolder(const QString &target);

    LauncherSettings *m_settings;
    QStringList m_userFolders;  // 用户添加的那些（不含自带那份）
    QString m_current;          // 归一化后的绝对路径，一定非空
    /// 路径 → 自定义名字。存的是**归一化之后**的路径（和 m_userFolders 同一套写法），
    /// 不然盘符大小写或斜杠方向换一下就会认不出这个名字。
    QMap<QString, QString> m_names;
};
