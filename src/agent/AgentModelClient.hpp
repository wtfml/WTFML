/**
 * @file AgentModelClient.hpp
 * @brief 模型客户端的抽象接口（Stage 1A 只有 OpenAI-compatible 实现）。
 *
 * 信号约定：
 * - `textDelta` / `reasoningDelta` 只带增量文本；
 * - `finished` 带完整回复与用量；
 * - `failed` 的 `Agent::Error` 负责分类、是否可重试、`Retry-After`；
 * - 取消/切换会话后旧 `requestId` 的回调由上层丢弃。
 */

#pragma once

#include "AgentTypes.hpp"

#include <QObject>
#include <QStringList>

class AgentModelClient : public QObject {
    Q_OBJECT

public:
    explicit AgentModelClient(QObject *parent = nullptr) : QObject(parent) {}

    virtual void start(const Agent::ModelRequest &request) = 0;
    virtual void cancel(const QString &requestId) = 0;

    /// “测试连接”：只报告成功/HTTP 状态/耗时，不显示 key 或响应正文。
    virtual void testConnection(const Agent::ModelRequest &request) = 0;

    /// 列模型（`GET /models`）。设置页选完厂商/填完 key 后调一次，用来填「模型」下拉。
    /// 默认实现什么都不做 —— 不支持这条路的后端（或测试替身）就让它静默失败，
    /// 调用方回落到内置清单（`AgentProviderCatalog`）。
    virtual void listModels(const Agent::ModelRequest &request) { Q_UNUSED(request); }

signals:
    void textDelta(const QString &requestId, const QString &text);
    void reasoningDelta(const QString &requestId, const QString &text);
    void finished(const QString &requestId, const Agent::ModelReply &reply);
    void failed(const QString &requestId, const Agent::Error &error);
    void connectionTested(const QString &requestId, bool ok, int httpStatus,
                          const QString &summary);
    /// 列模型的结果：`models` 里是服务端给的 id（原样，不去重排序之外不做加工）。
    void modelsListed(const QString &requestId, bool ok, const QStringList &models,
                      const QString &summary);
};
