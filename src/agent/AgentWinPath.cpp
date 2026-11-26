/**
 * @file AgentWinPath.cpp
 * @brief 相对路径规则 + Win32 句柄级校验（见 .hpp 的安全边界）。
 */

#include "AgentWinPath.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace AgentWinPath {
namespace {

const QChar kSlash(0x2F);
const QChar kBackslash(0x5C);

QString canonicalTextPath(const QString &path) {
    QString value = QDir::fromNativeSeparators(path);
    while (value.endsWith(kSlash)) value.chop(1);
    return value;
}

QString longPathPrefix() {
    return QString(2, kBackslash) + QLatin1Char('?') + kBackslash;
}

QString uncPrefix() {
    return QString(2, kBackslash) + QLatin1Char('?') + kBackslash +
           QStringLiteral("UNC") + kBackslash;
}

QString stripWinPrefix(QString path) {
    const QString unc = uncPrefix();
    const QString lp = longPathPrefix();
    if (path.startsWith(unc)) {
        path = QString(2, kBackslash) + path.mid(unc.size());
    } else if (path.startsWith(lp)) {
        path = path.mid(lp.size());
    }
    return canonicalTextPath(path);
}

bool reservedSegment(const QString &segment, QString *problem) {
    QString base = segment;
    const int dot = base.indexOf(QLatin1Char('.'));
    if (dot >= 0) base = base.left(dot);
    const QString upper = base.toUpper();
    static const QStringList fixed{QStringLiteral("CON"), QStringLiteral("PRN"),
                                   QStringLiteral("AUX"), QStringLiteral("NUL")};
    if (fixed.contains(upper)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "路径使用 Windows 保留设备名：%1").arg(segment);
        return true;
    }
    static const QRegularExpression com(QStringLiteral("^(COM|LPT)[1-9]$"));
    if (com.match(upper).hasMatch()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "路径使用 Windows 保留设备名：%1").arg(segment);
        return true;
    }
    return false;
}

#if defined(Q_OS_WIN)
struct HandleCloser {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~HandleCloser() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

bool fillInfo(HANDLE handle, const QString &expectedPath, Info *info, QString *problem) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "无法读取文件属性。");
        return false;
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "路径包含重解析点（junction 或符号链接），已拒绝。");
        return false;
    }
    wchar_t finalBuffer[32768] = {};
    const DWORD length = GetFinalPathNameByHandleW(handle, finalBuffer, 32767, FILE_NAME_NORMALIZED);
    if (length == 0 || length >= 32767) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "无法读取最终路径。");
        return false;
    }
    const QString finalPath = stripWinPrefix(QString::fromWCharArray(finalBuffer, int(length)));
    const QString expected = canonicalTextPath(expectedPath);
    if (QString::compare(finalPath, expected, Qt::CaseInsensitive) != 0) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentWinPath", "最终路径与预期不一致，可能经 junction 或符号链接指向别处：%1")
                           .arg(finalPath);
        }
        return false;
    }

    FILE_ID_INFO fileId{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &fileId, sizeof(fileId))) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "无法读取文件 ID。");
        return false;
    }
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "无法读取文件时间。");
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "无法读取文件大小。");
        return false;
    }
    info->finalPath = finalPath;
    info->exists = true;
    info->isDirectory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info->reparse = false;
    info->volumeSerial = fileId.VolumeSerialNumber;
    info->fileId = QByteArray(reinterpret_cast<const char *>(fileId.FileId.Identifier),
                              int(sizeof(fileId.FileId.Identifier)));
    info->size = static_cast<qint64>(size.QuadPart);
    info->mtimeMs = (static_cast<qint64>(basic.LastWriteTime.QuadPart) / 10000) - 11644473600000LL;
    return true;
}

bool openAndValidate(const QString &path, bool directory, Info *info, QString *problem) {
    const QString native = QDir::toNativeSeparators(path);
    DWORD flags = FILE_READ_ATTRIBUTES | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0) |
                  FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentWinPath", "无法打开路径（错误码 %1）：%2").arg(int(GetLastError())).arg(path);
        }
        return false;
    }
    HandleCloser closer{handle};
    return fillInfo(handle, path, info, problem);
}
#endif

}  // namespace

bool normalizeRelative(const QString &relative, QString *normalized, QString *problem) {
    const auto reject = [problem](const QString &text) {
        if (problem != nullptr) *problem = text;
        return false;
    };
    if (relative.isEmpty()) return reject(QCoreApplication::translate("AgentWinPath", "相对路径为空。"));
    if (relative.size() > 1024) return reject(QCoreApplication::translate("AgentWinPath", "相对路径过长。"));
    QString value = QDir::fromNativeSeparators(relative);
    if (value.startsWith(kSlash) || value.startsWith(kBackslash) || QDir::isAbsolutePath(value) ||
        QRegularExpression(QStringLiteral("^[A-Za-z]:")).match(value).hasMatch() ||
        (value.size() >= 2 && value.at(0) == kBackslash && value.at(1) == kBackslash)) {
        return reject(QCoreApplication::translate("AgentWinPath", "只接受实例目录内的相对路径。"));
    }
    if (value.contains(QLatin1Char(':'))) return reject(QCoreApplication::translate("AgentWinPath", "路径不能包含冒号（ADS）。"));
    for (const QChar ch : value) {
        if (ch.unicode() < 0x20 || ch == QLatin1Char('<') || ch == QLatin1Char('>') ||
            ch == QLatin1Char('"') || ch == QLatin1Char('|') || ch == QLatin1Char('?') ||
            ch == QLatin1Char('*')) {
            return reject(QCoreApplication::translate("AgentWinPath", "路径包含非法字符。"));
        }
    }
    const QStringList parts = value.split(kSlash);
    QStringList cleaned;
    for (const QString &part : parts) {
        if (part.isEmpty()) continue;
        if (part == QStringLiteral("..") || part == QStringLiteral(".")) {
            return reject(QCoreApplication::translate("AgentWinPath", "路径不能包含 . 或 .. 段。"));
        }
        if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))) {
            return reject(QCoreApplication::translate("AgentWinPath", "路径段不能以点或空格结尾：%1").arg(part));
        }
        if (reservedSegment(part, problem)) return false;
        cleaned.append(part);
    }
    if (cleaned.isEmpty()) return reject(QCoreApplication::translate("AgentWinPath", "相对路径为空。"));
    if (normalized != nullptr) *normalized = cleaned.join(kSlash);
    return true;
}

bool resolveRoot(const QString &root, Info *info, QString *problem) {
#if defined(Q_OS_WIN)
    return openAndValidate(root, true, info, problem);
#else
    Q_UNUSED(info);
    if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "非 Windows 系统尚不支持句柄级路径校验。");
    return false;
#endif
}

bool resolveExisting(const QString &root, const QString &relative, bool expectDirectory,
                     Info *info, QString *problem) {
    QString normalized;
    if (!normalizeRelative(relative, &normalized, problem)) return false;
    const QString path = QDir::cleanPath(root + kSlash + normalized);
#if defined(Q_OS_WIN)
    if (!openAndValidate(path, expectDirectory, info, problem)) return false;
    if (expectDirectory && !info->isDirectory) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "目标不是目录。");
        return false;
    }
    return true;
#else
    Q_UNUSED(expectDirectory);
    Q_UNUSED(info);
    if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "非 Windows 系统尚不支持句柄级路径校验。");
    return false;
#endif
}

bool resolveParent(const QString &root, const QString &relative, Info *info, QString *problem) {
    QString normalized;
    if (!normalizeRelative(relative, &normalized, problem)) return false;
    QStringList parts = normalized.split(kSlash);
    parts.removeLast();
    const QString parentRelative = parts.join(kSlash);
    if (parentRelative.isEmpty()) {
#if defined(Q_OS_WIN)
        return openAndValidate(root, true, info, problem);
#else
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentWinPath", "非 Windows 系统尚不支持句柄级路径校验。");
        return false;
#endif
    }
    return resolveExisting(root, parentRelative, true, info, problem);
}

QString identityToken(const Info &info) {
    return QStringLiteral("v1:%1:%2:%3:%4")
        .arg(info.volumeSerial, 0, 16)
        .arg(QString::fromLatin1(info.fileId.toHex()))
        .arg(info.size)
        .arg(info.mtimeMs);
}

bool verifyIdentityToken(const QString &token, const Info &info) {
    const QStringList parts = token.split(QLatin1Char(':'));
    if (parts.size() != 5 || parts.at(0) != QStringLiteral("v1")) return false;
    bool volumeOk = false;
    const quint64 volume = parts.at(1).toULongLong(&volumeOk, 16);
    if (!volumeOk || volume != info.volumeSerial) return false;
    if (parts.at(2).toLower() != QString::fromLatin1(info.fileId.toHex())) return false;
    bool sizeOk = false;
    const qint64 size = parts.at(3).toLongLong(&sizeOk);
    if (!sizeOk || size != info.size) return false;
    bool mtimeOk = false;
    const qint64 mtime = parts.at(4).toLongLong(&mtimeOk);
    return mtimeOk && mtime == info.mtimeMs;
}

bool isProtectedRelative(const QString &relative) {
    QString normalized;
    if (!normalizeRelative(relative, &normalized, nullptr)) return true;
    const QString first = normalized.section(kSlash, 0, 0).toLower();
    return first == QStringLiteral("versions") || first == QStringLiteral("libraries") ||
           first == QStringLiteral("assets") || first == QStringLiteral("java") ||
           first == QStringLiteral("servers");
}

bool isTextExtension(const QString &relative) {
    const QString suffix = QFileInfo(relative).suffix().toLower();
    if (suffix.isEmpty()) return false;
    static const QStringList text{QStringLiteral("txt"), QStringLiteral("log"),
                                  QStringLiteral("json"), QStringLiteral("json5"),
                                  QStringLiteral("toml"), QStringLiteral("yaml"),
                                  QStringLiteral("yml"), QStringLiteral("properties"),
                                  QStringLiteral("cfg"), QStringLiteral("conf"),
                                  QStringLiteral("ini"), QStringLiteral("md"),
                                  QStringLiteral("xml"), QStringLiteral("csv"),
                                  QStringLiteral("tsv")};
    return text.contains(suffix);
}

bool looksBinary(const QByteArray &bytes) {
    if (bytes.contains('\0')) return true;
    const int sample = qMin<int>(bytes.size(), 8192);
    int controls = 0;
    for (int i = 0; i < sample; ++i) {
        const uchar ch = static_cast<uchar>(bytes.at(i));
        if (ch < 0x09 || (ch > 0x0d && ch < 0x20)) ++controls;
    }
    return sample > 0 && controls * 100 > sample * 2;
}

}  // namespace AgentWinPath
