/**
 * @file ThemeImage.cpp
 * @brief ThemeImage 的实现：读一张图、缩到长边 2560 以内、写进 cache/theme。
 */

#include "ThemeImage.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QObject>
#include <QSaveFile>
#include <QStringList>

#include "AppPaths.hpp"
#include "ImageDecode.hpp"

namespace ThemeImage {
namespace {

/// 这个槽位名能不能当文件名。**只挡路径分隔符和空串**：槽位名来自 `Theme::partKey()`
/// （我们自己的那张表，全是 ASCII 小写字母），这一道是防"以后有人拿用户输入喂进来"。
bool isSafeSlot(const QString &slot) {
    if (slot.isEmpty() || slot.contains(QLatin1Char('/')) || slot.contains(QLatin1Char('\\')) ||
        slot.contains(QLatin1String(".."))) {
        return false;
    }
    return true;
}

/// 盘上已经收着的那一份（`<槽位>.jpg` 或 `<槽位>.png`）。没有就返回空串。
QString existingFile(const QString &slot) {
    const QString dir = AppPaths::themeImageDir();
    const QStringList candidates{QStringLiteral(".png"), QStringLiteral(".jpg")};
    for (const QString &suffix : candidates) {
        const QString path = dir + QLatin1Char('/') + slot + suffix;
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return QString();
}

}  // namespace

QString store(const QString &slot, const QString &sourcePath, QString *errorOut) {
    if (errorOut != nullptr) {
        errorOut->clear();
    }
    const auto fail = [errorOut](const QString &reason) {
        if (errorOut != nullptr) {
            *errorOut = reason;
        }
        return QString();
    };

    if (!isSafeSlot(slot)) {
        return fail(QStringLiteral("内部错误：这个槽位的名字不能当文件名用。"));
    }
    const QFileInfo source(sourcePath);
    if (!source.isFile()) {
        return fail(QStringLiteral("这个文件读不到：%1").arg(sourcePath));
    }
    if (source.size() > kMaxSourceBytes) {
        return fail(QStringLiteral("这张图太大了（%1 MB），换一张小一点的。")
                        .arg(QString::number(source.size() / 1024.0 / 1024.0, 'f', 1)));
    }

    // 先问尺寸（这一步不解码），再按目标尺寸解码 —— 见头文件那段 NOTE。
    QImageReader reader(sourcePath);
    reader.setAutoTransform(true);  // 手机拍的竖图靠 EXIF 里的方向标，不认它就会躺着
    const QSize original = reader.size();
    if (original.isValid() && qMax(original.width(), original.height()) > kMaxSide) {
        reader.setScaledSize(original.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    if (image.isNull()) {
        // Qt 认不得这一份字节时还有第二条路：WIC（`ImageDecode`）。**这条必须有**：
        // 这份 Qt 里没有 webp 解码器（`plugins/imageformats/` 只有 gif / ico / jpeg /
        // svg，见 ImageDecode.hpp 那段），而用户从浏览器存下来的图十有八九是 webp ——
        // 挑图那个对话框的筛选里就写着 `*.webp`，那一格点下去要是回一句"这不是一张
        // 能打开的图片"，那是我们自己的筛子撒的谎。
        //
        // NOTE: 这条路只能**先整串读进来、解出全图、再缩**（WIC 那条口吃的是字节串），
        // 所以它比上面那条多占一张全尺寸位图的内存。上面那条（Qt 自己认得的那一大
        // 半）走的才是"在解码里顺手缩"。这一发是用户点名的操作，这点内存换"打得开"
        // 值得；尺寸上限那一刀照样在（下面是同一个 kMaxSide）。
        QFile bytes(sourcePath);
        if (bytes.open(QIODevice::ReadOnly)) {
            // 这一档的上限比图标那一档宽一档：这是**用户自己挑的本机文件**（不是网上
            // 来的那一类），一张 24 MP 的照片完全正常；默认那 16 MP 会把它挡在门外。
            image = ImageDecode::decode(bytes.readAll(),
                                        ImageDecode::Limits{8192, 64LL * 1024 * 1024});
            if (!image.isNull() && qMax(image.width(), image.height()) > kMaxSide) {
                image = image.scaled(kMaxSide, kMaxSide, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
        }
    }
    if (image.isNull()) {
        return fail(QStringLiteral("这不是一张能打开的图片：%1").arg(source.fileName()));
    }

    QDir().mkpath(AppPaths::themeImageDir());
    // 扩展名跟着**内容**走，不跟着原文件名：一张 .jpeg 里没有透明通道就存成 .jpg，
    // 省下一半以上的地方（背景图这一档用不着 PNG 那点无损）。
    const bool keepAlpha = image.hasAlphaChannel();
    const QString suffix = keepAlpha ? QStringLiteral(".png") : QStringLiteral(".jpg");
    const QString target = AppPaths::themeImageDir() + QLatin1Char('/') + slot + suffix;

    // 先写新的、提交成功之后才删旧的那一份。不能用 `QImage::save(QString)`（内部是普通
    // QFile）：写不进去（盘满 / 没权限 / 进程正好在这一刻被杀）时旧文件已经被截断了 ——
    // 与头文件那句"失败时不动盘上原来那一份"正好相反。写侧走 `QSaveFile`
    // （同目录临时文件 + 原子改名），旧的一份留到提交之后才清。
    const QString stale = existingFile(slot);
    QSaveFile file(target);
    bool ok = file.open(QIODevice::WriteOnly);
    if (ok) {
        ok = keepAlpha ? image.save(&file, "PNG") : image.save(&file, "JPG", 92);
    }
    if (ok) {
        ok = file.commit();
    }
    if (!ok) {
        file.cancelWriting();
        return fail(QStringLiteral("写不进 %1，检查一下磁盘权限。")
                        .arg(QDir::toNativeSeparators(AppPaths::themeImageDir())));
    }
    if (!stale.isEmpty() && stale != target) {
        QFile::remove(stale);
    }
    return target;
}

void remove(const QString &slot) {
    if (!isSafeSlot(slot)) {
        return;
    }
    const QString path = existingFile(slot);
    if (!path.isEmpty()) {
        QFile::remove(path);
    }
}

}  // namespace ThemeImage
