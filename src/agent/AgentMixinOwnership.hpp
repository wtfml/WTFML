/**
 * @file AgentMixinOwnership.hpp
 * @brief 从模组 JAR 的声明里建「mixin 配置名 → 模组」映射
 *        （docs/dev/AGENT_AUTONOMY.md S5.3：崩溃报告里点名的配置要能落到具体模组）。
 *
 * 只认模组自己声明的配置名：`fabric.mod.json` / `quilt.mod.json` 的 `mixins`、
 * `META-INF/mods.toml`（NeoForge 同形）的 `[[mixins]] config=`、`META-INF/MANIFEST.MF`
 * 的 `MixinConfigs:`。读不到声明就跳过，**不按文件名猜**（`examplemod.mixins.json`
 * 归给 `examplemod` 只是巧合，模组作者可以起任何名字）。
 *
 * 读 JAR 走 `ZipUtil::readMetadataBatch()`：一个 PowerShell 进程读完整批，不逐个起进程，
 * 也不解压到磁盘。
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

namespace AgentMixinOwnership {

struct Entry {
    QString config;    ///< 归一化后的配置名（小写、去目录、去 `.json`）
    QString modId;     ///< 能读到就填
    QString fileName;  ///< `mods/` 下的文件名
};

/// 归一化：取文件名、去 `.json`、转小写。
QString normalizeConfig(const QString &name);

/// 从一份 JAR 的声明字节建映射（纯函数，测试直接喂夹具）：
/// 键是 JAR 内的条目名（`fabric.mod.json` 等），值是该条目的字节。
QList<Entry> fromDeclarations(const QHash<QString, QByteArray> &declarations,
                              const QString &fileName);

/// 扫 `modsDir` 下的 `.jar`（跳过 `.jar.disabled`），最多 `maxJars` 个。
/// 读不到的 JAR 计入 `skipped`，不编造归属。
QList<Entry> scan(const QString &modsDir, int maxJars, int *skipped, QString *problem);

}  // namespace AgentMixinOwnership
