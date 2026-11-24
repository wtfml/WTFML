/**
 * @file AgentCredentialStoreTests.cpp
 * @brief 凭据存储：DPAPI 后备往返、密文不含明文、两条路失败拒绝保存、清除。
 *
 * 这一节强制走 DPAPI 后备（测试属性），不创建真实凭据管理器 target；Credential Manager
 * 主路径由真机 G1A 验收，不在离屏测试里伪造。
 */

#include "Support.hpp"

#include "agent/AgentSecretStore.hpp"
#include "agent/AgentRedaction.hpp"

#include <QCoreApplication>
#include <QSettings>

void runAgentCredentialStoreTests() {
    QCoreApplication::instance()->setProperty("wtfml.testAgentDisableCredentialManager", true);
    const QString profileId = QStringLiteral("test-profile-agent-store");
    QString problem;
    AgentSecretStore::Backend backend = AgentSecretStore::Backend::Unavailable;
    check(AgentSecretStore::write(profileId, QStringLiteral("sk-test-1234567890abcdef"), &backend,
                                  &problem),
          "凭据：DPAPI 后备可写入", problem);
    check(backend == AgentSecretStore::Backend::DpapiFallback, "凭据：报 DPAPI 后备");
    const AgentSecretStore::ReadResult read = AgentSecretStore::read(profileId);
    check(read.ok && read.secret == QStringLiteral("sk-test-1234567890abcdef"),
          "凭据：后备往返一致", read.problem);
    check(!AgentSecretStore::backendKey(read.backend).isEmpty(), "凭据：backend 有稳定字符串");

    // 后备值在盘上必须是密文（不能直接搜到明文）。
    bool sawPlaintext = false;
    QSettings fallback(QStringLiteral("WTFML"), QStringLiteral("WTFML"));
    for (const QString &key : fallback.allKeys()) {
        if (fallback.value(key).toString().contains(QStringLiteral("sk-test-1234567890abcdef"))) {
            sawPlaintext = true;
        }
    }
    check(!sawPlaintext, "凭据：DPAPI 后备盘上不含明文 key");

    // 覆盖写只保留新值。
    check(AgentSecretStore::write(profileId, QStringLiteral("sk-new-abcdefghijklmnop"), &backend,
                                  &problem),
          "凭据：覆盖写", problem);
    check(AgentSecretStore::read(profileId).secret == QStringLiteral("sk-new-abcdefghijklmnop"),
          "凭据：覆盖后读到新值");

    // 模拟两条路都失败：拒绝保存，旧值保留。
    QCoreApplication::instance()->setProperty("wtfml.testAgentSecretProtectFail", true);
    check(!AgentSecretStore::write(profileId, QStringLiteral("sk-should-not-save-123456"), &backend,
                                   &problem),
          "凭据：两条路失败时拒绝保存");
    check(problem.contains(QStringLiteral("拒绝保存")), "凭据：失败原因可见", problem);
    check(AgentSecretStore::read(profileId).secret == QStringLiteral("sk-new-abcdefghijklmnop"),
          "凭据：拒绝保存后旧值仍在");
    QCoreApplication::instance()->setProperty("wtfml.testAgentSecretProtectFail", false);

    check(AgentSecretStore::clear(profileId, &problem), "凭据：清除成功", problem);
    check(!AgentSecretStore::read(profileId).ok, "凭据：清除后读不到");

    QCoreApplication::instance()->setProperty("wtfml.testAgentDisableCredentialManager", false);

    {
        const auto redacted = AgentRedaction::redact(
            "Authorization: Bearer abcdefghijklmnop password=hunter2 sk-abcdefghijklmnopqrstuvwxyz");
        check(redacted.containsCredentials() && redacted.hits >= 3,
              "脱敏：Bearer / 字段名 / sk- 三类都命中",
              QString::number(redacted.hits));
        check(!redacted.text.contains(QStringLiteral("abcdefghijklmnop")) &&
                  !redacted.text.contains(QStringLiteral("hunter2")),
              "脱敏：不保留原值");
        check(AgentRedaction::redact("普通的一句说明，password 规则至少要 8 个字符")
                  .text.contains(QStringLiteral("普通的一句说明")),
              "脱敏：不误伤普通说明文字");
    }
}
