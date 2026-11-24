修复按已批准的计划执行。需要验证时优先用 `instance.testLaunch`：它只在隔离工作区启动，不改原实例；在计划里标成验证步，失败或超时按证据停下。

需要实际修改时先 `repair.begin` 建事务工作区；写工具带 `workspaceId`，只改工作区；用 `repair.diff` 看改动；用 `repair.verify` 在工作区启动验证；验证通过后再用 `repair.finish(commit)` 提交，失败或不确定就 `repair.finish(rollback)`。事务活动期间不要直接改原实例。

先用 workflow.submitPlan 提交计划：每步只写一个工具调用，说明理由和可回退方式，并至少包含一个验证步。计划通过前不写入；通过后按顺序执行，每步之后重新检查关键项。失败就停下，写明证据和失败在哪一步，不继续猜。最后用 workflow.finish 提交报告，写清做了什么、证据、回退方式、未解决项；没有验证的改动不能报成已修复。
