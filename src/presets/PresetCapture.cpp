/**
 * @file PresetCapture.cpp
 * @brief 实现 PresetCapture.hpp（提取那一趟）。取舍都写在头文件里。
 *
 * 【这一层不做的事】
 *
 * 不建预设目录、不写 `manifest.json`、不改下标 —— 那些是 `PresetStore` 的活。这里只回答
 * 一个问题："这个实例现在这一份，能收成哪几段？"收出来的文件（`config/` 那几组）会落在
 * 调用方指定的 `files/` 里，这是提取本身不可分割的一部分（`GenericFiles` 的 `files/`
 * 就是那些文件的落点），没法也不该拖到调用方再办。
 */

#include "PresetCapture.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include "DataPackPreset.hpp"
#include "GenericFiles.hpp"
#include "InstalledModInventory.hpp"
#include "MalilibKeys.hpp"
#include "ResourcePackPreset.hpp"
#include "util/WriteDirectoryGuard.hpp"
#include "ServerListPreset.hpp"
#include "VanillaOptions.hpp"
#include "manage/ServerListFile.hpp"

namespace Presets {

namespace {

[[maybe_unused]] QString captureText(const char *source) { return QCoreApplication::translate("Presets", source); }

/// `options.txt` 的这个上限只是"别整块读进内存"的闸：真机上它是几十 KB。超过这个数
/// 我们不去猜它是不是坏文件，直接报给用户（宁可不收，也不收一半）。
constexpr qint64 kOptionsMaxBytes = 32LL * 1024 * 1024;

/// 读一整份文件。`ok = false` 时 `problem` 里是一句人话。
bool readWholeFile(const QString &path, QByteArray *out, QString *problem) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *problem = QCoreApplication::translate("Presets", "无法读取「%1」：%2").arg(path, file.errorString());
        return false;
    }
    *out = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        *problem = QCoreApplication::translate("Presets", "读取「%1」中断：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}

QString groupDisplayName(FileGroup group) {
    switch (group) {
    case FileGroup::Config:
        return QCoreApplication::translate("Presets", "配置文件（config/）");
    case FileGroup::DefaultConfigs:
        return QCoreApplication::translate("Presets", "默认配置（defaultconfigs/）");
    case FileGroup::ResourcePacks:
        return QCoreApplication::translate("Presets", "资源包（resourcepacks/）");
    case FileGroup::ShaderPacks:
        return QCoreApplication::translate("Presets", "光影包（shaderpacks/）");
    }
    return QString();
}

/// 收一个通用文件组：扫 → 拷 → 出段。失败时把原因写进 `problems`，别的组照收。
bool captureFileGroup(const CaptureRequest &request, FileGroup group,
                      QList<PresetSection> *sections, CaptureReport *report) {
    QList<GenericItem> items;
    QStringList refused;
    QString why;
    if (!scanFileGroup(request.instanceRoot, group, &items, &refused, &why)) {
        report->problems.append(QCoreApplication::translate("Presets", "%1 无法读取：%2")
                                    .arg(groupDisplayName(group), why));
        return false;
    }
    for (const QString &one : refused) {
        report->problems.append(QCoreApplication::translate("Presets", "%1 中有内容未收集：%2")
                                    .arg(groupDisplayName(group), one));
    }
    if (items.isEmpty()) {
        report->notes.append(QCoreApplication::translate("Presets", "%1 为空。").arg(groupDisplayName(group)));
        return false;
    }

    // 先扫后拷：`sectionsFromFileGroup()` 要的是"这一组里有哪些项"，而那个清单在拷贝
    // 前后是同一份（拷贝只往预设那一侧写，不动实例）。次序反过来也行，但先扫能让
    // "扫描就失败"的那一趟连一个文件都不碰。
    const GenericReport extracted =
        extractFileGroup(request.instanceRoot, request.presetFilesRoot, group);
    if (!extracted.ok) {
        report->problems.append(QCoreApplication::translate("Presets", "%1 收集失败：%2")
                                    .arg(groupDisplayName(group), extracted.problem));
        return false;
    }

    QList<PresetSection> made;
    QString sectionWhy;
    if (!sectionsFromFileGroup(group, items, ApplyMode::CopyIfMissing, &made,
                                             &sectionWhy)) {
        report->problems.append(QCoreApplication::translate("Presets", "%1 收集失败：%2")
                                    .arg(groupDisplayName(group), sectionWhy));
        return false;
    }
    sections->append(made);
    switch (group) {
    case FileGroup::Config:
        report->configItems += static_cast<int>(items.size());
        break;
    case FileGroup::DefaultConfigs:
        report->defaultConfigItems += static_cast<int>(items.size());
        break;
    case FileGroup::ResourcePacks:
        report->resourcePackItems += static_cast<int>(items.size());
        break;
    case FileGroup::ShaderPacks:
        report->shaderPackItems += static_cast<int>(items.size());
        break;
    }
    return true;
}

}  // namespace

CaptureReport captureFromInstance(const CaptureRequest &request) {
    CaptureReport report;

    if (request.instanceRoot.trimmed().isEmpty() ||
        !QFileInfo(request.instanceRoot).isDir()) {
        report.problem = QCoreApplication::translate("Presets", "未找到实例目录，无法提取预设。");
        return report;
    }
    if (request.presetFilesRoot.trimmed().isEmpty()) {
        report.problem = QCoreApplication::translate("Presets", "未指定预设的 files 目录。");
        return report;
    }
    // 这一趟会往预设库里那一份的 `files/` 底下写东西（下面第一件事就是建目录）。
    // 和预设库自己的写入口同一把锁：另一份启动器持着这个数据根时一个字节都不写
    // （不然两份启动器会往同一份预设里交错拷文件）。
    {
        QString lockProblem;
        if (!WriteDirectoryGuard::acquire(request.presetFilesRoot, &lockProblem)) {
            report.problem = lockProblem;
            return report;
        }
    }
    if (!QDir().mkpath(request.presetFilesRoot)) {
        report.problem = QCoreApplication::translate("Presets", "无法创建预设文件目录：%1").arg(request.presetFilesRoot);
        return report;
    }
    if (request.selection.isEmpty()) {
        // 一样都没勾：这是一趟"什么都不做"，不是失败。调用方不该把这一趟当成"收成了
        // 一份空预设"存下去 —— 空段表和"一份新预设"在界面上是两件事。
        report.ok = true;
        report.notes.append(QCoreApplication::translate("Presets", "未选择要提取的内容。"));
        return report;
    }

    // ------------------------------------------------------------------
    // 1. options.txt（两段：基础设置 / 标准按键）
    // ------------------------------------------------------------------
    if (request.selection.vanillaOptions || request.selection.vanillaKeys) {
        const QString path = request.instanceRoot + QStringLiteral("/options.txt");
        const QFileInfo info(path);
        if (!info.exists()) {
            report.notes.append(captureText("实例里还没有 options.txt（游戏还没跑过），"
                                            "游戏设置和按键这一趟没有可收的。"));
        } else if (!info.isFile()) {
            report.problems.append(QCoreApplication::translate("Presets", "「%1」不是普通文件，未提取游戏设置。")
                                       .arg(path));
        } else if (info.size() > kOptionsMaxBytes) {
            report.problems.append(captureText("「%1」大到不像是 options.txt（%2 字节），"
                                               "这一趟没收游戏设置。")
                                       .arg(path)
                                       .arg(info.size()));
        } else {
            QByteArray bytes;
            QString why;
            if (!readWholeFile(path, &bytes, &why)) {
                report.problems.append(why);
            } else {
                const OptionsFile file = parseOptions(bytes);
                const QSet<QString> selected = defaultSelection(file);
                const QList<PresetSection> made = sectionsFromOptions(file, selected);
                for (const PresetSection &section : made) {
                    if (section.kind == SectionKind::VanillaOptions) {
                        if (!request.selection.vanillaOptions || section.items.isEmpty()) {
                            continue;
                        }
                        report.optionKeys += static_cast<int>(section.items.size());
                    } else if (section.kind == SectionKind::VanillaKeys) {
                        if (!request.selection.vanillaKeys || section.items.isEmpty()) {
                            continue;
                        }
                        report.keybindings += static_cast<int>(section.items.size());
                    } else {
                        continue;
                    }
                    report.sections.append(section);
                }
                if (report.optionKeys == 0 && request.selection.vanillaOptions) {
                    report.notes.append(QCoreApplication::translate("Presets", "options.txt 中无可收集的游戏设置。"));
                }
                if (report.keybindings == 0 && request.selection.vanillaKeys) {
                    report.notes.append(QCoreApplication::translate("Presets", "options.txt 中无可收集的按键。"));
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // 2. servers.dat
    // ------------------------------------------------------------------
    if (request.selection.servers) {
        const QString path = request.instanceRoot + QStringLiteral("/servers.dat");
        QList<NbtUtil::ServerEntry> entries;
        QString why;
        const NbtUtil::Status status = ServerListFile::read(path, &entries, &why);
        if (status == NbtUtil::Status::Missing) {
            report.notes.append(QCoreApplication::translate("Presets", "实例中无服务器列表文件（servers.dat）。"));
        } else if (status == NbtUtil::Status::Unreadable) {
            report.problems.append(QCoreApplication::translate("Presets", "无法读取服务器列表：%1").arg(why));
        } else if (entries.isEmpty()) {
            // 盘上有一份好好的、但一条都没有 —— 收一段空段下来没有意义（应用"追加 0 条"
            // 什么都不做）。这是"这一样是空的"，不是错误。
            report.notes.append(QCoreApplication::translate("Presets", "服务器列表为空。"));
        } else {
            report.sections.append(sectionsFromServerList(entries, ApplyMode::Append));
            report.servers += static_cast<int>(entries.size());
        }
    }

    // ------------------------------------------------------------------
    // 3. 四个通用文件组
    // ------------------------------------------------------------------
    if (request.selection.config) {
        captureFileGroup(request, FileGroup::Config, &report.sections, &report);
    }
    if (request.selection.defaultConfigs) {
        captureFileGroup(request, FileGroup::DefaultConfigs, &report.sections,
                         &report);
    }
    if (request.selection.resourcePacks) {
        const int before = static_cast<int>(report.sections.size());
        captureFileGroup(request, FileGroup::ResourcePacks, &report.sections, &report);
        if (static_cast<int>(report.sections.size()) > before) {
            const ResourcePackOrderState state = readResourcePackOrderState(request.instanceRoot);
            PresetSection &section = report.sections[before];
            if (!state.problem.isEmpty()) {
                report.problems.append(QCoreApplication::translate("Presets", "资源包启用顺序读取失败：%1").arg(state.problem));
            } else if (!state.hasList) {
                report.notes.append(captureText("实例的 options.txt 里还没有资源包列表，"
                                                "这一份只收文件、不带启用状态。"));
            } else if (annotateResourcePackSection(section, state)) {
                report.notes.append(QCoreApplication::translate("Presets", "已保存资源包启用顺序，共 %1 个已启用。")
                                        .arg(state.enabledOrder.size()));
            }
        }
    }
    if (request.selection.shaderPacks) {
        captureFileGroup(request, FileGroup::ShaderPacks, &report.sections,
                         &report);
    }

    // ------------------------------------------------------------------
    // 4. MaLiLib（一个模组一段）
    // ------------------------------------------------------------------
    if (request.selection.malilib) {
        MalilibExtractRequest extract;
        extract.instanceRoot = request.instanceRoot;
        // 空 = 扫 `config/` 下所有**结构上**认得出的（`malilibModIdsInInstance()` 那条
        // 判据）—— 不立模组名表：那种表永远不全，而结构识别认得出来 TweakerMore 这类
        // 名字里根本没有 "malilib" 的扩展。
        extract.includeValues = request.selection.malilibValues;
        extract.mode = ApplyMode::Merge;
        const MalilibExtractReport malilib = extractMalilibSections(extract);
        if (!malilib.ok) {
            report.problems.append(QCoreApplication::translate("Presets", "提取 MaLiLib 配置失败：%1").arg(malilib.problem));
        } else {
            report.sections.append(malilib.sections);
            report.malilibFiles += malilib.files;
            report.malilibHotkeys += malilib.hotkeys + malilib.hotkeyedValues;
            report.malilibValues += malilib.values;
            report.problems.append(malilib.skipped);
            if (malilib.files == 0) {
                report.notes.append(QCoreApplication::translate("Presets", "config/ 中未找到 MaLiLib 配置。"));
            }
        }
    }

    // ------------------------------------------------------------------
    // 5. 世界级数据包（一个世界里的包合成一段）
    // ------------------------------------------------------------------
    if (request.selection.dataPacks) {
        const DataPackCaptureReport packs =
            captureDataPacks(request.instanceRoot, request.presetFilesRoot);
        if (!packs.ok) {
            report.problems.append(QCoreApplication::translate("Presets", "数据包收集失败：%1").arg(packs.problem));
        } else {
            report.sections.append(packs.sections);
            report.dataPackItems += packs.packs;
            report.dataPackWorlds += packs.worlds;
            report.notes.append(packs.notes);
            report.problems.append(packs.problems);
        }
    }

    // ------------------------------------------------------------------
    // 收尾：勾了东西却一段都没有，就是这一趟没办成
    // ------------------------------------------------------------------
    if (report.sections.isEmpty()) {
        report.problem = report.problems.isEmpty()
                             ? QCoreApplication::translate("Presets", "所选类别无可提取内容，预设未保存。")
                             : report.problems.first();
        return report;
    }

    report.ok = true;
    return report;
}

QList<PresetSection> mergeCapturedSections(const QList<PresetSection> &existing,
                                           const QList<PresetSection> &captured) {
    QList<PresetSection> merged = existing;
    for (const PresetSection &one : captured) {
        int index = -1;
        for (int i = 0; i < merged.size(); ++i) {
            if (merged.at(i).id == one.id) {
                index = i;
                break;
            }
        }
        if (index >= 0) {
            merged[index] = one;
        } else {
            merged.append(one);
        }
    }
    return merged;
}

QList<ModReference> captureInstalledModReferences(const QString &instanceRoot,
                                                  int *unidentifiedOut,
                                                  QString *problemOut) {
    if (unidentifiedOut != nullptr) {
        *unidentifiedOut = 0;
    }
    if (problemOut != nullptr) {
        problemOut->clear();
    }
    QList<ModReference> references;
    // 只读盘上**已经记下来的**身份、并且按实际内容核 sha1；这一层不联网、不猜文件名。
    const InstalledModInventory inventory = scanInstalledMods(instanceRoot, nullptr);
    if (!inventory.problem.isEmpty()) {
        if (problemOut != nullptr) {
            *problemOut = inventory.problem;
        }
        return references;
    }
    QSet<QString> seen;
    int unidentified = 0;
    for (const InstalledModFile &file : inventory.files) {
        const QString key = file.projectKey;
        const int colon = key.indexOf(QLatin1Char(':'));
        if (colon <= 0 || colon + 1 >= key.size()) {
            ++unidentified;
            continue;
        }
        ModReference reference;
        reference.platform = normalizeModPlatform(key.left(colon));
        reference.projectId = key.mid(colon + 1);
        reference.required = true;
        const QString dedupe = reference.key();
        if (dedupe.isEmpty()) {
            ++unidentified;
            continue;
        }
        if (seen.contains(dedupe)) {
            continue;
        }
        seen.insert(dedupe);
        references.append(reference);
    }
    if (unidentifiedOut != nullptr) {
        *unidentifiedOut = unidentified;
    }
    return references;
}

}  // namespace Presets

