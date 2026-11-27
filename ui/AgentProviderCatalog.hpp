/**
 * @file AgentProviderCatalog.hpp
 * @brief 常见模型服务与模型的内置清单：选厂商自动填地址、选模型自动取上下文上限。
 *
 * 【为什么是编译进来的常量表】
 *
 * 这一层只解决"别让人手抄 baseUrl 和模型 id"。表里每一条都来自服务商的公开文档，
 * 不联网、不拉第三方目录、不写价格 —— 联网拉目录会让"配好 API 才能用"变成"配好 API
 * 还要先信任一个远端列表"，而这里要的只是省几个字。
 *
 * **表不是权限**：它给的是默认值，`AgentProviderAdapters::validateBaseUrl()` 那套
 * https / 回环规则照旧逐条生效；用户改成自建端点也照样能存。
 *
 * **拉不到就用这张表兜底**：设置页会先试 `GET /models`，401/网络/该厂商不支持时回落到
 * 这里的 `models`，并在旁边写一句"已用内置清单"。
 *
 * 【它为什么在 ui/】
 *
 * `ui/Agent*` 不许 include `src/`（见 AGENT.md §3）。这张表只是给设置页填默认值的界面数据，
 * 不参与请求构造、不参与策略判定 —— URL 合法性仍由 `src/agent/AgentProviderAdapters` 那套
 * https/回环规则逐条判，所以放在 `ui/` 这一侧、由验证工程单独断言它每一条都过得去。
 *
 * 订阅制/plan 类入口（OAuth 登录那种）暂不在这里 —— 见 `docs/ROADMAP.md`。
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

struct AgentCatalogModel {
    QString id;            ///< 要求里写的模型 id（区分大小写）
    QString name;          ///< 界面上的显示名；空 = 直接用 id
    int contextWindow = 0; ///< 上下文上限（token）；0 = 不知道，沿用 profile 里的值
    int maxOutput = 0;     ///< 默认单次输出上限（token）；0 = 沿用 profile 里的值
    bool supportsReasoning = false;
};

struct AgentCatalogProvider {
    QString id;          ///< 表内唯一 key
    QString name;        ///< 界面上的显示名
    QString baseUrl;     ///< 自动填进「接口地址」的那个值
    /// bearer / api-key-header / none；和 `AgentProviderProfile::authMode` 同一套取值。
    QString authMode = QStringLiteral("bearer");
    /// cloud / local；和 `AgentProviderProfile::deployment` 同一套取值。
    QString deployment = QStringLiteral("cloud");
    /// 本机服务（Ollama / LM Studio / vLLM）要允许 http://127.0.0.1。
    bool localHttpAllowed = false;
    /// 这家支不支持 `GET /models` 列模型（不支持就直接用下面的内置清单）。
    bool listsModels = true;
    QString note;  ///< 界面上那句补充说明（可空）
    QList<AgentCatalogModel> models;
};

namespace AgentProviderCatalog {

/// 全部内置厂商，按界面上的先后顺序。
QList<AgentCatalogProvider> providers();
/// 按 id 取；找不到返回空（`id` 为空的那个）。
AgentCatalogProvider find(const QString &providerId);
/// 按 baseUrl 反查是哪家（用户手改了地址就查不到，返回空）。
AgentCatalogProvider matchBaseUrl(const QString &baseUrl);
/// 在指定厂商的清单里按模型 id 找；找不到返回空模型（`id` 为空）。
AgentCatalogModel findModel(const QString &providerId, const QString &modelId);
/// 所有厂商里见过这个模型 id 的第一条（用于"用户手填了 id"时补上下文上限）。
AgentCatalogModel findModelAnywhere(const QString &modelId);

/// 自检：每条 baseUrl 形如 https://…/v1 或 http://127.0.0.1（本机服务），模型 id 非空且不重复，
/// 超限值在合理范围。返回问题清单（空 = 全部通过）。给验证工程用 —— 这一层不依赖
/// `src/agent/`，所以 URL 的完整校验仍由那边负责，这里只挡明显写错的条目。
QStringList validate();

}  // namespace AgentProviderCatalog
