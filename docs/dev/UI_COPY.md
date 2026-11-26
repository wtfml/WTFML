# WTFML 界面文案规范

本文档规定 WTFML 客户端所有用户可见界面文案的编写、润色与审校标准，供所有参与界面设计与代码开发的成员使用。核心目标是建立克制、书面、准确的交互语言，消除口语化与冗余说明，降低界面整体信息密度。

---

## 1. 核心口吻与原则

界面是工具的操纵界面，不是与用户的日常聊天对话。文案撰写遵循以下四项原则：

1. **书面克制**：使用规范的书面现代汉语。严禁使用口语化字词（如「挑一个」「看」「就」「跑一个」「先关掉」「删不掉」「搞定」「多半」「这会儿」「赶紧」等），严禁使用语气词（「啊」「吧」「呢」「哈」「喽」）。
2. **准确客观**：直接陈述状态、条件与结果。不推测原因（如「多半是网络卡了」），不进行情绪化拟人（如「启动器唯一的退路」），不恐吓用户（如「把系统拖死」）。
3. **消除祈使套话**：严禁在标签、提示或说明中堆叠「请」「请先」「请点击」「可以」「用于」等套话。动词直接指向操作对象。
4. **默认不设说明**：界面的清晰度应当优先由控件本身的排布、层级和标签保证。说明文字默认不写；若标签本身含义清晰，严禁额外添加一句话复述该标签。

限制：涉及不可逆破坏性操作（如删除实例存档、清除盘上数据）时，必须明确告知后果并保留二次确认。

---

## 2. 界面控件文案规范

### 2.1 按钮与可交互控件（动词短语）

- **规范**：按钮、菜单项、操作胶囊必须使用简明动词短语，长度以 2~4 个汉字为主。
- **结构**：`动词 + 名词`（如「启动游戏」「添加账号」「删除实例」「检查更新」）。
- **禁止**：
  - 不使用主谓句或整句口语（如「去下载页装新版本」→「下载游戏」；「删掉它」→「删除」；「顺带改个名」→「同时修改名称」）。
  - 不使用带有指示代词的废话（如「删除这个实例」→「删除实例」；「启动这个实例」→「启动实例」）。
  - 下拉控件或胶囊按钮严禁在文字末尾追加 `⌄`、`>` 等纯符号字符（箭头符号交由自绘样式层呈现）。

### 2.2 标题与分栏（名词短语）

- **规范**：页面标题、弹窗标题、卡片组标题与表格列头必须使用名词短语。
- **结构**：`名词` 或 `定语 + 名词`（如「客户端实例」「游戏日志」「错误详情」「执行步骤」）。
- **禁止**：
  - 严禁使用问句或口语祈使句作为标题（如「到了哪一步」→「执行步骤」；「出了什么问题」→「错误详情」；「选一个整合包」→「选择整合包」）。

### 2.3 说明文字（精简陈述句）

- **规范**：单句陈述句，一句不超过 30 个汉字，句末统一加全角句号「。」。
- **准入标准**（满足以下三条之一方可保留，其余一律删除）：
  1. **标签本身无法说清语义**（如「10 秒内再次启动同一实例时询问」）；
  2. **具有不可逆后果或隐蔽副作用**（如「导出不含图片，仅保存路径。」「关闭后仍写入日志文件。」）；
  3. **说明功能当前不可用的客观原因**（如「亚克力、云母材质仅支持 Windows 11。」）。
- **处理方式**：
  - 能做成控件占位文字（Placeholder）的，不写说明；
  - 能做成分段选项副文字的，并入选项；
  - 纯复述标题或标签字面含义的，直接删除。

### 2.4 错误提示（原因 + 怎么办）

- **规范**：所有弹窗、行内横幅（InlineMessage）、气泡提示中的错误文案，必须严格遵循 **「客观原因 + 解决方案」** 结构。
- **结构**：
  - 第一部分：告知发生什么问题以及明确的技术原因（不使用口语推测）；
  - 第二部分：告知用户当前应该采取的补救步骤或重试方式。
- **对比**：
  - 反例：「Java 运行时没下成，多半是网络不通。可以自己装一个再回来检测。」（猜测、口语）
  - 正例：「Java 运行时下载失败，网络连接超时。可手动安装 Java 后点击「检测已有 Java」，或在实例设置中指定路径。」

### 2.5 空状态（一句状态 + 一个动作）

- **规范**：任何列表、表格、卡片容器在无内容展示时，必须呈现为 **「一句状态描述 + 一个主操作按钮」**，严禁多重叙述或空置大片留白。
- **结构**：
  - 状态描述：名词短语（如「暂无实例」「未登录」「无运行中任务」）；
  - 操作按钮：核心动词短语（如「下载游戏」「添加账号」「新建会话」）。
- **禁止**：严禁在空状态中解释整个子系统的操作说明或同一件事反复强调（如账号弹层同时出现「还没有保存的账号」「在账号与外观里登录」「管理账号与皮肤」「打开」四重叙述）。

### 2.6 占位文字（Placeholder）

- **规范**：直接展示输入格式示例、默认值或留空含义。
- **禁止**：严禁出现「请输入...」「在此处填写...」「点我输入」等无意义套话。
- **示例**：
  - 代理地址输入框：`127.0.0.1:7890`（而非「在此输入代理服务器 IP 与端口」）
  - API 密钥输入框：`sk-...`（而非「请输入模型供应商的 API Key」）
  - 默认标题输入框：`留空使用默认名称`（而非「如果不填就会使用系统的默认名字」）

### 2.7 悬停提示（Tooltip）

- **规范**：悬停提示仅用于纯图标按钮补充语义、文字被省略截断时的完整展示、以及说明置灰禁用项的原因。
- **禁止**：已有可见文字标签的按钮与表单项，严禁重复设置与其文字完全相同的 Tooltip。

---

## 3. 统一叫法术语表

全仓涉及的核心功能与概念必须统一用词，以代码库中出现频次最高且语义最精确的中文名称为准。英文列与 `assets/i18n/wtfml_en.ts` 的实际译文一致，写法遵循本节「英文写法」。

| 推荐标准术语 | 英文对照 | 适用范围与定义 | 严禁使用的旧词与冲突叫法 |
|---|---|---|---|
| **实例** | Instance | 指代用户配置的一个独立 Minecraft 游戏运行环境（含特定版本、模组、配置、存档）。 | 严禁叫「游戏」（当指代配置环境时）、「版本」（当指代整个实例时）、「档」 |
| **账号** | Account | 指代玩家身份凭据（微软账号、离线账号）。 | 「账户」只用于操作系统账户（如「当前 Windows 账户」），玩家凭据一律写「账号」 |
| **下载源** | Download source | 指代游戏核心与资源下载的分发渠道（如官方源、BMCLAPI 镜像）。 | 严禁叫「下载节点」「下载渠道」「下载网站」 |
| **实例目录** | Instance folder | 特指某一个实例专用的独立文件存储路径（位于 versions/<name>/ 下）。 | 严禁混叫「游戏文件夹」「工作目录」 |
| **游戏目录** | Game directory | 指代存放多个实例的总目录（即包含 versions/、assets/ 的主目录）。 | 严禁与单个实例的「实例目录」混用；界面不再用「游戏文件夹」 |
| **数据目录** | Data directory | 特指启动器本身存储配置、令牌与全局日志的应用数据路径（AppData/Local/...）。 | 严禁简称为「文件夹」或与游戏目录混为一谈 |
| **加载器** | Loader | 指代各类模组加载程序（Forge、NeoForge、Fabric、Quilt）。界面中统一使用短名。 | 默认不展开写「模组加载器」，严禁使用「ModLoader」「Mod 加载器」 |
| **整合包** | Modpack | 指代打包分发的模组包（如 CurseForge 整合包、Modrinth 整合包）。 | 严禁使用「模组包」「Modpack」「modpack」 |
| **模组预设** | Mod preset | 指代对模组组合清单及配置项的投影定义文件；在预设管理页内部上下文明确时可简称为「预设」。 | 严禁简写为「配置预设」或与游戏预设混淆 |
| **微软登录、离线登录** | Microsoft sign-in / Offline sign-in | 登录入口和提示用全称；账号类型标签写 Microsoft / Offline。 | 界面不使用「正版」「正版登录」「Genuine login」 |
| **世界存档** | Worlds / world save | 列表页用 Worlds，读写、备份等文件操作用 world save。压缩归档用 archive。 | Archive（指游戏世界时） |
| **模组、数据包、光影包、原理图** | Mod, Data pack, Shader pack, Schematic | 文件类别页和列表标题用复数，单项和修饰语按上下文用单数。 | Module、Packet、Light and shadow package、Schematic diagram |
| **启用、禁用** | Enable / Disable，Enabled / Disabled | 按钮用动词，状态和统计标签用 Enabled 或 Disabled。 | Activating（指已启用时） |
| **日志、报告** | Log / Logs，Report / Reports | 单份记录用单数，页面集合、目录和导出选项按实际数量选用单复数。 | — |
| **工具箱** | Toolbox | 启动器内置的辅助工具集合页面。各工具统一使用规范短名（附魔助手、彩色文本、坐标换算、延迟测试、信标计算、酿造助手、旗帜设计、纹饰搭配、高速下载）。 | 严禁叫「常用工具」「实用工具」「辅助工具」，各工具禁止使用带有「生成器」「检测」的长名称 |
| **智能体** | Agent | 启动器内置的 AI 维护与对话交互功能。导航与页面标题统一叫「智能体」，对话流中助手的角色标签统一叫「助手」。 | 严禁在界面用户可见文案中直接暴露英文「Agent」或叫「AI 机器人」 |

### 英文对照词表

逐条列出中文概念对应的英文写法，写英文译文时查这张表。

| 中文 | 英文 | 备注 |
|---|---|---|
| 启动游戏（大按钮）| Play | 动词「启动」写 launch |
| 停止（游戏）| Stop | |
| 主页 / 管理 / 下载 / 设置 / 智能体 / 工具箱 | Home / Library / Download / Settings / Agent / Toolbox | 导航，已定 |
| 实例 | Instance | 不许 Example / game / version |
| 客户端实例 / 服务端实例 | Client instance / Server instance | 标签页写 Clients / Servers |
| 整合包 | Modpack | |
| 模组 | Mod | |
| 加载器 | Loader | 首次出现需要说清时写 mod loader |
| 资源包 / 数据包 / 光影包（光影）| Resource pack / Data pack / Shader pack（Shaders）| 两个词，同官方 |
| 原理图 / 投影 | Schematic | |
| 存档 / 世界 | World；文件操作写 world save | 列表页标题 Worlds |
| 原版 | Vanilla | 单作标签写 Vanilla，句中作定语写 vanilla |
| 正式版 / 快照 / 远古版 / 愚人节版 | Release / Snapshot / Old (Alpha/Beta)→ 短标签 Legacy / April Fools | |
| 账号 / 离线账号 / 微软账号 | Account / Offline account / Microsoft account | |
| 登录 / 退出登录 | Sign in / Sign out | 不写 Log in |
| 皮肤 / 披风 / 鞘翅 / 经典（粗手臂）/ 纤细 | Skin / Cape / Elytra / Wide / Slim | |
| 收藏（动词 / 状态）| Favorite / Favorites | |
| 分组 | Group | |
| 版本 | Version | |
| 游戏目录 / 实例目录 / 数据目录 | Game directory / Instance folder / Data directory | 「文件夹」泛指写 folder；界面不再用「游戏文件夹」 |
| 下载源 / 优先官方源 / 镜像 | Download source / Official first / Mirror | |
| 任务中心 / 后台任务 | Task center / Background tasks | |
| 预设 / 模组预设 | Preset / Mod preset | |
| 前置（前置模组、依赖）| Dependency / Dependencies | required mod 也可 |
| 启用 / 禁用 | Enable / Disable；状态 Enabled / Disabled | |
| 完整性检查 / 校验 | Integrity check / Verify | |
| 备份 / 还原 / 回滚 | Back up（动词）Backup（名词）/ Restore / Roll back | |
| 快照（升级前的整份备份）| Snapshot | |
| 升级（实例换游戏版本）/ 更新（模组等）| Upgrade / Update | |
| 覆盖 / 跳过 / 保留 | Overwrite / Skip / Keep | |
| 崩溃报告 / 日志 | Crash report / Log | |
| 服务端 / 控制台 | Server / Console | |
| 白名单 / 管理员 / 封禁 | Whitelist / Operators / Banned players | |
| 延迟测试 | Latency test（工具箱短名 Server ping）| |
| 附魔助手 / 坐标换算 / 信标计算 / 酿造助手 / 旗帜设计 / 纹饰搭配 / 彩色文本 / 高速下载 | Enchanting / Coordinates / Beacon / Brewing / Banners / Armor trims / Colored text / Fast download | 工具箱里的短名 |
| 智能体 / 会话 / 助手 | Agent / Chat（列表 Chats）/ Assistant | |
| 主题 / 强调色（主题色）/ 背景图 | Theme / Accent color / Background image | |
| 暮色雪山 / 冰川碧湖 / 落日海崖 | Alpine Dusk / Glacier Lake / Sunset Cliffs | 主题名 |
| 亚克力 / 云母 / 纯色 | Acrylic / Mica / Solid | |
| 内存（分配给游戏的）| Memory | |
| 展开 / 收起 | Expand / Collapse | |
| 保存 / 放弃修改 | Save / Discard changes | |
| 重试 / 继续 / 取消 / 关闭 / 完成 | Retry / Continue / Cancel / Close / Done | |
| 诊断（原「体检」）| Diagnostics | 服务端检查 |
| 常用设置（服务端）| General | |
| 配色 / 语义色 / 主题色 | Color scheme / Semantic colors / Accent color | |
| 危险 / 警告 / 正常 / 报错（语义色）| Danger / Warning / Normal / Error | |
| 并发数 / 速度限制 / 代理 | Concurrency / Speed limit / Proxy | |
| 跟随系统 | System default | |
| 强制结束 / 停止 / 停止中 | Force stop / Stop / Stopping… | |
| 异常退出 | Crashed | |
| 正在进行 / 等待中 / 已暂停 / 已完成 / 失败 / 已取消 / 已中断 | In progress / Waiting / Paused / Completed / Failed / Canceled / Interrupted | 任务状态；美式拼 Canceled |
| 固定这一版 / 不再固定 / 忽略这一版 / 不再忽略 | Pin version / Unpin / Ignore version / Unignore | |
| 已运行 X | Running %1 min / %1 h %2 min | |
| 搜索框占位 | Search mods… | 以「…」结尾；带「如 1.20.1」这类示例的不加 |
| 还没有… / 暂无… | No … | |
| 检查更新 | Check for updates | 各处都用这句 |
| 恢复默认（按钮）| Reset | 全部恢复写 Reset all |
| …失败（弹窗标题、状态）| Couldn't sign in / Couldn't update / Couldn't export | 单独一个「失败」状态才写 Failed |
| 可安装mod（分组、筛选）/ 置顶 | Modded / Pinned | |
| 跟随全局 / 自定义 | Global / Custom | 实例设置 |
| 打开目录 / 打开文件夹 | Open folder | |
| 启动前 / 启动后 / 下载时 | Before launch / After launch / On download | |
| 窗口 / 最大化 / 全屏 | Windowed / Maximized / Fullscreen | |
| 自动分配 / 手动分配 / 分配方式 | Automatic / Manual / Allocation | 内存 |
| 优先国内源 | Mirror first | |
| 磨砂效果 / 材质 / 着色强度 / 模糊强度 | Glass effect / Material / Tint strength / Blur strength | |
| 每日一言 | Daily quote | |
| 启用与顺序 / 依据 | Enable and order / Reason | |
| 回收站 | Recycle Bin | Windows 官方写法 |
| 读不到 / 打不开 / 建不了 / 写不进 | Can't read / Can't open / Can't create / Can't write | |
| 反选 / 全选 / 清除选择 | Invert selection / Select all / Clear selection | |
| 合并顺序 / 合并（铁砧） | Combine order / Combine | 附魔助手，不写 merge |
| 先前工作惩罚 / 已用次数 | Prior work penalty / Prior uses | 铁砧 |
| 顺带改名 | Include rename | 附魔助手 |
| 预览资源 | Preview assets | 纹饰预览 |
| 目标版本 / 不限版本 | Target version / Any version | 工具箱版本筛选 |
| 光柱配色 | Beam colors | 信标计算 |
| 形态 / 延长 / 普通（药水） | Form / Extended / Regular | 酿造助手；难度「普通」写 Normal |
| 记住输入 | Remember inputs | 工具箱 |
| 修改存档文件 / 世界设置 | Edit save file / World settings | 存档管理 |
| 未写入（规则状态） | Unset | 游戏规则表 |
| 暂存目录 | Staging folder | 世界导入 |
| 临时目录 | Temp folder | 不写 temporary folder / temp directory |
| Java 版本过低 | Java version too old | 不写 too low |
| 暂时无法操作 | Unavailable right now | Busy 留给「当前正忙」 |
| 备份完成 | Backup complete | |
| 打包失败 | Couldn't create archive | |
| 无法发布（文件提交）| Can't finalize | QSaveFile::commit() 等原子提交失败，不写 publish |
| 末地 | The End | 官方维度名，不简写 End |
| 在后台… | …in the background | 带 the |
| 结构/原理图 | Structures / schematics | |
| 封禁的 IP | Banned IPs | 与「封禁的玩家」同组 |
| 原地升级 / 更新副本 / 升级副本 | In-place upgrade / Updated copy / Upgraded copy | 实例升级 |
| 回滚点 | Rollback point | 模组/文件更新前自动留存 |
| 补齐 / 补齐报告 | Complete import / Completion report | 整合包缺失文件补齐 |
| 跳过已有项 | Skip existing | 文件/存档冲突弹窗按钮 |
| Java 管理 / 检测已有 Java / 缺失（Java） | Manage Java / Detect Java / Missing | Java 管理页 |
| 游戏核心 | Game core | 实例升级 |
| 游玩时长 | Playtime | 启动记录 |
| 来源未知 / 分发权限 | Unknown source / Distribution permission | 整合包导出 |
| 审批 / 允许本次 / 拒绝 | Approval / Allow once / Deny | 智能体操作确认 |
| 模型服务 / 模型配置 | Model service / Model profile | 智能体设置 |
| 接口地址 / API 密钥 / 上下文长度 / 单次最多输出 / 等待超时 | Endpoint URL / API key / Context length / Max output tokens / Request timeout | 智能体设置 |
| 思考过程 / 边生成边显示 / 严格模式 | Reasoning / Stream responses / Strict mode | 智能体设置 |
| 协议适配 | Protocol compatibility | 智能体高级设置 |
| 新建服务端 / 服务端控制台 / 服务端设置 | New server / Server console / Server settings | 服务端页面 |
| 适配版（平台适配版） | Adapted release | 实例更新模组替换 |
| 方块统计 | Block statistics | 原理图查看 |
| 账号与外观 / 管理账号与皮肤 | Accounts and skins / Manage accounts and skins | 账号窗口标题与入口按钮 |
| 风景（背景材质） | Scene | 与 Acrylic / Mica / Solid 同组 |
| 历史版本 | Version history | 配置文件编辑页 |
| 移出列表 / 移出队列 | Remove from list / Remove from queue | 不写 Delete |
| 快速启动 | Quick launch | 主页收藏实例区 |
| 错误报告 | Error report | 崩溃/日志导出 |
| 游戏日志 | Game log | 独立日志窗口 |
| 更新中心 | Update center | 实例资源更新管理页 |
| 原始文本 | Raw text | 服务端配置（`server.properties`） |
| 服务器简介 | Server MOTD | `server.properties` 的 `motd` |
| 游戏设置预设 | Game settings preset | `options.txt` 预设 |
| 开关（NBT 布尔类型） | Boolean | 游戏规则值类型，与 Integer / Text 并列 |
| 要处理 / 提醒 / 说明（服务端诊断） | Action needed / Warning / Info | 计数总括用 `Action needed: %1` / `Warnings: %1` / `Info: %1` |
| 综合预设 | Composite presets | 预设分类与筛选 |
| 草稿 | Draft | 预设草稿 |
| 完成安装 | Complete installation | 整合包补齐弹窗按钮，与「补齐」Complete import 区分 |
| 继续未完成操作 / 继续未完成 | Resume unfinished operation / Resume unfinished | 二次确认写 `Click again to resume` |
| 原因报告 | Report | 操作失败原因报告，不写 Cause report |
| 资源索引 / 资源对象 | Asset index / Asset object | 游戏资产校验与下载（`assets/`），不写 Resource index |
| 原版按键 | Vanilla key binds | 预设段落名，与 MaLiLib hotkeys 区分 |
| 通用文件组 | Generic file group | 预设文件组 |
| 启动诊断 | Startup diagnostics | 客户端诊断报告；级别用 Problem / Warning / Info / Normal |
| 更新记录 | Update history | 实例升级记录弹窗 |
| Auto-safe | Auto-safe | 智能体事务自动授权机制名，不翻译 |
| 崩溃急救 / 模组维护 | Crash rescue / Mod maintenance | 智能体技能模式名 |
| 依赖感知二分 | Dependency-aware bisect | 智能体模组排查 |
| 事务工作区 | Transaction workspace | 智能体隔离修复工作区 |
| 修复病例 | Repair cases | 智能体本机记忆（记住修复病例 → Remember repair cases） |

### 英文写法

- 美式英语，sentence case（只有首词和专有名词大写）：`Import modpack`、`Open folder`。游戏内专名按官方大小写。
- 短。按钮 1–3 个词（Play、Cancel、Delete、Check for updates），标签和标题是名词短语，中文 2–4 个字的英文不超过 18 个字符。
- 说明和提示写成自然、书面、完整的句子，主动、直接，不逐字直译中文语序。不写 please、sorry、successfully，不用感叹号，不口语，不用 gonna / kinda。
- 错误提示先说发生了什么，再说怎么办。「失败」写 `Couldn't …`，「无法」写 `Can't …`，不写 Unable to / Failed to / It is impossible to。
- 「正在…」写 `Downloading…`；「已…」写 `Deleted` / `3 files deleted`；「暂无…」写 `No …`，不加 yet。
- 单复数：带数量的句子优先写成不用管单复数的样子（`Mods: %1`、`%1 selected`）；写成 `%1 mods` 也可以，不写 `mod(s)`。
- 标点：中文「」→ 直引号 `"`；`：` → `: `；`，、` → `, `；`；` → `; ` 或拆句；`（）` → ` (…)`；`…` 保留成一个 `…` 字符；` · ` 保留。译文里不许有中文字和全角标点（`…` 除外）。
- 原样保留：占位符 `%1 %2 %L1 %n`（可以换次序，一个都不能少）、HTML 标签、`&` 快捷键、换行 `
` 的个数、首尾空格、路径、命令、配置键、文件名。
- 搜索框占位以 `…` 结尾（`Search mods…`）；带「如 1.20.1」这类示例的占位不加。
- 以上规则由 `pwsh tools/check-i18n.ps1` 在提交和 CI 里校验，命中禁用写法的条目会被逐条列出。

### Minecraft 原版名称

附魔、状态效果、药水、方块与物品、盔甲纹饰、旗帜图案及世界规则的中英对照见
[Minecraft 原版术语表](../../assets/i18n/minecraft_terms.csv)。术语表使用 Java 版 26.3 客户端的美式英语
和该游戏安装缓存的简体中文资源，注记列记录游戏中的语言键。遇到版本差异时，按功能所针对的游戏版本核对该键。

「力量」在弓的魔咒中用 `Power`，在状态效果中用 `Strength`。「摔落缓冲」用 `Feather Falling`。
酿造助手使用完整物品名，如「神龟药水 / Potion of the Turtle Master」。

### NBT 标签类型

NBT 读取与写回的界面文案使用常见数据格式术语：

| 标签类型 | 界面名称 |
|---|---|
| TAG_String | 字符串 |
| TAG_ByteArray | 字节数组 |
| TAG_IntArray | 整数数组 |
| TAG_LongArray | 长整数数组 |
| TAG_Compound | 复合 |

未知类型写「未知的标签类型」，不写「不认识」。文件截断、末尾多余数据等错误按「发生了什么」直述，如「文件在此处中断：需要 %1 个字节，剩余 %2 个」。

### 迁移与预设

- 实例迁移、升级副本里的「搬」只复制、不删源文件，统一写「迁移」或「复制」；真正移动文件的地方才写「移动」。
- 预设落法「缺了才拷」写「仅在缺失时复制」。
- 计划或某一段无法执行时写「无法完成」，不写「做不了」。
- 没有发生变化写「无变化」，不写「没有变化」。
- 读写中途失败写「读取 / 写入…中断」，不写「读到一半出错了」。
- 需要用户逐项确认的状态写「待选择」，不写「要你选」；同一项被多个动作占用写「占用」，不写「抢着用」。
- 拒绝写入时用「未」：写「未覆盖」「未删除」「未执行」，不写「没有覆盖」等。
- 范围与指代写「目标中」「实例中」「其他」，不写「目标里」「实例里」「其它」。

---

## 4. 标点与格式规范

### 4.1 标点符号

- **中文全角标点**：正文中必须统一使用全角中文标点符号（，。！？：；“”「」）。
- **省略号**：统一使用标准全角中文省略号 `…`（Unicode U+2026），**严禁使用英文半角连续句点 `...`**。
  - 省略号用于动词后表示该操作会弹出后续输入对话框或次级配置窗口（如「设置…」「导出…」「重命名…」）。若点击后直接就地生效，严禁加省略号。
  - 省略号用于加载或执行中状态展示（如「正在获取…」「正在启动…」）。
- **引号与括号**：
  - 界面强调与引用文件名、实例名、选项名时，统一使用直角引号「」；
  - 界面补充说明一律使用全角圆括号（），括号内文字若为短语则末尾不加句号。

### 4.2 空格规则

- **中英文与数字混排**：中文文字与英文字符（含 ASCII 英文单词、代码标识符）、中文文字与半角数字之间，**必须保留一格半角空格**。
  - 正例：`已运行 12 分钟`、`Java 17 运行时`、`分配 4096 MB 内存`、`支持 .mrpack 格式`
  - 反例：`已运行12分钟`、`Java17运行时`、`分配4096MB内存`、`支持.mrpack格式`
- **标点与空格**：全角标点符号前后严禁留空格。

### 4.3 数字、单位与量词

- **数字一律半角**：所有数值一律采用 ASCII 半角阿拉伯数字。
- **存储与内存单位**：
  - 统一大写，且必须与数字之间保留一格半角空格（如 `4096 MB`、`2048 MiB`、`16 GB`）。
  - 对外用户可见文案统一采用主流的 `MB`、`GB`；底层对齐系统二进制计算时若采用 `MiB` / `GiB`，同一页面内单位必须保持一致，不可上下跳变。
- **时间与计数量词**：
  - 时间单位采用中文「秒」「分钟」「小时」「天」，数字与单位之间留一格半角空格（如 `10 秒`、`3 小时`）。
  - 计数量词采用规范汉字（如 `%1 个实例`、`共 %2 项`、`第 %3 步`），不使用口语量词（如「这一份」「那一群」）。
- **尺寸乘号**：分辨率或尺寸描述统一使用数学乘号 `×`（U+00D7），不使用英文字母 `x` 或 `*`（如 `854 × 480`）。

---

## 5. 正反例对照（8 组真实场景）

以下正反例均摘录自 WTFML 现有代码库与用户体验反馈中的真实文案：

### 例 1：说明文字去口语与精简化
- ❌ 反例（`ToolboxPage.cpp:1078`）：「把要合的附魔列在上面，点「计算」看最省等级的合并顺序。」
- ✅ 正例：「添加待合并附魔，计算最少铁砧经验消耗顺序。」

### 例 2：按钮文案动作化与去祈使
- ❌ 反例（`MainWindowUI.hpp:1088`）：「去下载页装新版本」
- ✅ 正例：「下载游戏」

### 例 3：标题与小节去口语问句
- ❌ 反例（`MainWindowUI.hpp:2215` / `2216`）：「到了哪一步」 / 「出了什么问题」
- ✅ 正例：「执行步骤」 / 「错误详情」

### 例 4：错误提示符合「原因 + 怎么办」
- ❌ 反例（`JavaSettingsController.cpp:56`）：「没能下到 Java %1。多半是网络不通（下载走的是 Adoptium）。可以自己装一个，再回来按「检测已有 Java」；或者去「实例设置…」里把某一份现成的 Java 指过去。」
- ✅ 正例：「Java %1 下载失败，网络连接超时。可手动安装 Java 后点击「检测已有 Java」，或在实例设置中指定路径。」

### 例 5：空状态精简为「状态 + 单一动作」
- ❌ 反例（`MainWindowPanels.cpp` / `UI.hpp`）：「还没有保存的账号。在「账号与外观…」里登录或添加离线账号。账号与外观… / 管理账号、皮肤与披风 [打开]」
- ✅ 正例：状态文本「未登录」，操作按钮「添加账号」

### 例 6：状态与破坏性操作说明去拟人与废话
- ❌ 反例（`FolderController.cpp:44`）：「「%1」是启动器自带的游戏文件夹，移不掉。它是别的目录都被移走时唯一的退路。」
- ✅ 正例：「「%1」为默认游戏目录，不可移除。」

### 例 7：输入框占位与提示规范化
- ❌ 反例（`ToolboxPage.cpp:1927`）：「（可选）粘上人家给的那一串摘要」
- ✅ 正例：占位文字「对照摘要（可选）」

### 例 8：破坏性确认对话框客观化
- ❌ 反例（`MainWindowServer.cpp:496-500`）：「删掉这个服务端。「%1」这一整个目录会被删掉 —— 里面的世界（world/）、模组、配置一起没。删掉的东西会进回收站，要恢复可以从那里找回来。确定要删吗？ [删掉它] [取消]」
- ✅ 正例：标题「删除服务端」，内容「将删除「%1」服务端目录（含存档、模组与配置）。文件将移至回收站。\n\n确定删除？」，按钮「删除」 / 「取消」

---

## 6. 未覆盖项

以下范围不在本规范直接改动之列，列入后续专项处理：

| 模块 / 范围 | 当前状态 | 后续计划 |
|---|---|---|
| 多语言国际化（i18n）机制 | 当前源码直接内嵌中文（tr() 与 QStringLiteral 混用） | 先按本规范统一中文基线，后续统一抽至 Qt 翻译源码（.ts） |
| `ToolboxController.cpp` 算法运行时详细推导文本 | 包含约 90 条细分算法推导和合成矩阵调试输出 | 随工具箱页面改造重构输出模型时统一改写 |
| 诊断工具 `WTFMLNetworkDiag.exe` | 独立控制台测试工具，输出面向技术排查人员 | 保持开发者诊断口吻，不适用普通用户端规范 |
