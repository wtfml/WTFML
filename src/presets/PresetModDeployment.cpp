#include "PresetModInstaller.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QTimer>

namespace Presets {
void PresetModInstaller::deployOne(int index) {
    if (m_done) return;
    if ((m_running && m_running()) || (m_planCurrent && !m_planCurrent())) {
        finish(tr("实例或所选预设状态已变化，已停止后续部署；已写入项保留。")); return;
    }
    if (index >= m_downloadRows.size()) {
        m_result.ok = m_result.optionalFailures == 0;
        m_result.lines.prepend(tr("模组预设：安装 %1 项，可选项失败/跳过 %2 项。")
            .arg(m_result.installed).arg(m_result.optionalFailures));
        finish(); return;
    }
    const auto next = [this, index] { QTimer::singleShot(0, this, [this, index] { deployOne(index + 1); }); };
    const auto row = m_downloadRows.at(index);
    if (m_skippedDownloads.contains(row.key)) {
        m_fileStates[row.key] = QStringLiteral("skipped"); next(); return;
    }
    const QString mods = QDir(m_root).filePath(QStringLiteral("mods"));
    if (!QFileInfo(m_root).isDir() || QFileInfo(m_root).isSymLink() || QFileInfo(m_root).isJunction()
        || !QFileInfo(mods).isDir() || QFileInfo(mods).isSymLink() || QFileInfo(mods).isJunction()) {
        finish(tr("实例或模组目录已变化，未重新创建或继续写入。")); return;
    }
    const QString path = stagedPath(index);
    const auto verified = verifyDownloadedFile(path, row.file.size, row.file.sha1);
    if (!verified.ok || verified.unverified) {
        if (row.required) { finish(tr("必要模组暂存内容已变化，已停止部署。")); return; }
        ++m_result.optionalFailures; m_fileStates[row.key] = QStringLiteral("skipped"); next(); return;
    }
    const QString target = QDir(mods).filePath(row.file.filename);
    if (QFileInfo(target).isSymLink() || QFileInfo(target).isJunction()) {
        finish(tr("目标模组路径已变为链接，未继续写入。")); return;
    }
    if (QFileInfo::exists(target)) {
        if (!verifyDownloadedFile(target, row.file.size, row.file.sha1).ok) {
            finish(tr("同名模组已存在且内容不同，未覆盖：%1").arg(row.file.filename)); return;
        }
        m_fileStates[row.key] = QStringLiteral("enabled"); next(); return;
    }
    QFile input(path); QTemporaryFile output(QDir(mods).filePath(QStringLiteral(".wtfml-preset-XXXXXX")));
    if (!input.open(QIODevice::ReadOnly) || !output.open()) {
        output.close(); input.close();
        finish(tr("无法写入模组：%1").arg(row.file.filename)); return;
    }
    while (!input.atEnd()) {
        const auto bytes = input.read(65536);
        if (bytes.isEmpty() || output.write(bytes) != bytes.size()) {
            output.close(); input.close(); finish(tr("模组写入失败：%1").arg(row.file.filename)); return;
        }
    }
    if (input.error() != QFile::NoError || !output.flush()) {
        output.close(); input.close();
        finish(tr("无法提交完整模组文件：%1").arg(row.file.filename)); return;
    }
    output.close(); input.close();
    if ((m_planCurrent && !m_planCurrent()) || QFileInfo(mods).isSymLink() || QFileInfo(mods).isJunction()) {
        finish(tr("发布前预设或目标目录已变化，未发布新模组。")); return;
    }
    // QFile::rename refuses an existing destination. QSaveFile::commit would
    // overwrite a file created externally after our earlier existence check.
    // QTemporaryFile retains its native handle after close(); rename through the
    // same object so Qt releases that handle before the Windows rename operation.
    if (!output.rename(target)) {
        finish(tr("发布模组失败或目标已被其他程序创建，未覆盖：%1").arg(row.file.filename)); return;
    }
    output.setAutoRemove(false);
    m_fileStates[row.key] = QStringLiteral("enabled"); ++m_result.installed;
    if (!saveExecutionRecord()) { finish(tr("文件已写入，但报告更新失败，已停止后续部署。")); return; }
    // Yield only between complete atomic files, with no input/output handle alive
    // when another GUI action can cancel or close this operation.
    next();
}
}
