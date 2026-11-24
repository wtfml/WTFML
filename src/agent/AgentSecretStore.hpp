/**
 * @file AgentSecretStore.hpp
 * @brief 模型服务凭据的唯一落点：Windows 凭据管理器主路径 + DPAPI 后备。
 *
 * 规则（docs/features/AGENT.md §5.2 / G1A）：
 * - 两条路都失败 ⇒ **拒绝保存并保留旧值**，绝不降级成明文/QSettings 明文；
 * - 读不到就报不可用，不猜；
 * - 后备值解开后如果凭据管理器可写，就迁过去再删旧值；
 * - 清除失败必须让调用方看见。
 *
 * 测试钩子（只通过 QCoreApplication 属性，产品入口不设置）：
 * - `wtfml.testAgentDisableCredentialManager` = true：强制走 DPAPI 后备；
 * - `wtfml.testAgentSecretProtectFail` = true：模拟两条路都写不进去。
 */

#pragma once

#include <QString>

namespace AgentSecretStore {

enum class Backend { CredentialManager, DpapiFallback, Unavailable };

QString backendKey(Backend backend);

struct ReadResult {
    QString secret;
    Backend backend = Backend::Unavailable;
    bool ok = false;
    QString problem;
};

bool credentialManagerAvailable();

/// 写入。成功时 @p backendOut 是实际落点；失败时旧值保留。
bool write(const QString &profileId, const QString &secret, Backend *backendOut = nullptr,
           QString *problem = nullptr);

/// 读。先凭据管理器，再 DPAPI 后备；迁移成功时 backend 报 CredentialManager。
ReadResult read(const QString &profileId);

/// 删除两条路的值。任一路失败返回 false（调用方必须显示“没清掉”）。
bool clear(const QString &profileId, QString *problem = nullptr);

bool hasSecret(const QString &profileId);

QString credentialTarget(const QString &profileId);

}  // namespace AgentSecretStore
