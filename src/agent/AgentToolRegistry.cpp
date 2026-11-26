/**
 * @file AgentToolRegistry.cpp
 * @brief 工具 schema 与参数校验（见 .hpp）。
 */

#include "AgentTool.hpp"

#include "AgentCommandTools.hpp"
#include "AgentLauncherTools.hpp"
#include "AgentModToolEngine.hpp"
#include "AgentTestLaunchToolEngine.hpp"
#include "AgentRepairToolEngine.hpp"
#include "AgentWebTools.hpp"
#include "AgentWorkflowEngine.hpp"

#include <QCoreApplication>
#include <QJsonValue>

namespace {
QJsonObject objectSchema(const QStringList &required, const QJsonObject &properties) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("required"), QJsonArray::fromStringList(required)},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
}

bool validateValue(const QJsonValue &schemaValue, const QJsonValue &value, const QString &path,
                   QString *problem) {
    const QJsonObject schema = schemaValue.toObject();
    const QString type = schema.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("string")) {
        if (!value.isString()) {
            if (problem != nullptr) *problem = QStringLiteral("%1 must be a string.").arg(path);
            return false;
        }
        const int maxLength = schema.value(QStringLiteral("maxLength")).toInt(-1);
        if (maxLength >= 0 && value.toString().size() > maxLength) {
            if (problem != nullptr) *problem = QStringLiteral("%1 exceeds maxLength.").arg(path);
            return false;
        }
    } else if (type == QStringLiteral("integer") || type == QStringLiteral("number")) {
        if (!value.isDouble()) {
            if (problem != nullptr) *problem = QStringLiteral("%1 must be a number.").arg(path);
            return false;
        }
        if (type == QStringLiteral("integer") && value.toDouble() != double(value.toInt())) {
            if (problem != nullptr) *problem = QStringLiteral("%1 must be an integer.").arg(path);
            return false;
        }
        const int maximum = schema.value(QStringLiteral("maximum")).toInt(-1);
        if (maximum >= 0 && value.toDouble() > maximum) {
            if (problem != nullptr) *problem = QStringLiteral("%1 exceeds maximum.").arg(path);
            return false;
        }
    } else if (type == QStringLiteral("array")) {
        if (!value.isArray()) {
            if (problem != nullptr) *problem = QStringLiteral("%1 must be an array.").arg(path);
            return false;
        }
        const QJsonArray array = value.toArray();
        const int maxItems = schema.value(QStringLiteral("maxItems")).toInt(-1);
        if (maxItems >= 0 && array.size() > maxItems) {
            if (problem != nullptr) *problem = QStringLiteral("%1 exceeds maxItems.").arg(path);
            return false;
        }
        const QJsonObject items = schema.value(QStringLiteral("items")).toObject();
        if (!items.isEmpty()) {
            for (int i = 0; i < array.size(); ++i) {
                if (!validateValue(items, array.at(i),
                                   path + QStringLiteral("[%1]").arg(i), problem)) {
                    return false;
                }
            }
        }
    } else if (type == QStringLiteral("object")) {
        if (!value.isObject()) {
            if (problem != nullptr) *problem = QStringLiteral("%1 must be an object.").arg(path);
            return false;
        }
        const QJsonObject object = value.toObject();
        const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
        for (const QJsonValue &required : schema.value(QStringLiteral("required")).toArray()) {
            if (!object.contains(required.toString())) {
                if (problem != nullptr) {
                    *problem = QStringLiteral("%1 is missing required field %2.")
                                   .arg(path, required.toString());
                }
                return false;
            }
        }
        const bool allowAdditional = schema.value(QStringLiteral("additionalProperties")).toBool(true);
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!properties.contains(it.key())) {
                if (!allowAdditional) {
                    if (problem != nullptr) {
                        *problem = QStringLiteral("%1 contains unknown field %2.").arg(path, it.key());
                    }
                    return false;
                }
                continue;
            }
            if (!validateValue(properties.value(it.key()), it.value(), path + QLatin1Char('.') + it.key(),
                               problem)) {
                return false;
            }
        }
    }
    const QJsonArray allowed = schema.value(QStringLiteral("enum")).toArray();
    if (!allowed.isEmpty()) {
        bool found = false;
        for (const QJsonValue &candidate : allowed) {
            if (candidate == value) found = true;
        }
        if (!found) {
            if (problem != nullptr) *problem = QStringLiteral("%1 is not in allowed enum values.").arg(path);
            return false;
        }
    }
    return true;
}
}  // namespace

QString AgentToolRegistry::riskKey(AgentRisk risk) {
    switch (risk) {
    case AgentRisk::ReadOnly: return QStringLiteral("read-only");
    case AgentRisk::NetworkRead: return QStringLiteral("network-read");
    case AgentRisk::Write: return QStringLiteral("write");
    case AgentRisk::Destructive: return QStringLiteral("destructive");
    case AgentRisk::Command: return QStringLiteral("command");
    case AgentRisk::ExternalMcp: return QStringLiteral("external-mcp");
    }
    return QStringLiteral("external-mcp");
}

QList<AgentToolSpec> AgentToolRegistry::fileBuiltins() {
    QList<AgentToolSpec> list;
    list.append(AgentToolSpec{
        QStringLiteral("fs.list"), QCoreApplication::translate("AgentToolRegistry", "查看目录概况"),
        QStringLiteral("Return directory metadata only (file counts and sizes) inside the locked instance; does not return filenames."),
        AgentRisk::ReadOnly,
        objectSchema({QStringLiteral("path")},
                     {{QStringLiteral("path"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 1024}}}})});
    list.append(AgentToolSpec{
        QStringLiteral("fs.listNames"), QCoreApplication::translate("AgentToolRegistry", "查看完整文件名"),
        QStringLiteral("Return full filenames in a directory; requires user confirmation every time, truncated at 2000 items."),
        AgentRisk::ReadOnly,
        objectSchema({QStringLiteral("path")},
                     {{QStringLiteral("path"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 1024}}},
                      {QStringLiteral("limit"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                   {QStringLiteral("maximum"), 2000}}}})});
    list.append(AgentToolSpec{
        QStringLiteral("fs.read"), QCoreApplication::translate("AgentToolRegistry", "读取文本文件"),
        QStringLiteral("Read a text file inside the locked instance; default 256 KB, max 1 MB; rejects binary files."),
        AgentRisk::ReadOnly,
        objectSchema({QStringLiteral("path")},
                     {{QStringLiteral("path"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 1024}}},
                      {QStringLiteral("maxBytes"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                   {QStringLiteral("maximum"), 1048576}}}})});
    list.append(AgentToolSpec{
        QStringLiteral("fs.write"), QCoreApplication::translate("AgentToolRegistry", "写入文本配置"),
        QStringLiteral("Only allows config/**, defaultconfigs/**, and options.txt; requires previous SHA-256 and fileIdentityToken; "
                       "per-action approval, pre-write backup, atomic write, and post-write verification."),
        AgentRisk::Write,
        objectSchema({QStringLiteral("path"), QStringLiteral("content")},
                     {{QStringLiteral("path"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 1024}}},
                      {QStringLiteral("content"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 1048576}}},
                      {QStringLiteral("expectedSha256"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 64}}},
                      {QStringLiteral("fileIdentityToken"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 256}}},
                      {QStringLiteral("workspaceId"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 80}}}})});
    list.append(AgentToolSpec{
        QStringLiteral("artifact.read"), QCoreApplication::translate("AgentToolRegistry", "读取模型 artifact"),
        QStringLiteral("Read only model-readable artifacts of this session; re-redacted on every read; raw local archives are unreadable."),
        AgentRisk::ReadOnly,
        objectSchema({QStringLiteral("artifactId")},
                     {{QStringLiteral("artifactId"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 64}}},
                      {QStringLiteral("offset"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
                      {QStringLiteral("length"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                   {QStringLiteral("maximum"), 1048576}}}})});
    return list;
}

QList<AgentToolSpec> AgentToolRegistry::builtins() {
    QList<AgentToolSpec> list = fileBuiltins();
    list.append(AgentLauncherTools::specs());
    list.append(AgentTestLaunchToolEngine::specs());
    list.append(AgentRepairToolEngine::specs());
    list.append(AgentModToolEngine::specs());
    list.append(AgentWorkflowEngine::specs());
    list.append(AgentWebTools::specs());
    list.append(AgentCommandTools::specs());
    return list;
}

QString AgentToolRegistry::wireName(const QString &internal) {
    QString wire = internal;
    wire.replace(QLatin1Char('.'), QLatin1Char('_'));
    return wire;
}

QString AgentToolRegistry::internalName(const QString &wire) {
    for (const AgentToolSpec &spec : builtins()) {
        if (spec.name == wire) return spec.name;
    }
    for (const AgentToolSpec &spec : builtins()) {
        if (wireName(spec.name) == wire) return spec.name;
    }
    return wire;
}

AgentToolSpec AgentToolRegistry::find(const QString &name) {
    const QString internal = internalName(name);
    for (const AgentToolSpec &spec : builtins()) {
        if (spec.name == internal) return spec;
    }
    return {};
}

bool AgentToolRegistry::validateArguments(const QJsonObject &schema, const QJsonObject &arguments,
                                          QString *problem) {
    return validateValue(schema, arguments, QStringLiteral("arguments"), problem);
}
