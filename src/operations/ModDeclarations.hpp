/**
 * @file ModDeclarations.hpp
 * @brief 模组 JAR 里的**本地声明**：加载器、游戏/加载器/Java 版本范围、前置依赖、mod id。
 *
 * 从 `UpgradeAssessment.cpp` 抽出来（docs/dev/AGENT_AUTONOMY.md S5.4）：升级评估和智能体的
 * 兼容性分析读的是同一份声明，两份实现的下场是"升级说支持、体检说缺前置"。这里不碰界面、
 * 不读盘 —— 调用方把 JAR 里那几个条目的字节递进来，这一层只解析。
 *
 * 支持：`fabric.mod.json`、`quilt.mod.json`、`META-INF/mods.toml`、
 * `META-INF/neoforge.mods.toml`、`mcmod.info`。读不懂时给 `problem`，**不猜**。
 *
 * 限制：这是包声明，不是运行时保证；范围无法解析时按 `unknown` 处理。
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

namespace ModDeclarations {

enum class Loader { Unknown, Fabric, Quilt, Forge, NeoForge };

QString loaderKey(Loader loader);
Loader loaderFromKey(const QString &key);

/// 要读的 JAR 条目（唯一出处：升级评估和智能体工具都从这里取）。
QStringList metadataEntries();

/// 一条声明。`game` / `loaderRange` / `javaRange` / `breaks` 是声明里的**原样值**
/// （字符串、字符串数组或空），怎么比交给 `ModVersionDeclaration`。
struct Declaration {
    Loader loader = Loader::Unknown;
    QString basis;        ///< 来自哪个条目、哪个字段
    QJsonValue game;
    QString loaderBasis;  ///< `loaderRange` 是从哪个字段读的（报告里写依据）
    QJsonValue loaderRange;
    QString javaBasis;
    QJsonValue javaRange;
    QJsonValue breaks;
    QString problem;      ///< 非空 = 这条声明不可用
};

struct Mod {
    QString fileName;
    QString modId;              ///< 第一个读到的 mod id（空 = 不知道）
    QString name;               ///< 显示名（空 = 用文件名）
    QString version;            ///< 本地版本（空 = 不知道）
    QList<Declaration> declarations;
    QStringList requiredModIds; ///< required 的模组前置（已去掉游戏/加载器/java）
    QStringList requiredLoaderIds;
    QString problem;            ///< 读不到/读不懂声明

    bool hasUsableDeclaration() const;
};

/// 解析一个 JAR 的声明。
/// @param entryProblems `ZipUtil::readMetadataBatch()` 回读不动的条目（条目名 → 原因）。
void parse(const QString &fileName, const QHash<QString, QByteArray> &entries,
           const QHash<QString, QString> &entryProblems, Mod *mod);

/// 解析单个声明条目（嵌套 JAR 与旧版注解那两条路也用它）。
/// @param requiredModIds 可选：这一条声明里的 required 前置。
bool parseEntry(const QString &entryName, const QByteArray &bytes, Declaration *declaration,
                QStringList *requiredModIds = nullptr, QStringList *requiredLoaderIds = nullptr);

}  // namespace ModDeclarations
