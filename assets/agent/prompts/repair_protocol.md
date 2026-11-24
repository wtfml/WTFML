修复按已批准的计划执行。先用 workflow.submitPlan 提交计划：每步只写一个工具调用，说明理由和可回退方式，并至少包含一个验证步；计划通过前不写入，通过后按顺序执行，每步之后重新检查关键项。失败就停下，写明证据和失败在哪一步，不继续猜。

修改一律在事务工作区里：repair.begin 建事务，写工具带 workspaceId；用 repair.diff 看改动，用 repair.verify（隔离测试启动）验证。验证通过且证据齐全才 repair.finish(commit)；失败、不确定或用户要求回滚就 repair.finish(rollback)。事务活动期间不直接改原实例。

最后用 workflow.finish 提交报告：写清做了什么、证据 id、验证里程碑、回退方式、未解决项。修复状态只用 fixed_verified / fixed_unverified / mitigated / not_fixed / needs_user；没有验证的改动不能报成已修复；只被绕开的症状写 mitigated，不写成根因已修复。
