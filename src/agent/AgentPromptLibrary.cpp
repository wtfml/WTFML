/**
 * @file AgentPromptLibrary.cpp
 * @brief 技能提示词从 Qt 资源加载、组合、哈希与大小校验（docs/dev/AGENT_AUTONOMY.md S5.0）。
 */

#include "AgentPromptLibrary.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QResource>
#include <QStringList>

/// 提示词 qrc 编进静态库时，没人引用这个初始化函数就会被链接器丢掉。
/// Q_INIT_RESOURCE 不能写在命名空间里；产品里 qrc 已自动初始化时再调用也无副作用。
static void initAgentPromptResources() {
    Q_INIT_RESOURCE(agent);
}

namespace {

constexpr int kMaxPromptBytes = 64 * 1024;
constexpr int kMaxCommonBytes = 4 * 1024;
constexpr int kMaxSkillBytes = 8 * 1024;
constexpr int kMaxComposedBytes = 16 * 1024;
constexpr int kMaxIdLength = 64;

QString normalizedPrompt(QString text) {
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text.trimmed();
}

QString hashOf(const QString &text) {
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}

void ensureResources() {
    static const bool initialized = [] {
        initAgentPromptResources();
        return true;
    }();
    Q_UNUSED(initialized);
}

}  // namespace

bool AgentPromptLibrary::validId(const QString &id) {
    if (id.isEmpty() || id.size() > kMaxIdLength) return false;
    for (const QChar ch : id) {
        const ushort code = ch.unicode();
        const bool allowed = (code >= 'a' && code <= 'z') || (code >= '0' && code <= '9') ||
                             code == '_' || code == '-';
        if (!allowed) return false;
    }
    return true;
}

QString AgentPromptLibrary::resourcePath(const QString &id) {
    if (!validId(id)) return {};
    return QStringLiteral(":/agent/prompts/%1.md").arg(id);
}

int AgentPromptLibrary::maxPromptBytes() {
    return kMaxPromptBytes;
}

int AgentPromptLibrary::maxCommonPromptBytes() { return kMaxCommonBytes; }

int AgentPromptLibrary::maxSkillPromptBytes() { return kMaxSkillBytes; }

int AgentPromptLibrary::maxComposedPromptBytes() { return kMaxComposedBytes; }

AgentPrompt AgentPromptLibrary::load(const QString &id, QString *problem) {
    if (problem != nullptr) problem->clear();
    if (!validId(id)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentPromptLibrary", "提示词 id 不合法：%1").arg(id);
        return {};
    }
    ensureResources();

    static QMutex cacheMutex;
    static QHash<QString, AgentPrompt> cache;
    {
        QMutexLocker locker(&cacheMutex);
        const auto it = cache.constFind(id);
        if (it != cache.constEnd()) return it.value();
    }

    const QString path = resourcePath(id);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentPromptLibrary", "找不到技能提示词资源：%1").arg(path);
        return {};
    }
    const int budget = id == QStringLiteral("common") ? kMaxCommonBytes : kMaxSkillBytes;
    QByteArray bytes = file.read(budget + 1);
    if (bytes.size() > budget) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentPromptLibrary", "提示词超过预算（%1 字节）：%2").arg(budget).arg(path);  // i18n-ignore
        }
        return {};
    }
    if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);

    QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar::ReplacementCharacter)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentPromptLibrary", "技能提示词不是有效 UTF-8：%1").arg(path);
        return {};
    }
    text = normalizedPrompt(text);
    if (text.isEmpty()) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentPromptLibrary", "技能提示词为空：%1").arg(path);
        return {};
    }

    const AgentPrompt prompt{text, hashOf(text)};
    {
        QMutexLocker locker(&cacheMutex);
        cache.insert(id, prompt);
    }
    return prompt;
}

AgentPrompt AgentPromptLibrary::compose(const QString &skillId, bool includeRepairProtocol,
                                        QString *problem) {
    if (problem != nullptr) problem->clear();
    if (!validId(skillId)) {
        if (problem != nullptr) *problem = QCoreApplication::translate("AgentPromptLibrary", "技能提示词 id 不合法：%1").arg(skillId);
        return {};
    }

    QStringList ids{QStringLiteral("common"), skillId};
    if (includeRepairProtocol) ids.append(QStringLiteral("repair_protocol"));

    QStringList parts;
    for (const QString &id : ids) {
        QString localProblem;
        const AgentPrompt prompt = load(id, &localProblem);
        if (!localProblem.isEmpty()) {
            if (problem != nullptr) {
                *problem = QCoreApplication::translate("AgentPromptLibrary", "技能提示词不可用（%1）：%2").arg(id, localProblem);
            }
            return {};
        }
        parts.append(prompt.text);
    }

    QString text = parts.join(QStringLiteral("\n\n"));
    if (text.toUtf8().size() > kMaxComposedBytes) {
        if (problem != nullptr) {
            *problem = QCoreApplication::translate("AgentPromptLibrary", "技能提示词组合后超过预算（%1 字节）").arg(kMaxComposedBytes);  // i18n-ignore
        }
        return {};
    }
    return AgentPrompt{text, hashOf(text)};
}
