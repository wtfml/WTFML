# 外部输入路径与数据安全边界

本文档汇总 WTFML 对外部输入名称、文件路径、图片解码、脚本执行和智能体数据外发的安全边界与校验规范，供核心与功能模块开发者查阅。

---

## 1. 外部输入名称到盘上路径的转换

对于所有从外部清单、网络接口或归档中读取并拼装为本地路径的字符串，统一实施前置安全校验：

| 输入来源 | 目标本地路径 | 校验判据 | 关键断言 |
| --- | --- | --- | --- |
| `version.json` 的 `assetIndex.id` | `<assets>/indexes/<id>.json` | `AssetNames::isSafeIndexId()` | `verify_downloads` ZAO2 |
| 资源索引内对象的 `hash` | `<assets>/objects/<前两位>/<完整哈希>` | `AssetNames::isSha1Hex()` | `verify_downloads` ZAO1 |
| 平台模组版本的 `filename` | `<实例>/mods/<filename>` 等 | `InstanceFiles::validateFileName()` | `verify_instances` N0 |
| 升级审计表 `wtfml.preset-mod-install.json` 的 `filename` | `<暂存>/mods/<filename>` | `InstanceFiles::validateFileName()` | `verify_instances` N0 |

### 核心约束

- **统一判据**：路径段校验统一由 `InstanceFiles::validatePathSegment()` 处理，拦截 `..`、目录分隔符、NTFS 备用数据流（`:`）、保留字符（`< > " | ? *`）、控制字符、尾部点/空格及 Windows 保留设备名称（如 `CON`、`PRN`、`AUX`、`NUL`、`COM1` 等）。文件名在此基础上增加长度上限（`kMaxFileNameLength = 200`）。
- **对象哈希严格匹配**：对象哈希限定为恰好 40 位十六进制字符，保证 `<前两位>/` 目录结构恒为有效双字符十六进制。
- **前置拦截与终态保证**：不合规的索引号直接记为失败任务，下载停止；非法文件名在发起下载前（`ResourceInstallController::installNow()`）即时阻断并提示错误原因，不执行无效文件写入。

---

## 2. 外部图片解码尺寸保护

PNG、BMP、WebP 等格式的头部信息可能声明超大分辨率，引发解码内存暴增。系统通过 `ImageDecode::decode()`（`src/util/ImageDecode.hpp` 的 `Limits`）实施先测后解的尺寸保护：

- 尺寸检查分别在 `QImageReader::size()`（读取像素前）与 Windows Imaging Component (WIC) 的 `GetSize()` 阶段执行，解码后再行复核。
- 各场景尺寸限制：

| 场景位置 | 图片来源 | 防护上限 |
| --- | --- | --- |
| `ResourceStore`（资源图标） | Modrinth / CurseForge | 4096 / 16 MP |
| `InstanceIconController`（包内图标） | 模组声明、`pack.png`、`icon.png`、`logo.png` | 每张压缩条目 ≤ 1 MiB，4096 / 16 MP；后台缩放至 128 像素 |
| `ServerPingController` / `ToolboxController`（服务器图标） | 服务器回复的 data URL、本地缓存 | 4096 / 16 MP |
| `ThemeImage`（用户背景图） | 用户指定的本地文件 | 8192 / 64 MP（源文件 ≤ 32 MB） |
| 皮肤导入入口 | 用户指定的皮肤文件 | 256×256（源文件 ≤ 16 MB；最终规格仍按 `SkinApi::isAcceptableSkinSize` 判定 64×64 / 64×32） |
| `VersionIcons::fileMark`（实例图标） | 实例目录内的 `icon.png` | 8192 / 64 MP |
| `InstanceFilesPage::directoryThumbnail`（存档卡片） | 存档目录内的 `icon.png` | 1024 / 1 MP |
| `InstanceSettingsLifecycle`（截图缩略图） | `screenshots/` 目录内的截图 | 16384 / 64 MP |

---

## 3. 基础执行与归档安全

- **归档解压防护**：`ZipUtil` 逐项拦截 `..`、前导斜杠及盘符冒号；整合包清单由 `ModpackImport::isSafeRelativePath()` 逐段校验。
- **动态库与 Native 路径**：由 `LibraryRules::libraryRelativePathIsSafe()` 统一管控 artifact 与 natives 的相对路径。
- **PowerShell 执行安全**：
  - 启动参数一律采用 `-EncodedCommand` 与 `psQuote()` 拼接；
  - 程序路径强制使用绝对系统路径 `%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe`（由 `PowerShell::executable()` 提供解析并缓存），防止当前工作目录或 PATH 劫持；
  - 解压命令统一使用 `-LiteralPath`，防止方括号等特殊字符被识别为通配符展开。
- **界面文本渲染**：外部展示的富文本一律执行 `toHtmlEscaped()` 转义，外部超链接统一禁用 `openExternalLinks`。

---

## 4. 智能体（Agent）

### 4.1 网络与凭据

- 模型 API 只允许 `https://`；loopback `http://` 需显式勾选并持续提示；拒绝 userinfo、片段、查询串。
- 不跟随重定向，不忽略 TLS 证书错误；API Key 只进认证头，不进 URL、日志、会话或错误。
- API Key 只落 Windows 凭据管理器，主路径不可用时用 DPAPI 后备；两条路都失败拒绝保存并保留旧值，不降级为明文或 QSettings 明文。
- 界面默认星号遮挡，不主动回显；换 Windows 用户或换机器后凭据读不到，需要重新输入。

### 4.2 外发与脱敏

- 文件、日志、工具结果先过 `AgentEgressPolicy`，再按需过 `AgentRedaction`。
- 自动读取：日志、崩溃报告、锁定实例元数据、目录元数据、全局实例总数；首次使用提示「可能发送给当前模型服务」。
- 逐次确认：配置文件、完整文件名清单、其他文本、全局实例完整列表；没有会话级目录放行。
- 硬阻断：凭据文件、凭据存储、二进制内容。
- 严格模式把自动读取类别改为逐次确认，硬阻断不变。
- `AgentRedaction` 当前方案：键名按 `_`、`-`、`.` 和小写/数字后接大写切词根；命中 `password/passwd/pwd/secret/token/credential(s)/cookie/csrf/session/apikey` 或相邻 `api/key`、`private/key`、`access/key` 时遮值；值少于 6 字符不遮；固定形态覆盖 Bearer、`sk-`、JWT、`AKIA…`、PEM 私钥块。完整说明见 [AGENT.md](AGENT.md) §8.2。
- 凭据文件名按 dotenv 家族（`.env`、`*.env`、`*.env.*`）、证书后缀（`pem/key/p12/pfx/jks`）和 `credential`/`secret`/`token`/`apikey`/`api_key`/`api-key` 词根判定；不命中的文件名进入普通文本审批。

### 4.3 文件工具与审批

- 工具只接受相对路径；实例来自会话 `instanceLock`，不接受模型传入的实例 id。
- 路径校验：字符串拒绝绝对路径、`..`、盘符、UNC、ADS、保留名、尾点/空格、控制字符；已存在路径再用 `CreateFileW` + `GetFinalPathNameByHandleW` + `FileIdInfo` + `FileAttributeTagInfo` 核最终路径、卷序列/文件 ID、reparse。句柄校验降低 TOCTOU 竞争窗口，不宣称彻底解决。
- `fs.list` 只发目录元数据；`fs.listNames` 每次审批；`fs.read` 拒绝二进制和凭据文件；`fs.write` 只开放 `config/**`、`defaultconfigs/**`、`options.txt`。
- `fs.write` 必须带旧哈希与身份 token；写前 DPAPI 备份、原子替换、写后回读；对象/内容变化拒绝。`versions/libraries/assets/java/servers` 原始写拒绝。
- 模型可读 artifact 只存脱敏内容并 DPAPI 加密；本地原始存档 id 不在模型可见空间；`artifact.read` 每次重新脱敏。
- 审批请求持久化但不存「已批准」布尔；重启后作废，只能重新请求。
- 维护备份 `owner=agent`，元数据保留 `sourceSessionId`；删除会话不删备份；`owner=launcher/user/shared` 不允许 Agent 清理。

### 4.4 维护工作流与启动器工具（Stage 2B）

- 诊断结果先过 `AgentDiagnosticProjection` 再进模型上下文：只留 severity/code/title/hint、计数、Java 主版本、退出码、OOM/缺前置等布尔事实；Java 可执行文件路径、数据根、日志/崩溃报告绝对路径、账户与环境变量、原始 stack trace 一律不进模型。
- 日志与崩溃报告片段在发送前把本机绝对路径替换成 `<path>`（`AgentDiagnosticProjection::scrubPaths`），再遮凭据形态内容。
- 完整实例清单、完整模组文件名、设置写、模组启停、备份创建与恢复逐次审批；`settings.get` 不返回 `java.path`，路径类字段从结果里剔除。
- 工作流只读模式拒绝一切写工具；要求计划的模式里，写工具必须等于**已批准计划的当前一步**，跳步被拒绝；某一步失败后写工具全部锁住。
- `workflow.finish` 本地核对计划/验证步/证据，缺一项就拒绝；模型不能靠自然语言声称完成。
- 维护备份路径全部是实例目录内的相对路径；恢复前先做快照，恢复时拒绝绝对路径与 `..`；只恢复 `owner=agent` 的备份。

### 4.5 MCP（Stage 2A，stdio）

- 服务器必须由用户显式添加：绝对路径可执行文件 + 参数数组，不解析 `PATH`、不自动下载、不把 `npx`/`uvx` 当默认。
- 启动闸：首次启动、配置修改、手动重连都要确认；确认记录绑定 `configHash` 与可执行文件身份（路径、SHA-256、大小、mtime、卷序列+文件 ID）。任一项变化旧确认失效；文件缺失/读不动直接拒绝启动。
- 不自动重连、不自动重启；崩溃后 pending 请求以错误结束，进程树用 Job Object 清理。
- 每个 MCP 工具逐次审批，不信任服务器声明的只读注解；启动确认与工具审批互相独立。
- 工具结果按不可信数据处理：遮凭据、16 KB 截断、标 `untrusted`；`isError` 映射成工具错误。
- stdout 只走协议（垃圾行有上限），stderr 进脱敏日志；环境变量只记名字，不落盘秘密值。
- 服务器反向请求只回 `ping`，其它方法统一 `-32601`，不挂起。

### 4.6 联网与固定诊断命令（Stage 3）

- 只允许 `http/https`；拒绝 userinfo、片段、空 host；`file:`/`data:`/`ftp:` 一律拒绝。
- 解析目标 host 的**全部** A/AAAA，任一落入 loopback/私网/链路本地/组播/保留/未指定（含 `::ffff:` 映射）就整条拒绝。
- 连接**已校验的 IP**（pinned-IP）；`Host` 头与 TLS 主机名校验用原 host；重定向最多 5 跳，每跳重新解析 + 全地址校验 + pinned 连接；第一版不继承系统代理。
- 只读文本类 Content-Type；响应 2 MB、正文 20 万字符封顶；HTML 去 script/style 后转文本；结果遮凭据、标不可信。
- `web.search` 只支持用户自己的 SearXNG；搜索词会发给该实例，每次调用都要审批。
- `command.diagnose` 只执行编译进程序的固定 spec，host 参数只接受域名/IP 字面量，不经过 shell；默认关闭、每次审批；Job Object 清理进程树；输出 256 KB 封顶。
- `command.run`（任意可执行文件）未开放。

### 4.7 当前限制

- `AgentRedaction` 是键名词根 + 固定令牌形态匹配，不能当作通用凭据兜底：词根表之外的自造字段名要靠形态匹配命中；值少于 6 个字符的字段不遮。
- 凭据文件识别按 dotenv 家族（`.env`、`*.env`、`*.env.*`）、证书后缀和 `credential`/`secret`/`token`/`apikey`/`api_key`/`api-key` 词根判定，文件名里不含这些词根的名字进入普通文本审批。
- 大文本 `truncated=true` 时 artifact 只保存本次读取窗口（默认 256 KB、最大 1 MB），不是整个原文件；脱敏未命中时仍可被 `artifact.read` 分段取回。
- 日志自动读取仍可能带出 Windows 用户名、启动参数回显、绝对路径、显卡/驱动/音频设备等硬件指纹；这些留给后续日志/诊断字段投影。

## 5. 验证入口

自动化范围与断言数见 [docs/dev/TESTING.md](../dev/TESTING.md)；智能体专项为 `verify_agent`、`ui_check_agent`、`main-window-agent`。

## 6. 未覆盖

| 项 | 状态 |
| --- | --- |
| 网络错误矩阵（401/403、429、断网、流中断、重定向、TLS、代理、400 错误体） | 待人工执行 |
| 真实文件系统竞争/杀毒软件占用 | 未覆盖 |
| 真机高 DPI/读屏 | 未覆盖 |
| 日志字段投影（用户名/参数/路径/硬件指纹） | 留给后续阶段 |

## 7. 微软登录回调

- 启动器先绑定本地回环，由系统分配端口，再打开浏览器。授权请求与令牌交换使用同一次绑定得到的完整回调 URI。
- 每次授权用系统随机源分别生成 32 字节的 PKCE `code_verifier` 和 `state`。授权使用 S256，换取令牌时提交同一个 verifier。
- 回调检查 `GET`、已配置的回调路径与本次唯一 `state`，匹配后关闭监听并处理授权结果。打开浏览器失败、超时或处理结束时清理本次状态。

