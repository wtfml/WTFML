# 测试与验证

给改代码的人看：怎么在本机跑测试、CI 怎么分片、测试代码放哪、新增用例守什么约定。前半（到「新增测试约定」）是操作手册，后半是专项说明和已知问题。逐轮的通过数流水在 [TEST_RUN_HISTORY.md](../archive/TEST_RUN_HISTORY.md)，性能基线在 [PERFORMANCE.md](PERFORMANCE.md)。

## 五分钟上手

环境：Windows、PowerShell 7、CMake、MinGW 13.1 和 Qt 6.10.1 的 `bin` 在 `PATH` 里。在仓库根目录运行：

```powershell
./tools/test.ps1                                   # 按改动选模块：比较 origin/main...HEAD，并合并未提交/未跟踪文件
./tools/test.ps1 -Suite quick                      # 固定的快速专项（16 项）
./tools/test.ps1 -Suite full                       # 全部非 visual 测试（改了共享代码、发版前用）
./tools/test.ps1 -Suite full -ListOnly             # 只列测试，不构建、不执行
./tools/test.ps1 -Suite quick -NoBuild             # 用现有二进制跑，不保证对应最新源码
```

默认构建目录 `build/verification-mingw13`，首次按 `verification` preset 配置；已有目录沿用它自己的配置，不会换编译器。脚本构建前会强制结束正在运行的 `WTFMinecraftLauncher`（`-NoBuild` / `-ListOnly` 不会）。失败时看 `build/test-runs/<时间>-<随机号>/` 里的日志，控制台只显示末尾摘要。

### 日常看效果

改完代码只想看界面，不跑测试：

```powershell
./tools/dev-build.ps1      # 增量编 build/verification-mingw13 的 exe，并行度默认 CPU 核数减 2（-Jobs 覆盖）
./tools/dev-run.ps1        # 用动态 Qt 起 exe，不阻塞终端；打印 exe 路径与 PID
```

exe 在 `build/verification-mingw13/WTFMinecraftLauncher.exe`，改完代码重跑 `dev-build.ps1` 即可。两个脚本自己把 Qt 的 `bin` 与 MinGW 13.1 的 `bin` 前置进 PATH、设 `QT_PLUGIN_PATH`，路径读构建目录的 `CMakeCache.txt`，读不到用本机默认；目录不存在时 `dev-build.ps1` 按 `verification` preset 配置一次（带 `-DWTFML_BUILD_NETWORK_DIAG=ON`）。有 `ctest`/`mw_check`/`ui_check` 在跑时它拒绝构建，`-Force` 跳过；两个脚本都支持 `-DryRun` 只看命令。

### 套件

`tools/test.ps1 -Suite <名>` 共 14 个套件：

| 套件 | 选到的测试 |
| --- | --- |
| `affected`（默认） | `tools/test-impact.ps1` 算出的模块（见「按改动选择模块」），排除 `visual` 标签；只改文档则跳过构建和测试 |
| `quick` | `test-runner`、`test-modules`、`java-manage`、`mod-action-panel`、`workspace`、`activity-selection`、`theme`、`ui-refresh`、`install-layout`、`lifecycle`、`toolbox-polish`、`toolbox-layout`、`armor-trim`、`core-agent`、`ui-agent`、`main-window-agent` |
| `full` | 全部非 `visual` 测试 |
| `core` | `core` 标签：核心基线 + 12 个核心分片 + 引导检查 |
| `window` | `main-window`、`main-window-agent`、`window-chrome`（真 windows 平台插件，窗口摆屏外不激活：WM_NCHITTEST、系统标题栏、原生子窗口、隐藏再显示） |
| `java` | `java-manage` |
| `mods` | `mod-action-panel`（模组操作栏、迁移与依赖检查） |
| `upgrade` | `instance-upgrade`（原地升级、完整快照、故障回滚）；只构建独立的 `mw_check_instance_upgrade` |
| `network` | `download-retry`、`high-speed-download`、`segmented-download`、`segment-install`、`preset-install`、`network-diag`；本机可控服务器，不访问外网 |
| `visual` | `visual` 标签：完整历史 UI（`wtfml.ui`） |
| `ci-app` / `ci-core` / `ci-ui` / `ci-integration` | 各自分片标签的非 visual 测试，CI 用，见「CI 分片」 |

`-Modules <名,…>` 只能配 `affected` 或 `ci-*` 套件，按测试名与所属分区取交集；`affected` 显式传 `-Modules` 时以传入的为准（不再用改动影响分析覆盖，也不因无改动/仅文档改动跳过），交集为空时直接报错退出并列出这些模块里的测试实际所在分区。例如：

```powershell
./tools/test.ps1 -Suite affected -ChangedPath tests/core_modules/AsciiAliasTests.cpp
./tools/test.ps1 -Suite affected -Modules ui
./tools/test.ps1 -Suite ci-core -Modules core-java
./tools/test.ps1 -Suite ci-ui -Modules theme
```

### 常用参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `-BaseRef` | `origin/main` | `affected` 的比较基准；读不到会失败，不静默漏测 |
| `-ChangedPath` | — | 直接指定改动路径（`affected`） |
| `-BuildDir` | `build/verification-mingw13` | CI 用 `build/ci` |
| `-ConfigurePreset` | `verification` | CI 用 `ci-verification` |
| `-BuildJobs` | 2 | 构建并行度，1–16 |
| `-TestJobs` | 2 | 测试并行度，1–2；真正并行的只有三个已隔离模块，见「并行」 |
| `-BuildTimeoutSeconds` | 600 | 配置和每个构建目标各自的上限，不得超过 900 |
| `-TestTimeoutSeconds` | 900 | 整个 CTest 阶段的上限，不得超过 900 |
| `-RunnerMode` | `Auto` | `Compact` 把专用 UI/集成测试程序合并链接（完整分区运行时更省链接），`Standalone` 关闭；对应 CMake 选项 `-DWTFML_COMPACT_TEST_RUNNERS=ON/OFF`，默认 OFF |
| `-NoBuild` / `-ListOnly` | 关 | 跳过构建 / 只列测试 |

### 直接用 CMake preset

`CMakePresets.json` 里有四个配置、四个构建和三个测试 preset：

| 配置 preset | 生成器 / 档位 | 构建目录 | 用途 |
| --- | --- | --- | --- |
| `dev-debug` | MinGW Makefiles / Debug，测试 OFF | `build/dev-debug-mingw13` | 日常开发调试 |
| `release` | MinGW Makefiles / Release，测试 OFF | `build/release-mingw13` | 只编产品 |
| `verification` | MinGW Makefiles / Release，测试 ON，`-Werror` | `build/verification-mingw13` | 本机验证，`tools/test.ps1` 的默认 |
| `ci-verification` | Ninja / `-O1 -DNDEBUG`，测试 ON，`-Werror`，网络诊断 ON，编译器取 `PATH` 里的 `g++` | `build/ci` | CI |

```powershell
cmake --preset verification
cmake --build --preset verification          # 并行 2
ctest --preset verification                  # 排除 visual 标签，串行，失败时打印输出，没有测试即报错
ctest --preset visual                        # 只跑 visual 标签（完整历史 UI）
```

测试 preset 有 `verification`、`ci-verification`、`visual` 三个；`ctest --preset` 串行执行，`tools/test.ps1` 额外带超时、独立报告、按改动选择和三个模块的并行。已有 `build/ci` 若是 Makefiles 生成器，换 Ninja 要用新目录。

### 例行检查（改完测试文件跑一遍）

```powershell
./tools/check-test-modules.ps1      # 测试入口 / 模块归属 / 大小上限
./tools/check-surface-registry.ps1  # 新对话框 / showPage 页面有没有登记进 sweep 表面清单
./tools/check-doc-links.ps1         # 文档相对链接与源码里的 docs 引用
./tools/check-i18n.ps1              # 新代码里的中文包没包 tr()（基线见 docs/dev/I18N.md）
./tools/test.ps1 -Suite quick
```

`check-test-modules.ps1` 即 CTest 的 `wtfml.test-modules`：入口不超过 4000 行、职责模块不超过 5000 行，禁止入口 include `.cpp`，核对核心用例的归属表，检查设置沙盒隔离，并确认所有带 `ci-*` 分区标签的非 `visual` 测试都归入 `tools/test-impact.ps1` 的某个模块。新增用例放进相应模块，不靠抬高阈值堆入口。`check-surface-registry.ps1` 即 CTest 的 `wtfml.surface-registry`，判据与豁免写法见「新增测试约定」下面那条。

## 环境变量

| 变量 | 作用 |
| --- | --- |
| `WTFML_TEST_TEMP` | 测试进程的临时目录根。默认用系统临时目录；受限令牌环境（Windows 沙箱、某些企业策略）下子进程只能写工作区，要把它指到工作区里，否则取默认路径的 `QTemporaryDir` 会拿到"拒绝访问"，对应模块直接返回 2（ctest 报 `Failed`、0.03 秒、只跑 2 条断言） |
| `WTFML_TEST_JAVA_DOWNLOAD=1` | 让 `verify` 的「E. Java 运行时选择」真的去下载运行时。默认只在本机找（`ensureJavaForVersion(..., allowDownload=false)`），找不到就打印"本机没有，按设置未下载"，不碰网络 |
| `QT_QPA_PLATFORM=offscreen` | UI 测试和 `WTFMLNetworkDiag --self-test` 用的离屏平台 |
| `WTFML_UC_SKIP_U10O=1` / `WTFML_SKIP_JUNCTION=1` | `ui_check` 的 U10O 整段 / junction 局部的逃生口；默认不设 |
| `WTFML_FILES_PAINT_PROBE_ROWS=<行数>` | `mw_check_mod_action_panel` 打印文件页首绘/重列/滚动耗时（只打印、不断言），见 [LIST_PAINT.md](perf-notes/LIST_PAINT.md) |
| `WTFML_MOD_SELECTION_PROBE_ROWS=<行数>` | 同一专项里的反选规模测量 |
| `WTFML_PRESET_SCALE_ROOT` / `WTFML_TEST_GTNH_ARCHIVE` / `WTFML_TEST_PACK_ARCHIVES` / `WTFML_LIVE_DIRECT_URL` | 可选的大规模预设、整合包样本、实网直链样本；不设则跳过对应分支 |

受限环境下的典型用法（路径换成本机的）：

```powershell
$env:WTFML_TEST_TEMP = 'D:\Desktop\the_project\.tmp-test'
./tools/test.ps1 -Suite network -BuildDir D:\Desktop\the_project\build-audit -NoBuild
```

本机模拟服务器需要允许 `127.0.0.1` 回环连接。沙箱里出现 `Permission denied` 时，保留失败日志并换用允许回环的环境，它不说明下载实现失效。

## 超时、日志和失败

- CTest 内各专项自带的超时继续生效；核心保留 900 秒预算，避免把 CI 慢误判为死锁。
- 详细断言输出写日志；每 30 秒显示阶段、已耗时间和最新日志。超时终止该进程树，阶段记录退出码 124，脚本返回非零。
- 每次运行的报告在 `build/test-runs/<时间>-<随机号>/`：阶段 stdout/stderr、`summary.json`、`impact.json`（`affected`，写明改动文件、模块、测试名和扩大范围的原因）、CTest `results.xml`（正常收尾时生成；超时以 summary 和日志为准）。阶段失败后停止后续阶段，不自动复跑覆盖首次失败；CTest 本身会跑完本组其余用例，便于一次收集多个问题。
- 测试进程的临时目录在 `WTFML_TEST_TEMP`（或系统临时目录）下的 `wtfml-tests-<随机号>`，保留供诊断；不自动上传，避免大文件或敏感材料混进 CI 附件。
- 同一构建目录用命名互斥锁，第二份脚本直接报错。手动构建绕得过这把锁，所以运行脚本期间不要另开构建。

### 并行

`-TestJobs` 默认 2，但只有 `core-accounts`、`core-activity`、`core-toolbox` 三个模块被核对过可以并行（CTest 里带 `isolated-parallel` 标签、`RUN_SERIAL FALSE`）；其它测试包括 `network-diag` 一律 `RUN_SERIAL`。这三个模块各自隔离 QSettings、当前与默认游戏根，用各自的临时夹具，不共用辅助进程。同机一次对照：并行 3/3 墙钟 6.86 秒，串行 9.50 秒。

不要靠调 `-j` 提高并行度。旧测试仍有固定数据目录：`mw_check` 系的沙盒是 `applicationDirPath()/check-settings` 与 `applicationDirPath()/.minecraft`（后者是产品口径的"启动器级默认数据根"，`TaskHistory` 等账本落在那儿），同目录下各专项 exe 共用这两个位置，并行会互删对方的设置与账本。要开放并行，先让每个进程有独立的 `applicationDirPath`，或让产品支持测试专用的默认根注入。2026-10-22 的 `-j2` 全量并行没有净收益（38/38，246 / 239 秒；串行 213.94 秒）。扩大并行名单要附重叠证据，`check-test-modules.ps1` 检查入口的隔离声明有没有遗漏。

## CI 分片

`.github/workflows/windows-ci.yml` 的 `plan` 作业先用 `tools/test-impact.ps1` 算出要跑的分片，再起 `verify` 矩阵，最后 `build-and-test` 汇总（保留这个必需检查名；文档类改动没有分片可跑时直接通过，计划失败不会假绿）。触发：push `main`、PR、手动、每周日 19:00 UTC 定时、`v*` 标签；定时/手动/标签/基准提交读不到时跑全部四路。同事件同分支的新提交取消旧运行。

四个分片各自串行跑测试，每个 CMake 测试自动附带所属分片标签：

| 入口 | 构建目标 | 验证范围 |
| --- | --- | --- |
| `-Suite ci-app` | 主程序、网络诊断工具 | 主程序编译及诊断工具自测 |
| `-Suite ci-core` | 所选 `verify_<域>` / `verify_baseline` | 核心、测试入口自检、分片完整性检查 |
| `-Suite ci-ui` | 所选 `ui_check_<专项>`；历史模块选 `ui_check` | 控件专项和完整驱动启动检查（`ui-bootstrap`），不运行历史 visual |
| `-Suite ci-integration` | `mw_check` 及所选 `mw_check_<专项>` | 主窗口及全部集成专项 |

- `wtfml.ci-partitions`（`tools/check-ci-partitions.ps1`）对比四组的并集与 full：缺项、重复、空组都会失败。
- 矩阵作业命令是 `./tools/test.ps1 -Suite <分片> -Modules <模块> -BuildDir build/ci -ConfigurePreset ci-verification -BuildTimeoutSeconds 900 -BuildJobs 2`，用 Qt 6.10.1 / MinGW 13.1（`install-qt-action`），超时 45 分钟。
- `visual` 只在定时或手动勾选时，于 `ci-ui` 作业里追加执行：`./tools/test.ps1 -Suite visual -BuildDir build/ci`。
- 日志与 JUnit 附件无论成败都上传，保留 14 天，按分片命名不互相覆盖。
- 仅 `ci-verification` 用 Ninja 和 `-O1 -DNDEBUG`（取代高编译成本的默认 `-O3`）；验证程序保留各自的 `-O0`，严格警告与 `-Werror` 不变。CI 产物不是发布包。
- 编译缓存用 Mozilla sccache 加 GitHub 缓存，只编缓存未命中的单元，缓存服务不可用时退回真实编译；每个作业仍用 2 个编译进程（私有仓库 runner 内存有限）。每次构建后输出 `sccache --show-stats` 和 `CMakeCache.txt` 里的 `CMAKE_CXX_COMPILER_LAUNCHER`，只记录、不判失败，缓存变量没生效时日志里能直接看到。
- `ci-app` 在 Ninja 下先刷新构建图，枚举主程序对象，每 32 个编译单元一个独立阶段（`tools/ci-object-batches.ps1`），最后构建完整主程序目标和诊断工具并执行测试；每批沿用构建超时、失败不重试，枚举为空直接失败。这样冷编译不会全挤进同一个 900 秒预算。

CTest 的两种"排除 visual"选到的数量不同：`ctest -E visual` 按名字排除，`wtfml.ui` 名字里没有 visual，所以会被选到；`tools/test.ps1 -Suite full`、`ctest --preset verification` 和 CI 按标签排除（`-LE visual`），`wtfml.ui` 带 `visual` 标签，不跑。2026-11-05 实测（Agent 套件加入前）：`-Suite full` 38 项，`ctest -E visual` 39 项，都全过。

## 按改动选择模块

`tools/test-impact.ps1` 是路径到测试模块的唯一映射。本地用法：

```powershell
./tools/test-impact.ps1 -ChangedPath src/controllers/UpgradeAssessment.cpp   # 查看计划
./tools/test.ps1 -Suite affected -BaseRef HEAD~1 -ListOnly                    # 只列出待运行测试
```

- 文档改动或无改动：跳过构建和测试。
- 更新控制器：构建应用，跑更新专项和基础 UI 专项，不跑无关的核心/主窗口大用例。
- 模组、网络、预设、Java、工具箱等按关联选专项；"只改一个文件"不是漏掉关联测试的理由。
- 共用主窗口、设置/网络/工具基础层、构建配置、未知路径：退回全部非 visual 测试。
- 模块到分片：`app` → `ci-app`；`core` / `core-*` / `runner` → `ci-core`；`ui`、`ui-history`、`theme`、`popups`、`resources`、`toolbox`、`ui-agent` → `ci-ui`；`window`、`agent-window`、`workspace`、`java`、`mods`、`upgrade`、`network`、`presets`、`toolbox` → `ci-integration`。
- 浮层实现或用例改动选中 `wtfml.popups` 和 `wtfml.popups-scale`；两档均检查靠屏幕右缘、有滚动条的下拉弹层是否留足最长选项的宽度。

测试源码已物理拆分，专项不依赖编译/链接完整的 `ui_check` 或 `mw_check`。CTest 注册同时生成 `test-targets.txt`，运行器先选测试，再按归属构建独立目标；脚本类测试无需构建 C++。

## 测试代码的模块边界

- `tests/core_modules/`：每个用例函数一个 `.cpp`（现有 136 个 `.cpp`，其中 120 个 `*Tests.cpp`），按领域组织成 12 个独立可执行程序：`verify_downloads`、`verify_install`、`verify_instances`、`verify_java`、`verify_accounts`、`verify_resources`、`verify_launch`、`verify_servers`、`verify_activity`、`verify_presets`、`verify_toolbox`、`verify_agent`，对应 CTest 的 `wtfml.core-<域>`。`verify_baseline`（`wtfml.core`）保留原 main 里的基础断言，这些入口的并集覆盖旧核心集合。`verify` 是兼容手动模式，CI 只检查它能链接和启动（`wtfml.core-bootstrap`）。
- 整份 `verify` 与分片的数量不能相加：每个分片程序各自再跑一遍模块级引导断言，所以核心总量以整份 `verify` 或 ctest 为准（2026-11-05：整份 6457/6457，分片明细见流水）。
- `tests/ui_check/modules/`（104 个 `.cpp`）：导航、主题、任务、配置编辑、内容页、账号、日志等各自一个编译单元。历史顺序测试仍由 `ui_check` 编排，保留共享窗口和执行顺序；快速专项（`fold-motion`、`ui-refresh`、`files-rows`、`install-layout`、`input-widths`、`toolbox-polish`、`armor-trim`、`lifecycle`、`resource-packs`、`theme`、`ui-agent`、`window-size`）独立构建和注册。`ui-bootstrap` 只检查完整驱动能链接、启动和读取图标。
- `tests/mw_check/modules/`（67 个 `.cpp`）：原命名函数、历史界面接线段和快速专项各自独立；下载、预设、更新、Java、模组等专项各有入口，不链接完整主窗口测试驱动。
- `mw_check` 的夹具头分三层：`SupportCore.hpp`（断言与不碰界面的小工具）、`SupportFakes.hpp`（假外网与预置皮肤夹具）、`Support.hpp`（真窗口那套 UI 夹具与各节入口声明），`Support.cpp` 放共用的非模板实现。跨段变量显式传参，统计计数共享，历史测试顺序不变。三个原始入口保留编排和手动探针。
- 生产实现依次复用 `wtfml_core_runtime`、`wtfml_ui_runtime`、`wtfml_mw_runtime`，测试程序之间相同实现和 Qt 元对象不重复编译；图标资源在 UI 入口显式初始化。
- 原有 `vm-batch` / `vm-fetch-fail` / 联网安装等手动模式仍显式运行，不会混进常规 CI。

### 夹具生命周期（ui_check 的大模块）

大测试共享长寿命夹具时先要说清谁活多久：

| 夹具 | 谁建的 | 活多久 | 模块怎么拿 |
| --- | --- | --- | --- |
| `QApplication` / 主题 / 字体 | `tests/ui_check/main.cpp` 开头 | 整个进程 | 按 `QApplication &` 传（只读） |
| `ui`（`MainWindowUI`）、`window`（`QMainWindow`） | `main.cpp` | 整个进程 | 按引用传 |
| `tabs` / `bar`（底栏）/ 实例下拉框等控件指针 | `main.cpp` 从 `ui` 取 | 整个进程（控件归 `ui` 所有） | 按 `T &` / `T *` 传；只有真的会换掉这个指针时才写 `T *&` |
| 各模块自己造的数据目录 | 模块内的 `QTemporaryDir`（`scratch` / `scratch2` …） | 到所在那一层大括号为止 | 不出模块 |

- `*&` 是例外：全仓只有 `UiInstallFormLayoutChecks` 真的会给形参重新赋值（换掉 `loaderCard` / `installFooter`，因为它把安装页重搭了一遍）。其余模块的 `*&` 是历史上照抄签名，没有实际用途。
- 没人用的形参要删（`UiContentPagesChecks` 已改成无参，因为函数体里的 `bar` 是页内自己的控件）。
- `ui_check` 开头把主题规格钉在冰川碧湖预设（`Theme::presetSpec("emerald")`，只设规格、不 `apply()`）：整份像素基线量的是浅色那一身。出厂默认是暮色雪山，由 W1 / W8 / W9 / 收尾 和 `ui_check_review_shots` 的第一趟（临时设置目录 + `Theme::load()`）单独验。改默认配色时先看这两处。`ui_check_review_shots --r70kbd --theme=emerald|dark` 是开发用截图工具（不进启动器）：实例页用方向键选中一行，出鼠标 / 键盘各一张，看行尾按钮和当前行焦点标记。

`UiContentPagesChecks.cpp` 原来的 18 段已全部拆出（2026-11-04）：`UiServerPageChecks`（U10L，服务器页）、`UiBatchManagementChecks`（U10Q，批量管理）、`UiRowActionsChecks`（U10R）、`UiDetailEntryChecks`（U10N）、`UiDownloadNewChecks`（U10M）、`UiEmptyPageChecks`（U10f）、`UiQuickPlayChecks`（U10S）、`UiSearchFilterChecks`（U10P）、`UiElidedNameChecks`（U10k）、`UiScrollAnchorChecks`（U10M-2）、`UiMediumPagesChecks`（U10O）、`UiToggleChecks`（U10d）、`UiDirectoryButtonsChecks`（U10b2）等，夹具都是显式形参（共用 `InstanceSettingsDialog &dialog`、两个名字、`GlobalValues &global`、临时目录）。拆分前后 `ui_check` 断言数不变，`UiContentPagesChecks.cpp` 从 4060 行降到 294 行。

#### 其余大模块的夹具

2026-11-05 把剩下的大文件按同一张表过了一遍，没有第二个"共用长寿命夹具"的同类问题：

| 模块 | 夹具 | 结论 |
| --- | --- | --- |
| `UiThemePaletteChecks` | 收进程级的 `tabs` / `bar` | 天生要，比色就比那两个控件 |
| `UiInstanceGroupingChecks` | 收进程级的 `ui` / `instanceList` | 天生要，量的是主窗口那棵树 |
| `UiInstanceOverridesChecks` | 自建 25 个对话框，用完即弃 | 没有共享夹具问题 |
| `UiConfigEditorChecks` | 自建 3 个对话框 | 同上 |
| `UiSkinMeshChecks` | 自建控件树 | 同上 |
| mw_check `MwInstanceGroupsChecks` | 按场景自建 15 扇主窗口 | 同上 |
| mw_check `InstanceUpgradeChecks` | 纯事务/目录夹具 | 同上 |

再拆这些文件的理由应该是编译时间或领域边界，不是夹具生命周期；判断"这个模块要不要整份 `ui`"时按这张表，不按文件大小切。

## 新增测试约定

- 小专项实现放独立 `.cpp`，`.hpp` 只声明入口；不要把整段实现 include 进巨型 `main.cpp`。用例放进所属模块，入口只负责夹具、调用顺序和退出状态。
- 每个专项登记到 CTest，设置标签、独立超时和运行环境：用 `wtfml_add_test(NAME ... COMMAND <独立目标>)` 同步登记构建归属，漏归属或重复归属明确失败。
- 每项使用独立临时目录、临时端口；夹具失败要明确报错，不能让依赖该夹具的断言假绿。
- 性能规则：功能断言和性能基线分开。offscreen、调试器、未优化（`-O0`）构建的耗时不代表产品性能；构建/启动/CI 的基线与量法见 [PERFORMANCE.md](PERFORMANCE.md)。规模测量用环境变量开关，只打印、不设耗时断言。
- 已知崩溃保留首次证据、独立复现，不以"重跑直到绿"为验收规则。
- 新增的 `tests/` 文件要能被 `tools/test-impact.ps1` 的映射认出（否则 `wtfml.test-modules` 会报"没有领域归属"）；新增 `assets/` 或工具脚本要补映射。
- 新加 `QLineEdit` / `QSpinBox` / 可编辑下拉框：短值调 `InputSizing::shortValue`，长值调 `InputSizing::longValue` 写理由（`ui/InputSizing.hpp`）。`wtfml.input-widths` 在 1280×800 与最小窗口（`MainWindowPageLayout::kMainWindowMinWidth/Height`，900×590）下扫主窗口各页和覆盖层页面，没登记又宽过 420 px 的输入框判失败；`WTFML_INPUT_WIDTH_DUMP=1` 打印每个输入框，`WTFML_INPUT_WIDTH_SHOTS=<目录>` 出前后样张（加 `WTFML_INPUT_WIDTH_BEFORE=1` 撤掉短值登记再拍）。限制：覆盖不到 `PresetManagePage`（`ui_check` 夹具未链接）和表格单元格内的编辑器。
- 新写 `QDialog` / `ChromeDialog` / `GlassPopover` 子类，或新开一页 `showPage()`：登记进 `tests/ui_check/surface-registry.txt`，让 `wtfml.surface-sweep` 真的扫到。`tools/check-surface-registry.ps1`（CTest `wtfml.surface-registry`，CI 的 repo-rules 也跑）核对两边：代码里的对话框子类、就地建的裸对话框、`overlay->showPage()` 目标都必须在清单里；清单里的表面键也必须在 sweep 里 `record()` 过。判据宁可多报不漏报，确需豁免的在类声明行、构造行或 `showPage()` 调用行尾写 `// surface-registry-ignore: 理由`（理由不能是空的）。

### 表面巡检（sweep）

`wtfml.surface-sweep`（`tests/ui_check/modules/UiSurfaceSweepCheck.cpp`，判据在 `SurfaceScan.hpp`）把 96 个界面表面逐个打开，扫「还长着 Qt 默认样子」的控件。它现在是守卫：计入的残留不为 0 就退出码 1，输出逐条点名表面键、类别、控件类名和 objectName。

例外不另开白名单文件，只写在 `SurfaceScan.hpp` 的 `checkBoxException()`：键是控件 objectName，值是一句理由，扫到记 `exempt`、不计数。加例外等于承认这里就要原生样子，人看过再写。

单跑实测 8–21 秒（验证构建、offscreen，多趟），CTest 超时 180 秒。出样张用 `WTFML_SURFACE_SHOTS=<目录>`；`WTFML_SURFACE_THEME=emerald|ocean` 换主题（判据不随主题变，`WTFML_SURFACE_DEBUG_VIEWS=1` 打印每个视图挂的代理）。

### 夹具与探针技巧

- **假外网**：`tests/mw_check/modules/SupportFakes.hpp` 的 `FakeNetwork` 按请求行路径（去掉查询串）路由，`hits` / `paths` 记每发的次数与原文，`notFound` + `unrouted` 记没演到的请求并回 404。构造 `MainWindow` 的入口（`wtfml.main-window`、`wtfml.workspace`）在构造前把五条 `Launcher/*Base` 写进沙盒 INI，收尾断言 `notFound == 0`，断言只看次数、不看响应次序。新增的网络入口没被演到会直接变红。曾靠它抓到 `sessionServerBase()` 回 `…/session` 而产品拿到的是完整档案前缀，皮肤档案请求一直被 404、界面只是"没有皮肤"、测试却全绿；现夹具为 `…/session/minecraft/profile`。
- **默认根隔离**：`mw_check/main.cpp` 与 `modules/ModuleMain.cpp` 在测试控制器初始化前建每进程 `QTemporaryDir`，注入 `wtfml.testDefaultMinecraftDir` 应用属性并重定向默认 INI；正式入口不设该属性，便携式目录规则不变。断点续传子进程同样先建独立设置和默认数据根，工作目录由父进程指定。工作区另起真实 `sandbox-probe` 子进程核对父子设置互不可见、退出后目录回收。
- **浮层防漏**：`ui_check` 主入口与 `modules/ModuleMain.cpp` 在 `QApplication` 上挂 `PopupWindowGuard`（`modules/Support.hpp`），本进程每块 `Qt::Popup` / `Qt::ToolTip` 顶层窗一上屏就验无边框 + 半透明，收尾记一条断言。白名单只有一条：没调过 `Theme::apply()` 的进程里 Qt 自建的 QMenu、下拉弹层容器、补全列表。新浮层在构造里调 `Popups::preparePopup()` 即可通过。
- **Windows 句柄占用**：用允许读写共享、不允许删除共享的原生句柄占住文件，调用生产流程。`ModpackCompletionTests.cpp` 的 ZG9 占住 `wtfml.importing`，断言最后删除失败时返回失败、报告退回未完成、启动门禁仍生效；升级夹具占住测试 JAR，验证拒绝发布、原 JAR 逐字节一致、完整快照存在、错误包含占用 PID。
- **后台作业**：用信号量控制真实后台读取，验证输入可响应、单读取与最新待选合并、旧结果丢弃、页面销毁与换根；等待有界（信号量最多 3 秒，测试自身最多 4 秒），不等全局线程池，删除页面后等实际加载结束再断言，避免提前销毁掩盖迟到回调。
- **提示探针**：实现在 mw fixture 的 `PromptProbe.cpp`，扫描实际提示并按代次有界等待（20 秒），命中后收尾；不要只找"第一个打开的覆盖层"（会漏另一窗口的提示），也不要固定 0/300 ms 两次尝试。`ui_check` 的 `dismissNextInline()` 按"票号"走：只有最近一次调用排下的两发有效，兑现一次即作废，否则 300 ms 兜底会把下一条提示当成自己的答案。
- **`ui_check` 里的连接与定时器**：凡是 `[&]` 捕获栈变量的 `connect` / 定时器，上下文对象不许是活得比它久的对象（`&dialog`、`&window`、`overlay`、`page`），要用和被捕获变量同一作用域的栈上 `QObject scopeGuard`。违反时产生的悬空 lambda 会在 PageHeap 下当场崩，正常入口下只是偶发 `c0000374`（见「历史崩溃复核」）。
- **合成鼠标点实例行**：`QTreeView` 在 `AnimatingState` 会把按下整条吞掉（见 `ui/RowActivation.hpp` 文件头），`itemClicked` 一发都不发；用和 T10 相同的信号路 `clickTreeRow`，坐标与"不在三颗按钮上"由前提断言钉住。
- **DPAPI**：Windows/MinGW 的 `wtfml.token-protection-fault` 只让该测试进程的导入符号模拟 DPAPI 失败，不替换系统 DLL、不注入其它进程，正式程序没有加密故障开关；凭据测试强制走 DPAPI 后备，不创建真实凭据管理器 target。
- **凭据日志回归**：`ci-core -Modules core-launch,core-accounts` 验证按值遮盖、所有分块边界、逐字符输出、EOF 残片、重叠凭据和报告导出；`ci-integration -Modules window` 含真实子进程 stdout/stderr 分块测试和在线 BAT 导出检查。只用合成凭据和隔离身份。游戏/模组自行写的日志及系统进程参数不在启动器输出过滤的保护范围。

## 测试入口自身的回归

`./tools/test-runner-check.ps1`（CTest `wtfml.test-runner`）用独立的带空格/中文路径构造成功、断言失败、超时和空测试集四种情况，验证退出码、环境恢复、独立报告和 JUnit 生成；另覆盖 `RunnerMode` 选择、构建目标去重、空选择拒绝。故意失败的内部样例不混入普通 JUnit，原始输出在 `build/test-runner-selfchecks/<随机号>/`，只有入口自身判断失败才使本项变红。

设置沙盒另有一条硬判据（`./tools/check-test-modules.ps1`，CTest `wtfml.test-modules`）：`tests/` 下每个带 `main()` 的进程——包括 `ui_check_*` / `mw_check_*` 那些模块可执行文件——都必须 `QSettings::setDefaultFormat(IniFormat)` + `setPath(IniFormat, UserScope, <本进程临时目录>)`，并在隔离之后调用 `requireIsolatedSettings(<沙盒根>, <名字>)`（`tests/support/SettingsIsolation.hpp`）。不隔离就当场打印路径并以 97 退出，扫描不到这四样东西则 `wtfml.test-modules` 直接红。

只写 `setPath()` 不算隔离：Windows 上默认格式是 `NativeFormat`，`setPath` 管不到它，`QSettings()` 仍然读写 `HKCU\Software\WTFML\WTFML`。2026-11-13 用户真实设置里的 `Theme/BackdropTint` / `Theme/BackdropBlur` 被写成 0，就是自动化在**真**设置上点到了设置页那两根强度滑块。起产品 exe 的自动化一律 `--acceptance-profile <临时目录>`（`tools/dev-run.ps1 -AcceptanceProfile` 会建好这一份）：设置落在 `<目录>/settings`、默认游戏目录落在 `<目录>/game`、智能体数据目录落在 `<目录>/agent`，三份都在这一份里，跑完整个删掉；确需动真设置的脚本要在头部写一行 `# settings-isolation: real-settings（为什么）` 备案，`check-test-modules.ps1` 认这一行。

## 智能体（Agent）测试套件

独立核心域和两个专项 UI / 集成模块，不联网、不写真实用户目录：

```powershell
./build/verification-mingw13/tests/verify_agent.exe                    # 核心：SSE/适配/会话/加密/凭据/上下文/Loop/技能/Markdown/文件工具
./build/verification-mingw13/tests/ui_check/ui_check_agent.exe         # 智能体页 + 设置页（offscreen）
./build/verification-mingw13/tests/mw_check/mw_check_agent.exe         # 真 MainWindow + 假模型客户端
```

CTest 名：`wtfml.core-agent`、`wtfml.ui-agent`、`wtfml.main-window-agent`（`-Suite quick` 与 `window` 都包含）。每个测试用独立 `QTemporaryDir`，`AgentPaths` 通过 `QCoreApplication::setProperty("wtfml.testAgentDataDir", <临时目录>)` 隔离，QSettings 重定向到同一沙盒。

- Stage 1A 覆盖：SSE、适配、会话、加密、凭据、上下文、Loop、技能、Markdown。
- Stage 1B 覆盖：路径攻击矩阵与句柄级校验、外发边界表、审批矩阵、artifact 分离、实例锁、维护备份生命周期、文件工具引擎、Loop 工具调用/审批/拒绝回灌、UI 工具卡/审批卡；本地假服务器验证 400 错误体保留，工具线上名不含点号。
- Stage 2B 覆盖：诊断字段白名单投影、19 个启动器工具的分派与审批、工作流计划校验/顺序闸/失败锁写/完成闸、多文件维护备份往返、模式胶囊与工作流状态行、真 MainWindow 里只读模式拒绝写工具。
- Stage 2A 覆盖：MCP 帧与上限、`configHash`/身份变化失效、确认闸与重连确认、逐次审批与异步交付（`Pending` + completion sink）、stdout 垃圾行、崩溃不自动重启。`verify_agent --mcp-fake-server` 是假 MCP 服务器的入口（同一个可执行文件的隐藏模式），客户端用例用它起真子进程。
- Stage 3 覆盖：地址策略（scheme/A-AAAA/私网各档/pinned-IP/content-type/HTML 转文本）、联网工具用假传输验审批与逐跳校验、固定诊断命令的开关/白名单/审批/真跑一条；G3 的真网/代理/证书项在人工清单里。
- Stage 4：`AgentPerformanceTests.cpp` 打印 `[perf]` 基线与 `ui_check_agent` 的 `agent-page-2000-messages` 是长会话回归对照（数字见 `docs/dev/perf-notes/AGENT_STAGE4.md`）；发布前跑 `pwsh tools/check-agent-release.ps1`（明文密钥/假端点/`command.run`/审批闸/危险开关）。
- junction 夹具建不出来时输出 `SKIP`，不计入通过。
- 计数见 [TEST_RUN_HISTORY.md](../archive/TEST_RUN_HISTORY.md)；待人工测试项见 [Stage 1A](../features/AGENT_STAGE1A.md) / [Stage 1B](../features/AGENT_STAGE1B.md)。G1B 的脱敏/凭据文件/artifact 三项已在代码里修复，仍需复测 R1–R4、R6、R7 与 W4/W5、A1、P4、P5；网络错误单独延后。记录见 [ACCEPTANCE_LOG.md](../archive/ACCEPTANCE_LOG.md)。

## network-diag 在本机怎么跑

`WTFMLNetworkDiag` 是带窗口的诊断工具：不带参数运行会开窗等交互，看起来像停住。验证用 ctest 里同样的命令：

```powershell
$env:QT_QPA_PLATFORM='offscreen'
build-audit/tools/network_diag/WTFMLNetworkDiag.exe --self-test
```

2026-11-05 本机实测 48 条全过、约 19 秒、退出码 0，与 CI 一致，所以本机跑全量不需要排除 `network-diag`。

## 设置页验收截图（offscreen，不弹窗）

`ui_check_settings_shots` 在 offscreen 下用 `QWidget::grab()` 出设置页截图：暮色雪山 / 冰川碧湖 × 最小窗口 900×590 / 1280×800 × 4 个页签，另有「个性化」页展开「高级」和表单收窄到 560 的 stack 态；两档尺寸各另拍一张拉开的「配色」下拉弹层（`look-preset-popup-*`），并印出下拉框宽度与最长的预设名宽度，看四字名放不放得下。

```powershell
cmake --build build/verification-mingw13 --target ui_check_settings_shots
$env:QT_QPA_PLATFORM = 'offscreen'
./build/verification-mingw13/tests/ui_check/ui_check_settings_shots.exe <输出目录>
```

## 人工验收探针（不进 CTest）

需要真机环境，故意不登记到 CTest；常规回归以临时目录夹具为主。

```powershell
# 真实产品双进程：第二个窗口必须被拒绝（需要交互式 Windows 桌面）
python tools/acceptance_directory_boundary.py `
  --exe build/verify/WTFMinecraftLauncher.exe `
  --qt-bin D:/Qt/6.10.1/mingw_64/bin

# 大实例复制 / 文件占用 / 升级取消 / 指定阶段外部终止 + 重启恢复
# --kill-stage 让探针停在生产事务的 publish 或 verify 阶段，再由脚本 TerminateProcess；
# 不用固定 sleep（快盘会跑完、慢盘可能还没到那一阶段）。
# --json-out 记录规模、每阶段耗时、卷类型和最长等待；恢复后脚本还会重跑一次完整升级。
# 把 --work-dir 指到慢盘，并用真实杀毒软件实时扫描，才是完整慢盘验收。
python tools/acceptance_large_instance.py `
  --verify-exe build/verify/tests/verify_instances.exe `
  --qt-bin D:/Qt/6.10.1/mingw_64/bin `
  --work-dir E:/wtfml-acceptance --payload-mb 1024 --files 6000 `
  --kill-stage publish --json-out build/acceptance-large.json
```

`acceptance_large_instance.py` 要求空工作目录，失败/异常保留现场，载荷用 SHA-256 校验；恢复探针用生产 Publish 回调 `_Exit(86)`，要求 marker 和恢复 count>0。`verify_instances` 的 `--large-copy` / `--large-upgrade-cancel` / `--large-upgrade-hold` / `--large-upgrade-complete` / `--upgrade-recover` 是它用的探针入口；`--large-upgrade-hold` 先打印 `UPGRADE_STAGE`，到指定阶段再打印 `UPGRADE_HOLD` 并停住，父进程确认这一行后才杀。直接运行 `verify_instances` 不带参数只跑常规回归。

真实升级 / 回滚 / 主菜单探针（`verify_instances`）：

```powershell
verify_instances --upgrade-copy <gameDir> <sourceId> <newId> <targetCoreId>
verify_instances --upgrade-inplace <gameDir> <instanceId> <targetCoreId>
verify_instances --upgrade-rollback <gameDir> <instanceId> <transactionId>
verify_instances --upgrade-inplace-crash <gameDir> <instanceId> <targetCoreId>
verify_instances --upgrade-recover <gameDir>
verify_instances --launch-main-menu <gameDir> <versionId> <javaPath> <xmx> <timeoutSec> <log>
```

`--launch-main-menu` 先看声音/资源强标记，否则要求游戏窗口出现且进程继续存活 20 秒；它先 `prepareNatives()` 再 `checkPreLaunch()`，和产品启动路径一致，参数、Java、natives、进程管理走生产 `LauncherManager`，但不是 UI 点击路径。

P1 独立验收工具（先加载本机 Qt/MinGW 运行库路径，证据目录必须是新的绝对路径，不覆盖原有游戏或证据，都有独立截止，不需要真实账号）：

```powershell
# 构建原生验收窗口（不进入默认构建目标）
cmake --build build/verification-mingw13 --target wtfml_desktop_acceptance --parallel 2
pwsh -NoProfile -File tools/acceptance_desktop.ps1 -Extended -Root build/new-native-ui-run
pwsh -NoProfile -File tools/acceptance_desktop.ps1 -LogStress -Root build/new-native-log-run

# 真实系统键盘事件观察；90 秒后自行退出，按 Tab/Shift-Tab，产出 input-report.json 和截图
build/verification-mingw13/tests/mw_check/wtfml_desktop_acceptance.exe --root D:/new-owned-evidence/keyboard --manual-input

# 真实产品入口/同目录互斥/日志并发；不改真实注册表配置
python tools/acceptance_directory_boundary.py --exe build/release-mingw13/WTFMinecraftLauncher.exe --root D:/new-owned-evidence/startup
# --in-place 用于已经部署好运行库的同一目录；省略时会创建三个独立安装位置

# 公网样本经生产下载引擎下载，另验大小与 SHA-1；最多约 180 秒
python tools/acceptance_public_downloads.py --exe build/release-mingw13/tools/network_diag/WTFMLNetworkDiag.exe --root D:/new-owned-evidence/network

# 指定文本样本审计（只扫描各目录顶层；不会递归遍历游戏）
python tools/audit_login_outputs.py --self-test --root build/new-native-log-run --report build/new-output-audit.json

# 下载恢复现场（拒绝覆盖已有目录，每程序 180 秒截止）
pwsh -NoProfile -File tools/acceptance_download_recovery.ps1 ...
```

- `-Extended` 是三主题 / 两尺寸 / 十场景的受控几何与 Tab 事件测试，动效用 `Anim::setEnabled()` 拨，不碰系统设置。
- `-LogStress` 用真实会话控制器与原生日志窗口，两路各 25000 行，检查上限、分离、过滤、导出脱敏及心跳，输入是受控输出。
- `--manual-input` 不生成键盘事件，记录收到的原生事件、焦点和截图；没有 Tab 或焦点失败返回非零。
- 游戏主菜单专项沿用隔离 Fabric 1.21.11 夹具：`--game-root` 的测试目录要含 `p1-game-acceptance.json`（格式 `wtfml-isolated-game/1`）并提供 `--java-path`；只创建私有 DesktopTest 离线配置，正常退出记录 `game-exit.json`，300 秒截止。进程退出 0 不单独证明主菜单，要另行观察。不要给真实游戏目录添加测试标记。
- 大实例整套脚本 `tools/acceptance_p0_*.py`、`tools/prepare-p0-vhd.ps1` 用于 P0 磁盘/权限/事务矩阵，记录见 [ACCEPTANCE_LOG.md](../archive/ACCEPTANCE_LOG.md)。
- 失焦磨砂真机判定：`pwsh tools/defocus-glass.ps1 -Backdrop acrylic -Theme dark`（`-Rollback` 做系统默认退色的对照）。起自己的实例摆在自己的红蓝棋盘上，前台只在本脚本自己的窗口之间交接，比「聚焦 vs 失焦」「失焦时换背后颜色」两组整窗差；`-Theme` 临时写 HKCU 的 `Theme/Preset`，结束还原。退出码 0 = 失焦前后一样，1 = 退色 / 变了，2 = 没采到。需要交互式桌面，会短暂占前台。
- UI 压力入口：`./tools/stress-ui-offscreen.ps1 -Runs 50 -StopOnFailure` 逐趟保存日志，首失败保留在 `build/ui-stress/<时间>/` 并返回非零。
- 单独定位更新中心：构建 `mw_check` 后运行 `mw_check.exe update-center-check`（约 2 秒，须设独立进程截止）。

## 已知问题与经验

### 2026-10-20 CI 失败核对

- 主程序冷构建到约 85% / 165 个对象时触发单阶段 900 秒上限（退出 124）。解法是 `ci-object-batches.ps1` 分批构建，保持每阶段 900 秒保护，不靠无限延长超时。
- 网络诊断的 `expired node is reprobed once and missing ranges complete safely` 曾在 CI 失败。夹具现在只按请求行目标路径路由，不在请求头/查询串里匹配地址，完整请求只响应一次；断言仍是严格的两次调度探测，次数不符但下载成功时也输出计数、结果与引擎日志。旧日志没有实际计数，无法确定 CI 那次失败的唯一根因。

### 历史崩溃复核

- **core / verify 的 WER 转储**：ZJ3 夹具用 `std::thread` 调 QtCore，已换 `QThread`。
- **ui_check offscreen `c0000374`**：把 `QAccessible::setActive(false)` 改成 `true` 后连续 10/10 崩在 `InstanceSettingsDialog::~InstanceSettingsDialog()` 的 `QHash<QString, QString>` 释放；保持 `false` 连续 50/50 通过。offscreen 插件下 A11y 恒开，而产品 windows 插件只有读屏软件连着才开，所以这是验证环境隔离的问题。
- **PageHeap 下 `m_fileDirs` use-after-free（已定位并修掉，2026-10-24）**：`gflags /p /enable ui_check.exe /full` 下完整 `ui_check` 崩在 `InstanceSettingsDialog` 析构释放 `m_fileDirs`。根因在 `UiContentPagesChecks.cpp` 的 U10b 段：

  ```cpp
  QObject::connect(&dialog, &Dlg::openInstanceFolderRequested, &dialog,
                   [&lastPath, &opens](const QString &path) { lastPath = path; ++opens; });
  ```

  上下文 `&dialog` 活到整个 U10 块结束，`lastPath` / `opens` 是那一节栈上的变量，节一结束就没了而连接还在。产品把每页的 `openFolderRequested` 信号接信号转成 `openInstanceFolderRequested`，U10e 点一下模组页"打开文件夹"就回到悬空 lambda，`lastPath = path` 提前释放了旧值（"游戏目录"那条路径）。正常入口下只是悄悄提前释放一块数据，这就是偶发 `c0000374` 的另一半；PageHeap 把它变成当场访问违规。修法：连接上下文换成同作用域的栈上 `QObject scopeGuard`（规则见「夹具与探针技巧」）。
- 已排除的假设：`ScrollBarStyler` 在 `QEvent::Show` 里同步 `QScrollBar::setStyleSheet()` 重入 style polish。同形探针在 full PageHeap 下跑 500 轮、0 毫秒延后版再跑 500 轮对照均无访问违规，真实入口 PageHeap 6/6、正常入口 50/50 也没事，所以保持同步实现（延后会让新滚动条晚一个事件循环才有样式）。
- 复验：PageHeap 下完整 `ui_check` 连跑 6 趟全部跑完，只剩 Z8"两万行点一下 ≤ 250 ms"会红（252–300 ms，正常入口 59–74 ms，是 PageHeap 的分配开销）；关掉 PageHeap 后正常入口 50/50 通过。诊断结束必须 `gflags /p /disable ui_check.exe`，PageHeap 不能留在机器上（它让正常入口的 Z8 假红，也拖慢一切）。

### Windows 占用（实例升级）

升级替换 JAR 夹具曾有 4 条失败，不是判据错误：Windows Restart Manager 查到持有测试 JAR 的编辑器进程，原实例目录改名被拒（错误 5）。成功夹具改到用户缓存下独立 `QTemporaryDir`，失败仍保留并打印路径。产品只在保留原目录失败时动态查询 Restart Manager，根目录及 `mods` 目录最多 128 个文件、最多 32 个占用进程，不递归扫描存档、不关闭进程；未知/过多/查询不可用时保留原始 Windows 错误。

### 长路径

- 本机 `LongPathsEnabled=0`，不修改注册表冒充支持。`LongPathTests`（core-instances，仅 Windows 执行）验证超过 260 字符的模组/世界操作、真实独占句柄占用失败，以及超过 600 字符目标的 ZIP 整包/子树解压、打包/条目读取/批量元数据/GZip，恶意条目不越界。PowerShell 5.1 后端普通长路径失败、扩展路径成功；ZIP 拼接要统一物理分隔符（正斜杠会导致解压失败）。
- 安装 / 外部 Java（2026-11-03 探针）：`CreateFileW` / `GetFileAttributesW` 带 `\\?\` 前缀能打开超过 260 字符的路径；`CreateProcessW` 起超过 260 字符的 exe，裸路径、加 `\\?\`（161 / 206）、只给命令行三种写法全部失败；`SetCurrentDirectoryW("\\?\…")` 失败（206），相对路径起进程也不行；该卷没有 8.3 短名（`GetShortPathNameW` 原样返回），"换成短名再起"没有退路。`longPathAware` 清单单独不起作用。
- 因此产品只做一件事：`src/launcher/ProcessPathLimit`（纯判据，`>=260` 个字符）在 `QProcess::FailedToStart` 之后补一句"哪条路径、多少个字符、上限 260、挪到浅目录"，覆盖游戏启动（程序 / `--gameDir` 工作目录）、服务端启动、加载器安装器、服务端安装器四处。不做预检拦截：换一台开了长路径的机器，超长路径也许起得来，预检会误杀。判据边界（259 / 260、挑最长那条、完整路径、本机分隔符）由 core-launch 的 `ProcessPathLimitTests`（ZPL 一节）钉住。

### 核心套件不下载 Java（2026-11-04）

`verify.exe` 的「E. Java 运行时选择」原来直接调 `ensureJavaForVersion()`，本机没有对应主版本就去 Mojang/Adoptium 下几百 MB 运行时，CI 慢网络曾跑满 900 秒上限、整个 `ci-core` 判死（2026-11-03）。现在默认只在本机找，开关见「环境变量」。同一处 `runProcess()` 的 5 秒超时会 `kill()` 并等待收尸（以前超时直接返回，子进程还活着，Qt 打 `QProcess: Destroyed while process ... is still running.`）。

## 未覆盖

| 项 | 状态 |
| --- | --- |
| 多 DPI（100% / 125% / 150%）、透明背景、动效逐帧、真实滚轮/光标位置 | offscreen 不覆盖；150% 真屏巡检已做（见流水），100% / 125% 待补，见 [ROADMAP](../ROADMAP.md) |
| 读屏软件 / 真实 A11y | 未验收；offscreen 下 A11y 恒开，与产品不同 |
| 严格断网 | `full` 不是严格断网测试：主窗口历史用例会触发后台 Mojang/镜像清单请求，需要改成显式夹具或拦截 |
| 真代理链路、低速服务器矩阵 | 只有 `DownloadSpeedLimitTests` / `DownloadProxyTests` 在本地夹具上通过 |
| 物理慢盘 + 杀毒实时扫描下的大实例矩阵 | 只在普通 NTFS / NVMe 上跑过，需在发布机复跑 |
| 磁盘写满、断电、强制结束进程、任意外部写入者 | 世界导入空间预检与 `level.dat` 指纹检查不是事务锁，不覆盖这些 |
| 真实 Java 安装、进世界、多 Java 版本、多模组组合 | 主菜单验收用的是干净副本 |
| 并行测试 | 仅三个已隔离模块，其余串行；逐模块隔离未完成 |
| 内存诊断工具链 | 未建 |
| 手动实网探针（`icon-live`、`cf-resolve-live`、`resource-live`、`java-self-managed`、`multimc-import-live` 等） | 不在 CTest 里，`full` 不包含 |
