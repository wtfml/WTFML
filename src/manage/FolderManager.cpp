/**
 * @file FolderManager.cpp
 * @brief 实现「游戏文件夹」列表的读取、添加、移除、改名、删除与切换。
 * @note 见 FolderManager.hpp 的说明。
 */

#include "FolderManager.hpp"
#include <QCoreApplication>
#include <QThread>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <string>

#include "../settings/LauncherSettings.hpp"
#include "../util/AppPaths.hpp"
#include "util/DirectorySessionLock.hpp"

#if defined(Q_OS_WIN)
// NOTE: 放在所有 Qt 头之后（windows.h 定义的东西太多，先于 Qt 包含容易撞名）。这一套和
// LauncherManager.cpp 里那段同一个写法。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <shellapi.h>  // FOF_ALLOWUNDO / FOF_SILENT 这一族（WIN32_LEAN_AND_MEAN 不自动拉）
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>
#endif

namespace {

/// 这个目录项是不是一个"指到别处去"的链接（Windows 的 junction / 符号链接，Unix 的符号
/// 链接）。删目录树的时候必须认它，否则会把链接那一头的东西一起删掉。
///
/// NOTE: 判据**不能**只写 `isSymLink()`，也不能用 `canonicalFilePath() !=
/// absoluteFilePath()` —— 实际行为是：
/// · `mklink /J` 造出来的 junction，`isSymLink()` 是 **false**（Qt 把它报成
/// 普通目录），`canonicalFilePath()` 也**原样返回字面路径**（根本不解链接）；
/// · 只有 `isJunction()` 认得出它。
/// junction 恰恰是普通用户（不需要管理员权限）挂目录时最常用的那一种，所以只判
/// isSymLink() 等于对这一类完全没设防。
bool isLinkToElsewhere(const QFileInfo &info) {
    return info.isJunction() || info.isSymLink();
}

/// 删一个**文件**（文件符号链接也走这里）。
///
/// 第一次失败时**清掉只读位再试一次**。Qt 6.10 在 Windows 上的
/// `QFile::remove()` 就是一句 `DeleteFile()`（qfilesystemengine_win.cpp），**不会**替你
/// 摘 `FILE_ATTRIBUTE_READONLY` —— 而只读文件恰恰删不掉（ERROR_ACCESS_DENIED）。用户从只读
/// 介质拷来的整合包、被别的启动器/网盘标成只读的 jar，都会卡在这一步，而调用方那句
/// 提示写的是"多半是它正被游戏占着"：方向完全是错的，关掉游戏再删一百次也没用。
///
/// 只读位**只有在第一次真的失败之后才动**：正常文件一次成功，权限一个字节都不碰。
bool removeFileAllowingReadOnly(const QString &path) {
    if (QFile::remove(path)) {
        return true;
    }
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        return true;  // 已经没了（并发删掉的那一种）= 这一步的目的达成
    }
    const QFileDevice::Permissions perms = info.permissions();
    QFile::setPermissions(path, perms | QFileDevice::WriteOwner | QFileDevice::WriteUser);
    return QFile::remove(path);
}

}  // namespace

/// 递归删掉 @p path 这一棵，但**一个链接都不追进去**：链接只把链接本身摘掉，那一头的东西
/// 一个字节都不动。
///
/// NOTE: 为什么不能用 `QDir::removeRecursively()`。实际行为：
/// · 对一个**本身就是 junction** 的目录调用它 → junction 指向的那个真目录里的
/// 文件**没了**（目标是活的，链接没了）；
/// · 目录**里面**含一个 junction 时 → 同样跟着进去把目标删了。
/// 于是"删掉这一份游戏文件夹"会牵连到游戏文件夹**外面**的东西。把 `saves` 挂到
/// 网盘同步目录、把 `mods` 挂到另一块盘，都是很常见的做法 —— 而用户点的是"删掉这
/// 一份"，不是"删掉那边那块盘上的东西"（别的程序可能还在用同一份数据）。
///
/// @return 整棵都删干净了没有。删到一半失败时**返回 false 但仍然尽力删完**（剩下的留着，
/// 调用方照实报"没能删干净"，比留一半还不说话强）。
///
/// NOTE: 这个函数**故意留在匿名命名空间外面**（它上面那个 `} // namespace` 就是为它关的）：
/// 删实例那条路（VersionManager::deleteInstance）要用它，而 FolderManager.hpp 里
/// 已经把它声明成了一个自由函数。放在匿名命名空间里的话，头文件那条声明会和它
/// 变成**两个不同的重载**，调用处直接报 ambiguous。isLinkToElsewhere() 可以留在里面
/// —— 只有它用，而且它在上面、看得见。
bool removeTreeWithoutFollowingLinks(const QString &path) {
    const QFileInfo info(path);
    if (isLinkToElsewhere(info)) {
        return info.isDir() ? QDir().rmdir(path) : removeFileAllowingReadOnly(path);
    }
    if (!info.exists()) {
        return true;  // 已经不在了 = 这一步想要的结果已经达成
    }
    if (!info.isDir()) {
        return removeFileAllowingReadOnly(path);
    }
    const QDir dir(path);
    const QStringList entries =
        dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    bool ok = true;
    for (const QString &entry : entries) {
        // 注意用 && 的写法（不用短路）：一条失败也要接着删剩下的 —— 早退会把剩下的留下，
        // 而"剩一小半"比"整棵都在"更难收拾。所以先把结果收集起来，最后一起回答。
        if (!removeTreeWithoutFollowingLinks(dir.absoluteFilePath(entry))) {
            ok = false;
        }
    }
    if (!ok) {
        // 里面有东西没删掉，这一层（多半）也就空不了，别再多试一次 rmdir 掩盖原因。
        return false;
    }
    if (QDir().rmdir(path)) return true;
#if defined(Q_OS_WIN)
    // Windows 已接受 DeleteFile 的文件仍可能因其它共享句柄尚未关闭而处于
    // delete-pending；目录看起来已空，但第一次 RemoveDirectory 仍失败。
    // 只重试删除空目录本身，不重新递归，不碰此期间新增的文件，也不改 ACL。
    for (int attempt = 0; attempt < 4; ++attempt) {
        QThread::msleep(25);
        const QFileInfo remaining(path);
        if (!remaining.exists()) return true;
        if (isLinkToElsewhere(remaining) || !remaining.isDir()) return false;
        if (!QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                 QDir::NoDotAndDotDot).isEmpty()) return false;
        if (QDir().rmdir(path)) return true;
    }
#endif
    return false;
}

#if defined(Q_OS_WIN)
namespace {

/// 一个 HRESULT 的**人话**版本（`FormatMessageW` 那套；系统没给话时退回十六进制）。
QString systemMessage(HRESULT hr) {
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text;
    if (length != 0 && buffer != nullptr) {
        text = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
        LocalFree(buffer);
    }
    if (text.isEmpty()) {
        text = QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
    }
    return text;
}

/// 把 COM 起起来（工人线程多半还没起过）。Qt 自己的线程池不会替我们做这件事，而
/// `IFileOperation` 是 COM 对象 —— 不起就是 "CoInitialize has not been called"。
///
/// NOTE: 主线程上 Qt 已经用 `OleInitialize`（STA）起过了，这时 `CoInitializeEx` 回 S_FALSE，
/// **不能**再配对一次 `CoUninitialize`（会替 Qt 把它撤掉）。所以只有本次真的起成功才收尾。
class ComApartment {
public:
    ComApartment() {
        const HRESULT hr =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        m_initialize = SUCCEEDED(hr);
        // RPC_E_CHANGED_MODE = 这一根线程已经是别的套间（MTA）了：不重起，但可以接着试
        // —— 拿得到就干活，拿不到由调用方照实报错。
        m_usable = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
    ~ComApartment() {
        if (m_initialize) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment &) = delete;
    ComApartment &operator=(const ComApartment &) = delete;

    bool usable() const { return m_usable; }

private:
    bool m_initialize = false;
    bool m_usable = false;
};

/// 把 @p root 底下（**不含 root 自己**）的链接一个个摘掉，只摘壳子，那一头一个字节都不动。
///
/// NOTE: 为什么要先剥。回收站那一趟是 shell 在搬东西，而 shell 面对 junction 的行为不值得
/// 这一次冒险：一旦它顺着链接走进去，进的回收站里就是**别人那块盘上的东西**。这里先用和
/// `removeTreeWithoutFollowingLinks()` 同一条判据把链接摘掉，剩下的才是纯数据。
/// 链接本身没有回收价值（两个字节的壳子，一条 mklink 就回来了）。
bool stripLinksBelow(const QString &root, QString *problem) {
    const QDir dir(root);
    const QStringList entries =
        dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        const QString path = dir.absoluteFilePath(entry);
        const QFileInfo info(path);
        if (isLinkToElsewhere(info)) {
            const bool removed =
                info.isDir() ? QDir().rmdir(path) : removeFileAllowingReadOnly(path);
            if (!removed) {
                if (problem != nullptr) {
                    *problem = FolderManager::tr("无法移除链接「%1」。请检查是否被其他程序占用。")
                                   .arg(QDir::toNativeSeparators(path));
                }
                return false;
            }
            continue;
        }
        if (info.isDir() && !stripLinksBelow(path, problem)) {
            return false;
        }
    }
    return true;
}

/// 把 @p root 这一棵（**含 root 自己**）的 `FILE_ATTRIBUTE_READONLY` 摘掉，别的属性一个都不碰。
///
/// NOTE: 为什么回收站那一趟需要这一步。shell 把一件只读文件/一棵含着只读文件的目录搬进
/// 回收站时会回 `COPYENGINE_E_ACCESS_DENIED_SRC`（0x80270021，"源访问被拒绝"）—— 只读位
/// 挡住的正是它删条目那一步，树里其余部分一件问题都没有。拿到的句子却是"回收站不可用 /
/// 空间不足 / 被别的程序占着"，方向完全是错的。资源管理器碰到只读文件会先弹一句"确实要
/// 删除吗"，确认之后它自己摘只读位再删；这里没有"问一句"的机会（用户点的就是删除），
/// 所以摘位这件事由我们做。
///
/// NOTE: **只在第一次真的失败之后才调用**（和 `removeFileAllowingReadOnly()` 同一条约定）：
/// 一次就成功的那一趟一个属性都不碰，进回收站的那一份还是只读的。
///
/// @return 真的摘掉了几件。0 = 这一棵里没有只读的东西，重试没有意义。
int clearReadOnlyBelow(const QString &root) {
    const QFileInfo info(root);
    if (!info.exists() && !info.isSymLink()) {
        return 0;
    }
    if (isLinkToElsewhere(info)) {
        return 0;  // 链接在搬之前就剥掉了（stripLinksBelow），这里绝不跟进去
    }
    const QString path = info.absoluteFilePath();
    const std::wstring wide = QDir::toNativeSeparators(path).toStdWString();
    int cleared = 0;
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0 &&
        SetFileAttributesW(wide.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) != 0) {
        ++cleared;
    }
    if (info.isDir()) {
        const QDir dir(path);
        const QStringList entries =
            dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            cleared += clearReadOnlyBelow(dir.absoluteFilePath(entry));
        }
    }
    return cleared;
}

}  // namespace
#endif

/// 把 @p path 移进回收站（文件 / 目录 / 整棵树）。详见头文件那段约定。
///
/// NOTE: 用的是 **`IFileOperation` + `FOFX_RECYCLEONDELETE`**，不是旧的 `SHFileOperation`。
/// 差别在失败时的行为：旧 API 在"回收站放不进去"时（U 盘、网络盘、配额满了、关了回收站）
/// 会**悄悄**改成永久删除，正好是我们最不想要的那一种；`FOFX_RECYCLEONDELETE` 明确要求
/// "必须进回收站"，放不进去就报错 —— 删不掉比悄悄删掉强。
bool moveToRecycleBin(const QString &path, QString *error) {
    const auto fail = [error](const QString &text) {
        if (error != nullptr) {
            *error = text;
        }
        return false;
    };
    const auto succeed = [error] {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    };

    if (path.trimmed().isEmpty()) {
        return fail(FolderManager::tr("没有指定要删除的东西。"));
    }
    const QFileInfo info(path);

    // 链接本身：只摘壳子（头文件里那条约定）。回收一个链接没有任何意义。
    if (isLinkToElsewhere(info)) {
        const bool removed = info.isDir() ? QDir().rmdir(path) : removeFileAllowingReadOnly(path);
        return removed ? succeed()
                       : fail(FolderManager::tr("无法删除链接「%1」。请检查链接是否被其他程序占用。")
                                  .arg(QDir::toNativeSeparators(path)));
    }
    if (!info.exists()) {
        return succeed();  // 已经不在了 = 调用方要的结果已经达成
    }

    const QString target = QDir::cleanPath(info.absoluteFilePath());
    if (QDir(target).isRoot()) {
        // 整块盘不是"一样东西"：真移进回收站等于把盘清空。
        return fail(FolderManager::tr("「%1」是整块盘的根目录，不能整个删。").arg(target));
    }

#if defined(Q_OS_WIN)
    // shell 那一套只认反斜杠：`QDir::cleanPath()` 给的是正斜杠，直接喂
    // `SHCreateItemFromParsingName()` 会回 0x80070057（E_INVALIDARG，一句"参数错误"）。
    // 内部比对仍然用带正斜杠的 `target`（Qt 那边全程都认这一种）。
    const QString nativePath = QDir::toNativeSeparators(target);
    const std::wstring wide = nativePath.toStdWString();

    // 网络位置没有回收站。与其让 IFileOperation 报一句看不懂的错，不如先说清楚"是这儿
    // 没有回收站、所以什么都没动"。
    wchar_t volume[MAX_PATH + 1] = {};
    if (GetVolumePathNameW(wide.c_str(), volume, MAX_PATH + 1) != 0) {
        const UINT type = GetDriveTypeW(volume);
        if (type == DRIVE_REMOTE || type == DRIVE_CDROM || type == DRIVE_NO_ROOT_DIR) {
            return fail(FolderManager::tr("「%1」所在的 %2 不支持回收站。为避免永久删除，已保留文件。")
                            .arg(target, QString::fromWCharArray(volume)));
        }
    }

    // 动手之前先问一句「这一项本身删得动吗」：拿 `DELETE` 权限开一个句柄再立刻关掉，
    // 盘上**一个字节都不改**。这一问换来两件事：
    //
    //  · 报的是**确定的原因**（ACL 拒绝删除 / 被别的程序占着），而不是等回收站那一趟回来
    //    之后，用一句「回收站不可用、放不下这么大、或者被别的程序占着」让用户自己猜。
    //  · 判死发生在**动任何东西之前**。让 `IFileOperation` 自己去撞的话，它会先把子树里
    //    删得动的那些搬进回收站、最后才在那一层失败 —— 留下一半在里面、一半在外面的现场
    //    （实测：子树里的文件已经没了，目录还在，报「Access is denied」）。
    //
    // 这一问只挡「连这一项本身都删不动」。子树里某一件被占着，照旧由下面那一趟去撞
    // （`FOFX_EARLYFAILURE` 碰到就停，见 ZAO6）。
    //
    // NOTE: 只读文件、目录、以及 ACL 只拒绝「写」的那一件，这里都开得出来（ZAO2 / ZAO7
    // 量的就是它们），所以不会把「文件是只读的」误报成「权限不够」。删除本来就有两条路
    // （自己身上有 DELETE，或者所在目录有 FILE_DELETE_CHILD），`CreateFileW` 判的是同一条
    // 规矩，所以 ACL 得两条一起拒才挡得住 —— 测试里那个夹具就是这么造的。
    {
        DWORD probeError = 0;
        const HANDLE probe =
            CreateFileW(wide.c_str(), DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING,
                        info.isDir() ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
                        nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            probeError = GetLastError();
        } else {
            CloseHandle(probe);
        }
        if (probeError == ERROR_ACCESS_DENIED) {
            return fail(FolderManager::tr("「%1」或其上级目录的权限阻止删除。请检查「属性 → 安全 → 高级」中的删除权限，再重试。")
                            .arg(QDir::toNativeSeparators(target)));
        }
        if (probeError == ERROR_SHARING_VIOLATION || probeError == ERROR_LOCK_VIOLATION) {
            return fail(FolderManager::tr("「%1」正被其他程序占用，未执行删除。\n\n请关闭相关程序后重试。")
                            .arg(QDir::toNativeSeparators(target)));
        }
    }

    // 一棵树里的链接先剥掉（理由见 stripLinksBelow()）。
    if (info.isDir()) {
        QString linkProblem;
        if (!stripLinksBelow(target, &linkProblem)) {
            return fail(FolderManager::tr("未能移除「%1」中的链接，目录未移入回收站。\n\n%2")
                            .arg(QDir::toNativeSeparators(target), linkProblem));
        }
    }

    ComApartment com;
    if (!com.usable()) {
        return fail(FolderManager::tr("系统回收站组件无法启动，已保留「%1」。")
                        .arg(QDir::toNativeSeparators(target)));
    }

    // 抽成 lambda 是为了"只读文件挡路时再试一次"（见下面那一段）。
    bool opened = false;  // CoCreateInstance 过了没有：没过就不能报"移不进去"，那是另一句话
    const auto recycleOnce = [&wide, &opened](BOOL *abortedOut) -> HRESULT {
        IFileOperation *operation = nullptr;
        HRESULT result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                          IID_IFileOperation, reinterpret_cast<void **>(&operation));
        if (FAILED(result) || operation == nullptr) {
            if (operation != nullptr) operation->Release();
            return result;
        }
        opened = true;
        // FOFX_EARLYFAILURE：碰到第一个删不动的就停，尽量少动 —— 回收站那一趟默认会跳过
        // 失败项继续搬，最后只给一句"有的没成功"，用户已经分不清哪些进了回收站。
        result = operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT
                                              | FOF_NOERRORUI | FOFX_EARLYFAILURE
                                              | FOFX_RECYCLEONDELETE);
        IShellItem *item = nullptr;
        if (SUCCEEDED(result)) {
            result = SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_IShellItem,
                                                 reinterpret_cast<void **>(&item));
        }
        if (SUCCEEDED(result) && item != nullptr) {
            result = operation->DeleteItem(item, nullptr);
        }
        if (SUCCEEDED(result)) {
            result = operation->PerformOperations();
        }
        BOOL aborted = FALSE;
        operation->GetAnyOperationsAborted(&aborted);
        if (item != nullptr) item->Release();
        operation->Release();
        if (abortedOut != nullptr) *abortedOut = aborted;
        return result;
    };

    BOOL aborted = FALSE;
    HRESULT hr = recycleOnce(&aborted);
    if (!opened) {
        return fail(FolderManager::tr("无法访问回收站：%1\n「%2」已保留。")
                        .arg(systemMessage(hr), QDir::toNativeSeparators(target)));
    }
    if (FAILED(hr)) {
        // 目标本身或子树里有只读文件/目录：shell 不肯替我们摘只读位，这一趟回的是"源访问被拒绝"。
        // 摘掉只读位再试一次（理由见 clearReadOnlyBelow()；一次就成功的那趟一个属性都不碰）。
        if (clearReadOnlyBelow(target) > 0) {
            hr = recycleOnce(&aborted);
        }
    }

    if (FAILED(hr)) {
        return fail(FolderManager::tr("无法将「%1」移入回收站：%2\n\n请检查回收站与磁盘空间，关闭占用文件的程序后重试。")
                        .arg(QDir::toNativeSeparators(target), systemMessage(hr)));
    }
    if (aborted != FALSE) {
        return fail(FolderManager::tr("「%1」未完全移入回收站。请核对回收站与原目录中的文件。")
                        .arg(QDir::toNativeSeparators(target)));
    }
    if (QFileInfo::exists(target)) {
        // 没报错、也没打断，东西却还在：不能当作删成功（调用方会照实说"还在"）。
        return fail(FolderManager::tr("未能将「%1」移入回收站。目录仍在原位置。")
                        .arg(QDir::toNativeSeparators(target)));
    }
    return succeed();
#else
    // 非 Windows 平台没有"回收站"这个概念。这里**不**退回永久删除（那就是这一整套要防的
    // 事）；调用方会看到一句明确的失败。当前产品只发 Windows 版。
    return fail(FolderManager::tr("此平台不支持回收站，已保留「%1」。")
                    .arg(QDir::toNativeSeparators(target)));
#endif
}

///
/// NOTE: 目录必须**先建再进**：`QFile::copy()` 不会替你造父目录，mkdir 和拷文件分两步
/// 是必须的（先拷文件再建目录这条最常见的写法在这里会失败）。
///
/// NOTE: 顺序上**先把目标目录建出来**再往里灌。中途失败时盘上会留下一个"拷了一半"的
/// 目录 —— 这是故意的：调用方（复制实例、导入世界）拿到的 from/to 都是它自己算
/// 出来的，它比这里更清楚该不该收拾，而"留一半"比"悄悄删掉用户别的东西"安全。
/// 但**单个文件**永远要么完整、要么不在（临时名 + 改名，见下面那个 publish），
/// 不会出现"名字在、内容缺一截"的那种半截文件。
bool copyTreeWithoutFollowingLinks(const QString &from, const QString &to,
                                   const std::function<bool()> &cancelled, QString *problemOut) {
    if (problemOut) problemOut->clear();
    const auto failCopy = [&](const QString &reason) {
        if (problemOut) *problemOut = QCoreApplication::translate("FolderManager", "%1 → %2：%3").arg(from, to, reason);
        return false;
    };
    if (cancelled && cancelled()) return false;
    const QFileInfo source(from);
    if (isLinkToElsewhere(source)) {
        // 链接照拷的话，会在新位置复制出**那一头**的一整棵 —— 用户挂到别的盘上的东西
        // 会被无声无息地抄一份进来（可能几百 GB）。所以这里直接判失败，不是"跳过"。
        qWarning() << "copyTreeWithoutFollowingLinks: refusing to follow a link:" << from;
        return failCopy(QCoreApplication::translate("FolderManager", "源路径包含链接"));
    }
    if (source.isDir()) {
        if (QFileInfo::exists(to)) {
            qWarning() << "copyTreeWithoutFollowingLinks: target already exists:" << to;
            return failCopy(QCoreApplication::translate("FolderManager", "目标目录已存在"));
        }
        if (!QDir().mkpath(to)) {
            qWarning() << "copyTreeWithoutFollowingLinks: cannot create directory" << to;
            return failCopy(QCoreApplication::translate("FolderManager", "无法创建目标目录"));
        }
        const QFileInfoList entries = QDir(from).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QFileInfo &entry : entries) {
            if (!copyTreeWithoutFollowingLinks(entry.absoluteFilePath(),
                                               to + QLatin1Char('/') + entry.fileName(), cancelled, problemOut)) {
                return false;
            }
        }
        return true;
    }
    if (!source.isFile()) {
        // 既不是目录也不是文件：设备文件、坏掉的链接、或者这一瞬间被别的东西拿走了。
        // 报失败而不是跳过 —— 拷出来的一份"少了点东西"是最难发现的那种坏。
        qWarning() << "copyTreeWithoutFollowingLinks: cannot copy a non-file entry:" << from;
        return failCopy(QCoreApplication::translate("FolderManager", "源路径不是普通文件"));
    }
    if (QFileInfo::exists(to)) {
        // QFile::copy() 遇到已存在的目标**直接返回 false**（Qt 不肯覆盖），所以这一句
        // 其实只是把"为什么会失败"说清楚一点，真正挡住覆盖的是 copy() 自己。
        qWarning() << "copyTreeWithoutFollowingLinks: target already exists:" << to;
        return failCopy(QCoreApplication::translate("FolderManager", "目标文件已存在"));
    }
    // 先写**同目录**的临时名，再改名发布（和 `operations/TransferApply.cpp` 里那份
    // 单文件拷贝同一规矩）：直接往 to 上写，进程被杀 / 盘满 / 杀软扫描中断时盘上会留下
    // 一个**截断的半截文件** —— 它和"拷好了"长得一模一样，下一次"目标已存在"就再也
    // 不会重试。同卷改名是原子的：目标要么完整，要么根本不存在。失败/取消时临时名删掉。
    // 名字带 UUID：并发拷同一个目标时彼此不会撞上（`NewOnly` 也会挡）。
    // 临时名不拼完整目标文件名，避免合法长文件名加后缀后超过文件系统的单名长度上限。
    const QString staging = QFileInfo(to).absoluteDir().filePath(QStringLiteral(".wtfml-part-")
                            + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto publish = [&]() {
        QFile staged(staging);
        if (!staged.rename(to)) {
            const QString reason = staged.errorString();
            QFile::remove(staging);
            return failCopy(QCoreApplication::translate("FolderManager", "无法发布完整临时文件：%1").arg(reason));
        }
        return true;
    };
    if (cancelled) {
        QFile input(from), output(staging);
        if (!input.open(QIODevice::ReadOnly) ||
            !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            QFile::remove(staging);
            return failCopy(QCoreApplication::translate("FolderManager", "无法打开源或临时文件：%1；%2").arg(input.errorString(), output.errorString()));
        }
        while (!input.atEnd()) {
            if (cancelled()) {
                output.close();
                QFile::remove(staging);
                return false;
            }
            const auto bytes = input.read(1024 * 1024);
            if (bytes.isEmpty() || output.write(bytes) != bytes.size()) {
                const QString reason = QCoreApplication::translate("FolderManager", "%1；%2").arg(input.errorString(), output.errorString());
                output.close();
                QFile::remove(staging);
                return failCopy(QCoreApplication::translate("FolderManager", "分块读写失败：%1").arg(reason));
            }
        }
        if (input.error() != QFile::NoError || !output.flush() || cancelled()) {
            const QString reason = QCoreApplication::translate("FolderManager", "%1；%2").arg(input.errorString(), output.errorString());
            output.close();
            QFile::remove(staging);
            return failCopy(QCoreApplication::translate("FolderManager", "刷新临时文件失败或操作取消：%1").arg(reason));
        }
        output.setPermissions(input.permissions());
        output.close();
        return publish();
    }
    QFile input(from);
    if (!input.copy(staging)) {
        const QString reason = input.errorString();
        qWarning() << "copyTreeWithoutFollowingLinks: cannot copy" << from << "to" << to;
        QFile::remove(staging);
        return failCopy(QCoreApplication::translate("FolderManager", "文件复制失败：%1").arg(reason));
    }
    return publish();
}

FolderManager::FolderManager(LauncherSettings *settings, QObject *parent)
    : QObject(parent), m_settings(settings) {
    // 盘上那串路径是上一次运行写下的：用户可能已经把目录删了、挪了、或者换了盘符。
    // 一律重新归一化一遍，免得界面里同一个目录以两种写法各占一行。
    const QStringList stored = m_settings != nullptr ? m_settings->folders() : QStringList();
    for (const QString &entry : stored) {
        const QString path = normalized(entry);
        // 等于自带那份的要丢掉：启动器被拷到 D:\MC\ 之后，用户添加的
        // D:\MC\.minecraft 会和自带那份撞上，留着就是列表里两条一模一样的路径。
        if (path.isEmpty() || path == defaultFolder() || m_userFolders.contains(path)) {
            continue;
        }
        m_userFolders.append(path);
    }

    // 名字表：盘上那份是 `路径 + 制表符 + 名字`（见 LauncherSettings::folderNames()）。
    // 路径照旧重新归一化一遍，归一化之后认不出来的（那一项已经被挪走、或者盘上的
    // 路径写法变了）就丢掉 —— 名字是挂在具体某个目录上的，认不出挂给谁了留着没用。
    const QStringList storedNames =
        m_settings != nullptr ? m_settings->folderNames() : QStringList();
    for (const QString &entry : storedNames) {
        // 按**最后一个**制表符切，不是第一个：名字那一半由 validateDisplayName()
        // 挡着制表符（进不去），而路径那一半在 NTFS 上是**可以**带制表符的。
        // 按第一个切的话，`D:\MC\foo<TAB>bar` 这种目录配的名字会被读成
        // 路径 `D:\MC\foo` + 名字 `bar<TAB>…` —— 名字没了、还可能认给别的目录。
        const int tab = entry.lastIndexOf(QLatin1Char('\t'));
        if (tab <= 0) {
            continue;  // 连分隔符都没有（手改的）：这一项读不出是给谁起的名字
        }
        const QString path = normalized(entry.left(tab));
        const QString name = entry.mid(tab + 1);
        if (path.isEmpty() || name.isEmpty()) {
            continue;
        }
        m_names.insert(path, name);
    }

    m_current = normalized(m_settings != nullptr ? m_settings->currentFolder() : QString());
    if (m_current.isEmpty() || !QFileInfo(m_current).isDir()) {
        // 当前那一份不在盘上（U 盘今天没插、目录被删或被挪走）：这次先用自带那份。
        // **刻意不把盘上那个值改掉** —— 它可能只是今天没挂上，插回去还该接着用它。
        // 代价是这次会话里它不再生效，右边那句"当前文件夹"会照实说是哪一份。
        if (!m_current.isEmpty()) {
            qWarning() << "FolderManager: 记着的游戏文件夹不在盘上，这次先用自带那份："
                       << m_current;
        }
        m_current = defaultFolder();
    }
}

QString FolderManager::normalized(const QString &path) {
    if (path.isEmpty()) {
        return QString();
    }
    const QFileInfo info(path);
    if (info.exists()) {
        // 这一格**本身就是个链接**时不许解。canonicalFilePath() 会把
        // 符号链接解析成它指向的那个目录 —— 于是用户添加进来的 `D:\MC\link` 在列表里
        // 变成 `E:\real`，而"删除这一份游戏文件夹"随后调的就是
        // `removeTreeWithoutFollowingLinks(E:\real)`：链接本身还在，**另一块盘上的东西
        // 没了**。上面 isLinkToElsewhere() 那个 guard 判的正是 `isJunction() ||
        // isSymLink()`，可它**在更早的这一步就被解掉了**，所以它一辈子也看不到符号链接。
        //
        // NOTE: junction 不受影响 —— `canonicalFilePath()` 对 junction **原样返回字面路径**，
        // 根本不解释链接，所以它在下面那条路上活得好好的。这个不对称让"链"这一类
        // 很容易只防住能造出来的 junction、漏掉符号链接。
        //
        // 只把**父目录**归一（盘符大小写、`..` 这些照旧收拾掉），最后那一段原样留着 ——
        // 列表里那条还是用户亲手加进来的那一条，删的时候也只是把链接摘掉。
        if (isLinkToElsewhere(info)) {
            const QFileInfo parent(info.absolutePath());
            const QString parentPath = parent.canonicalFilePath();
            const QString base = parentPath.isEmpty() ? info.absolutePath() : parentPath;
            return QDir::cleanPath(base + QLatin1Char('/') + info.fileName());
        }
        // 目录真在的时候用 canonicalFilePath：它顺带统一盘符大小写、解掉符号链接和
        // `..`，否则 `D:/mc/.minecraft` 和 `d:\mc\.minecraft` 会被当成两个文件夹。
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) {
            return QDir::cleanPath(canonical);
        }
    }
    // 目录还不存在（新装的启动器、还没下过东西）：只能按字面归一到绝对路径。
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString FolderManager::defaultFolder() { return normalized(AppPaths::defaultMinecraftDir()); }

bool FolderManager::isDefaultFolder(const QString &path) {
    const QString target = normalized(path);
    return !target.isEmpty() && target == defaultFolder();
}

QString FolderManager::officialLauncherFolder() {
    // `%APPDATA%` 不存在 = 不是一台有用户配置的 Windows（服务账户 / 精简环境）。这时候
    // 返回空串，上层按"没有这一份"处理 —— 不要退到 QStandardPaths，它给的可能是另一个
    // 位置（甚至我们的沙盒），那就不是在说"官方启动器"了。
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty()) {
        return QString();
    }
    return QDir::cleanPath(appData + QStringLiteral("/.minecraft"));
}

QStringList FolderManager::reuseSourceFolders() const {
    QStringList roots;
    const auto appendUnique = [&roots](const QString &path) {
        if (path.isEmpty() || !QFileInfo(path).isDir()) {
            return;
        }
        const QString clean = QDir::cleanPath(path);
        for (const QString &existing : roots) {
            if (QDir::cleanPath(existing).compare(clean, Qt::CaseInsensitive) == 0) {
                return;
            }
        }
        roots.append(clean);
    };
    // 顺序有讲究：自带那份 + 用户添加的在前（用户自己挑的目录更可能是他常玩的那一份），
    // 官方那一份垫后。
    for (const QString &folder : folders()) {
        appendUnique(folder);
    }
    appendUnique(officialLauncherFolder());
    return roots;
}

QStringList FolderManager::folders() const {
    QStringList all;
    all.append(defaultFolder());
    all.append(m_userFolders);
    return all;
}

QString FolderManager::currentFolder() const { return m_current; }

bool FolderManager::addFolder(const QString &path) {
    const QString target = normalized(path);
    if (target.isEmpty() || !QFileInfo(target).isDir()) {
        return false;
    }
    // 自带那份、以及已经在列表里的：不重复追加，但它确实"现在在列表里"，所以算成功。
    if (target == defaultFolder() || m_userFolders.contains(target)) {
        return true;
    }
    m_userFolders.append(target);
    if (m_settings != nullptr) {
        m_settings->setFolders(m_userFolders);
    }
    emit foldersChanged();
    return true;
}

bool FolderManager::removeFolder(const QString &path) {
    const QString target = normalized(path);
    // 自带那份移不掉：它是 AppPaths 的默认值，也是"别的目录都不在盘上"时的唯一退路。
    if (target.isEmpty() || target == defaultFolder()) {
        return false;
    }
    if (!m_userFolders.contains(target)) {
        return false;
    }
    forgetFolder(target);
    return true;
}

void FolderManager::forgetFolder(const QString &target) {
    m_userFolders.removeOne(target);
    // 名字跟着一起清：移除之后界面上已经没有这一条了，留着它只是让盘上多一项
    // 认不出挂给谁的记录（重新加回来是"新加了一份文件夹"，该用默认显示名）。
    m_names.remove(target);
    if (m_settings != nullptr) {
        m_settings->setFolders(m_userFolders);
    }
    storeNames();
    emit foldersChanged();

    // 移除掉的正好是当前在用的那一份 → 退回自带那份。不退回的话启动器会指着一个
    // 已经不在列表里的目录（界面上没有它，用户想再选回来都没地方点）。
    if (target == m_current) {
        m_current = defaultFolder();
        if (m_settings != nullptr) {
            m_settings->setCurrentFolder(QString());
        }
        emit currentFolderChanged(m_current);
    }
}

QString FolderManager::validateDisplayName(const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return QString();  // 空名字 = 清掉名字，合法
    }
    // 换行和制表符进不去：前者进不了界面上一行那么高的标签，后者是**落盘的字段分隔符**
    // （见 LauncherSettings::folderNames()），放进去等于把一个名字劈成两半。
    for (const QChar ch : trimmed) {
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r') || ch == QLatin1Char('\t')) {
            return tr("名字里不能有换行。");
        }
    }
    if (trimmed.length() > maxDisplayNameLength()) {
        return tr("名字最多 %1 个字。").arg(maxDisplayNameLength());
    }
    return QString();
}

QString FolderManager::displayName(const QString &path) const {
    const QString target = normalized(path);
    if (target.isEmpty()) {
        return QString();
    }
    return m_names.value(target);
}

bool FolderManager::setDisplayName(const QString &path, const QString &name) {
    const QString target = normalized(path);
    if (target.isEmpty() || !folders().contains(target)) {
        return false;
    }
    const QString wanted = name.trimmed();
    const bool changed = wanted.isEmpty() ? m_names.remove(target) > 0
                                          : m_names.value(target) != wanted;
    if (!wanted.isEmpty()) {
        m_names.insert(target, wanted);
    }
    if (changed) {
        storeNames();
        emit foldersChanged();
    }
    return true;
}

bool FolderManager::deleteFolder(const QString &path, QString *error) {
    const QString target = normalized(path);
    // 自带那份删不掉，理由和"移不掉"是同一条（AppPaths 的默认值 + 唯一退路），
    // 而且它多半正是启动器此刻在用的那一份 —— 日志、设置、缓存都写在里面。
    if (target.isEmpty() || target == defaultFolder()) {
        if (error != nullptr) {
            *error = tr("默认游戏目录不可删除。");
        }
        return false;
    }
    if (!m_userFolders.contains(target)) {
        if (error != nullptr) {
            *error = tr("这一份已经不在列表里了。");
        }
        return false;
    }
    QString lockProblem;
    if (!DirectorySessionLock::acquire(target, &lockProblem)) {
        if (error) *error = lockProblem;
        return false;
    }
    // 盘上那个目录不存在（U 盘没插、已经被手工删掉）不算失败：列表里那一条照样要清掉。
    // 存在的话**移进回收站**（不是永久删除）—— 这一颗按钮就在存档目录旁边，误点一次不该
    // 把几百 GB 的整合包彻底抹掉。
    if (QFileInfo::exists(target)) {
        QString recycleProblem;
        if (!moveToRecycleBin(target, &recycleProblem)) {
            if (error != nullptr) {
                // 失败原因照实说：移到一半失败时目录是**半截**的（前面那些已经进了回收站），
                // 而用户需要知道现在盘上和回收站里到底是什么状态。
                *error = tr("「%1」未完全移入回收站：%2\n\n已移入的文件可从回收站恢复。")
                             .arg(QDir::toNativeSeparators(target), recycleProblem);
            }
            return false;
        }
    }
    forgetFolder(target);
    return true;
}

void FolderManager::storeNames() {
    if (m_settings == nullptr) {
        return;
    }
    QStringList entries;
    // 顺序按 folders() 走（自带那份没有名字、不进这一项）：盘上那份表读起来就是
    // 界面上从上到下那个顺序，排查的时候对得上。
    for (const QString &path : folders()) {
        const QString name = m_names.value(path);
        if (!name.isEmpty()) {
            entries.append(path + QLatin1Char('\t') + name);
        }
    }
    m_settings->setFolderNames(entries);
}

bool FolderManager::setCurrentFolder(const QString &path) {
    const QString target = normalized(path);
    if (target.isEmpty() || !QFileInfo(target).isDir() || !folders().contains(target)) {
        return false;
    }
    QString lockProblem;
    if (!DirectorySessionLock::acquire(target, &lockProblem)) {
        emit directoryAccessDenied(lockProblem);
        return false;
    }
    if (target == m_current) {
        // 值没变，但盘上记的可能是那个"不在盘上、这次先不用"的旧目录（见构造函数）。
        // 顺手把盘上写回当前这一份，免得每次开窗都警告一遍。不发信号：界面没必要重灌。
        if (m_settings != nullptr) {
            m_settings->setCurrentFolder(m_current == defaultFolder() ? QString() : m_current);
        }
        return true;
    }
    m_current = target;
    if (m_settings != nullptr) {
        m_settings->setCurrentFolder(target);
    }
    emit currentFolderChanged(m_current);
    return true;
}
