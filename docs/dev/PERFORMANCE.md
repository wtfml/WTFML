# 性能基线与测量

给要量耗时、或要判断"这次改动有没有变快"的人看：每类性能指标用什么命令量、现行基线数字是多少、历次优化前后的对照。功能行为的约定在 `FEATURES.md`，验证工程的约定在 [TESTING.md](TESTING.md)。

规则：功能断言和性能基线分开。验证工程（`verify*` / `mw_check` / `ui_check`）按设计是 `-O0`、offscreen，它们的耗时不进基线；基线只用 `Release` 的产品构建或测量专用构建（探针构建 `WTFML_PERF_PROBE`、下载基准 `WTFML_PERF_DOWNLOAD`）。每条数字标注日期、构建档位和跑数；不同机器、并行度、提交的数字不能横向比较。

## 怎么量

| 要量什么 | 命令 | 产出 |
| --- | --- | --- |
| 构建耗时（冷构建、增量、重链） | `pwsh -File tools/perf-build.ps1 -Jobs 8`（加 `-ProductOnly` 只编产品） | `build/perf-report-<时间戳>/`：原始日志、`baseline.json`、`summary.md` |
| 专用测试运行器重链 | `pwsh -File tools/perf-test-links.ps1 -Jobs 6 -TimeoutSeconds 300` | `build/perf-test-links-<时间戳>/summary.json` |
| 启动与就绪（真产品 exe） | `pwsh -File tools/perf-startup.ps1 -Runs 6 -Instances 0` | `startup.json`、`summary.md` |
| 界面交互（探针构建） | `pwsh -File tools/perf-startup.ps1 -Probe -Exe build/perf-probe/WTFMinecraftLauncher.exe -Runs 3 -Instances 40` | `run-NN-probe.json` |
| 主线程卡顿归属 | `tools/symbolize-stalls.ps1`，见 [MAIN_THREAD_STALLS.md](perf-notes/MAIN_THREAD_STALLS.md) | 带函数名/行号的栈 |
| 并发下载（本机 HTTP） | `pwsh -File tools/perf-download.ps1` | `build/perf-download-<时间戳>/` |
| CI 分片与缓存 | `pwsh -File tools/perf-ci.ps1 -Limit 8` | `ci.json` |
| 发布体积 | `pwsh -File tools/prepare-release-copy.ps1 -BuildDir <静态构建目录>`；`tools/measure-release-size.ps1` | 见 [RELEASE_SIZE.md](perf-notes/RELEASE_SIZE.md) |

`perf-startup.ps1` 开头检查有没有启动器在跑，有就拒绝。每次运行都在 `<报告目录>/profile` 建一份验收配置，用 `--acceptance-profile <目录>` 起 exe：设置、游戏目录（夹具造在它的 `game/versions`）、智能体数据都在这一份里，跑完整个删掉，不碰用户注册表（`startup.json` 的 `acceptanceProfile`）。`-InstanceMeta` 给每个实例多写一份 `wtfml.meta`；默认夹具没有它，两种夹具的数字不能横向比。`-Probe` 另带首帧守卫，见「界面交互」那一节。

## 基线速查表

下表是各类指标当前能引用的最新数字，括号是日期，细节在对应章节。

| 指标 | 数值 | 条件 | 章节 |
| --- | --- | --- | --- |
| 冷构建（带 38 个验证目标） | 449.7 s（10-25，`-j8`）；2013 s ≈ 33.5 min（11-05，`-j4`，PCH + 全部测试） | 全新目录 | 构建耗时 |
| 冷构建（只编产品） | 148.7 s（10-25，`-j8`）；PCH 打开后 75 s（11-01，`-j12`） | 全新目录 | 构建耗时 |
| 增量：改一个叶子 .cpp | 95.3 s（带测试）/ 5.2 s（只编产品） | 10-25 | 构建耗时 |
| 窗口可见（visible） | 764 ms（空目录）/ 851 ms（40 实例）；200 个带 meta 实例 **383.4 ms**（10-31，探针构建，去掉启动路径上的回退字形之后） | Release `-O3` | 启动与就绪 |
| 首次进设置页 | 18.5 ms（原 110.9 ms） | 探针构建，10-25 | 界面交互 |
| 任务中心单条更新 | 1.8 ms（原 61 ms） | 200 条任务，10-25 | 界面交互 |
| 1000 条预设搜索 | 0.5–0.6 ms（原 5.8–6.2 ms） | 10-25 | 界面交互 |
| 1000 份预设冷读 / 热读 / 下次启动 | 0.40 s / 11–12 ms / 18.4 ms | 11-01、11-02 | [PRESET_LIST_IO.md](perf-notes/PRESET_LIST_IO.md) |
| 1000 行文件列表首绘 / 重列 | 285.7 ms / 311.9 ms（先排后挂） | 10-31，offscreen | [LIST_PAINT.md](perf-notes/LIST_PAINT.md) |
| 并发下载（loopback，400 × 256K） | 约 146 MiB/s，6–24 路无差别；48 路 105.7 MiB/s | 10-25 | 并发下载 |
| 静态单文件发布体积 | 73,531,415 → 43,288,540 字节（去符号，−41.1%） | 11-01 | 发布体积 |

## 环境

| 项 | 值 |
| --- | --- |
| 机器 | Intel Core Ultra 9 275HX / 31.4 GB / Windows 11 家庭版 中文版 26200 |
| 编译器 | MinGW 13.1.0（`D:/Qt/Qt/Tools/mingw1310_64`）；静态发布用 `C:/mingw64`（GCC 15.2） |
| Qt | 6.10.1（`D:/Qt/Qt/6.10.1/mingw_64`）；静态版 `D:/Qt/static-6.10.1/install` |
| 构建 | MinGW Makefiles / `CMAKE_BUILD_TYPE=Release` / 并行 8（各节另注） |
| 提交 | 2026-10-25 的首批数字在 `1473e52` + 未提交工作区（`tools/perf-*.ps1`、`src/perf/`、`CMakeLists.txt` 的探针开关）上量；之后每节自注日期与构建 |

## 构建耗时

```powershell
# 带 38 个验证目标（CI 和日常验证就是这么编的）
pwsh -File tools/perf-build.ps1 -Jobs 8
# 只编产品，用来分清产品/验证工程各占多少
pwsh -File tools/perf-build.ps1 -Jobs 8 -ProductOnly
```

脚本用全新构建目录（非空目录默认拒绝：复用目录量出来的"冷构建"只是空转）。

### 2026-10-25（`-Jobs 8`，同一台机器、同一次会话）

带测试（`build/perf-report-20261025-185304/`，690 个对象、1.55 GB）：

| 步骤 | 秒 | 重编 TU | 链接 exe | 链接静态库 |
| --- | ---: | ---: | ---: | ---: |
| configure | 3.7 | 0 | 0 | 0 |
| cold-full（全新目录） | 449.7 | 690 | 35 | 9 |
| incremental-noop | 9.7 | 0 | 0 | 0 |
| leaf-cpp（`src/util/SystemMemory.cpp`） | 95.3 | 2 | 34 | 1 |
| fanout-header（`src/util/AppPaths.hpp`） | 409.5 | 357 | 35 | 8 |
| big-tu（`src/MainWindow.cpp`） | 102.3 | 2 | 13 | 1 |
| link-only（删主程序 exe 后只链接） | 3.2 | 0 | 1 | 0 |

只编产品（`build/perf-report-20261025-191453/`，191 个对象）：

| 步骤 | 秒 | 重编 TU | 链接 exe | 链接静态库 |
| --- | ---: | ---: | ---: | ---: |
| configure | 2.2 | 0 | 0 | 0 |
| cold-full | 148.7 | 191 | 2 | 1 |
| incremental-noop | 1.5 | 0 | 0 | 0 |
| leaf-cpp | 5.2 | 1 | 1 | 0 |
| fanout-header | 62.5 | 61 | 2 | 1 |
| big-tu | 33.9 | 2 | 1 | 0 |
| link-only | 3.1 | 0 | 1 | 0 |

对照读出的事：

1. 带测试的增量耗时大头是重链 34 个验证 exe，不是编译：同一个叶子 .cpp，带测试 95.3 s、只编产品 5.2 s；noop 9.7 s 对 1.5 s。单次链接约 2–3 s，乘上目标数就是全部。这笔钱买的是 38 个独立进程的隔离；要省就得减少"产品库一变就全体重链"的目标数。
2. `AppPaths.hpp` 是高扇出头：带测试 357 个 TU / 409.5 s，只编产品 61 个 TU / 62.5 s。它的绝大多数消费者是验证工程，产品这边仍有 61 个。
3. `MainWindow.cpp` 带测试时编两遍（产品一次、验证工程的 mw runtime 一次）：102.3 s 对 33.9 s。
4. 冷构建 449.7 s 里产品占 148.7 s，其余约 300 s 是 38 个验证目标。

### 2026-10-26 专用测试运行器热重链

`tools/perf-test-links.ps1 -Jobs 6 -TimeoutSeconds 300`：同一构建目录分别预热，再只触碰共享静态库的时间戳，两种模式都通过同一个汇总目标遍历一次依赖图，记录实际 C++ 编译与链接次数。

| 模式 | C++ 重编 | 专用 exe 重链 | 秒 |
| --- | ---: | ---: | ---: |
| Standalone | 0 | 18 | 50.390 |
| Compact | 0 | 2 | 31.150 |

约减少 38.2%（`build/perf-test-links-20261026-181122/summary.json`）。共享模式保留 18 个独立 CTest 进程，合并的是链接输出；局部选择 Auto 仍独立构建。限制：单次热重链对照，与上面 8 路全工程基线不可直接横比；首次共享构建的预热成本不在表内。

失败记录：175347 预热超时；180124 的逐目标遍历被汇总入口方式替代并停止；180715 因同秒时间戳只重链 1/2 个输出，被检查拒绝。最终脚本在触碰前留出一个 Make 时间刻度，181122 两种模式的零编译/链接数校验均通过。

### 2026-11-05 `build-audit`（Release + `WTFML_BUILD_TESTS=ON`，`--parallel 4`）

- 全热（无改动）：6.7 s。
- 单 TU 增量（`touch src/controllers/BackupController.cpp`）：66.7 s，编译那个 TU 只占几秒，其余是产品与测试目标重新链接。
- 冷构建（全新目录 `build-cold-measure`、同参数、PCH + 全部测试目标）：configure 5.4 s + 构建 2013 s ≈ 33.5 min，产物目录约 2.4 GB。
- 改公共头（`ServerModel.hpp`）触发的整棵重编约 21 分钟（同日一次真实重编，`-j6`）。

### 共享头扇出

口径：数每份 `.d` 依赖文件里有哪些 TU 依赖这份头。这是静态扇出（动这份头要重编多少 TU），不是耗时。要看的是产品那一栏。排前面的十几份头彼此只差几十个 TU，因为它们共享同一个传递底座（工具 / 路径 / 版本显示）。验证那一栏远大于产品栏，因为验证工程有一份公共夹具，夹具引一份头等于全体验证 TU 都引。

**2026-11-05**（`build-audit`，1089 个 TU：产品 271 / 验证 818；仓库内头 272 份；产品栏只数 `WTFMinecraftLauncher.dir` 的 `.d`）：

| 头 | 依赖 TU | 其中产品 |
| --- | ---: | ---: |
| `src/util/AppPaths.hpp` | 470 | 91 |
| `src/download/DownloadSpeedLimit.hpp` | 457 | 79 |
| `src/download/DownloadConcurrency.hpp` | 457 | 79 |
| `src/manage/InstanceFiles.hpp` | 433 | 76 |
| `src/download/NetworkManagerFactory.hpp` | 368 | 77 |
| `src/download/LocalFileReuse.hpp` | 358 | 74 |
| `src/download/DownloadManager.hpp` | 358 | 74 |
| `src/settings/GameWindowSettings.hpp` | 415 | 62 |
| `src/util/ZipUtil.hpp` | 338 | 68 |
| `ui/TaskCenterPanel.hpp` | 310 | 68 |
| `src/settings/EffectiveJavaSettings.hpp` | 331 | 63 |
| `ui/Theme.hpp` | 299 | 60 |
| `src/server/ServerRestart.hpp` | 403 | 59 |
| `ui/MemorySlider.hpp` | 284 | 59 |

与 10-28 相比榜首没变，产品栏从 71 涨到 91，是因为产品 TU 从 218 涨到 271（这份目录里 `Q_OBJECT` 类更多，moc 出的 `moc_*.cpp` 也算 TU，两份目录不完全同源）。`ui/Theme.hpp` 总依赖从 436 降到 299，主要是验证夹具不再整份引它，不是谁删了 include。

**2026-10-28**（`build/verification-mingw13`，1412 个 TU：产品 218 / 验证 1194；头 297 份）：

| TU | 产品 | 验证 | 头 |
| ---: | ---: | ---: | --- |
| 478 | 71 | 407 | `src/util/AppPaths.hpp` |
| 436 | 85 | 351 | `ui/Theme.hpp` |
| 433 | 52 | 381 | `src/download/DownloadSpeedLimit.hpp` |
| 433 | 52 | 381 | `src/download/DownloadConcurrency.hpp` |
| 420 | 62 | 358 | `src/manage/InstanceFiles.hpp` |
| 397 | 45 | 352 | `src/download/LoaderMeta.hpp` |
| 394 | 40 | 354 | `src/download/ResourceCatalog.hpp` |
| 393 | 44 | 349 | `src/manage/VersionDisplay.hpp` |
| 371 | 70 | 301 | `ui/TaskCenterPanel.hpp` |
| 370 | 35 | 335 | `src/settings/GameWindowSettings.hpp` |
| 355 | 50 | 305 | `src/download/NetworkManagerFactory.hpp` |
| 345 | 62 | 283 | `ui/TaskProgressBar.hpp` |
| 345 | 62 | 283 | `ui/SkinViewer.hpp` |
| 345 | 48 | 297 | `src/download/LocalFileReuse.hpp` |
| 345 | 30 | 315 | `src/server/ServerRestart.hpp` |
| 344 | 29 | 315 | `src/download/ServerInstall.hpp` |
| 341 | 61 | 280 | `ui/GroupDialog.hpp` |
| 341 | 46 | 295 | `src/util/ZipUtil.hpp` |
| 341 | 29 | 312 | `src/server/ServerModel.hpp` |
| 340 | 62 | 278 | `ui/MemorySlider.hpp` |

### 共享头扇出的时间成本与预编译头（2026-11-01）

**动一份头要等多久**（验证工程，`touch` 那份头 → `cmake --build build/verification-mingw13 --parallel 12`，每份都在上一次构建完全就绪之后动）：

| 动的头 | 重编 TU | 墙钟 |
| --- | ---: | ---: |
| `src/util/AppPaths.hpp` | 411 | 297 s |
| `ui/Theme.hpp` | 328 | 238 s |
| `ui/MainWindowUI.hpp` | 271 | 218 s |

**单个 TU 的解析成本**（`g++ -fsyntax-only`，与产品同一条命令：`-O3 -std=c++23` + `includes_CXX.rsp`，每份跑 3 次取最低）：

| TU 里只有 | 解析 |
| --- | ---: |
| 空 TU | 66 ms |
| `#include "AppPaths.hpp"` | 66 ms |
| `#include <QString>` + `<QWidget>` | 1427 ms |
| `#include "Theme.hpp"` | 1566 ms |
| `#include "InstanceFilesPage.hpp"` | 1481 ms |
| `#include "MainWindowUI.hpp"` | 1853 ms |

1.4 s 的底座是 Qt 控件头，项目自己的头只在上面加 0.1–0.4 s（`AppPaths.hpp` 不引 Qt，几乎是 0）。所以砍项目头换不来什么，能吃回来的是每个 TU 重复解析同一批 Qt 声明的那 1.4 s。

**预编译头**：`QWidget` 那张 `.gch` 建一次 4.4 s、177,643,858 字节，读回同一个 TU：1427 → 337 ms（`MainWindowUI.hpp` 1853 → 825，`InstanceFilesPage.hpp` 1481 → 402）；空 TU 66 → 335 ms，那 335 ms 是固定读回成本，只有本来要解析 Qt 的 TU 才赚。`<QtWidgets>` 整个模块那张 11.0 s、425,264,619 字节，读回 3445 ms，比不用 PCH 还慢，不要。

**`WTFML_USE_PCH`**（默认 ON，只给主程序）：

```cmake
if(WTFML_USE_PCH)
    target_precompile_headers(WTFMinecraftLauncher PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:<QtWidgets/QWidget>>")
endif()
```

| 全量产品构建（`WTFML_BUILD_TESTS=OFF`、Release、严格警告、全新目录、`--parallel 12`） | TU | 墙钟 |
| --- | ---: | ---: |
| `WTFML_USE_PCH=OFF` | 237 | 95 s |
| `WTFML_USE_PCH=ON` | 238（多的是 `.gch` 本身） | **75 s（−21%）** |

带测试的全量构建（PCH ON、严格警告 + `-Werror`、801 个 TU）378 s，没有 A/B（OFF 那份跑到一半因机器内存吃紧被停，不是构建报错）。测试 TU 不受益：验证工程是几十个独立 target，每个都要自己一份 `.gch`（几十份 × 177 MB），所以只给主程序挂。静态发布那份（`build/static-release`，GCC 15.2 / `C:/mingw64`）也重编通过：231 TU、95 s，`.gch` 190.8 MB。打开 PCH 后 `build/verification-mingw13` 整棵重编通过（226 TU，`--parallel 8`），`ctest --preset verification` 38/38（403.85 s）。

踩过的坑：

- PCH 与使用端的编译选项必须逐字一致，否则 GCC 静默不用它，看起来"没变快"。`-Winvalid-pch` 才看得见；这次是建 `.gch` 时带了 `-O3`、编译 TU 时漏了，警告是 `created and used with differing settings of '-munroll-only-small-loops'`。
- PCH 会兜住漏写的 include（主程序少写 `#include <QWidget>` 也能编过），这份 PCH 只有一条头，影响面到此为止。

限制：CI（sccache）上的效果没量过，也没验证 sccache 对 GCC PCH 的缓存行为；每个 g++ 进程映射那 177 MB，本机 31.4 GB 没碰到边界；每个构建目录多一份 177 MB。证据：`build/tmp-wtfml-pch-{off,on}-build.log`、`build/tmp-wtfml-pch-tests-build.log`、`build/tmp-wtfml-verify-pch-build.log`、`build/tmp-wtfml-pch-static-build.log`（临时文件，不入库）。

## 启动与就绪

```powershell
pwsh -File tools/perf-startup.ps1 -Runs 6 -Instances 0
pwsh -File tools/perf-startup.ps1 -Runs 6 -Instances 40
```

量的是 `build/verification-mingw13/WTFMinecraftLauncher.exe`（Release `-O3`）。每次运行：进程创建 → 标题为 `WTF Minecraft Launcher` 的可见顶层窗口出现（`visibleMs`）→ 该窗口开始应答 `WM_NULL`（`readyMs`，消息循环能转了）→ 之后 3 秒内每 20 ms 探一次，记窗口应答的最长一次（`maxBlockMs`）和超时次数。

验收配置（`--acceptance-profile`）自带的整窗截图（`window-<pid>.png`）在窗口出现后 1.2 s 才拍，编码和写盘放在线程池里：此前它在 500 ms 时于主线程上耗时约 300 ms，`maxBlockMs` 在 2026-11 的报告里约为 300 ms，测到的就是这张截图，不是界面本身。

### 2026-10-25 基线

| 场景 | 次数 | visible 中位 | 范围 | ready 中位 | maxBlock 范围 | 常驻内存 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 空数据目录 | 6 | 764 ms | 728–822 | 840 ms | 1.0–14.7 ms | 136–146 MB |
| 40 个实例 | 6 | 851 ms | 841–871 | 866 ms | 30.9–54.0 ms | 141–146 MB |

证据：`build/perf-runtime-default/`、`build/perf-runtime-40instances/`。两种场景都没有超过 500 ms 的无应答。40 个实例比空目录多约 90 ms，界面卡顿从约 10 ms 抬到 30–54 ms：实例列表是第一处运行期开销。

### 启动优化记录（2026-10-31）

各项是各自的同版 A/B（同一台机器、同一份夹具、背靠背），夹具是 200 个各带 `wtfml.meta` 的实例，探针构建（Release `-O3`、只编产品）；不同项的 exe 不同，数字不能首尾相接。

| 改动 | 改前 → 改后 visible 中位 | maxBlock 中位 | 说明 |
| --- | --- | --- | --- |
| `wtfml.meta` 读侧缓存 | 922.8 → 840.2 ms（范围 904.1–931.5 / 816.9–865.1） | 203.9 → 150.0 ms（202.2–205.2 / 144.0–188.7） | 两轮之间只 stash 掉 `src/manage/VersionManager.hpp/.cpp` 重编；exe md5 `5be401d6` / `d1036ec0` |
| 加载器 / 版本类型一次问清楚（`instanceTraits()`） | 815.6 → 654.6 ms（809.3–821.9 / 640.8–662.8） | 148.0 → 19.8 ms（132.9–163.1 / 19.1–20.9） | 改前 n=2、改后 n=3；exe md5 `ad7d5f51` / `549da091` |
| 实例目录扫描：两趟 readdir 合成一趟 | 656.4 → 623.5 ms（653.0–681.4 / 615.3–625.5） | 范围重叠，不计收益 | 每轮 `scan` 18–23 → 7–10 ms；exe md5 `6959ce8e` / `7ff4cdd0` |
| 主页那颗箭头改画位图 | 670.0 → 383.4 ms（642–722 / 374–391） | 范围重叠（13.1–33.1 / 12.9–15.5） | 少约 287 ms；exe md5 `0bfb054c` / `ad323ad6` |

#### `wtfml.meta` 读侧缓存

缓存把"打不开"换成"stat 一下"；按"在不在 + 修改时刻 + 字节数"失效，写侧（重命名 / 分组 / 图标）写完当场丢掉那一格。`verify` 的 O12 钉住四种失效：外面的改写（改大 / 改小）、写侧改完当场可见、文件被删后回到"没有"。缝只有"同一毫秒里、同样字节数"的外部改写，代码注释里写明接受它。

无 meta 的夹具没有可缓存的东西：同日无 meta 的 200 实例是 788–829 ms / 144–165 ms（`build/perf-instances-200-metacache`，exe md5 `9d709661`）。早先同一版二进制的分段探针（无 meta 夹具）显示一轮刷新的两大块是每行图标（`marks` ≈ 91 ms）与树填充（`fill` ≈ 37 ms）。

#### 加载器 / 版本类型一次问清楚

`marks` 不是画图标：每行要问"是哪个加载器"（实例行标志、默认分组再问一遍、频道判定还要问版本类型），三个问题各自把 `wtfml.meta` 读一遍、再各自把合并过的版本 json 认一遍（扫 `libraries`、展开 `arguments.game`）。现在 `VersionManager::instanceTraits()` 一次返回加载器名、加载器版本、版本类型；meta 读一次，json 只在缺答案时才认，结果记在 `mergedVersionJsons` 里（`MergedJsonEntry`）。`instanceModLoader()` 变成它的一层包装。

分段探针（`TEMP REFRESH-PROBE`，打在 `refreshInstalledVersions()`，量完已删；每进程两轮刷新，单位 ms）：

| 段 | 改前（6 轮） | 改后（8 轮） |
| --- | ---: | ---: |
| `scan`（`installedVersions()` 扫盘） | 17–25 | 18–24 |
| `parts`（`displayParts()` 那圈） | 1–9 | 1–11 |
| `marks`（每行标志 / 分组 / 频道） | 35, 39, 43, 61, 63, 121 | 41, 42, 42, 102（进程第一轮）；1, 1, 2, 2（第二轮） |
| `fill`（`rebuildInstanceTree()`） | 38, 40, 42, 43, 45, 50 | 3–6（每轮） |

进程第一轮刷新仍要 41–102 ms（102 是冷盘那次），付的是"200 份版本 json 各解析一遍"的一次性成本；第二轮起 1–2 ms。改前每一轮都付 35–63 ms。`fill` 一起掉下来，是因为分组那一趟原来对每个实例各自问一次加载器和类型。无 meta 的实例走 json 那半，收益同样是一轮只认一遍，没有 A/B 数字。

验证：`verify_instances` 的 QQ 一段（26 项）钉住三条契约：meta 写了加载器就以 meta 为准、没写才去 json 的 `libraries` 认（含薄 json 从父版本继承）；`clearVersionJsonCache()` 之后必须重新认；外部直接改 `wtfml.meta`，同一会话的下一次问就看得见。复跑 `verify_instances` 1657/1657。

#### 实例目录扫描

`resolveInstance()` 里 `DirListing` 原来先 `entryList("*.jar")` 再 `entryList("*.json")`，是两趟完整目录读取。现在 `InstanceFiles::scanDir()` 一趟 `entryList(QDir::Files)` 取全，按后缀分进两个表；顺序逐字不变（按文件名排），后缀比较不区分大小写（`A.JAR` 改之前就是候选 jar）；目录不存在时 `entryList()` 给空表，原来的 `dir.exists()` 早退一并去掉。分段探针里 `marks` 也变了（第一次 94 → 62、之后 44 → 25），那是进程第一轮解析 200 份 json 的一次性成本，受磁盘/AV 状态影响大，不计入这次的收益。验证：`verify_instances` J 段与 `InstanceFileTests` 真值表；整轮 `ctest` 39/39。

### 启动第一屏别用默认字体里没有的字形

#### 主页那颗箭头

上一步之后启动是 615–625 ms。往下拆：`setupUi()` ≈ 335–358 ms，其中四页 ≈ 296–316 ms，几乎全在主页（271–275 ms），主页里又几乎全在右栏账号卡片的几何计算：

```
STARTUP-PROBE home left 1        ← 左栏全部搭完，1 ms
STARTUP-PROBE home card 1        ← 卡片外壳，1 ms
STARTUP-PROBE home account 10    ← 账号按钮 + 三个标签，9 ms
STARTUP-PROBE home viewer 10     ← SkinViewer 构造
STARTUP-PROBE home skin 10       ← 兜底皮肤
STARTUP-PROBE home minheight 279 ← profileCard->heightForWidth(...) 这一段 269 ms
```

`heightForWidth()` 逐个问子控件的 `sizeHint()`（做法见 `HomeProfileCard`），逐个计时：

```
STARTUP-PROBE carditem QPushButton 264   ← 就是这一下
STARTUP-PROBE carditem SkinViewer 0
```

裸 `QPushButton` 量 `sizeHint()` 只要 7 ms；带上这套属性（kind / homeAction / accountHeader）、带子布局也是 0 ms。差别只剩那颗箭头 `QLabel("⌄")`：**U+2304 不在默认界面字体的覆盖表里**，Qt 第一次量这段文字就要去字体库里做一遍回退查找：

```
STARTUP-PROBE probebtn   QSize(54, 34) 7     ← 裸按钮
STARTUP-PROBE glyph      QSize(10, 19) 268   ← 就一个 "⌄"
STARTUP-PROBE plainlabel QSize(24, 18) 0     ← 中文/ASCII 不付这笔
```

不是 QSS、布局或皮肤，是一个字形没在默认字体里。改法：箭头改用 `VersionIcons::chevron()` 画的位图（和底栏、版本树里的箭头同一份笔画），控件、尺寸、换主题重画由 `Theme::follow()` 管；U+2304 从启动路径上拿掉。A/B 数字见上表，`git stash` 前后各编一次，不带探针步骤的探针构建（`WTFML_PERF_PROBE=ON`、Release）各 n=3。

> 后续：主页右栏那张账号卡片（连同这颗箭头）已经删掉，账号入口挪到导航轨底部（头像 + 名字，点开账号弹层）。下面这条结论照旧适用于启动第一屏上的任何字形。

这 270 ms 是字体回退机器的一次性初始化，不是每颗字形的价。同一进程里接着量别的回退字形都是 0 ms：

```
GLYPH-PROBE ascii   QSize(21, 16) 7
GLYPH-PROBE cjk     QSize(24, 16) 0
GLYPH-PROBE chevron QSize(9, 16) 271   ← U+2304，第一次回退查找
GLYPH-PROBE check   QSize(12, 16) 0    ← U+2713，后面的都免费
GLYPH-PROBE warn    QSize(17, 16) 0    ← U+26A0
GLYPH-PROBE chevron-again QSize(9, 16) 0
```

所以改动的效果是"这笔钱不再压在启动上"：首次画出别的页（实例文件页的 `⚠`、任务中心的 `✓` / `✗`）时仍会付一次，下一小节把这些清掉。绝对值取决于这台机器默认字体里有没有那一格；"启动第一屏别用字体里没有的字形"这条规矩与机器无关。

#### 界面正文里的回退字形

别的页正文里还有 `✓` / `✗` / `⚠` / `☑`，谁先画出来谁付那约 270 ms。测量用临时探针 `build/tmp-wtfml-fontprobe/main.cpp`：一个码点一个独立进程，量第一次 `QFontMetrics::horizontalAdvance()` 的耗时（回退机器是进程内一次性的，连着量一串会让后面全是 0）。判据只有一条：主字体里有没有。`QFontMetrics::inFont()` 不行，它问的是整条回退链，本机上每个字符都回 1。

```
U+0041 A     metrics=6ms      U+2304 ⌄     metrics=281ms
U+2190 ←     metrics=4ms      U+2303 ⌃     metrics=275ms
U+2192 →     metrics=4ms      U+26A0 ⚠     metrics=277ms
U+2022 •     metrics=4ms      U+2713 ✓     metrics=279ms
U+00D7 ×     metrics=4ms      U+2717 ✗     metrics=272ms
U+2605 ★     metrics=4~5ms    U+25B6 ▶     metrics=287ms
U+25A0 ■     metrics=4ms      U+2610 ☐     metrics=294ms
U+25A1 □     metrics=4ms      U+2611 ☑     metrics=276ms
U+25CF ●     metrics=4ms      U+24D8 ⓘ     metrics=285ms
U+203B ※     metrics=4ms      U+23F3 ⏳    metrics=277ms
U+221A √     metrics=4ms      U+2194 ↔     metrics=277ms
U+FF01 ！    metrics=4ms      0x1F3xx 那一片（emoji） 未测，按回退字形对待
```

`ui/TextMarks.hpp` 是唯一出处（五格 + 三个"记号 + 空格 + 正文"的重载）：

| 原来 | 现在 | 首次排版 |
| --- | --- | --- |
| `✓`（任务中心、日志、升级向导步骤） | `√` U+221A | 279 → 4 ms |
| `✗`（同上） | `×` U+00D7 | 272 → 4 ms |
| `⚠`（预设页、键位冲突、原理图兼容提示） | `※` U+203B | 277 → 4 ms |
| `☑` / `☐`（迁移 / 升级计划表） | `■` / `□` U+25A0 / U+25A1 | 294 / 276 → 4 ms |
| `▶ 启动实例`（实例概览主动作） | `启动实例` | 287 → 0 ms |

`○ ● → • ★` 本来就在主字体里。注意：`TextMarks.hpp` 头注释列了不许再用的字符（`✔ ✘ ⚡ ▶ ◀ ↻ ⟳ ⓘ ℹ ⌛ ⏳ ✎ ✏ ⌄ ⌃ ↔`），它们是同一套回退字形，要图标走 `VersionIcons`。`tests/ui_check/modules/UiTaskCenterChecks.cpp` 的 I1 直接对 `TextMarks::done()` 那几格的字符本身下断言，有人把 `✓` 写回去会红；任务中心 I1/I2、任务详情 I6、原理图 SG1、迁移计划、实例补全那句日志的断言也改成对 `TextMarks` 下断言。

限制：用户在 Minecraft 日志、模组名、服务器 MOTD 里带进来的回退字形照样会触发一次（外部数据），保证的是界面自己写的话不碰回退路径；数字来自本机（Windows 11 中文版，默认界面字体 Microsoft YaHei UI 9pt）。

## CI 分片与缓存

```powershell
pwsh -File tools/perf-ci.ps1 -Limit 8
```

只调 GitHub REST 的 GET（`/actions/runs`、`/actions/runs/{id}/jobs`），凭据用 git 里已存的那一份，token 不打印。

2026-10-25：最近 5 次运行的 `plan` 与 `build-and-test` 都在 2 秒内失败、`runner_name` 为空、没有任何步骤，check-run 注解是：

> The job was not started because recent account payments have failed or your spending limit needs to be increased. Please check the 'Billing & plans' section in your settings

CI 在那天没有真正跑过，所以分片/缓存耗时没有可测对象，要等账号计费处理完（此后没有重测）。证据：`build/perf-ci-20261025-185917/`（`ci.json` 里每次运行每个作业的结论）。

## 界面交互

验证工程是 `-O0`，量不了产品的手感，所以有一份只用于测量的产品构建 `-DWTFML_PERF_PROBE=ON`（默认 OFF，和 `WTFML_DIAG_DOWNLOAD` 同一模式）：

```powershell
$Compiler = 'D:/Qt/Qt/Tools/mingw1310_64/bin/g++.exe'   # tools/perf-build.ps1 的默认值
$QtPrefix = 'D:/Qt/Qt/6.10.1/mingw_64'
cmake -S . -B build/perf-probe -G "MinGW Makefiles" `
    '-DCMAKE_BUILD_TYPE=Release' `
    "-DCMAKE_CXX_COMPILER=$Compiler" `
    "-DCMAKE_PREFIX_PATH=$QtPrefix" `
    '-DWTFML_BUILD_TESTS=OFF' '-DWTFML_PERF_PROBE=ON' `
    '-DWTFML_STRICT_WARNINGS=ON' '-DWTFML_WARNINGS_AS_ERRORS=ON'
cmake --build build/perf-probe --parallel 8
```

探针在 `src/perf/PerfProbe.cpp`：argv 里带 `--wtfml-perf-probe=<文件>` 才动，否则这个 exe 和普通启动器一样。它按用户点的按钮发动作（`TitleBar::navButton()`、设置页的主题下拉框），不绕业务层。

- 每步量两段：`actionMs`（同步那一截，翻页后的懒加载/列表重建都在里面）和 `repaintMs`（把整窗立刻重画一遍，是上限，真实使用只重画脏的几块）。
- 首帧守卫：首帧 = `actionMs + repaintMs`；异步才出现的东西（如后台快照到位后才摆的预设弹框）用 `nestedFirstFrameMs`。`waitMs`（等数据）不算首帧。阈值 50 ms，超了又不在 `tools/perf-firstframe-allowlist.txt` 里，探针以非零退出（`firstFrameGuard.failed`），这一趟测量算失败。
  - 允许名单是 `键<TAB>值` 的纯文本，键 = 探针步骤名，值 = 放行时记录到的参考首帧（不参与判定）；只许删不许加，修好一条删一条。
  - `scan:` / `theme:` / `tasks:` / `presets:` / `concurrent:` 是故意同步的扫描、换主题重刷或合成负载，`*-wait` 是等待那一半，都不进守卫。
  - 守卫只认 Release 探针的数。`ui_check_surface_sweep` 的 offscreen `-O0` 数字只能横向比，不拿来判。
- 100 s 硬收尾：那一刻还没走完就按已有的几步落盘，JSON 里写 `hardFinalized: true`。启动脚本只等 120 s，验收配置到点会关窗，而中途关窗时报告根本不会生成；网络卡在某一步的等待里、或者某一步弹出了提示，都会走到这一档。它只决定收不收尾，不改任何一步的量法。
- 20 ms 的 `Qt::PreciseTimer` 看门狗记主线程空档：`maxGapMs` 含 20 ms 节拍，`maxStallMs` 是减掉节拍之后的值，`over50ms` 数超过 50 ms 的节拍。看门狗在 `app.exec()` 之前就开始，所以最长空档包含启动期那一段。
- 步骤：先单独跑一遍本地 Java 扫描（`scan:java`）→ 四个导航页各一次 → 四个主题预设各一次 → 回主页 → 还原主题 → 再进一次设置页（`nav:settings-again`，第一次与第二次的差就是首次 Java 扫描的价）→ 回主页。卡顿归属（`stalls.maxGapFromStep/maxGapAtStep`、`slowSpans` 保存 50 ms 以上的跨度）和采栈见 [MAIN_THREAD_STALLS.md](perf-notes/MAIN_THREAD_STALLS.md)。

```powershell
pwsh -File tools/perf-startup.ps1 -Probe -Runs 3 -Instances 0  -Exe build/perf-probe/WTFMinecraftLauncher.exe -ReportDir build/perf-probe-empty-n3
pwsh -File tools/perf-startup.ps1 -Probe -Runs 3 -Instances 40 -Exe build/perf-probe/WTFMinecraftLauncher.exe -ReportDir build/perf-probe-40-n3
```

### 2026-10-25 基线（`build/perf-probe/`，Release `-O3`、只编产品、探针 ON；n=3 中位）

| 步骤 | 空目录 动作 ms | 空目录 重画 ms | 40 实例 动作 ms | 40 实例 重画 ms |
| --- | ---: | ---: | ---: | ---: |
| `scan:java`（单独跑一遍本地 Java 扫描） | 72.7 | 0 | 76.3 | 0 |
| `nav:home` | 0 | 0.9 | 0 | 0.9 |
| `nav:instances` | 28.0 | 3.8 | 30.1 | 4.5 |
| `nav:download` | 4.9 | 3.2 | 5.1 | 3.5 |
| `nav:settings`（第一次） | 86.0 | 4.7 | 86.1 | 4.6 |
| `theme:emerald`（本来就是它） | 0 | 1.3 | 0 | 1.4 |
| `theme:ocean` | 52.5 | 2.8 | 54.4 | 3.7 |
| `theme:dark` | 41.4 | 3.0 | 40.9 | 2.9 |
| `theme:system` | 40.1 | 3.3 | 41.6 | 3.3 |
| `nav:home-again` | 3.9 | 4.0 | 3.9 | 4.1 |
| `theme:restore` | 39.6 | 5.4 | 39.7 | 5.5 |
| `nav:settings-again`（第二次） | 3.9 | 3.8 | 4.2 | 4.1 |
| `nav:home-final` | 3.2 | 3.7 | 3.4 | 3.8 |

主线程（20 ms 看门狗，每跑约 79 个节拍）：

| 场景 | 最长空档 | 减掉节拍 | 超过 50 ms 的节拍 | 窗口可见 |
| --- | ---: | ---: | ---: | ---: |
| 空目录（n=3） | 96 / 101 / 124 ms | 76 / 81 / 104 ms | 5 / 5 / 7 次 | 551–579 ms（中位 566） |
| 40 个实例（n=3） | 101 / 106 / 116 ms | 81 / 86 / 96 ms | 5 / 7 / 8 次 | 610–615 ms，另一次 1064 ms |

证据：`build/perf-probe-empty-n3/`、`build/perf-probe-40-n3/`（每次的 `run-NN-probe.json` 和中位表在 `summary.md`；`run-00` 是热身，不进结论）。

读出的事：

1. 第一次进设置页 86–90 ms，第二次 4 ms；差的约 82 ms 里 73 ms 是本地 Java 扫描（`scan:java` 单独量 72.7 / 76.3 ms，这是本机有几份 JDK 时的价）。`scan:java` 排在最前，所以表里 `nav:settings` 的扫描已是热缓存，86 ms 是"扫描（热）+ 首次铺开"，冷启动那一跑（没有 `scan:java` 步骤）是 89.9 ms。这一处已改成后台扫描，见下。
2. 换主题 40–54 ms（`ocean` 最贵），能感觉到一顿；"换成已经在用的那一档"是 0 ms，控制器里的去重生效。这一处已优化，见下。
3. 40 个实例几乎不动"点实例页"（28.0 → 30.1 ms）：实例列表由代理画、不建行控件，这一层没有随实例数线性涨。
4. 主线程最长空档 96–124 ms（减节拍 76–104 ms）比任何单个动作都长，里面含启动期；归属靠 [MAIN_THREAD_STALLS.md](perf-notes/MAIN_THREAD_STALLS.md) 的采栈。
5. 探针构建的窗口可见时间 551–615 ms（40 实例有一次 1064 ms 离群值，中位不受影响），比"启动与就绪"那 728–851 ms 短；两者构建目录和 exe 不同，不能相减。

### 设置页 Java 扫描移出 GUI（2026-10-25）

基准 `f702eeb`，沿用同一个 `build/perf-probe`、Release、Qt 6.10.1，不改探针步骤；各预热一次、测三次、0 实例。测量期间另有并行 2 的验证编译，不是空闲机器基准。

| 首次 `nav:settings` | 三次 actionMs（不含预热） | 中位数 | repaint 中位数 |
| --- | --- | ---: | ---: |
| 修改前 | 110.9 / 113.2 / 109.6 | 110.9 ms | 5.7 ms |
| 修改后 | 17.0 / 18.5 / 19.6 | 18.5 ms | 4.8 ms |

同步翻页开销降约 83%。扫描工作没有消失，只是不再阻塞 GUI；探针仍故意先同步执行 `scan:java` 来测扫描本身，所以总看门狗峰值不代表优化后的翻页阻塞。

实现：首次扫描和自定义 Java 探测在后台执行，输入捕获 Java 根目录/路径；重复刷新合并，目录或路径改变后旧结果作废，控制器销毁后不回调旧界面；显式检测/下载结果优先于旧自动扫描；扫描结果只更新管理展示，不自动改写用户 Java 选择，不提前下载运行时。

证据：`build/perf-java-before-async-20261025/`、`build/perf-java-after-async-20261025/`（两份 `startup.json` 均 `registry.restoredVerbatim=true`）。功能验证 `20261025-195745-67ed6c08`：主窗口 2696/2696、Java 管理专项通过（合计 99.04 s），新增九条受控后台任务检查（即时返回、界面事件、去重、作废、析构回调）；`20261025-200518-3e90d6a3` 普通启动器构建成功，网络诊断自检通过（18.98 s）。

### 任务列表局部复用（2026-10-25）

测量构建增加固定视图负载：200 条任务（20 进行中、180 已完成），每条两步，按 150 ms 间隔更新 20 次，每次只变一条进行中任务的进度/说明。用真实 `TaskCenterPanel` 控件、内嵌三列布局和真实绘制；不创建下载器或游戏进程，所以只反映视图刷新成本，不含网络吞吐、下载线程争用或完整账本接线。窗口 900×620、40 个合成实例；同机同 Release 探针，前后各预热一次、计三次，测量时不并行编译。

| 指标 | 修改前（每趟中位） | 最终修改后（每趟中位） |
| --- | --- | --- |
| 单条更新 actionMs，20 次/趟 | 61.1 / 60.1 / 61.0 ms | 1.8 / 1.8 / 1.8 ms |
| 所有更新 actionMs 范围 | 58.9–64.4 ms | 1.7–2.6 ms |
| 首次展开 actionMs | 79.8 / 81.4 / 79.5 ms | 77.9 / 78.0 / 81.1 ms |
| 整趟最差主线程阻塞（含其他探针步骤，已减 20 ms 节拍） | 107 / 94 / 99 ms | 104 / 92 / 111 ms |

刷新中位约降 97%，首次展开没有变慢，整趟最大阻塞没有改善。做法是只复用未变化的行，改变的行仍按原规则创建；行号、回调版本、调色板/字体和所有行内展示字段参与失效；详情独立读取最新快照，滚动位置更新后保留。不是完整虚拟列表，首次创建、左栏和详情重建仍有成本。曾出现首次展开变成约 110 ms 的中间版本，原因是无滚动时也强制同步布局；最终仅在恢复非零滚动位置时强制布局。

证据：改前 `build/perf-task-before-20261025/`（exe MD5 `0654703fb1ce04409ad973b49550f5ed`），最终 `build/perf-task-final-20261025/`，均 `registry.restoredVerbatim=true`。复现：`tools/perf-startup.ps1 -Exe build/perf-probe/WTFMinecraftLauncher.exe -Probe -Runs 3 -Warmup 1 -Instances 40`；JSON 的 `taskWorkload` 标明合成来源与行数，`tasks:open-200`、`tasks:update-NN`、`tasks:close` 是对应步骤。

失败记录：`build/perf-task-after-20261025/` 预热成功、首次计量提前结束且无报告，未定位根因（当时另有验证编译，但没有证据说是它）。旧脚本把"已退出"误记为 `cleanExit=true`、未保存退出码，已改为记录实际退出码、非零时报错、缺报告仍失败；之后两套空闲复测各三次完整通过。中间结果不用于前后对比。功能验证 `20261025-205703-6211941e`：ui-refresh、install-layout、lifecycle、theme 四模块通过，新增检查覆盖持续进度、未变行保留、滚动位置、详情新鲜度、回调替换、调色板变化和移除释放；`20261025-205805-3251de49` 的 workspace、activity-selection 通过（0.82 s）。

### 大量预设筛选与后台目录读取（2026-10-25）

分三种成本测量：

1. **视图**：1000 条合成预设（半数"建筑"、半数"生存"，含名称/说明、默认标记、类别），依次搜索七个字符串（全匹配、半匹配、清空、数字、说明、无匹配、再清空），只调用真实输入框。
2. **磁盘**：启动沙盒里新建随机临时目录，写 1000 份有效 UUID 预设及顺序索引，用生产 `Presets::list()` 连读三遍（只含元数据，不是复杂配置/模组文件）。
3. **后台读取**：同一临时目录交给生产 `CatalogLoader`，记录提交、完成总耗时及读取期间 GUI 定时器。该步骤不含主窗口导航、实例下拉框和完整控制器完成回调的成本，页面接入由受控模块测试验证。

同机 Release，40 个合成实例、900×620 窗口，前后各预热一次、计三次，测量期间无并行构建/测试：

| 指标 | 原实现（逐趟） | 新实现（逐趟） |
| --- | --- | --- |
| 创建 1000 行视图 actionMs | 14.3 / 14.0 / 14.3 | 8.9 / 9.3 / 9.1 |
| 七次搜索 actionMs 中位 | 6.2 / 5.8 / 5.9 | 0.5 / 0.6 / 0.6 |
| 七次搜索范围 | 0.5–6.7 ms | 0.4–1.0 ms |
| 同步读取 1000 份元数据 | 未测 | 首读 2491.1 / 2477.8 / 2545.1 ms |
| 后台读取完成耗时 | 未测 | 2595 / 2529 / 2589 ms |
| 后台提交 actionMs | 未测 | 0.0 / 0.1 / 0.0（四舍五入到 0.1 ms） |
| 后台读取期间 GUI 心跳数 | 未测 | 83 / 81 / 82 |
| 后台读取期间最大 GUI 间隔 | 未测 | 44 / 41 / 45 ms（含 20 ms 节拍） |

搜索中位约降 90%；文件读取本身仍约 2.5 s，扫描移出 GUI 避免的是整段同步阻塞，不是 I/O 加速。同步测试的最大间隔约 2.5 s（标签 `presets:read-store-*`，造数也会阻塞）是探针故意同步读取的结果，后台响应单看 `backgroundPresetStore`。此后读盘本身的优化（缓存、并行、落盘）见 [PRESET_LIST_IO.md](perf-notes/PRESET_LIST_IO.md)，现在 1000 份冷读 0.40 s。

实现：普通预设页首次打开走后台加载，重复打开同一在途目录合并，同步刷新会作废在途结果；工作线程只捕获根路径/读取函数、完成回 GUI，控制器或页面销毁后不操作旧控件；搜索/分类用代理模型保留源条目，列表刷新后按实际可见选中项显示详情；加载与错误提示放在滚动编辑区外，加载期间禁用修改，未解决的坏预设仍明确展示。作废只丢弃结果，不中断进行中的文件读取，协作式读取取消尚未做。

证据：原视图 `build/perf-preset-before-20261025/`（exe MD5 `a69ad3eef94e58ab82c9668a9cd7aecc`），新视图/读取 `build/perf-preset-async-20261025/`，均 `registry.restoredVerbatim=true`；`build/perf-preset-final-20261025/` 记录同步扫描原始成本（`presetStoreWorkload.ok=true`）。探针后台等待最多 30 s，整个探针进程受启动脚本 120 s 限制，样本准备/行数校验失败返回非零。测量构建曾因新增 Host 字段缺默认初始化在严格警告下失败，补了默认初始化后通过。功能验证：`20261025-213510-15e5e224` preset-install 通过（1.15 s，覆盖 1000 条源模型稳定性、搜索/类别组合、默认切换、坏项不可操作、隐藏请求项回退，后台加载去重、根目录代际、显式作废、GUI 交付、加载中响应、提示位置、控制器/页面生命周期）；`20261025-213614-450707b8` ui-refresh/install-layout/lifecycle/theme 通过（2.50 s）。

### 换主题：只给露着的滚动条重挂样式（2026-10-31）

```powershell
pwsh -File tools/perf-startup.ps1 -Probe -Runs 3 -Instances 0 -Exe build/perf-probe/WTFMinecraftLauncher.exe -ReportDir <目录>
```

上面基线里换主题是 40–55 ms 一档。在 `Theme::apply()` 里临时加 `THEME-PROBE` 计时（`setPalette()` / `styleScrollBars()` / `repolishWidgets()` 三段各一行 `qInfo`，量完已删）拆开看，空目录夹具、拆分前后各两轮、只列真换了主题的四次（探针构建 md5 `ad5ef306` → `21a6933b`，最终 `76b1a0d5`）：

| 段 | 改前 ms | 改后 ms |
| --- | ---: | ---: |
| `setPalette()` | 0.36–1.08 | 0.36–0.82 |
| `styleScrollBars()` | 14.7–25.9（ocean ≈ 25，其余 ≈ 14.7–16.0） | 0.05–0.36 |
| `repolishWidgets()` | 22.5–26.7 | 22.5–25.7 |

原注释写"很便宜"的 `styleScrollBars()` 其实是第二贵的一笔：它对 `QApplication::allWidgets()` 里每一根 `QScrollBar` 单独 `setStyleSheet()`，一次切主题要给全应用几百根一起重挂。现在只处理 `isVisible()` 的；藏着的滚动条身上记的代数标记仍是旧的，`ScrollBarStyler`（应用级事件过滤器）收到 `QEvent::Show` 时发现标记不对才补一发，所以它们第一次露出来时颜色已是新的。

最终（同一台机器、同一份探针配置、空目录夹具，n=3 中位；改前 md5 `ea78d72f`，改后 `76b1a0d5`）：

| 步骤 | 改前 ms | 改后 ms |
| --- | ---: | ---: |
| `theme:ocean` | 54.9 | 28.9 |
| `theme:dark` | 40.6 | 26.5 |
| `theme:system` | 40.1 | 26.6 |
| `theme:restore` | 41.1 | 24.9 |

同一份报告里其余步骤没动：`nav:instances` 25.6 → 25.1、`nav:settings` 9.9 → 9.2、`nav:settings-again` 3.5 → 3.9；窗口可见 595.0 → 610.3 ms（范围 579–607 / 600–613 互相叠着，这条路径不经过主题，不计收益也不计回归）。

剩下的大头是 `repolishWidgets()`（22.5–25.7 ms）。把它也改成懒的，需要每个藏着的控件在每次 `Show` 时自己记"这一身是哪一代主题"，漏一处就会有老配色停在屏上；要做得配钩子加真机视觉验收，目前没做。

验证：`ui_check` 第 ⑭ 组三条钉住"藏着的没有控件级样式 / 换主题不给它白跑一趟 / 一露头就补上新主题样式"，外加原有的滚动条配色检查。全量复跑：`ui_check` 3136/3136、`mw_check` 2722/2722、`verify` 6182/6182。限制：探针构建（Release、`-DWTFML_PERF_PROBE=ON`、只编产品）量的是换一次主题同步那一截，不含整窗重画那几毫秒。

### 安装确认弹窗：预设数据只扫一趟盘（2026-10-31）

路径：下载页 → 选版本 → 装 → 确认弹窗里的四类预设（游戏设置 / 模组 / 资源包 / 数据包）。原来弹窗按四类各问一次 `presetsInCategory()`，每类里都调 `availablePresets()`（即 `Presets::list()`，把每份 manifest 读一遍），最后为"复合预设"那张表再扫一遍并按份读 manifest：四类加大表共五趟全量扫盘，复合预设的 manifest 还在一趟里被读两遍。

现在 `PresetController::installPickerData()` 扫一次，复合预设每份只读一次 manifest，四类的选项由 `InstallPickerData::optionsFor()` 从这份快照派生；`InstallPageController` 用同一份数据拼四类和复合表，这一段不再碰盘。

折算（没有探针，按当时 1000 份预设单趟同步读 2.2–2.5 s 折算，未在弹窗上实测）：旧写法 1000 份时是五趟 ≈ 11 s 的冻结，新写法一趟；20 份时约 220 ms → 44 ms。

验证：`mw_check_preset_install` 新增十项（四类各自只出现自己的、复合预设的子项按父项次序出现在对应类里、缺的那一类不凭空造、未知类别回空、复合项带着父项名字和读回来的 `components`，不再第二次读 manifest）；复跑 `verify` 6182/6182、`verify_presets` 762/762、`ui_check` 3136/3136、`mw_check` 2722/2722。

### 五笔卡界面线程的活挪到后台（2026-11-14）

量法：Release 产品 exe（`verification` preset，ccache 自动套着），`--acceptance-profile <临时目录>`（设置与游戏目录都在里面，不碰真注册表）。夹具 = 20 份实例（版本清单那一条另跑 200 份）+ 一份重实例（60 模组 / 24 张 1920×1080 截图）+ 40 份预设 + 一张 4K JPEG。临时探针打在五处生产代码里（`TEMP-128`，量完已删），只记"这一段在界面线程上花了多久"；每项跑 3 次，表里给的是范围。

| 位置 | 改前（界面线程） | 改后（界面线程） | 改后（后台线程） |
| --- | ---: | ---: | ---: |
| 版本清单回来刷新实例，20 份 | 5 ms，扫盘 + 重建都在回调里 | 0 ms 发请求；3 ms 收尾（下一轮事件循环） | — |
| 同上，200 份 | 31–34 ms | 0 ms 发请求；17 ms 收尾 | 扫盘不在界面线程（差出来的那 14–17 ms） |
| 安装页「预设」弹框取数据，40 份预设、摘要缓存冷 | 25–26 ms | 0 ms（只发 `CatalogLoader` 请求） | 20–21 ms |
| 截图页 24 张缩略图解码 | 274–280 ms | 0 ms（只派发） | 40–45 ms（线程池） |
| `Glass::enabled()` ×2000 | 70–78 ms（35–39 µs/次） | 0 ms（<0.5 µs/次） | — |
| 同一张 4K 背景图解码 ×5 | 164–210 ms（每次 31–34 ms） | 34–38 ms | —（第一次 34–37 ms，后四次命中缓存） |

版本清单那条的"改前"比 125 那轮加载盘点里的 63 / 116 ms 小：那一份夹具带 `wtfml.meta`、盘是冷的，这里的热盘夹具扫得更快；要看的是"这一次回调还占界面线程多少"，不是绝对值。

五笔改动：

- `VersionManager::requestInstalledVersions()`：扫盘挪进 `QtConcurrent`，结果从 `installedVersionsScanned` 回来。每次请求 +1 一代，旧的（晚到的）直接丢，在途那一发被取消旗叫停；watcher 挂在版本管理器上，析构后不再回调。`MainWindow::refreshInstalledVersionsAsync()` 只在版本清单从网络回来那条路上用，同步入口 `refreshInstalledVersions()` 留给"改名 / 删实例 / 下载收尾"这些要立刻看见的场合。
- `PresetController::requestInstallPickerData()`：列表走 `Presets::CatalogLoader` 的异步快照（同一份目录的并发请求并进一趟），复合预设的 manifest 也在工作线程里读；安装页点「预设」不再在这一帧读盘，数据回来才摆弹框（布局不动）。
- 截图页缩略图：`QImageReader` 缩放解码放进 `QtConcurrent`，结果按 `路径|大小|修改时间` 缓存；列表项建出来就挂一张透明占位图，图回来只换内容，行高不跳。表头里"写得荒唐"（>64 MP）的那一份直接不解（原来会整张读一遍）。
- `Glass::enabled()`：四档判据（用户选的档 + 高对比度 + 系统透明开关 + 系统背板可用）一次读完缓存住；`setMode()` 写完当场失效，系统设置变化由 `install()` 装的过滤器在 `ThemeChange` / `ApplicationPaletteChange` / `ApplicationStateChange` 时失效。
- 主题背景图：`Theme::cachedImage()` 按 `路径|大小|修改时间` 缓存解码后的 `QImage`（按张数封顶 6 张），`ThemeBackground` 和 `Glass::Backdrop` 都走它；同一张图在换主题那一下只解一次。

验证：`verify_instances` 的 K128 两段（连着发两次只回最新那一发、对象析构后不再回调）、`ui_check_theme`（写设置后 `enabled()` 立刻反映新值，直接写盘 + `invalidateSettingsCache()` 同样生效）；`tools/check-test-modules.ps1` PASS。

### 工具箱按工具懒建（2026-11-14）

改法：`ToolboxPage` 构造只搭表头和目录页；某件工具第一次进才搭那一片（`ensureTool()` → `toolBuilt`），搭完留着。`ToolboxController` 在 `toolBuilt` 里给那一片灌表、恢复记住的输入、算初值。探针新加 `toolbox:tool:<键>`（从目录页点那一格）和 `toolbox:catalog` 两类步骤。

量法：`tools/perf-startup.ps1 -Probe -Instances 0`，看各趟 `run-*-probe.json` 的 `firstFrameMs`。改前和改后背靠背量，但当时另有成员在编译，负载记在表头里。改前那一列的工具页在 `nav:toolbox` 时已经全搭好，只是切过去；改后那一列是第一次进，要现搭。

| 步骤 | 改前（n=3 中位；负载 55–70%，无 cc1plus） | 改后 A（2 趟；负载 51–69%，6–11 个 cc1plus） | 改后 B（1 趟；负载 91–93%，4–5 个 cc1plus） |
| --- | ---: | ---: | ---: |
| `nav:toolbox` | 132.8 | 14.0 / 14.8 | 13.6 |
| `toolbox:tool:anvil` | 9.7 | 18.0 / 25.5 | 17.5 |
| `toolbox:tool:coord` | 14.8 | 24.5 / 25.2 | 25.2 |
| `toolbox:tool:beacon` | 24.8 | 43.7 / 48.7 | 48.0 |
| `toolbox:tool:brewing` | 19.9 | 36.1 / 35.7 | 37.7 |
| `toolbox:tool:text` | 20.3 | 38.3 / 32.8 | 39.1 |
| `toolbox:tool:banner` | 17.9 | 46.7 / 45.7 | **55.7** |
| `toolbox:tool:trim` | 22.5 | **56.5 / 54.6** | **63.1** |
| `toolbox:tool:download` | 17.8 | 27.0 / 24.4 | 23.9 |
| `toolbox:tool:server` | 16.2 | 19.6 / 18.9 | 18.7 |
| `toolbox:catalog` | 8.9 | 11.2 / 10.5 | 10.4 |

B 比 A 多做了一件事：纹饰页那十二个下拉框改成一个框一次整批插入。负载变了，所以这一项单独省了多少看不出来。

拆开量过一次纹饰页的第一次进：搭这一片本身约 2 ms。搭好以后挪进滚动区那两次重挂父对象，或者先就地搭在滚动区里，样式表都要把整棵子树重新过一遍，都是 25 ms 上下。灌下拉表、显示、布局之后是 30 ms 上下。先就地搭再显示，总数和先搭后挪一样，所以没留那个改法。

纹饰、旗帜两页第一次进时，在编译负载下会超过 50 ms 守卫，守卫会让这一趟以非零退出。空闲重量（2026-11-14，步骤 135；Release 探针，`-Runs 3 -Instances 20 -InstanceMods 60`；两轮分别记到开跑 45% / 37%、跑完 29% / 22% 的 `Win32_Processor.LoadPercentage`，没有 cc1plus、没有别的构建在跑）：`toolbox:tool:trim` 两轮各三趟 55.1 / 52.6 / 53.4 与 57.1 / 57.7 / 58.5 ms（中位 53.4、57.7）都在线上，按任务单进 `tools/perf-firstframe-allowlist.txt`（参考值 57.7，不进判定）；`toolbox:tool:banner` 两轮 42.3 / 39.8 / 39.6 与 41.0 / 38.7 / 40.9 ms（中位 39.8、40.9）在线内，没进名单。要把纹饰页压回线内，可以选空闲时分片预搭（每片 ≤16 ms），或者减少这一页的控件数。

### 实例文件页改成代理画行（2026-11-14，步骤 139）

文件管理八页（`InstanceFilesPage`）原来每一行 `setItemWidget()` 挂一套真控件（图标、名字、小字、开关、三颗工具按钮），进模组页那一下光建 60 行就要几十毫秒。现在行由 `InstanceFilesPage::RowDelegate` 画，数据全在条目上，刷新只换条目、视图不重建（做法见 [LIST_PAINT.md](perf-notes/LIST_PAINT.md)「现行做法」）。

量法：Release 探针（`-DWTFML_PERF_PROBE=ON -DWTFML_BUILD_TESTS=OFF`，严格告警当错误），`pwsh -File tools/perf-startup.ps1 -Probe -Runs 3 -Instances 20 -InstanceMods 60`，改前 = main 187b75f，改后 = 本分支，两边背靠背跑，同一台机器（Ultra 5 225H / 31.5 GB），没有别的启动器在跑。当天在线版本清单最新是 26.3，探针的安装页那段（`install:*`，OptiFine 没有 26.3）在改前改后两个 exe 上都卡到 120 s 不退出，和这一步无关；这次测量在两个 exe 里都临时加了一行"跳过安装页及其后几段"（量完已撤，没有提交），实例页那几步排在它前面，不受影响。CPU 负载（`Win32_Processor.LoadPercentage`）：改前那一趟开跑时 96%，改后那一趟开跑时 35%、跑完 56%。改前那趟负载更高，所以改前数字偏高一些，但改后三次都低于阈值。

| 步骤 | 改前首帧（3 次） | 改后首帧（3 次） | 改前 `files page refresh`（产品日志） |
| --- | ---: | ---: | ---: |
| `instance:page:mod` | 102.7 / 68.4 / 67.9 ms | 28.8 / 31.7 / 44.1 ms | 117 ms（60 条） |
| `instance:page:mod-second` | 91.8 / 60.2 / 63.5 ms | 20.0 / 20.7 / 23.8 ms | 91 ms（60 条） |

首帧 = `actionMs + repaintMs`（守卫口径）。改后 `files page refresh` 那条日志没再出现：它只在一趟刷新超过 40 ms 时才打印。`instance:page:mod` 和 `instance:page:mod-second` 已从 `tools/perf-firstframe-allowlist.txt` 删掉（138、139、140 都合入后名单剩 `instance:open-settings`、`instance:open-settings-again` 2 条；140 的纹饰页在空闲下仍超线，步骤 135 又加回 `toolbox:tool:trim`，见上一节）。

另外：`ui_check_files_rows`（`wtfml.files-rows`）钉住"60 行不建 60 套控件"（空页和 60 行时页面子控件数一样，视口里 0 个）和"点开关真改盘上文件名、刷新后滚动不跳"。

### 实例设置按页懒建、收页不再换父容器（2026-11-14）

量法：探针构建（Release + `-DWTFML_PERF_PROBE=ON`，只编产品），沙盒夹具 = 4 份实例，第一份塞 60 模组 / 12 资源包 / 6 光影 / 5 数据包 / 8 存档 / 24 张截图 / 4000 行 `latest.log` / 24 份 config；改前改后各一趟（同一个夹具、背靠背）。测量时机器上另有两个成员的编译和一个别的探针在跑（负载 60% 上下），绝对值偏高，正式判定要等空闲时重跑；两趟都在 `install:*` 那一段被 100 s 硬收尾截断（`hardFinalized: true`），实例设置那一段之前已经走完。

| 步骤 | 改前 | 改后 |
| --- | ---: | ---: |
| `instance:open-settings` | 212.1 | 88.9 |
| `instance:open-settings-again` | 202.4 | 79.5 |
| `instance:close` | 131.4 | 10.7 |
| `instance:close-again` | 92.1 | 9.3 |
| `instance:page:launch`（第一次切过去，现在含建这一页） | 16.1 | 26.1 |
| `instance:page:logs` | 15.1 | 14.4 |
| `instance:page:mod` / `mod-second` | 51.6 / 47.5 | 56.0 / 47.5 |

产品自己的分段日志（`[UI timing]`，两趟的范围）：`settings dialog ctor` 30 → 9–10 ms，`overlay show settings` 113 → 42–49 ms，`settings first idle` 132 → 55–58 ms。

两类改动：

- 页第一次切过去才建（`InstanceSettingsDialog::ensurePage()`）：构造里只按 `pageCount()` 的顺序把堆叠页每一格的空位挖好、建出概览和「备份与操作」（后者要把概览上那几颗维护按钮搬走）。导航、`pageChoices()`、文件目录表这些都不靠"先把页建出来"——左栏那棵树本来就是按 `shownEntryKeys()` 现算的。
- 收页不换父容器：`OverlayHost::releaseContent()` 原来对每一页都 `setParent(m_parking)`"还回去"，实例设置这一页实测那一段 75 ms（整棵控件树重算继承的样式和调色板）。页自己声明 `wtfmlOverlayDisposesOnClose`（收页后由主人 `deleteLater()`）时只 `hide()`，删除照旧在 `pageClosed()` 那一发里办。

仍然超 50 ms 的是打开那一下（89 ms）：ctor 10 + 覆盖层挂载 49（换父容器 + 首帧铺开）+ 控制器那一段接线（读目录、认可用性、连几十条信号、`filesPage("save")` 那几处取页）；页本身已经不是大头。

### 列表与选择

实例文件页的大列表首绘/重列、反选的批量选择（1000 条：逐行 1279.164 ms / 1000 次通知，批量 1.430 ms / 1 次）见 [LIST_PAINT.md](perf-notes/LIST_PAINT.md)。

## 并发下载

产品 exe 没有假网络注入点，验证工程又是 `-O0`，所以有测量专用的 Release 工具 `tools/perf_download/`（`-DWTFML_PERF_DOWNLOAD=ON` 才编，默认 OFF，不进 ctest、不进发布物）。它在本机 `127.0.0.1` 起一个 HTTP/1.1 文件服务器（走另一条线程，免得服务器 socket 写入和被测对象抢主线程），用产品同一份 `DownloadManager` 下一批文件，量：

- 整批完成时间与吞吐（`transferBytes` 合计 ÷ 实际耗时）；
- 失败数、SHA-1 是否逐文件核对通过；
- 主线程 20 ms 看门狗的最长空档（`max_gap - 20 ms`，口径同"界面交互"）；
- 每个文件的进度/字节信号次数。

```powershell
cmake -S . -B build/perf-download -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=D:/Qt/Qt/Tools/mingw1310_64/bin/g++.exe `
  -DCMAKE_PREFIX_PATH=D:/Qt/Qt/6.10.1/mingw_64 `
  -DWTFML_BUILD_TESTS=OFF -DWTFML_PERF_DOWNLOAD=ON
cmake --build build/perf-download --target WTFMLPerfDownload -j 8
pwsh -File tools/perf-download.ps1            # 全表；-Only '*seg*' 只跑一部分
```

脚本每例先热身一轮（只留覆盖结果），再测三轮取中位；结果落在 `build/perf-download-<时间戳>/`（每例一份 JSON + `summary.json`）。

### 2026-10-25（Release、Qt 6.10.1、n=3 中位）

| 用例 | 文件 | 并发 | 分段 | 限速 | 完成 | 吞吐 | 主线程最长空档 |
| --- | ---: | ---: | :---: | ---: | ---: | ---: | ---: |
| small-100MiB-c6 | 400 × 256K | 6 | – | 不限 | 681 ms | 146.8 MiB/s | 9 ms |
| small-100MiB-c12 | 400 × 256K | 12 | – | 不限 | 690 ms | 144.9 MiB/s | 24 ms |
| small-100MiB-c24 | 400 × 256K | 24 | – | 不限 | 685 ms | 146.0 MiB/s | 41 ms |
| small-100MiB-c48 | 400 × 256K | 48 | – | 不限 | 946 ms | 105.7 MiB/s | 169 ms |
| many-1000x64K-c24 | 1000 × 64K | 24 | – | 不限 | 1313 ms | 47.6 MiB/s | 34 ms |
| large-200MiB-plain-c24 | 40 × 5M | 24 | – | 不限 | 424 ms | 471.7 MiB/s | 40 ms |
| large-200MiB-seg-c24 | 40 × 5M | 24 | 是 | 不限 | 793 ms | 252.2 MiB/s | 98 ms |
| limit-4MiBps-24MiB-c24 | 24 × 1M | 24 | – | 4 MiB/s | 6122 ms | 3.9 MiB/s | 3 ms |
| limit-4MiBps-1000x32K | 1000 × 32K | 24 | – | 4 MiB/s | 7775 ms | 4.0 MiB/s | 20 ms |
| limit-8MiBps-2x5M-c24 | 2 × 5M | 24 | – | 8 MiB/s | 1321 ms | 7.6 MiB/s | 2 ms |
| limit-8MiBps-2x5M-seg | 2 × 5M | 24 | 是 | 8 MiB/s | 1252 ms | 8.0 MiB/s | 17 ms |

读法：

1. 并发档位在这条链路上不产生收益（c6 / c12 / c24 都在 144–147 MiB/s，c48 反而更慢）：loopback 没有往返延迟可摊薄，瓶颈是两边 CPU、单线程服务器与磁盘写。产品默认 24 的依据是 `DownloadConcurrency.hpp` 里那条广域网实测（6 路 138.76 s → 24 路 72.63 s），不能拿这张表反推"并发数无所谓"。
2. 分段在无时延链路上是净开销：同一批 200 MiB，plain 472 MiB/s、seg 252 MiB/s。分段的收益在单连接被限速或长肥管道一侧，不在本机。
3. 小文件的成本主要是每文件连接与调度：400 × 256K 是 146 MiB/s，1000 × 64K 掉到 48 MiB/s。
4. 主线程空档随并发数上升（6 路 9 ms → 48 路 169 ms），吞吐没跟着涨，48 档在这条链路上是纯负担。

### 速度上限被小文件放大（已修）

基准第一轮就跑出反例：1000 × 32 KiB、限 4 MiB/s，1.25 s 下完（25 MiB/s，6.25 倍上限）。QNAM 会把正文提前读进自己的缓冲，`finished` 那一刻缓冲里剩下的字节由 `stream->drain(reply)` 一次写完，这一段绕过令牌桶；文件比缓冲（256 KiB）小时整份都在这一段里。`DownloadManager` 改了三处（注释里有唯一出处）：

1. 限速时缓冲按"一拍分到多少"给（`throttledReadBufferBytes()`，一拍 = 100 ms 的份额 × 2，不低于 8 KiB），让 TCP 在源头上被按住；不限速仍是 256 KiB。
2. 收尾那一口记账：`finished` 时把缓冲区剩余字节从令牌余额里扣掉（允许余额为负）；`onSpeedPumpTick` 只抹正的余额（负数是已写出的字节，不是攒下的额度）。
3. 欠账没还清不发新作业：`pumpDownloadQueue()` 在余额 < 1 字节时直接返回，由限速泵下一拍补上再发车，否则小文件可以一个接一个"整份从收尾那一口过"。

| 用例 | 改前 | 改后 |
| --- | ---: | ---: |
| limit-4MiBps-24MiB-c24 | 4622 ms / 5.2 MiB/s | 6122 ms / 3.9 MiB/s |
| limit-4MiBps-1000x32K | 1250 ms / 25.0 MiB/s | 7775 ms / 4.0 MiB/s |

24 MiB 那跑略高于 4 MiB/s 是令牌桶 burst 容量（1/4 秒 = 1 MiB）允许的。

第一版按并发档位（24）去分缓冲，文件多时没问题，只开一两条作业时每条只剩几 KB，而限速泵每 100 ms 才读一次，速率跟设定值脱钩：

| 用例（2 × 5 MiB、限 8 MiB/s） | 按 24 路分、上限 256 KiB | 按在飞条数分、上限 256 KiB | 最终 |
| --- | ---: | ---: | ---: |
| plain | 8202 ms / 1.2 MiB/s | 2203 ms / 4.5 MiB/s | 1321 ms / 7.6 MiB/s |
| segmented（对照） | 1271 ms / 7.9 MiB/s | 1354 ms / 7.4 MiB/s | 1252 ms / 8.0 MiB/s |

第二列那 256 KiB 的封顶是新瓶颈：两条回复一拍最多读 512 KiB ⇒ 上限 5.1 MiB/s。最终版把分母改成 `activeDownloads` 的实际条数（这条回复自己此刻还没进表，+1），高水位 = 一拍份额 × 2，只用一个总内存闸（在飞条数 × 高水位 ≤ 32 MiB）兜极值，它只在限速超过约 160 MiB/s（手改值，预设里没有）时才真正钳住。前两列是当时的临时跑法，没留 JSON；第三列起是 `tools/perf-download.ps1` 的两个固定用例（`limit-8MiBps-2x5M-*`）。不限速路径未改：同一批小文件 c24 改前 141.8 MiB/s、改后 146.0 MiB/s（噪声内），`large-*` 两跑与改前同量级。

限制：loopback 量的是调度 / 写盘 / 主线程影响，不是广域网；服务器与客户端同机争 CPU 与磁盘；每个文件内容相同（只压连接与调度）；真实下载期间任务中心的刷新没量。

证据：`build/perf-download-report-20261025/`（改前全表）、`build/perf-download-report-before-limit-fix/`（改前限速用例）、`build/perf-download-report-20261025-final/`（上表：11 个用例、每例逐文件 SHA-1 核对通过）。功能回归：`build/verification-mingw13` 严格构建通过，`ctest` 38/38（354.20 s）。

## 智能体崩溃快照与日志检索（2026-11-24）

`logs.search` / `logs.readRange` 与 `crashSnapshot` 都在调用线程上跑，所以要量的是"一次调用会占住界面多久"。夹具是 `AgentLogIndexTests` 里按 `WTFML_AGENT_LOG_SCALE_MB` 生成的合成日志（每行约 70 字节，每 2 万行一条命中行），验证构建 `-O0`、NVMe、热缓存，单跑：

| 项 | 100 MB / 161 万行 |
| --- | --- |
| 首次建行索引 + 读 200 行（`readRange`） | 98 ms |
| 全文搜索 20 条（`search`，ASCII 词，字节匹配） | 353 ms |

改前同一份夹具在每行都做 `QString::fromUtf8` 的版本上是 847 ms；只解码要输出的行之后降到 353 ms。索引只存每行起始偏移（100 MB / 161 万行约 13 MB），按文件大小 + 修改时间失效，最多缓存 4 份。

量法：`WTFML_AGENT_LOG_SCALE_MB=100 ./build/verify-s53/tests/verify_agent.exe`，输出里找「日志规模测量」那一行；只打印，不断言（生成 100 MB 夹具要几秒，CI 不跑）。

限制：这是 `-O0` 验证构建、热缓存、合成内容的数字；冷盘、真实日志（行长分布更散）会更慢。搜索按 6 s 预算截断，没有异步化。

## 发布体积

2026-11-01：静态 Qt 的 `-O3` 单文件 73,531,415 字节，`strip --strip-unneeded` 后 43,288,540（−41.1%），导入 DLL 清单逐条相同；启动三跑 visible 原件 188 / 199 / 189 ms、去符号副本 171 / 185 / 190 ms，没有变慢。`-Os`（MinSizeRel）再去 4.9 MB，但启动中位 189 → 238 ms，不采用；`-flto=auto` 在这条工具链上链接不过（16 个自家符号被判未定义），不引入。工具是 `tools/prepare-release-copy.ps1`（只动副本、留 `.debug`、写 `release-copy.json`）。构建口径（必须用 `C:/mingw64` 编译器）、证据目录和未验项见 [RELEASE_SIZE.md](perf-notes/RELEASE_SIZE.md)。

## 未覆盖

| 项 | 状态 |
| --- | --- |
| 真实下载执行期间的任务中心刷新 | 只有合成视图负载与行复用基线；下载线程/磁盘/网络争用未覆盖 |
| 更大预设体量、复杂段/模组内容、增删改后的目录重扫、单份大清单预览、超过 1000 条的内存/滚动、多盘/慢盘与外部改动一致性 | 未测；1000 条视图与读盘见"大量预设筛选与后台目录读取"和 [PRESET_LIST_IO.md](perf-notes/PRESET_LIST_IO.md) |
| 大量实例下的滚动、分组展开 | 只量了"点实例页"这一下 |
| 预设目录读取的协作式取消 | 作废只丢弃结果，不中断进行中的读取 |
| CI 分片/缓存耗时 | 账号计费问题处理前没有可测对象 |
| `repolishWidgets()` 22.5–25.7 ms | 未改，见"换主题" |
| 智能体日志搜索的冷盘与异步化 | 只量了热缓存、-O0 合成夹具；同步扫描未改 |
| 智能体模组扫描与平台查询 | 未量：2000 个 JAR 的声明读取、SHA-1、Modrinth 批量查询与下载都只在假宿主上跑过 |

任何"变快了"的说法，以这份文档里同口径的数字为准。

## 专项记录（perf-notes/）

| 文档 | 内容 |
| --- | --- |
| [REQUEST_BUDGETS.md](perf-notes/REQUEST_BUDGETS.md) | 元数据请求的并发名额与截止：渠道 × 名额 × 截止表 |
| [MAIN_THREAD_STALLS.md](perf-notes/MAIN_THREAD_STALLS.md) | 主线程卡顿：采栈、符号化、已量到的卡顿、起线程偶尔要 3 秒、后台加载时界面响应 |
| [PRESET_LIST_IO.md](perf-notes/PRESET_LIST_IO.md) | 预设列表读盘：摘要缓存、并行冷读、盘上摘要 |
| [LIST_PAINT.md](perf-notes/LIST_PAINT.md) | 大列表首绘/重列/滚动、先排 item 后挂控件、反选批量选择 |
| [RECOVERY_PERFORMANCE.md](perf-notes/RECOVERY_PERFORMANCE.md) | 恢复与续装的响应性：九步后台化 |
| [RELEASE_SIZE.md](perf-notes/RELEASE_SIZE.md) | 发布体积：去符号、`-Os`、LTO 实测 |
