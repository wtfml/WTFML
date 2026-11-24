# 测试运行流水（2026-10-24 至 2026-11-10）

从 `docs/dev/TESTING.md` 搬来的逐轮测试记录：每条一行，格式「日期｜套件｜结果｜备注」。短编号都在 `build/test-runs/<日期>-<编号>/`，是本地产物，不进仓库。只留每个主题最后一次有意义的计数；中间的失败重跑并入备注。当前的跑法与约定看 [TESTING.md](../dev/TESTING.md)。

## 离线回归与专项

| 日期 | 套件 | 结果 | 备注 |
| --- | --- | --- | --- |
| 10-24 | workspace + activity-selection | 32/32 + 36/36 | `190936-7867d634`；串行 |
| 10-24 | main-window | 2670/2670（93.22 s） | `191430-9e580103` |
| 10-24 | download-retry / high-speed / segmented / segment-install | 4 模块通过（63.76 s） | `213703-14906487`；含子进程硬退出后的续传恢复；高速下载截图移入夹具目录 |
| 10-24 | workspace + activity-selection | 36/36 | `214635-43b35350`；新增真实 `sandbox-probe` 子进程核对设置/数据根隔离 |
| 10-24 | 实例（世界导入空间预检 ZJ7、预设索引 PA11/PA3） | core-instances 1479/1479；core-presets 678/678（22.29 s） | `231046-e2a4e5ac`、`231403-01d8b009`；索引上限 4 MiB、清单上限 16 MiB |
| 10-25 | 实例（`WorldFiles::readLevelData` 指纹） | core-instances 1483/1483 | `113734-2c8e0b97` |
| 10-25 | ui_check | 3119/3119；`--repeat until-fail:5` 共 48.28 s 全过 | `113931-d670e02a`；未启用 U10O/junction 跳过 |
| 10-25 | P1 2.1.1–2.1.4 | verify_install 378、verify_activity 148、verify_instances 1535、verify_resources 824、`mw_check_instance_upgrade` 232 PASS / 0 FAIL、mw_check 2696、ui_check 3119、ctest 7 项全过 | 覆盖 Quilt 列表/键映射、子任务不进任务中心、报告 JSON 边界、世界导入冲突、`level.dat` 占用、适配替换与声明解析 |
| 10-25 | theme + main-window | wtfml.theme 通过；主窗口 2687/2687 | `142308-afc83d2a`、`143826-e67ea5d7`；首次沙箱运行 `143017-33091a97` 因本机回环 `Permission denied` 在 240 s 超时，保留 |
| 10-25 | 凭据日志遮盖 | core-accounts 365/365、core-launch 300/300、main-window 2701/2701 | `202329-6593da47`、`203357-93148414`；首轮 2700/2701 是新断言没适配 CRT 的 CRLF |
| 10-25 | Java 扫描后台化 | main-window 2696/2696 + Java 管理专项（99.04 s） | `195745-67ed6c08` |
| 10-25 | 任务行局部复用 | ui-refresh / install-layout / lifecycle / theme 通过；workspace + activity-selection 通过 | `205703-6211941e`、`205805-3251de49` |
| 10-25 | 大量预设 | preset-install 通过（1.15 s）；四个 UI/主题模块 2.50 s | `213510-15e5e224`、`213614-450707b8` |
| 10-26 | GTNH 首启资源预取（2.1.6） | core-resources 899、main-window 2700；合入 main 后 main-window 2711、verify_instances 1570、`ctest -L ^ci-core$` 16/16 | `210107-7fda3d51`；首次 ci-core 红在 `wtfml.test-modules`：`OperationReportTests.cpp` 没有领域归属，补表后全绿 |
| 10-26 | VersionManager 拆分合并 | ci-ui 8/8、ci-integration 12/12（主窗口 2698）、visual 全过 | `014245-afb441e5`、`014427-520e7bac`、`015425-3101bd66`；拆分前 ci-core 16/16 `011825-36230178` |
| 10-26 | 模组页紧凑布局 + 预设后台刷新 | mods 通过；visual 3119/3119 | `020613-61c921fc`、`020924-761aad4d`；`020729-afc9ce60` 3117/3119，两条旧布局断言仍要求已退役的刷新工具行，已更新 |
| 10-26 | 服务器收藏 | core-servers 718、core-launch 309、main-window 2706、visual 3119 | `092219-218ed5bc`…`093303-bf79ff84`；逐字节核对 `servers.dat` 未变 |
| 10-26 | 预设重复扫描 / 详情后台读取 / 应用计划后台预览 | core-presets 678/678；preset-install 通过（含 4 MiB 清单与损坏 JSON） | `093654-53701f76`、`113225-8ab8980d`、`121807-505c1567`；`PresetManifestChecks.cpp`、`PresetPreviewChecks.cpp` 归 presets 分片 |
| 10-26 | Windows 长路径 | core-instances / resources / presets / toolbox 四分片通过（50.74 s） | `134526-d7250a6f`；`LongPathTests` 归 core-instances，仅 Windows 执行 |
| 10-26 | 下载预算 / 低速比较 | 四个 network 集成分片通过（64.95 s） | `135445-d6d0145f`；`RequestBudgetChecks` 归 download-retry |
| 10-26 | 共享专用运行器（`-RunnerMode`） | 共享集成分片 10/11（升级替换 4 条失败）；共享 UI 分片 8/8（3.99 s） | `174158-67fe2e6a`、`175243-04fd597f`；升级失败的定位见 TESTING.md「Windows 占用」 |
| 10-26 | 按职责拆分大文件后 | main-window 2707（91.65 s）、resources 825、ui 3119（11.19 s） | `190844-451f23e2`、`191423-324c6db0`、`191541-0cd0bd59` |
| 10-26 | 升级占用修复 | 升级分片通过（28.02 / 29.93 / 28.83 s） | `235716-ecd81d8b`、`235901-e9fa733e`、`000049-2a2650ea`（10-27） |
| 10-27 | 下载批次恢复 | network 五入口通过（69.97–75.29 s）；完整主窗口 2715/2715（87.78 s） | `205645-8abd2c9a`、`205222-bb92957e`；命令 `tools/test.ps1 -Suite network -BuildJobs 6 -BuildTimeoutSeconds 300 -TestTimeoutSeconds 180` |
| 10-27 | 2.2 剩余项 | core-presets 736、core-activity 148、core-servers 718；主窗口 2715/2715；`-Suite visual -RunnerMode Compact` 3121/3121；下载执行器四分片 | `100704-f337ffaf`、`103030-860cef89`、`104607-fe17c80d`；真实 NVMe 受限执行退出 -1073741819，日志保留，未宣称根因 |
| 10-27 | 2.1.7–2.2.3 | core-presets 695、core-resources 900；最终 ui_check 3119（10.31 s）；ci-ui 八入口 | `092103-82f9606e`、`092421-ee5c7925`、`012031-89fd1cbc`；清单字节不符是 Windows 换行转换，`.gitattributes` 固定 LF 后 900/900 |
| 10-28 | §2.1/§2.2 收尾 | network/下载/预设 5/5（72.48 s）；主窗口 2716/2716（93.18 s） | `080043-3fcca5f4`、`080259-731583c4`；`mw_check.exe update-center-check` 约 2 s |
| 10-28 | 保守开放并行 | 并行 3/3，墙钟 6.86 s（各进程合计 12.09 s）；串行 9.50 s | `123154-ac213ec8`、`123449-c4843d1e`；仅 core-accounts / core-activity / core-toolbox |
| 11-04 | 核心套件不下载 Java | `ci-core` 恢复稳定 | 2026-11-03 慢网络曾跑满 900 s 判死 |
| 11-05 | 整份 `verify` 与分片 | `verify.exe` 6457/6457（122.1 s）；分片 baseline 195、accounts 402、activity 148、launch 346、instances 1715、java 268、presets 867、resources 942、servers 762、toolbox 435、install 378、downloads 244 | 分片相加大于整份：每个分片程序各自再跑一遍模块级引导断言 |
| 11-05 | `-Suite full` | 38 项全过（394.28 s，含 network-diag）；`ctest -E visual` 39 项全过（410.98 s） | 本机 `build-audit`；Agent 套件加入前 |
| 11-05 | `-Suite network` | 6/6（107.94 s）；复测 5/5（89.16 s） | 设 `WTFML_TEST_TEMP` 之后；之前本机 3/6 失败，根因见 TESTING.md「环境变量」 |
| 11-05 | network-diag | 48 条全过（19.0 / 18.9 s，退出 0） | `build-audit/tools/network_diag/WTFMLNetworkDiag.exe --self-test`，`QT_QPA_PLATFORM=offscreen` |
| 11-05 | 例行检查 + `-Suite quick` | 归属检查 PASS；quick 12/12（11.85 s）；文档引用检查 PASS | 当时 129 条 md 链接 + 48 条源码 docs 引用 |
| 11-10 | Agent Stage 1A | `verify_agent` 145、`ui_check_agent` 28、`mw_check_agent` 22 | `wtfml.ui`、`main-window`、`token-protection-fault`、核心基线、`test-modules/test-runner/ci-partitions` 同轮通过；细节见 [AGENT_STAGE1A](../features/AGENT_STAGE1A.md) |
| 11-10 | Agent Stage 1B | `verify_agent` 306、`ui_check_agent` 39、`mw_check_agent` 31 | junction 夹具建不出来时输出 `SKIP`，不计通过；见 [AGENT_STAGE1B](../features/AGENT_STAGE1B.md) |

## 真机与实网验收

| 日期 | 内容 | 结果 | 备注 |
| --- | --- | --- | --- |
| 10-24 | 大实例（1 GiB 单文件 + 6000 小文件，普通 NTFS） | 大复制 3.61 s；文件独占失败 3.92 s 且不发布半成品；升级取消 0.23 s；kill 后恢复 0.03 s；真实双进程脚本 10/10 | 第二个进程停在拒绝对话框、以 1 退出 |
| 10-25 | 同上复跑（D: NVMe，Defender 实时保护开） | 大复制 3.70 s；独占失败 3.88 s；取消 0.23 s；`publish` 阶段外部终止后恢复 count=1、0.04 s；完整升级重试 52.61 s | 证据 `build/acceptance-20261025/`；物理慢盘未跑 |
| 10-26 | 2.1.5 升级/回滚/主菜单（四个加载器，干净副本，离线身份） | 见下表 | 证据 `build/acceptance-215/` |
| 10-26 | 游戏目录改名后恢复（2.1.5a） | Forge 实例 `publish` 阶段中断后改名，恢复 0.04 s；随后启动到主菜单 14.07 s | journal 的 `rootFingerprint` / `rootRelative` / `rootName` 已断言；指纹被改时不动盘 |
| 10-26 | 大实例脚本（16 MiB + 100 文件，`--kill-stage publish`） | 大复制 0.10 s、占用失败 0.10 s、取消 0.23 s、恢复 0.04 s、重试 1.48 s | `build/acceptance-215/large.json` |
| 10-26 | GTNH 首启（2.1.6c，空数据根，Temurin 21.0.12.1，GT_New_Horizons_2.8.4_Java_17-25.zip 595,584,698 B） | 导入 42.7 s；预取 1356 对象 + 2 份 JSON = 112.6 MiB（复用 12，20.5 s）；首启到主菜单 36.6 s | 启动期间 `assets/asset_director` 1358 个文件一个都没变，日志 0 条 `AssetDirector` 下载；只预取静态集合时启动又下 612 个对象、152.1 s，所以清单以"已校验缓存"为准（1368 个对象 / 128,399,283 B） |
| 11-03 | 真屏 UI/DPI 巡检（系统 150%） | 三主题 × 两尺寸 × 十场景 60 张截图全部保存；调色板失配 0；Tab 遍历 `visitedControls=6` | `build/tmp-desktop-extended-20261103/`；100% / 125% 未测 |
| 11-03 | 静态发布重建 + 软件渲染 | 74,103,888 → 去符号 43,707,356 字节（−41.0%）；真启动三跑 visible 199–204 ms；`QT_OPENGL=software` 24 张截图全部保存 | 见 [RELEASE_SIZE](../dev/perf-notes/RELEASE_SIZE.md) |
| 11-03 | 实网下载（无系统代理） | `icon-live 20`：搜索 833 ms、20 张图标 1884 ms 全回；`cf-resolve-live`：镜像 728 ms 换回 1 条；`resource-live mod` 22/22：下载 `sodium-fabric-0.9.3-beta.1+mc26.3.jar` 1,912,496 B | 同一条 `icon-live` 连跑两次，第一次搜索 15001 ms / 0 条，之后没复现；`resource-live` 曾在原生「下载到哪个文件夹」框上挂住，现改为从实例设置 → 模组页进入 |

2.1.5 升级 / 回滚 / 主菜单（秒；判据 `Sound engine started` / `OpenAL initialized`，Forge/NeoForge 只到主菜单）：

| 加载器 | Java | 复制更新 / 启动 | 原地更新 / 启动 | 回滚 / 启动 |
| --- | --- | --- | --- | --- |
| 原版 26.3 | 25 | 0.58 / 15.07 | 8.65 / 16.10 | 0.43 / 10.61 |
| Fabric 0.19.5 + 26.3 | 25 | 1.37 / 16.15 | 7.83 / 16.42 | 0.44 / 10.55 |
| Forge 47.4.10 + 1.20.1 | 17 | 0.36 / 13.91 | 3.09 / 14.67 | 0.40 / 13.96 |
| NeoForge 21.11.13-beta + 1.21.11 | 21 | 1.05 / 11.69 | 5.19 / 11.29 | 0.41 / 11.31 |

## 保留的失败记录

| 日期 | 编号 | 现象 | 处理 |
| --- | --- | --- | --- |
| 10-20 | 运行 `39304391380`、`39356133554` | CI 主程序冷构建到约 85% / 165 个对象触发单阶段 900 s 上限（退出 124） | 用 `ci-object-batches.ps1` 分批构建，不靠无限延长超时 |
| 10-26 | `174158-67fe2e6a`、`174527-3e506976` | 实例升级替换用例 4 条失败 | 原实例目录改名被拒（错误 5）；Restart Manager 查到持有测试 JAR 的编辑器进程，不是产品判据错误，夹具改到用户缓存下独立 `QTemporaryDir` |
| 10-27 | `001701-e5a39c68`、`002818-6bac57e4`、`003431-60df2817` 等 | 缺生产 provider 链接、解析接口错误、新报告使删除空目录夹具失效 | 均已修正后复测，原日志保留 |
| 10-27 | `091446-038c71eb` | preset-install 失败：暂存文件用另一个 `QFile` 改名，Windows 仍持有 `QTemporaryFile` 句柄 | 改为暂存对象自身 rename，没有取消"目标不覆盖"判据 |
| 10-27 | 真实导入探针 | 初次漏传清单指纹，工具收到成功但 `stageProof=false` 被判失败 | 修正指纹后取消、下载期强制终止与新进程恢复全过 |
