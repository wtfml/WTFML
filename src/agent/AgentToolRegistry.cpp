/**
 * @file AgentToolRegistry.cpp
 * @brief 工具 schema 与参数校验（见 .hpp）。
 */

#include "AgentTool.hpp"

#include "AgentCommandTools.hpp"
#include "AgentLauncherTools.hpp"
#include "AgentWebTools.hpp"
#include "AgentWorkflowEngine.hpp"

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
            if (problem != nullptr) *problem = QStringLiteral("%1 必须是字符串。").arg(path);
            return false;
        }
        const int maxLength = schema.value(QStringLiteral("maxLength")).toInt(-1);
        if (maxLength >= 0 && value.toString().size() > maxLength) {
            if (problem != nullptr) *problem = QStringLiteral("%1 超过最大长度。").arg(path);
            return false;
        }
    } else if (type == QStringLiteral("integer") || type == QStringLiteral("number")) {
        if (!value.isDouble()) {
            if (problem != nullptr) *problem = QStringLiteral("%1 必须是数字。").arg(path);
            return false;
        }
        if (type == QStringLiteral("integer") && value.toDouble() != double(value.toInt())) {
            if (problem != nullptr) *problem = QStringLiteral("%1 必须是整数。").arg(path);
            return false;
        }
        const int maximum = schema.value(QStringLiteral("maximum")).toInt(-1);
        if (maximum >= 0 && value.toDouble() > maximum) {
            if (problem != nullptr) *problem = QStringLiteral("%1 超过最大值。").arg(path);
            return false;
        }
    } else if (type == QStringLiteral("object")) {
        if (!value.isObject()) {
            if (problem != nullptr) *problem = QStringLiteral("%1 必须是对象。").arg(path);
            return false;
        }
        const QJsonObject object = value.toObject();
        const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
        for (const QJsonValue &required : schema.value(QStringLiteral("required")).toArray()) {
            if (!object.contains(required.toString())) {
                if (problem != nullptr) {
                    *problem = QStringLiteral("%1 缺少必填字段 %2。").arg(path, required.toString());
                }
                return false;
            }
        }
        const bool allowAdditional = schema.value(QStringLiteral("additionalProperties")).toBool(true);
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!properties.contains(it.key())) {
                if (!allowAdditional) {
                    if (problem != nullptr) {
                        *problem = QStringLiteral("%1 含未知字段 %2。").arg(path, it.key());
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
            if (problem != nullptr) *problem = QStringLiteral("%1 不在允许的枚举值里。").arg(path);
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
        QStringLiteral("fs.list"), QStringLiteral("查看目录概况"),
        QStringLiteral("只返回锁定实例内的目录元数据（数量/大小），不返回文件名。"),
        AgentRisk::ReadOnly,
        objectSchema({QStringLiteral("path")},
                     {{QStringLiteral("path"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 1024}}}})});
    list.append(AgentToolSpec{
        QStringLiteral("fs.listNames"), QStringLiteral("查看完整文件名"),
        QStringLiteral("返回目录内完整文件名；每次都需要用户确认，截断到 2000 项。"),
        AgentRisk::ReadOnly,
        objectSchema({QStringLiteral("path")},
                     {{QStringLiteral("path"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 1024}}},
                      {QStringLiteral("limit"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                   {QStringLiteral("maximum"), 2000}}}})});
    list.append(AgentToolSpec{
        QStringLiteral("fs.read"), QStringLiteral("读取文本文件"),
        QStringLiteral("读取锁定实例内的文本；默认 256 KB，最大 1 MB；二进制拒绝。"),
        AgentRisk::ReadOnly,
        objectSchema({QStringLiteral("path")},
                     {{QStringLiteral("path"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                   {QStringLiteral("maxLength"), 1024}}},
                      {QStringLiteral("maxBytes"),
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                                   {QStringLiteral("maximum"), 1048576}}}})});
    list.append(AgentToolSpec{
        QStringLiteral("fs.write"), QStringLiteral("写入文本配置"),
        QStringLiteral("只允许 config/**、defaultconfigs/**、options.txt；必须带旧内容哈希与身份 token；"
                       "每次审批，写前备份，原子写，写后校验。"),
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
                                   {QStringLiteral("maxLength"), 256}}}})});
    list.append(AgentToolSpec{
        QStringLiteral("artifact.read"), QStringLiteral("读取模型 artifact"),
        QStringLiteral("只读本会话模型可读 artifact；每次重新脱敏；本地原始存档不可读。"),
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
