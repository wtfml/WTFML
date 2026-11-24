/**
 * @file TransferPlanner.cpp
 * @brief TransferPlanner.hpp 的实现：只读盘、只算，给迁移 / 升级副本一份范围表。
 */

#include "operations/TransferPlanner.hpp"
#include <QCoreApplication>

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include "TextMarks.hpp"
#include "tools/FileHash.hpp"

namespace TransferPlanner {

namespace {

/// 一块内容在盘上占多少（只数文件、不读内容）。
struct TreeSize {
    int files = 0;
    qint64 bytes = 0;
};

TreeSize scanTree(const QString &root) {
    TreeSize total;
    QStringList stack;
    stack.append(root);
    while (!stack.isEmpty()) {
        const QDir dir(stack.takeLast());
        const QFileInfoList files =
            dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDir::NoSort);
        for (const QFileInfo &file : files) {
            ++total.files;
            total.bytes += file.size();
        }
        const QFileInfoList subs =
            dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::NoSort);
        for (const QFileInfo &sub : subs) {
            if (sub.isJunction() || sub.isSymLink()) continue;
            stack.append(sub.absoluteFilePath());
        }
    }
    return total;
}

TreeSize measure(const QString &absolutePath, bool isDirectory) {
    const QFileInfo info(absolutePath);
    if (!info.exists()) return TreeSize();
    if (isDirectory && info.isDir()) return scanTree(absolutePath);
    // 不是目录的（`options.txt` / `servers.dat`）按单文件算；万一它真是个目录，也照样数。
    if (info.isDir()) return scanTree(absolutePath);
    TreeSize one;
    one.files = 1;
    one.bytes = info.size();
    return one;
}

QString joinSize(int files, qint64 bytes) {
    return QCoreApplication::translate("TransferPlanner", "%1 个文件 / %2").arg(files).arg(FileHash::fileSizeText(bytes));
}

}  // namespace

Compatibility compatibility(const QString &sourceGameVersion, const QString &sourceLoaderName,
                            const QString &targetGameVersion, const QString &targetLoaderName) {
    const bool sourceVersionKnown = !sourceGameVersion.trimmed().isEmpty();
    const bool targetVersionKnown = !targetGameVersion.trimmed().isEmpty();
    // 「原版」是一个**读得出来**的结论（加载器那一格空着且版本 json 里没有 inheritsFrom），
    // 所以这里不把空加载器当成"认不出来"；只有版本那一格空着才进入 Unknown。
    if (!sourceVersionKnown || !targetVersionKnown) return Compatibility::Unknown;
    const bool sameGame = sourceGameVersion.compare(targetGameVersion, Qt::CaseInsensitive) == 0;
    const bool sameLoader = sourceLoaderName.compare(targetLoaderName, Qt::CaseInsensitive) == 0;
    if (sameGame && sameLoader) return Compatibility::Same;
    if (!sameGame && !sameLoader) return Compatibility::BothDiffer;
    if (!sameGame) return Compatibility::GameVersionDiffers;
    return Compatibility::LoaderDiffers;
}

QString compatibilityName(Compatibility compatibility) {
    switch (compatibility) {
    case Compatibility::Same: return QCoreApplication::translate("TransferPlanner", "一致");
    case Compatibility::GameVersionDiffers: return QCoreApplication::translate("TransferPlanner", "游戏版本不同");
    case Compatibility::LoaderDiffers: return QCoreApplication::translate("TransferPlanner", "加载器不同");
    case Compatibility::BothDiffer: return QCoreApplication::translate("TransferPlanner", "版本与加载器都不同");
    case Compatibility::Unknown: return QCoreApplication::translate("TransferPlanner", "未知");
    }
    return QString();
}

QString compatibilityText(Compatibility compatibility, const QString &sourceGameVersion,
                          const QString &sourceLoaderName, const QString &targetGameVersion,
                          const QString &targetLoaderName) {
    const auto loaderWord = [](const QString &name) {
        return name.isEmpty() ? QCoreApplication::translate("TransferPlanner", "原版") : name;
    };
    switch (compatibility) {
    case Compatibility::Same:
        return QCoreApplication::translate("TransferPlanner", "目标版本（%1 / %2）与源一致：模组与配置可以原样迁移。")
            .arg(sourceGameVersion, loaderWord(sourceLoaderName));
    case Compatibility::GameVersionDiffers:
        return QCoreApplication::translate("TransferPlanner", "目标游戏版本为 %1，源版本为 %2。模组和部分配置可能不兼容，默认不迁移模组。")
            .arg(targetGameVersion, sourceGameVersion);
    case Compatibility::LoaderDiffers:
        return QCoreApplication::translate("TransferPlanner", "源加载器为 %2，目标加载器为 %1，游戏版本均为 %3。模组和相关配置可能不兼容，默认不迁移模组。")
            .arg(loaderWord(targetLoaderName), loaderWord(sourceLoaderName), sourceGameVersion);
    case Compatibility::BothDiffer:
        return QCoreApplication::translate("TransferPlanner", "目标版本和加载器为 %1 / %2，源配置为 %3 / %4。模组和部分配置可能不兼容，默认不迁移模组。")
            .arg(targetGameVersion, loaderWord(targetLoaderName), sourceGameVersion,
                 loaderWord(sourceLoaderName));
    case Compatibility::Unknown:
        return QCoreApplication::translate("TransferPlanner", "无法识别源或目标的游戏版本或加载器。默认不迁移模组，可确认兼容性后手动勾选。");
    }
    return QString();
}

bool neverDefaultSelected(const QString &key) {
    return key == QStringLiteral("screenshots") || key == QStringLiteral("logs")
           || key == QStringLiteral("crashreports");
}

QStringList Plan::defaultSelectedKeys() const {
    QStringList keys;
    for (const Item &item : items) {
        if (item.exists && item.defaultSelected) keys << item.key;
    }
    return keys;
}

int Plan::selectedFileCount(const QStringList &keys) const {
    int files = 0;
    for (const Item &item : items) {
        if (item.exists && keys.contains(item.key)) files += item.fileCount;
    }
    return files;
}

qint64 Plan::selectedBytes(const QStringList &keys) const {
    qint64 bytes = 0;
    for (const Item &item : items) {
        if (item.exists && keys.contains(item.key)) bytes += item.bytes;
    }
    return bytes;
}

QString Plan::headline() const {
    if (!ok) return problem;
    const int count = defaultSelectedKeys().size();
    const QString prefix =
        upgrade ? QCoreApplication::translate("TransferPlanner", "升级副本计划：%1").arg(compatibilityName(compatibility))
                : QCoreApplication::translate("TransferPlanner", "迁移计划：%1").arg(compatibilityName(compatibility));
    const qint64 bytes = selectedBytes(defaultSelectedKeys());
    const int files = selectedFileCount(defaultSelectedKeys());
    return QCoreApplication::translate("TransferPlanner", "%1 / 默认迁移 %2 项（%3）").arg(prefix).arg(count).arg(joinSize(files, bytes));
}

QString Plan::detailText() const {
    if (!ok) return problem;
    QStringList lines;
    lines << headline();
    lines << compatibilitySentence;
    lines << QString();
    lines << (upgrade ? QCoreApplication::translate("TransferPlanner", "将复制以下内容到升级副本：")
                      : QCoreApplication::translate("TransferPlanner", "本次迁移包括："));
    for (const Item &item : items) {
        if (!item.exists) {
            lines << QCoreApplication::translate("TransferPlanner", "• %1：当前实例无此项").arg(item.label);
            continue;
        }
        // 勾选框记号：`☑` / `☐` 默认界面字体里没有（首次排版约 280 ms 的字体回退），
        // 换成同一份字体里有的 `■` / `□`（见 `ui/TextMarks.hpp` 头注释）。
        lines << QCoreApplication::translate("TransferPlanner", "%1 %2（%3）")
                     .arg(item.defaultSelected ? TextMarks::checked() : TextMarks::unchecked(),
                          item.label, joinSize(item.fileCount, item.bytes));
        if (!item.note.isEmpty()) lines << QStringLiteral("    %1").arg(item.note);
    }
    lines << QString();
    lines << QCoreApplication::translate("TransferPlanner", "日志、截图和崩溃报告默认不迁移，可手动选择。");
    lines << QCoreApplication::translate("TransferPlanner", "迁移只增加文件、不删除、不覆盖目标里已有的同名文件；" "两边的游戏都必须先关掉。");
    return lines.join(QLatin1Char('\n'));
}

QString Plan::confirmText(const QStringList &keys) const {
    const int files = selectedFileCount(keys);
    const qint64 bytes = selectedBytes(keys);
    QString text = upgrade
                       ? QCoreApplication::translate("TransferPlanner", "将创建升级副本，并将选中的 %1 项（%2）迁移进去。")
                             .arg(QString::number(keys.size()), joinSize(files, bytes))
                       : QCoreApplication::translate("TransferPlanner", "将把选中的 %1 项（%2）迁移到目标实例。")
                             .arg(QString::number(keys.size()), joinSize(files, bytes));
    text += QCoreApplication::translate("TransferPlanner", "\n\n只增加文件，不删除、不覆盖目标里已有的同名文件。");
    if (compatibility != Compatibility::Same) {
        text += QCoreApplication::translate("TransferPlanner", "\n\n兼容性结论：%1").arg(compatibilityName(compatibility));
    }
    return text;
}

Plan plan(const Inputs &inputs) {
    Plan result;
    result.upgrade = inputs.upgrade;

    const QString sourceDir = inputs.sourceDir.trimmed();
    if (sourceDir.isEmpty() || !QDir(sourceDir).exists()) {
        result.problem = QCoreApplication::translate("TransferPlanner", "未找到源实例目录：%1").arg(
            sourceDir.isEmpty() ? QCoreApplication::translate("TransferPlanner", "（未提供）") : QDir::toNativeSeparators(sourceDir));
        return result;
    }

    result.compatibility = compatibility(inputs.sourceGameVersion, inputs.sourceLoaderName,
                                         inputs.targetGameVersion, inputs.targetLoaderName);
    result.compatibilitySentence = compatibilityText(
        result.compatibility, inputs.sourceGameVersion, inputs.sourceLoaderName,
        inputs.targetGameVersion, inputs.targetLoaderName);
    // 加载器小版本不同：结论仍是 Same（模组默认勾选），但把事实说出来。
    if (result.compatibility == Compatibility::Same && !inputs.sourceLoaderVersion.isEmpty()
        && !inputs.targetLoaderVersion.isEmpty()
        && inputs.sourceLoaderVersion != inputs.targetLoaderVersion) {
        result.compatibilitySentence +=
            QCoreApplication::translate("TransferPlanner", "加载器小版本不同（源 %1 / 目标 %2），通常不影响启动；若启动失败，请先检查此项。")
                .arg(inputs.sourceLoaderVersion, inputs.targetLoaderVersion);
    }

    struct Row {
        const char *key;
        const char *label;
        const char *relativePath;
        bool isDirectory;
    };
    // 顺序就是界面上的顺序：先把"真的在玩的东西"摆前面，记录类垫底。
    static const Row rows[] = {
        {"saves", QT_TRANSLATE_NOOP("TransferPlanner", "存档"), "saves", true},
        {"mods", QT_TRANSLATE_NOOP("TransferPlanner", "模组"), "mods", true},
        {"config", QT_TRANSLATE_NOOP("TransferPlanner", "模组配置"), "config", true},
        {"defaultconfigs", QT_TRANSLATE_NOOP("TransferPlanner", "默认配置"), "defaultconfigs", true},
        {"resourcepacks", QT_TRANSLATE_NOOP("TransferPlanner", "资源包"), "resourcepacks", true},
        {"shaderpacks", QT_TRANSLATE_NOOP("TransferPlanner", "光影包"), "shaderpacks", true},
        {"options", QT_TRANSLATE_NOOP("TransferPlanner", "游戏设置"), "options.txt", false},
        {"servers", QT_TRANSLATE_NOOP("TransferPlanner", "服务器列表"), "servers.dat", false},
        {"structure", QT_TRANSLATE_NOOP("TransferPlanner", "结构文件"), "structures", true},
        {"screenshots", QT_TRANSLATE_NOOP("TransferPlanner", "截图"), "screenshots", true},
        {"logs", QT_TRANSLATE_NOOP("TransferPlanner", "日志"), "logs", true},
        {"crashreports", QT_TRANSLATE_NOOP("TransferPlanner", "崩溃报告"), "crash-reports", true},
    };

    for (const Row &row : rows) {
        Item item;
        item.key = QString::fromLatin1(row.key);
        item.label = QCoreApplication::translate("TransferPlanner", row.label);
        item.relativePath = QString::fromLatin1(row.relativePath);
        item.isDirectory = row.isDirectory;
        const QString absolutePath = sourceDir + QLatin1Char('/') + item.relativePath;
        item.exists = QFileInfo::exists(absolutePath);
        if (item.exists) {
            const TreeSize size = measure(absolutePath, item.isDirectory);
            item.fileCount = size.files;
            item.bytes = size.bytes;
        }
        // 默认勾选：记录类永不默认；模组只看兼容结论；其余有就勾。
        if (!item.exists || neverDefaultSelected(item.key)) {
            item.defaultSelected = false;
        } else if (item.key == QStringLiteral("mods")) {
            item.defaultSelected = result.compatibility == Compatibility::Same;
        } else {
            item.defaultSelected = true;
        }
        if (item.key == QStringLiteral("mods") && item.exists
            && result.compatibility != Compatibility::Same) {
            item.note = QCoreApplication::translate("TransferPlanner", "目标版本 / 加载器与源不一致，模组可能无法启动；" "确认需要原样迁移后再勾选。");
        } else if (neverDefaultSelected(item.key) && item.exists) {
            item.note = QCoreApplication::translate("TransferPlanner", "这些文件用于记录，默认不迁移。");
        } else if (result.compatibility == Compatibility::Unknown && item.exists
                   && item.key != QStringLiteral("saves")) {
            item.note = QCoreApplication::translate("TransferPlanner", "无法识别目标版本。确认兼容性后再选择是否迁移。");
        }
        result.items.append(item);
    }

    result.ok = true;
    return result;
}

}  // namespace TransferPlanner
