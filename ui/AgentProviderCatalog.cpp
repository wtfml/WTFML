/**
 * @file AgentProviderCatalog.cpp
 * @brief 内置厂商/模型清单（见 .hpp）。
 *
 * 数据来源是各家公开文档里明写的 OpenAI 兼容端点与模型 id。加一家的规矩：
 * - `baseUrl` 填到 `.../v1` 这一层（`chatCompletionsUrl()` 自己接 `/chat/completions`）；
 * - `contextWindow` 只填文档写了确切数字的，不确定就写 0（不猜）；
 * - `listsModels` 只在确实支持 `GET /models` 时为 true。
 */

#include "AgentProviderCatalog.hpp"

#include <QCoreApplication>
#include <QSet>

namespace {

AgentCatalogModel model(const QString &id, int contextWindow, int maxOutput,
                        bool reasoning = false, const QString &name = QString()) {
    AgentCatalogModel entry;
    entry.id = id;
    entry.name = name;
    entry.contextWindow = contextWindow;
    entry.maxOutput = maxOutput;
    entry.supportsReasoning = reasoning;
    return entry;
}

}  // namespace

namespace AgentProviderCatalog {

QList<AgentCatalogProvider> providers() {
    QList<AgentCatalogProvider> list;

    {
        AgentCatalogProvider p;
        p.id = QStringLiteral("openai");
        p.name = QStringLiteral("OpenAI");
        p.baseUrl = QStringLiteral("https://api.openai.com/v1");
        p.models = {model(QStringLiteral("gpt-4.1"), 1047576, 32768),
                    model(QStringLiteral("gpt-4.1-mini"), 1047576, 32768),
                    model(QStringLiteral("gpt-4o"), 128000, 16384),
                    model(QStringLiteral("gpt-4o-mini"), 128000, 16384),
                    model(QStringLiteral("o4-mini"), 200000, 100000, true),
                    model(QStringLiteral("o3"), 200000, 100000, true)};
        list.append(p);
    }
    {
        AgentCatalogProvider p;
        p.id = QStringLiteral("deepseek");
        p.name = QStringLiteral("DeepSeek");
        p.baseUrl = QStringLiteral("https://api.deepseek.com/v1");
        p.models = {model(QStringLiteral("deepseek-chat"), 65536, 8192),
                    model(QStringLiteral("deepseek-reasoner"), 65536, 8192, true)};
        list.append(p);
    }
    {
        AgentCatalogProvider p;
        p.id = QStringLiteral("dashscope");
        p.name = QCoreApplication::translate("AgentProviderCatalog", "通义千问（DashScope）");
        p.baseUrl = QStringLiteral("https://dashscope.aliyuncs.com/compatible-mode/v1");
        p.models = {model(QStringLiteral("qwen-max"), 32768, 8192),
                    model(QStringLiteral("qwen-plus"), 131072, 8192),
                    model(QStringLiteral("qwen-turbo"), 131072, 8192),
                    model(QStringLiteral("qwen3-coder-plus"), 262144, 65536)};
        list.append(p);
    }
    {
        AgentCatalogProvider p;
        p.id = QStringLiteral("moonshot");
        p.name = QCoreApplication::translate("AgentProviderCatalog", "Kimi（Moonshot）");
        p.baseUrl = QStringLiteral("https://api.moonshot.cn/v1");
        p.models = {model(QStringLiteral("kimi-k2-0905-preview"), 262144, 16384),
                    model(QStringLiteral("moonshot-v1-128k"), 131072, 8192)};
        list.append(p);
    }
    {
        AgentCatalogProvider p;
        p.id = QStringLiteral("zhipu");
        p.name = QCoreApplication::translate("AgentProviderCatalog", "智谱 GLM");
        p.baseUrl = QStringLiteral("https://open.bigmodel.cn/api/paas/v4");
        p.models = {model(QStringLiteral("glm-4.6"), 204800, 16384),
                    model(QStringLiteral("glm-4-plus"), 131072, 4096),
                    model(QStringLiteral("glm-4-flash"), 131072, 4096)};
        list.append(p);
    }
    {
        AgentCatalogProvider p;
        p.id = QStringLiteral("siliconflow");
        p.name = QStringLiteral("SiliconFlow");
        p.baseUrl = QStringLiteral("https://api.siliconflow.cn/v1");
        p.models = {model(QStringLiteral("deepseek-ai/DeepSeek-V3"), 65536, 8192),
                    model(QStringLiteral("Qwen/Qwen3-235B-A22B-Instruct-2507"), 262144, 8192)};
        list.append(p);
    }
    {
        AgentCatalogProvider p;
        p.id = QStringLiteral("openrouter");
        p.name = QStringLiteral("OpenRouter");
        p.baseUrl = QStringLiteral("https://openrouter.ai/api/v1");
        p.note = QCoreApplication::translate(
            "AgentProviderCatalog",
            "OpenRouter 的模型 id 带厂商前缀，例如 openai/gpt-4.1。");
        // 不写死具体模型：这家的清单变得快，靠 GET /models 拉。
        list.append(p);
    }
    {
        AgentCatalogProvider p;
        p.id = QStringLiteral("ollama");
        p.name = QCoreApplication::translate("AgentProviderCatalog", "Ollama（本机）");
        p.baseUrl = QStringLiteral("http://127.0.0.1:11434/v1");
        p.authMode = QStringLiteral("none");
        p.deployment = QStringLiteral("local");
        p.localHttpAllowed = true;
        p.note = QCoreApplication::translate(
            "AgentProviderCatalog",
            "本机 Ollama 默认不校验密钥；先 `ollama serve`，模型名用 `ollama list` 里的那个。");
        list.append(p);
    }
    {
        AgentCatalogProvider p;
        p.id = QStringLiteral("lmstudio");
        p.name = QCoreApplication::translate("AgentProviderCatalog", "LM Studio（本机）");
        p.baseUrl = QStringLiteral("http://127.0.0.1:1234/v1");
        p.authMode = QStringLiteral("none");
        p.deployment = QStringLiteral("local");
        p.localHttpAllowed = true;
        p.note = QCoreApplication::translate(
            "AgentProviderCatalog", "在 LM Studio 里先开 Local Server。");
        list.append(p);
    }
    {
        AgentCatalogProvider p;
        p.id = QStringLiteral("custom");
        p.name = QCoreApplication::translate("AgentProviderCatalog", "自定义（OpenAI 兼容）");
        p.baseUrl = QString();
        p.note = QCoreApplication::translate(
            "AgentProviderCatalog",
            "自建端点或上面没列的服务：自己填接口地址与模型 id。");
        list.append(p);
    }
    return list;
}

AgentCatalogProvider find(const QString &providerId) {
    for (const AgentCatalogProvider &provider : providers()) {
        if (provider.id == providerId) return provider;
    }
    return {};
}

AgentCatalogProvider matchBaseUrl(const QString &baseUrl) {
    const QString needle = baseUrl.trimmed();
    if (needle.isEmpty()) return {};
    for (const AgentCatalogProvider &provider : providers()) {
        if (provider.baseUrl.isEmpty()) continue;
        if (provider.baseUrl.compare(needle, Qt::CaseInsensitive) == 0) return provider;
    }
    return {};
}

AgentCatalogModel findModel(const QString &providerId, const QString &modelId) {
    const AgentCatalogProvider provider = find(providerId);
    for (const AgentCatalogModel &entry : provider.models) {
        if (entry.id == modelId) return entry;
    }
    return {};
}

AgentCatalogModel findModelAnywhere(const QString &modelId) {
    if (modelId.trimmed().isEmpty()) return {};
    for (const AgentCatalogProvider &provider : providers()) {
        for (const AgentCatalogModel &entry : provider.models) {
            if (entry.id.compare(modelId, Qt::CaseInsensitive) == 0) return entry;
        }
    }
    return {};
}

QStringList validate() {
    QStringList problems;
    QSet<QString> providerIds;
    for (const AgentCatalogProvider &provider : providers()) {
        if (provider.id.isEmpty()) {
            problems.append(QStringLiteral("provider id 为空"));  // i18n-ignore
            continue;
        }
        if (providerIds.contains(provider.id)) {
            problems.append(QStringLiteral("provider id 重复：%1").arg(provider.id));  // i18n-ignore
        }
        providerIds.insert(provider.id);
        if (provider.id != QLatin1String("custom")) {
            // 这一层不 include `src/`（ui/ 的规矩），只挡明显写错的形状：
            // https://…，或本机服务的 http://127.0.0.1…。完整校验在
            // `AgentProviderAdapters::validateBaseUrl()`，保存配置时照旧跑。
            const QString url = provider.baseUrl;
            const bool ok = url.startsWith(QLatin1String("https://")) ||
                            (provider.localHttpAllowed &&
                             (url.startsWith(QLatin1String("http://127.0.0.1")) ||
                              url.startsWith(QLatin1String("http://localhost"))));
            if (!ok) {
                problems.append(QStringLiteral("%1 的 baseUrl 形状不对：%2")  // i18n-ignore
                                    .arg(provider.id, url));
            }
        }
        QSet<QString> modelIds;
        for (const AgentCatalogModel &entry : provider.models) {
            if (entry.id.trimmed().isEmpty()) {
                problems.append(QStringLiteral("%1 里有一个空模型 id").arg(provider.id));  // i18n-ignore
                continue;
            }
            if (modelIds.contains(entry.id)) {
                problems.append(QStringLiteral("%1 里模型 id 重复：%2")  // i18n-ignore
                                    .arg(provider.id, entry.id));
            }
            modelIds.insert(entry.id);
            if (entry.contextWindow != 0 &&
                (entry.contextWindow < 1024 || entry.contextWindow > 20000000)) {
                problems.append(QStringLiteral("%1/%2 的上下文上限不可信：%3")  // i18n-ignore
                                    .arg(provider.id, entry.id)
                                    .arg(entry.contextWindow));
            }
            if (entry.maxOutput < 0 || (entry.contextWindow > 0 && entry.maxOutput > entry.contextWindow)) {
                problems.append(QStringLiteral("%1/%2 的输出上限不合理：%3")  // i18n-ignore
                                    .arg(provider.id, entry.id)
                                    .arg(entry.maxOutput));
            }
        }
    }
    return problems;
}

}  // namespace AgentProviderCatalog
