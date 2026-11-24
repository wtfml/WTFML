# 智能体自主修复（Stage 5）开发计划

本文是智能体从「诊断助手」扩展到「在锁定实例内自主修复」的开发拆解，给实现、评审和验收的人看。现行行为见 [AGENT](../features/AGENT.md)，未完成项总账见 [ROADMAP](../ROADMAP.md)，安全边界见 [SECURITY_BOUNDARIES](../features/SECURITY_BOUNDARIES.md)。本文的工具在实现前不进入产品，也不写进 `features/`。

## 1. 目标与完成判据

目标是在当前锁定实例中，用启动器自己的诊断、兼容性、备份、修复、启动测试和回滚能力，尽可能自主完成故障处理。凡工具和权限能完成的操作直接执行，不把人工操作步骤当成结果交回用户。

一个修复只有同时满足下面四条才算完成：

1. 根因或最小故障集合有本地证据，证据能落到具体文件、行号或工具结果。
2. 修改先落在隔离工作区，原实例在提交前一个字节不变。
3. 用启动器自己的启动流程实际启动过一次，并达到约定的里程碑。
4. 提交有备份，失败或验证不通过能回到原状。

报告状态只用这五个值；`rollback_failed` 等是事务内部状态，不当作修复报告状态：

| 状态 | 含义 |
| --- | --- |
| `fixed_verified` | 工作区验证达到里程碑，提交成功，计划要求的提交后验证（如有）也通过 |
| `fixed_unverified` | 工作区验证通过并已提交，但计划要求的提交后验证没做或没达到里程碑 |
| `mitigated` | 症状被绕开或范围被缩小，根因未确认 |
| `not_fixed` | 验证失败，已回滚或未提交 |
| `needs_user` | 缺工具、缺权限、缺外部信息，继续执行会越界 |

以 Mixin 冲突为例：现在能读日志、找到相关模组、提交禁用计划、执行禁用、再体检；缺的是隔离启动、判断这次启动到底成功还是失败、失败后回滚并继续验证下一个嫌疑。Stage 5 补的就是这段闭环。

不做的事：任意命令、任意 URL 下载、无人值守后台维护、直接修改用户世界存档、自动改驱动或系统设置。

## 2. 现状与缺口

现有工具的上限见 [AGENT](../features/AGENT.md) §9：`logs.tail` 最多 500 行、读盘 256 KB；`crash.list` 每份只读头 8 KB；`diagnostics.crash` 只给退出事实与规则提示（S5.3 之前的形态，之后由 `diagnostics.crashSnapshot` 接管结构化解析）；`instance.integrity` 只覆盖客户端 jar、库、资源索引和资源对象；`mods.list` / `mods.setEnabled` 不读模组声明；所有修复都停在「改完建议用户手动启动」。

| 故障 | 现在能做的 | 缺口 |
| --- | --- | --- |
| Java 版本不匹配 | `java.list`、`settings.set` 改 Java 或自动选择 | 改完不能实际启动验证，失败不能自动回退 |
| OOM | `memory.suggest`、`settings.set` 改 Xmx/Xms | 只按堆内存建议，不区分 Metaspace、Direct Buffer、native thread；无启动验证 |
| 缺模组前置 | `diagnostics.crash` 的规则提示、`mods.list` | 不读模组声明、不建已装依赖图、不能安装前置 |
| Mod 版本冲突 | `mods.list`、`mods.setEnabled` | 没有兼容矩阵，不能查兼容版本或下载候选文件 |
| Mod 文件损坏 | 无 | `instance.integrity` 不覆盖 `mods/`，不能重新下载修复 |
| 配置文件错误 | `fs.read` / `fs.write` 白名单 | 不解析配置结构，只能靠备份整体回退 |
| Mixin 冲突 | `diagnostics.crash` 的日志与规则提示 | 不认 mixin 配置归属，不能二分定位 |
| 游戏库文件损坏 | `instance.integrity` 只报 | 没有把 `VersionManager::repairVersionFiles()` 暴露成受控修复工具 |
| 显卡 / Native 崩溃 | 退出码和日志的有限分类 | 不解析 `hs_err_pid` 与 native 模块，不能安全复现 |
| 世界 / 存档崩溃 | 有限的崩溃诊断 | 不复制世界、不进世界验证，不能判断修复是否覆盖世界加载 |

## 3. 总体设计

### 3.1 分层

```text
ui/Agent* → AgentController → src/agent/
  ├─ AgentPromptLibrary        提示词加载与版本
  ├─ AgentCrashKnowledge       崩溃规则与里程碑规则
  ├─ AgentWorkspaceStore       隔离工作区
  ├─ AgentTestLaunch           启动测试
  ├─ AgentRepairTransaction    事务、差异、提交、回滚
  ├─ AgentModCompat            本地声明、平台兼容、二分
  └─ 现有工具与工作流
        ↓
AgentLauncherHost（同步只读/短操作）
AgentLaunchHost / AgentRepairHost（异步长操作）
        ↓
src/controllers/Agent*HostImpl → LaunchController / LauncherManager / VersionManager /
                                 ResourceStore / DownloadManager / AgentBackupIndex
```

`src/agent/` 仍不 include `ui/` 或 `MainWindow`；业务能力经宿主接口接入。启动测试、工作区复制、二分是异步工具：引擎返回 `Status::Pending`，结果经 `AgentToolSink` 回灌，不阻塞 UI，不开嵌套事件循环，取消后迟到结果丢弃。新工具不接受模型传入的实例 id，实例仍由会话锁定提供（D8）；工作区 id 只认本会话事务或测试启动登记过的值。

### 3.2 启动路径

测试启动必须走启动器自己的准备与启动代码，不拼 shell，不直接调用 `java.exe`。为此先做两件底层改动：

1. 引入 `LaunchRoots`（数据根、版本目录、版本 id、离线标记、测试身份）。`LauncherManager::generateJvmArguments()` 现在从 `AppPaths::minecraftDir()` / `AppPaths::versionDir()` 现算绝对路径，改为由调用方显式传入；`AsciiPathAlias`、`InstanceFiles::jarPath()` 等派生路径也走这一份 roots。普通启动传当前游戏根，测试启动传工作区根，不用 `AppPaths::setMinecraftDir()` 切换进程级状态。
2. S5.1 实际只复用 `LauncherManager::generateJvmArguments()`、`GlobalJavaSettings::ensureJavaForVersion()`（`allowDownload=false`）和实例设置合成，没有把 `LaunchController::onStartButtonClicked()` 的登录/向导/natives 整段抽出来。测试 profile 强制离线、固定测试名 `WTFML-Test`、不刷新令牌、不下载 Java、不连服务器、不带用户传入的参数；旧版需要 Java 8 ASCII 别名/natives 解压的实例暂不作为测试目标。

测试进程由 `AgentTestLaunchRunner` 自己持有 `QProcess`，不复用 `LauncherManager` 的用户会话表：不弹日志窗、不写「最近启动」、不在任务中心留「运行中」条目。进程环境、优先级和 Job Object 清理复用 `LauncherManager` 的同一套设置，超时或取消时整棵杀掉。

里程碑由规则文件判定，不只看进程还活着：

| 里程碑 | 判据 |
| --- | --- |
| `process_started` | `QProcess` 发出 `started` / 进入 Running，进程仍在运行 |
| `main_menu` | 新日志命中主菜单规则（原版/Forge/Fabric/NeoForge 各自的标记），并在观察窗口内无致命异常、无新崩溃报告 |
| `world_loaded` | 指定副本世界通过 quickPlay 启动，命中世界加载完成规则，并稳定到观察窗口结束 |
| `crashed` | 进程异常退出，或出现新崩溃报告 / `hs_err_pid`，或日志命中崩溃规则 |
| `timed_out` | 到超时仍未达到目标里程碑 |
| `cancelled` | 用户取消、会话关闭或启动器退出 |

里程碑判定在 `AgentTestLaunchMilestones`（纯函数），标记本身从 S5.3 起在 `assets/agent/knowledge/launch_milestones.json`，不再写进 C++；读不到标记时三个判定都返回 false（宁可超时，也不凭空认为进了主菜单）。

### 3.3 隔离工作区

工作区是一个独立游戏根，默认放在 `<游戏根>/cache/agent-workspaces/<事务或启动 id>/`，与源实例同卷，避免整份实例跨盘复制。物理路径由 `AppPaths` 新增 `agentWorkspacesDir()` 给出，`AgentPaths` 只放登记和事务数据。

- S5.1 复用 `VersionManager::copyInstanceFrom()` 的路径校验、暂存和发布逻辑，整份复制实例；`logs/`、`crash-reports/`、截图暂未排除，留给事务阶段按类别筛选。
- S5.1 用 `VersionManager::copyInstanceFrom()` 整份复制实例到工作区；`assets/`、`libraries/` 通过 `LaunchRoots` 指向当前游戏根只读共享，客户端 jar 取源实例，兄弟 jar 取工作区副本。工作区不建指向源目录的可写链接；后续事务阶段再做按类别筛选复制。
- 世界验证只允许工作区 `saves/` 里已经有的世界；S5.1 整份复制会带上源 `saves/`，启动时只按 `quickPlay.world` 进入副本，不回写源存档。`saves/` 永远不是提交范围。
- 复制前检查源实例未运行；后台复制前按源目录文件总大小检查 16 GB 上限与目标卷剩余空间（估算体积 + 1 GB），超过就拒绝，让用户改用只读诊断。
- 每个工作区在 `agent/workspaces/<id>.json` 登记来源、用途、创建时间、状态和日志位置；启动恢复扫描这张表，不接管未登记的目录。
- 测试启动和修复事务的工作区默认保留（稳定/崩溃/超时/提交后都留），日志、登记和事务备份留给后续排查；自动清理尚未实现，`repair.status` 只列出状态。

### 3.4 修复事务

事务是「工作区修改 → 启动验证 → 提交或回滚」的唯一载体，状态机：

```text
prepared → applying → verifying → ready_to_commit → committing → committed
     ↘ cancelled          ↘ failed → rolling_back → rolled_back / rollback_failed
```

- `repair.begin` 建事务、工作区和基线指纹，返回 `transactionId` / `workspaceId`。
- 现有写工具（`mods.setEnabled`、`fs.write`、`settings.set`）在事务内带 `workspaceId` 时只改工作区，不改原实例；`settings.set` 只记设置差异，测试启动时把差异叠加到有效设置上，提交时才写回启动器设置。
- 每次写操作记 `seq`、工具、参数摘要、修改前后哈希、文本差异、结果。提交前重新核对被改文件仍等于基线；源文件被用户或别的程序改过就拒绝提交，要求重新诊断。
- `repair.finish` 提交时只重放差异，不整份覆盖原实例。每个文件先备份到事务备份目录，再原子替换，再回读哈希；任一文件失败就按日志回滚已提交的文件。
- 事务级备份只覆盖该事务触达的文件，默认上限 2000 个文件 / 512 MB；超过时要求先做整实例备份，不把 `backup.create` 的 400 文件 / 32 MB 上限硬撑大。备份内容只在本地，模型只能看 id 和哈希；小文件沿用 `AgentBackupIndex` 的 DPAPI 容器，超过容器预算的文件用原文件副本加 SHA-256。
- 提交阶段不提供中途取消；取消请求在提交开始前生效，提交开始后最多执行到当前文件结束并回滚。
- 启动器退出或崩溃留下的 `committing` 事务，下次启动时只做恢复检查，不自动续跑提交；恢复结果写进 `repair.status` 和报告。

### 3.5 诊断知识库

崩溃类型、日志签名、模组归属规则和启动里程碑放在内嵌数据文件里，不写进提示词：

```text
assets/agent/
  prompts/                  技能提示词
  knowledge/crash_rules.json
  knowledge/launch_milestones.json
  agent.qrc                 上面两类文件共用一份清单
```

规则字段至少包括 `id`、`priority`、`phase`、`match`、`requires`、`suspects`、`confidence`、`confidenceBasis`、`recommendedTests`、`suggestedActions`。`confidence` 由规则和证据加分算出，封顶 0.95，不返回 1.0；模组只在日志里被提到、没有 stack/config 归属证据时，置信度不超过 0.4，角色标成 `related`，不标成根因。

规则加载时校验 JSON、规则 id 唯一、匹配串长度和数量上限；坏规则跳过并记日志，工具返回 `knowledgeVersion` 和 `matchedRules`，读不到规则时只给事实，不编造置信度。

### 3.6 提示词

提示词在 `assets/agent/prompts/`，由 `AgentPromptLibrary` 从 Qt 资源加载；`AgentSkill.cpp` 只留技能元数据。组合顺序固定为 `common.md` + 技能文件 +（崩溃急救/模组维护）`repair_protocol.md`；每个文件 UTF-8、无 BOM、统一 LF、非空，单文件硬上限 64 KB，`common + 技能 + 协议` 目标不超过 16 KB。

`AgentSkill` 增加 `promptId`、`promptHash`、`promptVersion`；`promptVersion` 是每个技能的人工版本号，改提示词时递增，`promptHash` 每次加载现算。`AgentSkill.cpp` 只留技能 id、显示名、工具白名单、只读/计划标记和提示词文件名，不再出现提示词正文。资源缺失时技能不可用并明确报错，不回退成空提示。

### 3.7 界面与用户可见状态

保持现有五个模式，Auto-safe 是崩溃急救/模组维护里的事务开关，不新增第六个模式。

- 事务审批卡：范围、空间估算、允许的文件/设置键/模组、测试计划、提交策略、超时和启动次数上限。
- 执行卡：阶段、已用时间、启动次数、当前候选集合、停止按钮；停止后先回滚再出报告。
- 报告卡：修复状态、根因与证据、改动文件与差异、验证里程碑、备份 id、回滚入口、未验证场景。
- 切换页面不停止事务；关闭启动器停止测试进程并保留事务日志，下次启动只做恢复检查。
- 新增界面文案走 `tr()` 并跑 `check-i18n.ps1`；提示词是模型输入，不进翻译文件。

## 4. 阶段计划

顺序：S5.2 是所有后续阶段的前置；S5.3–S5.5 在闭环底座可用后可并行；S5.6 在工具稳定后；S5.7 最后并需产品确认。每个阶段拆成可独立评审的提交，测试和文档跟对应改动一起走。

### S5.0 提示词外置（已完成 2026-11-24）

实现：

- `assets/agent/prompts/` 下放 `common.md`、`general.md`、`inspection.md`、`crash_rescue.md`、`upgrade.md`、`mods.md`、`repair_protocol.md` 和 `README.md`；`assets/agent/agent.qrc` 前缀 `/agent`，产品目标与 `wtfml_core_runtime` 各带一份，`ui_check_agent` / `mw_check_agent` 经 runtime 共用。
- `AgentPromptLibrary` 负责加载、组合、缓存、SHA-256、UTF-8/空文件/64 KB 上限校验；`Q_INIT_RESOURCE(agent)` 负责静态库场景的资源初始化。
- `AgentSkill` 增加 `promptId` / `promptHash` / `promptVersion` / `promptProblem`；`AgentSkill.cpp` 只留技能元数据和工具白名单。`AgentController` 在发送前和切换模式时拒绝坏提示词。
- `tools/check-agent-release.ps1` 检查提示词目录、qrc 登记、BOM、空文件、密钥形态、`AgentSkill.cpp` 无正文和 `Q_INIT_RESOURCE(agent)`。
- `tests/core_modules/AgentSkillsTests.cpp` 覆盖加载、组合、缺失、非法 id、大小上限和每个技能都带共享规则。

组合顺序为 `common` + 技能 +（崩溃急救/模组维护）`repair_protocol`。共享规则现在对所有技能生效；技能正文仍保持原语义，没有提前引入 S5.1 之后的工具。

验收：`verify_agent` core-agent 846/846、`ui_check_agent` 77/77、`mw_check_agent` 83/83；`check-agent-release.ps1`、`check-i18n.ps1`、`check-test-modules.ps1` PASS；产品与测试目标编译通过。

### S5.1 隔离工作区与启动测试（已完成 2026-11-24）

实现：

- `AgentWorkspaceStore`：`ws-` id、物理根 `<游戏根>/cache/agent-workspaces/<id>`、登记/日志在 `%LOCALAPPDATA%/WTFML/agent/workspaces/`、状态、列表、安全清理；`AppPaths::agentWorkspacesDir()`、`AgentPaths::workspaceRecordsDir()` 是唯一路径出处。
- `LauncherManager::generateJvmArguments()` 增加可选 `LaunchRoots`：普通启动不传、逐字节沿用 AppPaths；测试启动把 `versionDir` 指到工作区，`assets`/`libraries` 仍来自当前游戏根，客户端 jar 与兄弟 jar 分别取源/工作区路径。`startGame()` 的 `--gameDir`/`ensureInitialOptions()` 改为跟随参数里的 gameDir。
- `AgentLaunchHost` 异步接口 + `AgentTestLaunchToolEngine`：`instance.testLaunch` 参数解析、审批、Pending、结果回灌；`AgentCompositeToolEngine` 与 `AgentController` 已接入，`AgentLauncherHostImpl` 实现该接口。
- `AgentTestLaunchRunner`：工作区复制与 Java 探测在 `QtConcurrent`，进程、日志轮询和 Job Object 清理在主线程；离线测试身份、不刷新令牌、不下载 Java、不登记用户会话；拒绝 `-javaagent`/`-agentlib`/`-Xbootclasspath` 等参数。
- 里程碑规则：`AgentTestLaunch::AgentTestLaunchMilestones` 覆盖主菜单（`Sound engine started`/`OpenAL initialized`）、世界（`Preparing spawn area: 100%` 等）和日志崩溃标记；宿主另看退出码、新崩溃报告与 `hs_err_pid`。
- 技能：崩溃急救/模组维护的白名单加入 `instance.testLaunch`；工作流允许它作为 `verify=true` 的验证步（执行游戏但不写原实例）。
- 夹具与检查：`AgentWorkspaceStoreTests`、`AgentTestLaunchTests` 覆盖登记/清理、里程碑、参数、审批、异步交付；`check-agent-release.ps1` 纳入测试启动审批闸。

验收：`verify_agent` core-agent 905/905、`ui_check_agent` 77/77、`mw_check_agent` 83/83、`verify_launch` 370/370；产品目标编译通过；`check-agent-release.ps1`、`check-i18n.ps1`、`check-test-modules.ps1` PASS。真实 1.20.1 实例的启动/崩溃/超时仍需真机复跑（A9）。

限制：工作区目前整份复制实例，未做按类别筛选；测试启动不自动下载 Java；Java 8 的 ASCII 别名不用于工作区；工作区默认保留，启动恢复扫描与自动清理留给后续。

### S5.2 修复事务与回滚（已完成 2026-11-24）

实现：

- `AgentRepairTransactionStore`：事务日志、状态、基线 `相对路径 → SHA-256`、设置增量、验证结果；`AgentPaths::transactionsDir()` 是唯一路径出处。
- `AgentRepairHost` + `AgentRepairRunner`：`repair.begin` 复制工作区、从源实例记录基线；`repair.finish(commit)` 先核对源文件哈希、备份触达文件、原子重放差异、回读哈希，失败按已应用清单回滚；`repair.finish(rollback)` 只改事务状态，原实例不变。
- 工具：`repair.begin`、`repair.diff`、`repair.verify`、`repair.finish`、`repair.status`。`repair.begin`/commit 逐次审批，rollback 自动，`repair.diff`/`status` 自动。
- 写工具路由：`fs.write`、`mods.setEnabled`、`settings.set` 接受可选 `workspaceId`；事务内写工作区，事务外保持现行行为。事务活动期间不带 `workspaceId` 的直接写工具、`backup.restore` 一律拒绝。
- `settings.set` 在事务内只记增量，提交时才通过宿主写回；`repair.diff` 同时列出设置增量。
- 工作流：崩溃急救/模组维护白名单加入事务工具；`repair.verify` 可作为验证步；`repair.finish` 在失败后仍可回滚；`workflow.finish` 检测到未收尾事务时拒绝报完成。
- 测试：`AgentRepairTests` 覆盖基线/差异、事务日志、begin/verify/finish 审批与异步交付；`AgentWorkflowTests` 覆盖事务计划。

验收：`verify_agent` core-agent 944/944、`ui_check_agent` 77/77、`mw_check_agent` 83/83、`verify_launch` 370/370；主程序与测试目标编译；检查脚本 PASS。真实 1.20.1 的提交/回滚 A10 未跑。

限制：

- 事务提交只处理 `mods/config/defaultconfigs/options/settings`；世界/存档不提交。
- 事务备份是 `agent/transactions/<id>/backups/` 下的本地原文件副本，不是 DPAPI 容器；模型只拿相对路径和 id。
- 复制/提交的当前实现没有独立取消；关闭启动器会等待当前后台操作。启动恢复扫描尚未接到 UI，只有 `repair.status` 能列出中断事务。
- 提交后没有自动在原实例再启动验证；需要时由计划另行调用 `instance.testLaunch`。

### S5.3 崩溃快照与日志检索（已完成 2026-11-24）

新增工具：

| 工具 | 输入 | 返回 | 上限 |
| --- | --- | --- | --- |
| `diagnostics.crashSnapshot` | `reportId?` | 环境、崩溃阶段、异常链、模组嫌疑、命中规则、建议验证、证据 id | 报告 8 MB，输出 16 KB，10 秒 |
| `logs.search` | `query`、`source`、`before?`、`after?`、`maxMatches?` | 行号、脱敏摘录、总匹配数 | 200 条，500 行，256 KB |
| `logs.readRange` | `source`、`startLine`、`lineCount` | 行范围、总行数、是否截断 | 500 行，256 KB |
| `crash.readFull` | `reportId` | 分段目录、每段摘要、证据 id；原文经脱敏后进本地 artifact | 8 MB，输出 24 KB |

`diagnostics.crashSnapshot` 返回示例：

```json
{
  "environment": {"minecraftVersion": "1.20.1", "loader": "forge", "loaderVersion": "47.3.0", "javaMajor": 17},
  "crash": {"phase": "mod_initialization", "exception": "MixinApplyError", "causeChain": ["MixinApplyError", "InvalidInjectionException"]},
  "suspects": [{"modId": "examplemod", "version": "1.2.0", "role": "related", "confidence": 0.72, "confidenceBasis": "MIXIN_001 + stack frame", "evidence": ["E1", "E2"]}],
  "matchedRules": ["MIXIN_001"],
  "recommendedTests": ["inspect_mixin_ownership", "bisect_mod_pair"],
  "suggestedAction": "check_mod_compatibility",
  "truncated": false
}
```

实现要点：

- 日志按流式建行索引，缓存以文件大小 + 修改时间失效；`latest.log`、实例日志、崩溃报告分开索引。
- `logs.search` 首版做字面量和异常类型精确匹配；同一阶段后段接入带匹配上限的引擎（如带 match limit 的 PCRE2）后再开正则，不让模型用无界正则跑在 8 MB 日志上。
- `crashSnapshot` 解析 `Caused by` 链、System Details、Suspected Mods、Mod List、stack frame 的类名；`Suspected Mods` 只作为线索，不直接当根因。
- 读 JAR 内的 mixin 配置声明（`fabric.mod.json`、`META-INF/*.json` 等），建立配置名到 mod 的映射；读不到就标 `unknown`，不按文件名猜。
- 证据带 `id`、`kind`、`source`、`capturedAt`、`sha256`、`excerpt`；摘录过脱敏，路径换 `<path>`，不带账户、环境变量、硬件指纹。
- `crash.readFull` 只回分段摘要和行号；需要原文时走 `artifact.read`，每次重新脱敏。

规则先覆盖：Mixin 注入失败、堆 OOM、Metaspace/Direct Buffer/native thread、Java 版本不匹配、缺前置、重复模组、JAR 损坏、配置解析失败、native 崩溃、世界加载/实体 tick 崩溃。每类至少一组真实结构夹具；规则命中但证据不足时返回 `unknown` 和 `recommendedTests`，不编造责任模组。

实现：

- `AgentKnowledgeRules`：加载 `assets/agent/knowledge/crash_rules.json` 与 `launch_milestones.json`（`Q_INIT_RESOURCE(agent)`）；校验 id 唯一、phase 枚举、字面量长度与条数上限，坏规则跳过并记 `problems`，整份读不到时只给事实、`matchedRules` 为空。`AgentTestLaunchMilestones` 改为按 `launch_milestones.json` 判定。
- `AgentCrashSnapshot`：解析 `---- Minecraft Crash Report ----`、`hs_err_pid` 与游戏日志；取环境（游戏版本/加载器/加载器版本/Java 主版本）、`Description`、顶层异常与 `Caused by` 链、`Suspected Mods`、Forge `Mod List:` 表格、Fabric `Fabric Mods:` 与日志 `Loading N mods:` 清单、`Mixin apply for mod X failed`、`xxx.mixins.json`、失败区栈帧类名。
- 嫌疑分档：`candidate`（栈帧或 mixin 配置归属，规则值 +0.12/+0.05）、`related`（只被提到，≤0.4）、`unknown`（配置无归属）；封顶 0.95。只被提到、或只出现在模组清单里的模组不进 `candidate`，日志里先出现的模组不会被误当成根因。
- `AgentMixinOwnership`：读 `fabric.mod.json`、`quilt.mod.json`、`META-INF/mods.toml`、`META-INF/neoforge.mods.toml`、`META-INF/MANIFEST.MF` 的 mixin 声明，经 `ZipUtil::readMetadataBatch()` 一个进程读完一批（≤300 个 JAR）；只在崩溃文本点名了配置时才扫。
- `AgentLogIndex`：流式行索引（记录每行起始偏移，按文件大小 + 修改时间失效，最多 4 份缓存），搜索按行号回命中与前后文，ASCII 搜索词在 UTF-8 字节上匹配、只解码要输出的行；单行超长按一行截断处理，行号不漂。
- 工具：`diagnostics.crashSnapshot`、`logs.search`、`logs.readRange`、`crash.readFull` 全部只读自动；`crash.readFull` 把换掉本机路径后的原文存进模型可读 artifact，只回分段目录与摘要。崩溃急救/体检/模组维护的白名单加入这四个工具，`crash_rescue.md` 改为先调 `crashSnapshot`。
- 规则库覆盖 Mixin 注入失败、堆 OOM、Metaspace、Direct Buffer、native thread、Java 版本不匹配、缺前置、重复模组、JAR 损坏、配置解析失败、native 崩溃、世界加载、实体 tick、通用模组加载失败；`tests/meta/agent/` 每类一份结构夹具。
- `tools/check-agent-release.ps1` 增加知识文件检查：存在、登记进 `agent.qrc`、有效 JSON、无 BOM、≤256 KB、规则 id 唯一与 phase 合法、里程碑三个键齐。

验收：`verify_agent` 1166/1166、`verify_launch` 370/370、`ui_check_agent` 77/77、`mw_check_agent` 91/91；产品目标按 `-Wall -Wextra -Wpedantic -Werror` 编译通过；`check-agent-release.ps1`、`check-i18n.ps1`、`check-test-modules.ps1`、`check-doc-links.ps1`、`check-repo-layout.ps1` PASS。100 MB / 161 万行日志实测：首次行索引 + 读 200 行 98 ms，全文搜索 353 ms（见 [PERFORMANCE](PERFORMANCE.md)）。真实 1.20.1 实例上的崩溃报告与整合包模组集仍需真机复跑（A9）。

限制：

- `logs.search` 在调用线程上顺序扫描：100 MB 约 350 ms，超过 6 s 预算回部分结果并标 `truncated`。没有异步扫描，也不是无阻塞。
- 首版只做字面量匹配；正则要等带匹配上限的引擎。
- mixin 归属只读 JAR 声明，声明里的配置名与实际加载的配置不一致时仍会落到 `unknown`。
- 规则是启发式：置信度只表示"证据类型 + 规则强度"，不是概率；没有匹配规则时 `phase` 为 `unknown`。

### S5.4 模组兼容性与文件修复（已完成 2026-11-24）

改动：

- 把 `UpgradeAssessment.cpp` 里的本地声明解析抽成无 UI 的 `ModDeclarations`：`fabric.mod.json`、`quilt.mod.json`、`META-INF/mods.toml`、`META-INF/neoforge.mods.toml`、`mcmod.info`，补 mod id / version / 依赖 / 加载器 / Java 范围。
- 复用 `ModDependencies`、`ResourceStore`、`DownloadManager`、`InstanceUpdateCheckController` 的 SHA-1 身份缓存，不另写一套 Modrinth 客户端。
- 新增 `mods.analyzeCompatibility`：本地声明 + 已装依赖图 + 可选平台查询，输出 `issues[]` 和 `unknown[]`。
- 新增 `mods.verifyFiles`：本地校验 `mods/` 下 JAR 的 zip 结构、大小、SHA-1；按哈希在 Modrinth 找回原文件属于平台查询，走 `mods.analyzeCompatibility` 同一条外发确认。
- 新增 `mods.installFix`：只接受 `projectId` / `versionId` / `expectedSha1`，由本地解析出下载地址；下载到暂存目录，校验大小和哈希，写入事务工作区，必要时补齐必要前置。没有活动事务时拒绝，不直接改原实例。
- 新增 `instance.repairCore`：复用 `VersionManager::repairVersionFiles()`，只修客户端 jar / 库 / 资源索引，单独审批，不静默改共享库；有活动事务时拒绝执行，避免原件在工作区验证期间变化。

`mods.analyzeCompatibility` 的问题类型：

| 类型 | 判据 |
| --- | --- |
| `missing_dependency` | 本地声明要求的前置 mod id 不在已装清单，或只有 `.jar.disabled` |
| `incompatible_version` | 本地声明的游戏版本/加载器范围不满足，或平台版本与实例不匹配 |
| `loader_mismatch` | 模组声明的加载器与实例加载器不一致 |
| `duplicate_mod` | 同一个 mod id 出现多个文件 |
| `corrupt_file` | zip 结构损坏、大小为 0、哈希与平台记录不一致 |
| `unknown` | 声明读不懂、平台查询失败、没有身份记录 |

平台查询只在用户同意后发出，首次按实例/会话确认「把 N 个模组文件的 SHA-1 发给 Modrinth」，不发文件名；严格模式下每次确认；`offline=true` 只做本地分析。平台结果缓存 24 小时，键为 `sha1 + gameVersion + loader`。平台只接 Modrinth；CurseForge 作为后续项，不按名称或 slug 猜项目。必要前置在 Modrinth 找不到时整批停止，不改用 CurseForge，也不按名称搜索替代品。

实现：

- `ModDeclarations`（`src/operations/`）：从 `UpgradeAssessment.cpp` 抽出的本地声明解析，升级评估与智能体共用一份；除游戏/加载器/Java 范围外，补 mod id、版本、required 前置与加载器 id。`UpgradeAssessment` 改为调用它，`mw_check` 3192/3192 逐字节不变。
- `AgentModCompat`：纯函数判定 `loader_mismatch` / `incompatible_version` / `missing_dependency`（区分"未安装"和"只有 `.jar.disabled`"）/ `duplicate_mod` / `corrupt_file` / `unknown`，并能合并平台结果（平台版本的游戏版本、加载器、必需项目号）。
- 平台查询经 `AgentModHost`：只接 Modrinth、只发 SHA-1（每批 ≤100、每次 ≤2000、总 180 s），地址与解析复用 `ModpackExport`/`ResourceCatalog` 现有纯函数；首次按实例确认一次，严格模式每次，`offline=true` 不发。
- `mods.installFix`：要求活动事务与登记工作区；解析目标 → 下载到 `agent/staging/` → 校验大小/哈希 → 复制到工作区临时名 → 再校验 → 改名；失败撤销本次复制，工作区回到调用前；单文件 100 MB、单事务 500 MB、≤10 个文件；可补齐版本声明的必需前置。
- `mods.verifyFiles`：本地查 zip 结构、大小、SHA-1；`platform=true` 时按哈希找回候选版本（同一份平台确认）。
- `instance.repairCore`：每次审批，事务活动期间拒绝；经 `VersionManager::repairVersionFiles()` 重下客户端 jar、库和资源索引，等 `versionDownloadFinished` 收尾。
- 工具注册、技能白名单（体检/崩溃急救只读集合加分析与校验；崩溃急救/模组维护的写集合加修复与核心修复）与 `mods.md` 提示词（promptVersion 2）同步；`check-agent-release.ps1` 增加模组工具审批闸与"只认事务工作区"检查。
- 测试：`AgentModTests` 覆盖声明解析（fabric/quilt/mods.toml/mcmod.info）、每类判定、平台确认与拒绝、非严格/严格模式的确认次数、校验、安装成功与两类失败（哈希不符、前置缺失）、核心修复与事务拒绝；`mw_check_agent` 覆盖工具注册与真宿主接线的同步分支。

验收：`verify_agent` 1268/1268、`mw_check_agent` 93/93、`mw_check` 3192/3192、`verify_launch` 370/370、`ui_check_agent` 77/77；产品目标按 `-Wall -Wextra -Wpedantic -Werror` 编译通过；`check-agent-release.ps1`、`check-i18n.ps1`、`check-test-modules.ps1`、`check-doc-links.ps1`、`check-repo-layout.ps1` PASS。真实 Modrinth 查询、真实下载与 1.20.1 实例提交仍要真机复跑（A9/A10）。

限制：

- 平台只接 Modrinth；CurseForge 不参与，必要前置在 Modrinth 找不到时整批停止。
- 平台结论取决于 `version_files` 回的那一版元数据；文件被作者换过哈希时只能落到 `unknown`。
- `mods.installFix` 失败时只撤销本次复制的文件；它不改基线，提交/回滚仍由 `repair.finish` 负责。
- `instance.repairCore` 不保留共享库旧副本，失败按官方哈希重下恢复。
- 平台与下载的真实耗时没量（本机夹具全部走假宿主）。

### S5.5 依赖感知二分（已完成 2026-11-24）

`mods.bisect` 在工具内部完成多轮启动，不要求模型逐步调 `mods.setEnabled` / `instance.testLaunch`。这也绕开单轮 8 次工具调用的上限。

输入：

```json
{
  "workspaceId": "repair-workspace-001",
  "prioritizedCandidates": ["mod_a", "mod_b"],
  "maxLaunches": 12,
  "repeatPerCandidate": 2,
  "timeoutSeconds": 120,
  "respectDependencies": true
}
```

内部步骤：

1. 从 `mods.list`、本地声明和 `mods.analyzeCompatibility` 建启用集合与必需依赖闭包；加载器/API 前置（Fabric API、Forge、NeoForge、Quilt 等）始终保留。
2. 优先测试 `diagnostics.crashSnapshot` 的嫌疑模组，再按置信度、最近修改时间、文件名排序其余候选；每轮开始先跑完整启用集，确认崩溃可复现。
3. 用依赖约束的 delta debugging 缩减：每次尝试禁用一组候选，必需依赖始终跟着被保留的模组一起启用；能复现就保留缩减，不能复现就回退这一组。
4. 单个模组能复现就重复确认；两个模组同时启用才复现时按组合冲突报告；只在较大集合复现时返回最小集合，不硬说成单个模组。
5. 每次启动前把 `mods/`、`config/`、`options.txt` 重置到事务基线，清掉上一轮日志和崩溃报告；同一工作区复用，避免每轮整份复制。
6. 结果不一致时标 `flaky`，最多加一轮重复；仍不一致就 `inconclusive`，不猜。

上限：默认 8 次启动，最多 20 次；默认 45 分钟；连续 3 次启动基础设施失败就停；只允许在事务工作区执行；不写原实例、不下载、不改世界。

返回 `result`（`single` / `pair` / `set` / `not_found` / `not_reproducible` / `inconclusive`）、`culprits`、`minimalSet`、`launches`、`evidence`、`budgetExhausted`、`flaky`。`not_found` 表示基线可复现但候选集合内没缩出责任模组；`not_reproducible` 表示基线本身不崩。二分结束后把工作区恢复到基线，只留日志和结果；修复动作由后续工具显式执行。

实现：

- `AgentModBisect`：纯状态机（`step()` / `report()`），不读盘、不启动进程。流程是基线 →（有嫌疑模组时先只启用嫌疑试一轮）→ delta debugging 缩减到 1-minimal → `repeatPerCandidate` 次确认。依赖闭包保证被保留模组的 required 前置始终启用；加载器/API 前置（`fabric-api`、`forge`、`neoforge`、`quilt_loader` 等）不参与候选。
- 结果分类：`minimalSet` 是仍能复现的启用集合，`culprits` 去掉"只是被别的成员依赖"的那些（`app` 需要 `lib` 时报 `single`，`lib` 不背锅）；按 culprits 数量给 `single` / `pair` / `set`。停用全部候选仍崩 `not_found`；基线不崩 `not_reproducible`；连续 3 次启动失败或确认不一致回 `inconclusive`；启动次数/时间用完标 `budgetExhausted` 并返回当前集合。
- `mods.bisect` 工具：要求活动事务与登记工作区；每次计划审批；每轮按集合改 `.jar` ↔ `.jar.disabled`、把 `config/` 与 `options.txt` 恢复到二分开始时的快照、清 `logs/`、`crash-reports/`、`hs_err_pid*`，再经 `AgentLaunchHost` 启动一次（里程碑 `main_menu`，10–600 s）；结束后恢复启停与配置并删快照。不下载、不改世界、不写原实例。
- 假测试启动宿主做集成夹具：工作区里 `bad.jar`（依赖 `lib.jar`）存在就崩；每轮往 `config/a.txt` 写脏数据，验证每轮重置与收尾恢复。
- 技能：`mods.bisect` 进崩溃急救/模组维护的写集合，`mods.md` 提示词（promptVersion 3）说明何时用它、结果值怎么读。

验收：`verify_agent` 1327/1327（含 `AgentModBisectTests` 与工具级集成夹具）、`mw_check_agent` 94/94、`mw_check` 3192/3192、`verify_launch` 370/370、`ui_check_agent` 77/77；产品目标按 `-Wall -Wextra -Wpedantic -Werror` 编译通过；`check-agent-release.ps1`、`check-i18n.ps1`、`check-test-modules.ps1`、`check-doc-links.ps1`、`check-repo-layout.ps1` PASS。120 模组夹具在给了嫌疑候选时 5 次启动定位；不给嫌疑时用满 20 次上限内的 ddmin。

限制：

- 真机 120 模组整合包、真实游戏崩溃与取消清理仍需复跑（A11）；本机全部用脚本化结果与假启动宿主。
- 二分只定位不修复；确认阶段不一致最多加一轮，之后回 `inconclusive`。
- 不改世界、不验证存档一致性；`quickPlay` 进副本世界只是可选参数。

### S5.6 Prompt V2 与知识分层（已完成 2026-11-24）

在 S5.1–S5.5 的工具稳定后重写提示词，分三层：

| 层 | 内容 | 位置 |
| --- | --- | --- |
| 技能提示词 | 角色、阶段、工具使用顺序、审批/验证/回滚要求 | `assets/agent/prompts/*.md` |
| 诊断知识 | 崩溃签名、规则 id、置信度、建议验证 | `assets/agent/knowledge/*.json` |
| 工具 | 本地解析、启动、事务、兼容性、二分 | `src/agent/` |

`crash_rescue.md` 的目标定义改成执行器：在锁定实例内尽可能自主完成诊断、修复、验证和回滚；工具和权限能做的直接做，不只给操作步骤；需要用户介入时说明缺的是工具、权限还是信息。

必须写进提示词的条款：

- 诊断先调 `diagnostics.crashSnapshot`，按 `matchedRules` 和证据决定下一步，不在提示词里背崩溃类型表。
- 修改前先 `repair.begin`；写入只落工作区；用 `instance.testLaunch` 验证；失败或不确定就 `repair.finish(rollback)`。
- 只有 `fixed_verified` 能说修好；`fixed_unverified` 必须写清未验证场景；`mitigated` 不得写成根因已修复。
- 不猜模组责任、兼容版本、依赖或下载地址；所有结论引用证据 id。
- 不绕过审批、外发、路径和下载来源策略；不在自由对话里使用事务或二分工具。

自由对话模式改为显式工具白名单，不把 Stage 5 的修复工具带进去；崩溃急救与模组维护模式才开放事务、启动测试和二分。技能只能缩小工具集合，不改审批/外发策略。

后续（P2）修复病例库：`agent/repair-cases/` 存规则 id、加载器/游戏版本、模组集合哈希、执行动作、验证结果，默认关闭，只在本地参与排序，不把原文发给模型；保留 200 条 / 50 MB，用户可清空。

提示词只引用当期工具白名单里存在的工具；工具改名、删除或换阶段时同步改提示词和测试。提示词预算：`common.md` ≤4 KB，单个技能 ≤8 KB，`common + 技能 + repair_protocol` ≤16 KB；超过预算的规则进知识库或工具，不进提示词。

实现：

- 提示词 V2：`common.md`（执行优先、证据、不猜、审批边界）、`general.md`（自由对话：修复要切模式）、`inspection.md`、`crash_rescue.md`（执行器定义 + 六类条款 + 五个报告状态）、`upgrade.md`、`mods.md`、`repair_protocol.md`（计划、事务、验证、回滚、报告）。诊断签名全部留在 `assets/agent/knowledge/`，提示词不背类型表。
- 自由对话改为**显式工具白名单**：文件工具、只读诊断、设置/模组启停/备份、联网与诊断命令、`mcp__*`；不含 `repair.*`、`instance.testLaunch`、`mods.installFix`、`mods.bisect`、`instance.repairCore`、`workflow.*`。`AgentSkill::allows` 支持末尾 `*` 前缀通配（`mcp__*`）。
- 白名单在**每个模式**都生效：`AgentCompositeToolEngine::schemas()` 恒按当前技能过滤，`gate()` 先查白名单、再查工作流的计划/步进闸（此前只有非自由对话才查）。
- 提示词预算进 `AgentPromptLibrary`：`common` ≤4 KB、单技能 ≤8 KB、组合 ≤16 KB，超预算技能不可用并给出问题；`check-agent-release.ps1` 核对文件大小与“提示词里没有知识库签名”。
- 测试：`AgentSkillsTests` 断言六类条款、五个状态、无崩溃签名（拿 `crash_rules.json` 的 match/requires 逐条比对）、预算、提示词里的工具都在该模式白名单内、自由对话不放行修复与计划工具；`mw_check_agent` 断言自由对话拒绝修复工具、切到模组维护后只差已批准计划。
- 提示词版本递增：`general`/`inspection`/`upgrade` 2、`crash-rescue` 3、`mods` 4。

验收：`verify_agent` 1359/1359、`mw_check_agent` 98/98、`mw_check` 3192/3192、`verify_launch` 370/370、`ui_check_agent` 77/77；产品目标按 `-Wall -Wextra -Wpedantic -Werror` 编译通过；`check-agent-release.ps1`、`check-i18n.ps1`、`check-test-modules.ps1`、`check-doc-links.ps1`、`check-repo-layout.ps1` PASS。真实模型是否先诊断后动手仍需人工（A12）。

限制：

- 提示词与白名单只能约束模型“拿到什么工具、看到什么要求”，真实模型的行为要人工验收（A12）。
- 自由对话仍保留设置写、模组启停与备份（逐次审批），只摘掉了 Stage 5 的修复工具。
- 修复病例库（P2）未实现。

### S5.7 Auto-safe 事务授权（P1，需产品确认）

现有 D5 是「写操作逐次确认」。Auto-safe 把它提升为「一次批准一个有范围、有时效的事务」，需要新增设计约定 D12 并同步 [AGENT](../features/AGENT.md) §2/§9.2；未确认前不实现。

用户明确开启后，事务内允许：

- 只操作当前锁定实例和它的工作区。
- 只执行已批准计划里的操作；文件、设置键、模组范围在开始时列出。
- 每个修改先备份、记差异，验证失败自动回滚。
- `mods.installFix` 只允许 Modrinth 且哈希齐全的文件；不允许任意 URL。
- 提交到原实例包含在事务批准里；提交后按需做启动验证。

仍然逐次确认或直接拒绝：

- 超出计划范围的文件、设置键、模组或世界。
- `java.path`、`args.extraJvm` 等能改变可执行代码来源的设置。
- 删除文件、修改 `saves/`、下载未知来源文件、联网到非白名单主机。
- 事务过期、用户取消、启动器关闭、磁盘不足、源文件被外部修改。

限制：事务授权最长 60 分钟或 20 次启动；同一实例同时只有一个；默认关闭，不随会话记忆；没有后台/无人值守模式；关闭启动器即停止。每次事务结束生成审计报告：批准范围、实际操作、差异、验证、备份、回滚结果。

验收：一次批准后完成「禁用模组 → 启动验证 → 提交」不再逐步弹审批；计划外改 `options.txt` 或 `args.extraJvm` 被拒绝；验证失败自动回滚；取消/超时/关窗都能停进程并保留审计。

## 5. 新增工具与审批矩阵

| 工具 | 阶段 | 风险 | 审批 | 主要上限 |
| --- | --- | --- | --- | --- |
| `diagnostics.crashSnapshot` | S5.3 | 只读 | 自动 | 报告 8 MB，输出 16 KB，10 s |
| `logs.search` | S5.3 | 只读 | 自动 | 200 条，500 行，256 KB |
| `logs.readRange` | S5.3 | 只读 | 自动 | 500 行，256 KB |
| `crash.readFull` | S5.3 | 只读 | 自动 | 8 MB，输出 24 KB |
| `mods.analyzeCompatibility` | S5.4 | 只读 + 平台查询 | 本地自动；首次外发确认；严格模式每次 | 2000 模组，60 s 本地，180 s 平台 |
| `mods.verifyFiles` | S5.4 | 只读 | 本地自动；平台哈希查询按分析工具确认 | 2000 文件，单文件 512 MB，本地 60 s |
| `instance.testLaunch` | S5.1 | 执行游戏 | 独立调用每次（S5.2 事务内再按计划放行） | 1–600 s，重复 1–3，每个启动器窗口串行 |
| `instance.repairCore` | S5.4 | 写共享库/资源 | 每次 | 10 min，官方源哈希 |
| `mods.installFix` | S5.4 | 下载并写文件 | 每个文件；Auto-safe 内按事务 | 单文件 100 MB，事务 500 MB，10 文件 |
| `mods.bisect` | S5.5 | 多次启动 | 每次计划 + 事务 | 20 次启动，45 min |
| `repair.begin` | S5.2 | 复制/建工作区 | 每次 | 16 GB，空间预检 |
| `repair.diff` | S5.2 | 只读 | 自动 | 输出 32 KB |
| `repair.verify` | S5.2 | 执行游戏 | 事务内自动；独立每次 | 同 `instance.testLaunch` |
| `repair.finish` | S5.2 | 写原实例/回滚 | commit 每次；rollback 自动 | 2000 文件 / 512 MB 备份 |
| `repair.status` | S5.2 | 只读 | 自动 | 200 事务 |

新工具必须在 `AgentApprovalPolicy` 里声明风险等级；未声明按 `ExternalMcp` 逐次审批，不允许默认自动。

## 6. 数据与恢复

```text
agent/
  transactions/<id>/journal.json          事务状态、操作、基线、验证、备份引用
  transactions/<id>/diffs/                文本差异与文件清单
  transactions/<id>/backups/              该事务触达文件的备份
  workspaces/<id>.json                    工作区登记：来源、用途、状态、日志位置
  workspaces/<id>/logs/<launchId>/        测试启动原始日志（本地，不进模型）
  repair-cases/<id>.json                  本地修复病例（P2，默认关闭）
```

工作区在 `<游戏根>/cache/agent-workspaces/<id>/`，事务日志只记相对/受控路径，不把绝对路径发给模型。当前实现默认保留工作区（提交/回滚后都留），自动清理和启动恢复扫描尚未接到 UI；`repair.status` 只列出活动/中断事务、状态和验证结果，不自动续跑也不删未知目录。

## 7. 测试与验收

核心夹具放 `tests/meta/agent/`，用脱敏后的结构样本，不把真实用户日志和账户信息入库。

- 提示词：资源加载、组合顺序、哈希、缺失失败、大小上限、cpp 无正文。
- 里程碑与日志：原版/Forge/Fabric/NeoForge 主菜单、世界加载、崩溃、超时、CRLF、UTF-8、大行截断。
- 崩溃规则：Mixin、堆 OOM、Metaspace/Direct Buffer/native thread、Java 版本、缺前置、重复模组、JAR 损坏、配置解析、native、世界 tick；置信度和证据逐条断言。
- 启动参数：显式 `LaunchRoots`、离线、无令牌、无任意 JVM/游戏参数、工作区路径越界拒绝。
- 事务：复制排除项、基线哈希、工作区路由、提交重放、回滚、提交中断恢复、外部修改拒绝、磁盘不足、路径/junction 越界。
- 模组兼容：本地声明夹具、加载器不匹配、缺前置、重复、未知；Modrinth 假响应、哈希身份、缓存、离线模式。
- 二分：假启动结果驱动的单模组、双模组、依赖保留、flaky、预算耗尽、取消。
- UI：事务卡、启动进度、取消、回滚按钮、报告卡；`mw_check_agent` 用假宿主，不启动真游戏。

真机验收：

- 1.20.1 Forge 与 Fabric 各一个真实实例：损坏模组 → 工作区启动崩溃 → 回滚 → 修复 → 主菜单 → 提交；源实例在提交前哈希不变。
- 120 模组整合包：二分在 12 次启动内定位单个模组；必需前置始终启用；取消后无孤儿进程。
- 世界崩溃：只复制指定世界到工作区，原存档不被打开；报告写明只在副本验证。
- 缺 Java、缺核心文件、磁盘满、长路径、杀毒软件占用：返回明确原因，不假成功。
- OOM/native/显卡崩溃：分类正确，不把驱动问题当模组问题，不自动改系统设置。
- 真实模型：Mixin 夹具上先诊断后计划，失败会回滚，不把「建议重启」当完成。

性能记录写进 [PERFORMANCE](../dev/PERFORMANCE.md) 或 `docs/dev/perf-notes/`：工作区复制 1/5/10 GB、100 MB 日志索引与搜索、120 模组声明扫描、12 次二分总时长。

## 8. 未覆盖与未决

| 项 | 状态 |
| --- | --- |
| `logs.search` 正则 | 未做，需匹配上限引擎；首版只有字面量（大小写不敏感） |
| 日志扫描的异步化 | 未做：在调用线程上扫描，100 MB 约 350 ms，超 6 s 预算截断 |
| CurseForge 模组修复 | 不在 Stage 5；S5.4 只接 Modrinth，后续按现有来源表扩展 |
| 世界问题提交后验证 | 不做；只在副本世界验证，报告写明未在原件验证 |
| 显卡/驱动修复 | 只分类和建议，不改驱动、系统设置或渲染后端 |
| Java 运行时自动下载 | 测试启动不下载；缺 Java 直接报原因，由现有下载入口处理 |
| 非确定性崩溃 | 重复次数有上限，超限标 `flaky`/`inconclusive`（S5.5 已按此实现） |
| Auto-safe D12 | 需产品确认；未确认前保持 D5 逐次审批 |
| `instance.repairCore` 回滚 | 共享库不存旧副本；失败不删旧文件，按官方哈希重下恢复，报告写明不能回到旧版本（已按此实现） |
| 修复病例库 | P2，未实现（`agent/repair-cases/` 的落盘与本地排序还没有）；提示词只引用当期白名单里的工具 |
| 并发 | 每个启动器窗口的测试启动串行；多实例同时二分留到后续评估（二分本身复用这条串行约束） |
| 测试启动窗口 | 会打开真实游戏窗口，Windows 下不做无头渲染；抢焦点/最小化留给人工验收 |
| 工作区复制取消 | 复制阶段只丢弃结果，QtConcurrent 复制继续到结束；关闭启动器会等待当前复制 |
| 正版登录相关故障 | 测试启动强制离线，依赖正版登录/服务器的模组问题不在自动验证范围 |
| 性能数字 | 实现后补，当前只定预算和测量项 |
