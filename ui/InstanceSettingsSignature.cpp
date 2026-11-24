#include "InstanceSettingsSignature.hpp"

namespace InstanceSettingsSignature {
namespace {

QString optionalText(const std::optional<QString> &value) {
    return value.has_value() ? QStringLiteral("v:") + *value : QStringLiteral("-");
}

QString optionalNumber(const std::optional<int> &value) {
    return value.has_value() ? QStringLiteral("v:%1").arg(*value) : QStringLiteral("-");
}

QString optionalFlag(const std::optional<bool> &value) {
    return value.has_value() ? QStringLiteral("v:%1").arg(*value ? 1 : 0) : QStringLiteral("-");
}

} // namespace

QStringList signatureOf(const Inputs &inputs) {
    QStringList result;
    result << optionalText(inputs.javaPath) << optionalNumber(inputs.xmx)
           << optionalNumber(inputs.xms) << optionalFlag(inputs.autoMemory)
           << optionalText(inputs.extraJvmArgs)
           << optionalText(inputs.windowMode) << optionalNumber(inputs.windowWidth)
           << optionalNumber(inputs.windowHeight)
           << inputs.appendArgs
           << QStringLiteral("warn:%1").arg(inputs.warnJavaMismatch ? 1 : 0)
           << inputs.iconSignature
           << QStringLiteral("group:") + inputs.groupId
           << QStringLiteral("pin:%1").arg(inputs.pinned ? 1 : 0);
    // 「显示哪些页」：一条一格 `key=0/1`。这样"张三关掉、李四打开"和"两个人都动了但
    // 拼起来一样"分得开；顺序由调用方定，见头文件那条 NOTE。
    QStringList pageFlags;
    pageFlags.reserve(inputs.pageChecks.size());
    for (const auto &entry : inputs.pageChecks) {
        pageFlags << QStringLiteral("%1=%2")
                         .arg(entry.first, entry.second ? QStringLiteral("1") : QStringLiteral("0"));
    }
    result << pageFlags.join(QLatin1Char(','));
    return result;
}

} // namespace InstanceSettingsSignature
