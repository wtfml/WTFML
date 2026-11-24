/**
 * @file AgentLaunchHost.hpp
 * @brief 智能体测试启动的异步宿主接口：建工作区、启动游戏、观察里程碑、回灌结果。
 *
 * `AgentLauncherHost` 保持同步（只读事实/短操作）；测试启动可能跑几分钟，单独走这条
 * 异步接口，不阻塞 UI，不开嵌套事件循环。实现放在 `src/controllers/`；核心层的
 * `AgentTestLaunchToolEngine` 只认这张表，`verify_agent` 用假宿主逐格验。
 */

#pragma once

#include "AgentTestLaunch.hpp"

#include <QString>

#include <functional>

class AgentLaunchHost {
public:
    using Completion = std::function<void(const AgentTestLaunchSummary &)>;

    virtual ~AgentLaunchHost() = default;

    /**
     * @brief 在隔离工作区里启动一次（或按 options.repeat 多次）。
     * @param instanceId 会话锁定的实例 id；不接受模型传入。
     * @param workspaceId 已有工作区；空 = 新建临时工作区。
     * @param options 里程碑、超时、观察窗口、重复次数、是否进世界/复现崩溃。
     * @param done 结果回调；必须回到主线程。
     * @param problem 同步失败原因（工作区建不出来、实例在跑、Java 缺失等）。
     * @return 已开始返回 true；同步失败返回 false 并写 @p problem。
     */
    virtual bool startTestLaunch(const QString &instanceId, const QString &workspaceId,
                                 const AgentTestLaunchOptions &options, const Completion &done,
                                 QString *problem) = 0;

    /// 取消当前测试启动；进程树一起收掉。没有在跑时是空操作。
    virtual void cancelTestLaunch() = 0;
};
