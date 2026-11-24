/**
 * @file StorageAvailability.cpp
 * @brief 实现「这块盘还在不在」的判据。
 * @note 判据只有两条：卷认得出来（`isValid()`）且已就绪（`isReady()`）。
 * 两条都过就是可用 —— 不在这里判断空间和权限。
 */

#include "util/StorageAvailability.hpp"

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QStorageInfo>

namespace {

/**
 * @brief 往上找最近一个**已经存在**的祖先。
 *
 * 落点目录还不存在是常态（下载前 `versions/<id>` 没建、导入前 `saves/<世界>` 没建），
 * 直接拿这种路径去问 `QStorageInfo` 等于问一个不存在的卷。真正该问的是"东西最后会写在
 * 哪块盘上"，那就得顺着路径往上走到第一个存在的祖先。
 *
 * 一路走到根（`C:/`）时，`absolutePath()` 还是它自己 —— 靠那个相等判断收尾，不会死循环。
 */
QString nearestExistingAncestor(const QString &path)
{
    QString probe = QFileInfo(path).absoluteFilePath();
    while (!probe.isEmpty() && !QFileInfo::exists(probe)) {
        const QString parent = QFileInfo(probe).absolutePath();
        if (parent.isEmpty() || parent == probe) {
            break;
        }
        probe = parent;
    }
    return probe;
}

} // namespace

QString StorageAvailability::unavailableReason(const QString &path)
{
    if (path.isEmpty()) {
        return QObject::tr("没有拿到要用的路径。");
    }

    const QString probe = nearestExistingAncestor(path);
    if (probe.isEmpty()) {
        return QObject::tr("无法识别「%1」所在的磁盘。")
            .arg(QDir::toNativeSeparators(path));
    }

    const QStorageInfo storage(probe);
    if (!storage.isValid() || storage.rootPath().isEmpty()) {
        // 卷本身认不出来：盘符不在了（U 盘/移动硬盘拔掉后，盘符跟着消失）。
        // 这时连 rootPath() 都是空的，所以话里只能用探到的那个路径。
        return QObject::tr("找不到「%1」所在的磁盘（盘符可能已经不在了）。")
            .arg(QDir::toNativeSeparators(probe));
    }
    if (!storage.isReady()) {
        // 卷还在系统里登记着，但没就绪：常见于网络盘断开、盘刚拔出还没注销干净。
        return QObject::tr("「%1」所在的磁盘现在没有就绪（可能已经断开连接）。")
            .arg(QDir::toNativeSeparators(probe));
    }
    return QString();
}
