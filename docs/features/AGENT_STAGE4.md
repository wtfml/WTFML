# 智能体 Stage 4：收口与发布

本文给开发者和发布负责人看 Stage 4 的**已做项、自动化结果、发布检查与仍未完成的真机项**。设计见 [AGENT.md](AGENT.md) §14，人工验收清单见 [AGENT_STAGE3_4_ACCEPTANCE.md](AGENT_STAGE3_4_ACCEPTANCE.md)，性能数字见 [AGENT_STAGE4](../dev/perf-notes/AGENT_STAGE4.md)。

## 1. 这一阶段做了什么

- 全量回归按影响面跑了一遍（core / window / network / ci-ui），结果见 §2。
- 新增可重复的性能基线用例：核心层（2000 条会话读写/加密/上下文裁剪/token 估算）与智能体页（2000 条消息一次性重摆），数字记在 [perf-notes](../dev/perf-notes/AGENT_STAGE4.md)。
- 新增发布前检查脚本 `tools/check-agent-release.ps1`：明文密钥形态、测试假端点、`command.run` 是否被误注册、审批/确认闸调用点是否还在、危险开关默认值。
- 文档收口：`FEATURES.md`、`TESTING.md`、`ROADMAP.md`、`SECURITY_BOUNDARIES.md`、`AGENT.md` 与本篇对齐；阶段状态改为"已实现/待 G4 人工验收"。
- 真机/实网/高 DPI/读屏/长路径/换用户解密等**只能在真机做**的项没有执行，留在 G4 清单里。

## 2. 自动化回归（2026-11-19，`build/verify` Release）

| 套件 | CTest 选择 | 结果 |
| --- | --- | --- |
| core | `tools/test.ps1 -Suite core -NoBuild` | 15/15 通过（224 s） |
| window | `-Suite window` | 3/3 通过（252 s） |
| network | `-Suite network` | 6/6 通过（117 s） |
| ci-ui | `-Suite ci-ui` | 23/23 通过 |
| 智能体专项 | `-R core-agent|ui-agent|main-window-agent` | 3/3 通过；`verify_agent` 767、`ui_check_agent` 77、`mw_check_agent` 76 |

- 视觉套件（label `visual`）**没有跑**：需要真实显示与人工看样张，留给 G4。
- 第一次跑 core 时 `wtfml.core-resources` 红过一次，原因是工作区里 `assets/assetdirector/known-sets.json` 被检出成 CRLF（仓库里是 LF），嵌入字节与盘上文件对不上。把文件恢复成 LF 后同一套件 15/15 通过；这与智能体改动无关，属于检出环境问题。

## 3. 发布检查

`pwsh tools/check-agent-release.ps1` 结果：**PASS**。它查这几类：

| 检查 | 规则 |
| --- | --- |
| 明文密钥形态 | `src/`、`ui/` 的字符串字面量里不许出现 `sk-…`、`AKIA…`、`ghp_…`、长 Bearer 串；注释与占位提示不算 |
| 测试假端点 | 不许把 `localhost:port`、`127.0.0.1:port`、`example.com` 当默认地址；`setPlaceholderText` 的示例不算 |
| 任意命令 | `AgentToolRegistry.cpp` 里不许出现 `command.run` |
| 审批/确认闸 | 启动器工具、联网、诊断命令、MCP 工具都必须有 `AgentApprovalPolicy::request` 调用点；MCP 必须保留 `Gate::NeedsConfirmation` 与 `checkStart`；联网必须保留 `allAddressesAllowed` |
| 危险开关默认值 | 诊断命令默认 `false`；MCP 服务器默认 `enabled = false` |

脚本是只读的，有违规退出码 1；`dev` 上应该和 `check-doc-links` 一起跑。

## 4. 手工确认过的默认值

| 项 | 默认 |
| --- | --- |
| 诊断命令 | 关闭（`Agent/DiagnoseCommands=false`） |
| MCP 服务器 | 不带任何服务器；新增默认 `enabled=false`，首次启动必须确认 |
| 联网搜索 | 没有 SearXNG 地址时不提供 |
| 严格模式 | 关闭 |
| 会话存储 | 明文 JSON（不含密钥）；整会话加密需手动开启 |
| `command.run` | 不存在这个工具 |

## 5. 仍未完成（G4 人工项）

清单见 [AGENT_STAGE3_4_ACCEPTANCE.md](AGENT_STAGE3_4_ACCEPTANCE.md) 的 G4 一节，主要是：

- 真机装发布物（无 Qt 开发环境）启动、智能体页可打开；
- 真机/实网：真实模型流式与错误、真实实例的体检/崩溃急救/备份恢复、真实 MCP 服务器、真实联网站点与代理/证书场景；
- 中文/空格/长路径数据目录、受限权限账户；
- 高 DPI（125%/150%/200%）与读屏基础检查；
- 换 Windows 用户后的加密会话、DPAPI 后备、Credential Manager 主路径；
- 视觉套件与样张人工确认；
- 长会话滚动手感（性能基线里 2000 条一次性重摆 3820 ms，见 perf-notes）。

G4 未签之前不上发布物；`main` 只在 `dev` 上 CI 通过且 G4 通过后快进。

## 6. 未覆盖

| 项 | 状态 |
| --- | --- |
| 视觉套件（label `visual`） | 本轮未跑 |
| 真机高 DPI / 读屏 | 未覆盖 |
| 换用户解密失败/DPAPI 后备真机确认 | 未覆盖（自动化已有，真机未做） |
| 长时间运行内存/句柄 | 未测量 |
| 资源搜索/安装工具、Brave/Tavily、HTTP/SSE MCP、`command.run` | 未实现（ROADMAP 有） |
