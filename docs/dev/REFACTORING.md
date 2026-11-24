# 大文件职责拆分：代码布局与拆分约定

给要在 `src/`、`ui/`、`tests/` 里加代码或继续拆文件的人看：主要的大文件现在拆成了哪些翻译单元（TU）、拆分的依据、新增代码该放哪，以及拆分过程中留下的约定和坑。逐次拆分的历史压在文末附录。

行数是 2026-11-11 工作树的物理行数。

## 当前代码布局速览

### 拆分依据

按**状态所有权**拆，不按行数切：一套状态加上动它的那几条函数，同进同退，搬到同一个 TU 或控制器里。判断标准是"这一摊的状态只被这几条函数读写"，搬走不牵动别处的回调。症状上的理由是：同一组状态散在主窗口的几百个成员里时，漏掉其中一条的后果很难认（例如用户按了"忽略"，屏幕上那一行照旧写着"可更新到 X"，而名单里已经剔掉了）。

两种搬法：

1. **同一个类的定义分散到多个 `.cpp`**（`MainWindow`、`InstanceSettingsDialog`、`InstanceFilesPage`、`VersionManager`、`DownloadManager`）：类不动，断言不用改，TU 内的东西不对外。
2. **状态和动作整套搬进一个 `QObject` 控制器**（`src/controllers/`）：主窗口只留"仍然按名字被别处叫"的薄壳，状态和判据只在控制器里有一份。

### `src/MainWindow*`：窗口接线，按"主语"分 TU

`MainWindow.cpp`（1540 行）留构造、导航和下载/资源页（资源搜索、筛选、详情、下载入口）这一个领域；`MainWindow.hpp` 3029 行。其余接线按主语分在 28 个同前缀的 `.cpp`：

| 文件 | 行数 | 主语 |
| --- | ---: | --- |
| `MainWindowAccount.cpp` | 712 | 账号与外观：从开窗到登录失败弹窗 |
| `MainWindowSkin.cpp` | 241 | `PlayerSkinController` 的外观落到底栏头像、主页小人、账号窗提示 |
| `MainWindowServer.cpp` | 822 | 服务端两页：列表重扫、控制台、设置页动作、新建服务端 |
| `MainWindowSettings.cpp` | 562 | 「设置」选项卡：控件到配置键的接线 |
| `MainWindowTasks.cpp` | 372 | 任务账本 → 底栏摘要、任务中心行、取消/重试是否可用 |
| `MainWindowBottomBar.cpp` | 417 | 底栏该露什么、同步等待（`enterSyncWait()` / `leaveSyncWait()`）、下载进度投影 |
| `MainWindowInstanceTree.cpp` | 627 | 实例树 / 已安装版本 / 启动那一行；`installingVersionIds()` 是"谁现在不能启动"的唯一出处 |
| `MainWindowGroup.cpp` | 448 | 实例分组，落点都是 `InstanceGroupStore` |
| `MainWindowInstanceActions.cpp` | 297 | 一个实例本身：改名、固定、删除 |
| `MainWindowInstanceFiles.cpp` | 323 | 实例目录里的东西：页键 → 页面数据、启停/删除文件 |
| `MainWindowInstanceSettingsWiring.cpp` | 181 | 实例设置对话框要什么数据、按下之后交给谁 |
| `MainWindowInstanceEntry.cpp` | 117 | 进实例设置 / 快速游玩的唯一入口 `openInstanceSettings()` |
| `MainWindowOverlayGate.cpp` | 106 | 覆盖层占用：`overlayAcceptsNewPage()`、`runWhenOverlayFree()`、`openPageWhenIdle()` |
| `MainWindowDownloadOrigin.cpp` | 304 | 下载页来路（`m_downloadReturn` / `NavigationReturn`） |
| `MainWindowLaunch.cpp` / `MainWindowLaunchJava.cpp` | 264 / 149 | 启动与主页"最近玩的" / 启动前的 Java 与内存决定 |
| `MainWindowFolderColumn.cpp` | 171 | 左侧"游戏文件夹"那一列 |
| `MainWindowRepair.cpp` | 132 | 补齐与修复（修复文件、预取 AssetDirector 资源） |
| `MainWindowIntegrityBackup.cpp` | 130 | 实例体检与备份 / 恢复 |
| `MainWindowFileResource.cpp` | 132 | 从实例文件页点开一个资源 |
| `MainWindowResourceDependency.cpp` | 127 | 一键装前置的目标判据 |
| `MainWindowSessionEvents.cpp` / `MainWindowSessionExport.cpp` | 63 / 415 | 游戏会话事件 / 日志、崩溃、导出 |
| `MainWindowDropImport.cpp` | 131 | 拖放文件与导入整合包 |
| `MainWindowUpdateCenter.cpp` | 243 | 实例更新检测与更新中心 |
| `MainWindowEventFilter.cpp` | 257 | 画在行里的假按钮（代理画的，不是 QWidget）的事件路由 |
| `MainWindowLifecycle.cpp` | 257 | 析构 / 外框消息 / 关窗：后台活逐条等收尾 |

### `ui/`：页面，按"一页一摊"分 TU

- `MainWindowUI.cpp`（1733 行）：窗口、标题栏、四页的搭建调度；`setupUi()` 只剩外壳、按契约顺序摆四页、导航接线、底栏、覆盖层。页面实现在 `MainWindowDownloadPage.cpp`（1064）、`MainWindowSettingsPage.cpp`（1115）、`MainWindowPanels.cpp`（2366）、共享的 `MainWindowPageLayout.cpp/.hpp`（479 / 232，布局、表单/提示构造、尺寸定义只有一份）。
- `InstanceSettingsDialog`（同一个类，定义分散）：外壳 `InstanceSettingsDialog.cpp`（2153，构造、左栏导航、堆叠页、脏状态、拖放、拒绝关页）；`InstanceSettingsOverviewPage.cpp`（665）、`InstanceSettingsLaunchPage.cpp`（496）、`InstanceSettingsFilePages.cpp`（564，服务器列表 + 七份 `InstanceFilesPage`）；`InstanceSettingsLifecycle.cpp`（842）、`InstanceSettingsServers.cpp`（414）；纯函数 `InstanceSettingsSignature.{hpp,cpp}`（脏状态签名）、`InstanceSettingsPageChoices.hpp`（左栏页选择）；四个 `.cpp` 共用的搭控件小工具在 `InstanceSettingsDialogInternals.hpp`（272）。
- `InstanceFilesPage`（同一个类被"文件管理"八页复用）：外壳 `InstanceFilesPage.cpp`（718，`.hpp` 1426）；`…Rows.cpp`（790，行代理绘制与条目构建）、`…Filter.cpp`（314，搜索与状态筛选）、`…Refresh.cpp`（165，扫描，`m_entries` 的唯一产地）、`…State.cpp`（107，四档显隐）、`…Scroll.cpp`（115，重列前后的滚动锚点）、`…InfoPanel.cpp`（111，列表下"这一份是什么"，异步）、`…Structures.cpp`（125，结构/原理图来源排）、`…Capabilities.cpp`（153，能力开关与按钮文字）、`…DirectoryStats.cpp`（156，目录大小/世界上次游玩）、`…EventFilter.cpp`（357，视口上的行尾点击、悬停与拖出）、`InstanceFilesSelection.cpp`（选择、批量操作、操作栏）、`…Internals.hpp`（44）。
- `OverlayHost.{hpp,cpp}`：覆盖层与非阻塞开页（`showPage()`、`pageClosed()`、`queuePageOpen()`）。
- 新增页面实现文件要进 `ui/MainWindowPages.cmake`（产品与两个 UI 验证层共用的一张表），漏了不会编译失败，而是链接期一串 `InstanceSettingsDialog::…` 之类的 undefined reference。

### `src/controllers/`：状态所有者

117 个文件、约 66 个控制器。从主窗口搬出来的有：`UpdateCenterController`、`ServerSettingsPageController`、`InstallPageController`（含"实例预设"那一行和点「安装」的判断）、`ServerRunController`（服务端起/停/崩溃重启）、`DownloadCompletionController`（`versionDownloadFinished` 一条信号六种来路的收尾）、`DownloadTaskLedgerController`（这一批下载的账）、`ServerMenuController`、`ResourceInstallController`（把一份资源装进实例，含前置依赖检查）、`ResourcePageController`、`DownloadFilterController`、`MainWindowViewData`（视图模型，纯函数）、`ResourceSearchController`、`InstanceUpdateCheckController`、`InstallOccupancy`（四档"这一批替谁做的"）、`FileOperationOccupancy`（后台搬文件的引用计数）、`NavigationReturn`（来路）。控制器要传 `QObject(owner)` 做 parent（2026-10-29 复核发现 `ResourceInstallController` 没传，关窗时不释放）。

### 其它拆分

- `src/manage/VersionManager*`：核心 `VersionManager.cpp`（2360，版本清单 / 版本 json / 下载编排 / 批次收尾 / 实例元数据）、`…LoaderInstall.cpp`（2146）、`…Modpack.cpp`（758）、`…Export.cpp`（1051）、`…Internals.hpp`（311，原匿名命名空间里的助手）。类不动，批次收尾岔口 `onAllDownloadsFinished()` 留在核心。
- `src/download/DownloadManager*`：`DownloadManager.cpp`（2680，一条回复怎么流动）、`…Mirror.cpp`（190，去哪个源）、`…Throttle.cpp`（191，限速）、`…Segmented.cpp`（210，分段作业）、`…Metadata.cpp`（218，清单 → 作业计划，不碰网络）、`…LocalReuse.cpp`（271，本地复用与预检队列）、`…Recovery.cpp`（115）。
- `src/launcher/LaunchScriptExport.hpp`：导出 .bat 的引号规则与拼装（纯头文件，值进、文本出）。
- 测试：`tests/mw_check/modules/Support.hpp` 按"状态归谁"拆成三层（`SupportCore.hpp`、`SupportFakes.hpp`、`Support.hpp`）；`UiContentPagesChecks.cpp` 的 18 段拆进各自模块，详见 [TESTING.md](TESTING.md)「夹具生命周期」。

### 仍然较大的文件

| 文件 | 行数 | 说明 |
| --- | ---: | --- |
| `ui/MainWindowUI.hpp` | 4038 | 界面 API/状态头，页面实现已拆走 |
| `src/MainWindow.hpp` | 3029 | 成员声明；接线已分 28 个 TU |
| `ui/Theme.cpp` / `ui/ToolboxPage.cpp` | 2764 / 2670 | 未评估 |
| `src/download/DownloadManager.cpp` | 2680 | 主链（发、追字节、超时、落点探测） |
| `src/controllers/InstanceDialogController.cpp` | 2552 | 实例设置对话框的会话（`InstanceSettingsSession`） |
| `ui/MainWindowPanels.cpp` | 2366 | 面板实现 |
| `src/manage/VersionManager.cpp` | 2360 | 见上，不再按行数切 |
| `src/controllers/PresetController.cpp` | 2043 | 未评估 |

测试入口上限由 `tools/check-test-modules.ps1` 把关：入口不超过 4000 行、职责模块不超过 5000 行。

## 2026-11-05 复核：还剩什么"大文件按状态所有权拆分"

对 [主计划归档](../archive/ROADMAP_ARCHIVE_20261105.md) §4 里这一条的清单逐项核对：

| 清单里的项 | 现状 | 结论 |
| --- | --- | --- |
| 实例设置概览/启动/脏状态 | 概览/启动/文件页已是独立 TU；脏状态签名、写盘短路、左栏页选择都搬到纯函数（U9c / O13 / U9d） | 判据搬完；编辑控件按"再包一层只是纯转发"留外壳 |
| 长寿命 UI 测试夹具 | `UiContentPagesChecks.cpp` 4060 → 294 行，十八个模块 + `UiInstanceFilesTestHelpers.hpp` | 已收口；其余大测试复核无同类问题 |
| `InstanceFilesPage` 与下载协调职责 | 页里 0 处下载批次引用（`DownloadBatch*` / `installBatch`），下载协调不在这一页 | 清单描述已过时，没有可搬的东西 |
| MainWindow 安装/服务端/资源更新协调 | `MainWindow.cpp` 是下载/资源页这一个领域；服务端在 `MainWindowServer.cpp`；资源依赖两片各一百多行 | 余下的是"窗口把这些领域接起来"的协调层，没有第二个领域混进来；再按行数切只会多一层纯转发 |

这一条不再有待办。继续切 `MainWindow.cpp` 的动机只能是换一种组织方式，不是所有权边界，需要另行决定。（当日行数：`InstanceFilesPage.cpp` 682 / `.hpp` 1392，`MainWindow.cpp` 1505，`MainWindowServer.cpp` 815。）

## 约定与坑

这些是拆分过程中定下的，改相关代码时保持：

- **新增 `.cpp` 要登记在所有链接它的目标里**：根 `CMakeLists.txt`（产品）、`tests/CMakeLists.txt`（`wtfml_core_runtime`）、`tests/mw_check/CMakeLists.txt`、`tests/ui_check/CMakeLists.txt`，用到的工具再加 `tools/network_diag`、`tools/perf_download`。漏一张不在编译期报错，是链接期一串 `X::…` undefined reference。UI 页面文件走 `ui/MainWindowPages.cmake`。
- **匿名命名空间是内部链接**：把一个 `.cpp` 拆成几个 TU 时，原来顶上匿名命名空间里的助手要搬进共享的 `…Internals.hpp`，函数和 `const QString` 常量都写 `inline`（不写不在编译期报错，是链接期 duplicate symbol）。各抄一份会让"只改一处"变成静默不一致。只在一个 TU 里用的东西不要往上搬。
- **薄壳**：被别处按名字调用的主窗口方法（例如 `InstanceUpdateCheckController::finishUpdateCheck()` 和实例设置页按名字叫的几条）搬走后在主窗口留一条转发，壳不放逻辑。
- **控制器的生命周期**：状态里不留"还没建"这一档时，控制器在主窗口构造里就建，页面对象仍懒建；控制器传 `QObject(owner)` 当 parent；`~MainWindow()` 要把四趟后台活（体检 / 备份 / 图标反查 / 检查更新）逐条等收尾。
- **覆盖层与非阻塞开页**：
  - `OverlayHost::showPage(dialog, title, ownerTab[, onClosed])` 是 `execPage()` 的非阻塞兄弟，摆上去就返回；已有一页占着这一层时返回 false，且不碰正开着的那一页。所有收页路都汇到 `closeOverlay()`，收页钩子只有 `pageClosed(QWidget *)` 这一处，按页面对象认人。
  - 收页后要做的事接在对话框上当上下文（`Qt::SingleShotConnection`），不要接在 `this` 上（会每开一次多留一条永远匹配不上的连接）。
  - 在 `pageClosed` 的栈里不要直接开另一页：`QTimer::singleShot(0)` 排到下一轮。
  - `showPage()` 返回即摆好，所以 `open()` 里 `connect` 的 lambda 不能按引用捕栈上变量：`InstanceDialogController::open()` 把五个量搬进 `InstanceSettingsSession : QObject`（`setParent(dialog)`），捕获引用捕的是被引用对象，`[&dialog]` 一个字不用改。
  - 后台完成的事（更新检测回来、更新装完、体检报告）用 `openPageWhenIdle()` / `OverlayHost::queuePageOpen(show)`：被占着（整页、提示、收着的页）就排着，最多 4 发、超了丢最老的；用户自己点的才用 `runWhenOverlayFree()`（驱逐占着的页）。
  - 测试里 `deleteLater` 的页要补一发 `QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete)`，`processEvents()` 不递 `DeferredDelete`，覆盖层停放区里会留着旧页。
- **来路 `NavigationReturn`**（`src/controllers/NavigationReturn.hpp`，纯值：`rootTab` / `instanceId` / `pageKey` / `scrollIdentity` / `scrollDelta`）：主窗口上两个形状相同但寿命不同的位子，`m_downloadReturn`（这一次进下载页是从哪个实例点进来的）和 `m_instanceReturn`（回实例设置时把哪一页、滚到哪儿摆回去）。`openInstanceSettingsNow()` 在 `showPage()` 之前消费后者，对不上实例/页键整份作废；离开实例设置时当场量滚动位置（回来时是另一个对话框、另一批 `InstanceFilesPage`）；返回时先切回 `origin.rootTab` 再开页；升级入口直接带 `pageKey = "upgrade"`；详情页被别的根页的操作驱逐时，`pageClosed` 回调不再开实例设置。
- **安装占用 `InstallOccupancy`**：四档（导入 / 安装 / 补全 / 预取）的问法只在这里定义一次：`active()`（优先级 `kOrder`）、`modifying(id)` / `anyModifying()`（预取不算，它写共享的 `assets/asset_director`）、`instanceIds()`（四档全含）、`clearFor(id)`（清掉所有对得上的档，撞 id 时两档都松，否则另一档永远占着）。`setBatch()` 是唯一写入口，`Batch::None` 不是一档。预设那一侧窄的闸只认"导入"。
- **脏状态**：`InstanceSettingsSignature::signatureOf()` 的签名里"有没有值"和"值本身"都要进（`v:<值>` / `-`），否则"跟随全局"和"自定义成 4"会撞成同一份；"显示哪几页"按界面顺序，不按 `QHash` 遍历顺序。写盘前 `InstanceMetaFile::fieldsAlreadyEqual(盘上, 要写)` 一字不差就整次不写（null 只在盘上本来没有这个键时算相等），"打开设置看一眼、按确定"不推 mtime、不重扫。
- **导出启动脚本**（`LaunchScriptExport`）：含空格/制表符/空串/`&|<>()^"` 的参数要引号包住，`!` 故意不动（脚本没开延迟展开）；`%` 写两遍；注释按本地代码页写得出来，英文版 Windows 换英文；正版令牌换成 `%WTFML_MC_ACCESS_TOKEN%` 引用、原始令牌不落盘；拼完过一道已知凭据，对得上整份不写（`compose()` 返回空串）；程序名为空时裸 `java` 兜底。
- **内存档位**：两扇 `MainWindow` 各持一份 `GlobalJavaSettings` 时，B 停在"自动"的 2 秒定时器会把自动算出的 Xmx 盖回 A 刚切回"手动"的现场。`GlobalJavaSettings::save()` 的判据：本对象上一次保存时是自动档而盘上现在是手动档，说明档位是别人改的，这次不写 `Java/Xmx`、`Java/Xms`（别的项照写）。用例 `MemorySuggestionTests.cpp` 的 L5。
- **批量搬运要逐段验证**：2026-11-04 想一次再拆五段，脚本按"标记行之后第一个 `{`"配平，遇到字符串/注释里的花括号多吃了行，主文件被切碎。处理是 `git checkout` 回 HEAD，用已验证的 13 个模块文件当真相逐行回填，再跑 `ui_check` 3177/3177。下次每段单独验证、每段跑一次断言。

## 附录 A：拆分历史

只列拆分与搬迁；测试数字不在这里（见 [TEST_RUN_HISTORY.md](../archive/TEST_RUN_HISTORY.md)）。每次拆分的行为不变，原有断言保留。

| 日期 | 拆什么 | 去向 | 缝合线 |
| --- | --- | --- | --- |
| 10-25 | `MainWindow` 视图模型 | `controllers/MainWindowViewData` | 纯函数，不碰网络与盘；顺手删了九条没人调的重名助手 |
| 10-25 | 资源详情页全部状态 | `controllers/ResourcePageController` | `ResourcePageState` 整个搬走，主窗口留四条薄壳；控制器构造时就建 |
| 10-25 | 下载页顶上那一行筛子的灌与读 | `controllers/DownloadFilterController` | 灌与读只有一个门；搜索本身（请求钥匙、结果摆行）留主窗口 |
| 10-26 | `ui/MainWindowUI::setupUi()` 1590 行 | `setupHomePage/InstancePage/DownloadPage/SettingsTab`，再到 `MainWindowDownloadPage/SettingsPage/Panels/PageLayout.cpp` | 一页一摊，只共享已挂好的 `tabWidget`；`MainWindowUI.cpp` 6256 → 1602 行 |
| 10-26 | 资源搜索、更新检测 | `ResourceSearchController`、`InstanceUpdateCheckController` | 控制器持有实际发出的查询词 / 运行中的请求集合、页面弱引用和工作线程；主窗口在销毁 UI 前调用它等线程收尾 |
| 10-26 | `InstanceSettingsDialog` 构造函数 1631 行 | `setupOverviewPage / setupLaunchPage / setupFilePages`；`InstanceSettingsServers/Lifecycle.cpp` | 一栏一摊，原来共用一份 `grid` 且 `row` 从 0 数起，现在每摊自己建 |
| 10-26 | `VersionManager` 6137 行 | 核心 / `LoaderInstall` / `Modpack` / `Export` + `Internals.hpp` | 状态机的归属（`LoaderInstall` 只在安装那份里走动，`ModpackInstall` / `NamedInstance` 只在导入那份里） |
| 10-26 | `ModpackImportTests.cpp` 3272 行 | 导出格式、平台反查、打包/身份识别流水线、主题图像检查 | 整合包 354 处 check 保持顺序，仍归 core-resources |
| 10-28 ⑧ | 更新中心全部状态 | `UpdateCenterController` | 忽略/固定的账、页面对象、"这页说的是哪个实例"三样同进同退；三处重复的"重摆开着的页"收成 `refreshPageIfOpen()` |
| 10-28 ⑨ | `InstanceSettingsDialog.cpp` 3813 行 | `…OverviewPage` / `…LaunchPage` / `…FilePages` + `…Internals.hpp` | 三个"搭页"函数各自只在打开对话框时跑一遍；类不动 |
| 10-28 ⑩ | 服务端设置页六格状态 | `ServerSettingsPageController` | 页对象、说的是哪个服务端、能不能改（在跑 = 不能）、端口占用结论同进同退 |
| 10-28 ⑪ | 安装页四格状态 | `InstallPageController` | 原版号、选中的加载器、各加载器钉的版本、名称框是否被改过，共同决定树的样子和默认名 |
| 10-28 ⑫ | 「实例预设」那一行 | `InstallPageController` | 四类选择、紧凑弹窗、摘要小字同进同退；取消/前往预设管理一个字都不写 |
| 10-28 ⑬ | 服务端起/停/退出之后 | `ServerRunController` | 备份还原忙态、连崩次数、是否我们让它停的、起来时间，四格只决定两件事 |
| 10-28 ⑭ | 下载收尾与"这一趟排上了哪几份预设" | `DownloadCompletionController` | `versionDownloadFinished` 一条信号六种来路，靠"这次替谁做的"标记分辨 |
| 10-28 ⑮ | 点「安装」那一刻的判断 | `InstallPageController::installClicked()` | 原 170 行 `onInstallButtonClicked()` 读的全是这一页的状态 |
| 10-28 ⑯a | "这一批下载的账" | `DownloadTaskLedgerController` | 从 `setDownloadUiBusy(true)` 到终态信号才有意义的一套状态 |
| 10-28 ⑰ | 服务端页右键菜单 | `ServerMenuController` | 真有判断的只有两处：用不上的项不摆（装好了没有「修复」、没在跑没有「强制结束」）等 |
| 10-28 ⑱ | 把一份资源装进实例的前置判断 | `ResourceInstallController` | 落点（四类子目录或整合包缓存目录）等每条判断都带"为什么不能反过来写" |
| 10-28 ⑲ | 前置依赖检查 | `ResourceInstallController` | `DependencyCheck` 状态与 ⑱ 同一份 |
| 10-29 | 实例文件选择 | `ui/InstanceFilesSelection.cpp` | 选择状态、批量操作接线、操作栏布局；状态仍由原类持有；反选改成选择模型一次事务 |
| 10-31 | 四档"替谁做的" `QString` | `controllers/InstallOccupancy` | 判据原先被抄了好几遍，漏一档的症状是"装着的实例还能点启动" |
| 10-31 | 导出启动脚本正文 | `launcher/LaunchScriptExport.hpp` | 拼的规矩全是看不见的，原先只有开对话框、真令牌、双击才验得到 |
| 11-01 | 八页改非阻塞开页 | `OverlayHost::showPage()` | 开页后不再收结果的页（任务详情、体检报告、工具箱、更新中心、服务器控制台、服务器设置、Java 管理、账号与外观） |
| 11-02 | 分组设置、预设编辑器、资源详情、实例设置改走 `showPage()` + `pageClosed()` | `MainWindow` / `PresetController::Host` / `ResourcePageController::open(onClosed)` / `InstanceSettingsSession` | 要结果的页用 `QDialog::result()` 经回调取 |
| 11-02 | 三份散落的来路 | `controllers/NavigationReturn` | 来路原先不是一份东西，返回时根页现算 |
| 11-03 | `InstanceFilesPage.cpp` 2447 行 → 行构建 | `…Rows.cpp` | 四摊互相看不见的状态：扫描、筛选、行构建、选择 |
| 11-03 | `MainWindow.cpp` 设置选项卡接线（517 行） | `MainWindowSettings.cpp` | 只认两样：`MainWindowUI` 控件和"落到哪个键" |
| 11-03 | 服务端两页接线（747 行） | `MainWindowServer.cpp` | 一整片服务端 |
| 11-03 | 任务中心（326 行） | `MainWindowTasks.cpp` | "账本 → 界面"的唯一一道门 |
| 11-03 | 脸上那张图（196 行） | `MainWindowSkin.cpp` | 只认一张图：`PlayerSkinController` 的外观 |
| 11-03 | 实例树 / 已安装版本 / 启动行（595 行） | `MainWindowInstanceTree.cpp` | 界面上列出的实例和版本 |
| 11-03 | 账号与外观（655 行） | `MainWindowAccount.cpp` | "现在是谁、他长什么样" |
| 11-03 | 实例分组（411 行） | `MainWindowGroup.cpp` | 落点都是 `InstanceGroupStore` |
| 11-03 | 实例这一行能做的事（262 行） | `MainWindowInstanceActions.cpp` | 一个实例本身，不是它的文件或分组 |
| 11-03 | 实例文件与设置的读写（282 行） | `MainWindowInstanceFiles.cpp` | 某个实例目录里的东西 |
| 11-03 | 日志 / 崩溃 / 导出（317 行） | `MainWindowSessionExport.cpp` | "刚刚那一次发生了什么"；会话一结束启动事实和日志就没了，要先摆好 |
| 11-03 | 实例设置背后的页面数据与动作（140 行） | `MainWindowInstanceSettingsWiring.cpp` | 对话框要什么、按下去交给谁 |
| 11-03 | 下载页来路（265 行） | `MainWindowDownloadOrigin.cpp` | 只有 `m_downloadReturn` 一件事 |
| 11-03 | 启动与主页"最近玩的"（225 行） | `MainWindowLaunch.cpp` | 这一次要不要启动、启动后窗口怎么收 |
| 11-03 | 左侧游戏文件夹列（111 行） | `MainWindowFolderColumn.cpp` | 现在用的是哪个游戏文件夹 |
| 11-03 | 启动前 Java 与内存决定（116 行） | `MainWindowLaunchJava.cpp` | 这一次启动的 Java 与内存 |
| 11-03 | 补齐与修复（103 行） | `MainWindowRepair.cpp` | 还差什么、现在能不能补；共用守卫 |
| 11-03 | 体检与备份/恢复（96 行） | `MainWindowIntegrityBackup.cpp` | 体检四发 + 备份恢复 |
| 11-03 | 从文件页点开资源（101 行） | `MainWindowFileResource.cpp` | 只有 `InstanceFiles` 来路才露"模组设置" |
| 11-03 | 进实例设置 / 快速游玩（85 行） | `MainWindowInstanceEntry.cpp` | 用户要从哪一页进那个实例 |
| 11-03 | 覆盖层能不能开新页（77 行） | `MainWindowOverlayGate.cpp` | 覆盖层的占用 |
| 11-03 | 游戏会话事件（39 行） | `MainWindowSessionEvents.cpp` | 主窗口与 `GameSessionController` 之间的单线 |
| 11-03 | 导入整合包与拖放（52 行） | `MainWindowDropImport.cpp` | 用户扔进来一个文件；同一文件落在不同页意思不同 |
| 11-03 | 更新检测与更新中心（202 行） | `MainWindowUpdateCenter.cpp` | 一条顺序不能换的链：检测 → 回调 → 更新中心 |
| 11-03 | 底栏 / 同步等待 / 下载进度投影（342 行） | `MainWindowBottomBar.cpp` | 底栏这会儿该露什么 |
| 11-03 | 行内小按钮事件路由（220 行） | `MainWindowEventFilter.cpp` | 画在行里的假按钮靠视口过滤器认"点中了没" |
| 11-03 | 窗口生命周期（202 行） | `MainWindowLifecycle.cpp` | 窗口要没的时候什么不能半路丢下 |
| 11-03 | 一键装前置的判据（90 行） | `MainWindowResourceDependency.cpp` | 点"装上前置"那一下目标是什么；详情页开着的实例优先，判据带 `open` 免得旧值顶替下拉框 |
| 11-03 | `InstanceFilesPage.cpp` 余下各摊 | `…Filter / Scroll / InfoPanel / Structures / Capabilities / DirectoryStats / Refresh / State / EventFilter.cpp` | 各自一套状态：筛选条件、`m_scrollAnchor`、`m_infoPath` 与过期答案、每趟现算排数的来源下拉框、能力开关、目录统计、`m_entries` 唯一产地、四档显隐、悬浮操作条（不在布局里）的悬停 |
| 11-03 | `DownloadManager` 五层 | `…Mirror / Throttle / Segmented / Metadata / LocalReuse.cpp` | 各层状态只被自己那一组函数动，搬走不牵动网络主链：下载源（只读不写运行状态）、限速（`m_speedBucket` 等）、分段（`m_segmentJobs`）、清单读成作业清单（不碰网络）、先问盘（借文件 + 整份核对） |
| 11-03 | `mw_check` 夹具头 `Support.hpp` 2400 行 | 三层 `SupportCore / SupportFakes / Support` | 按"谁拥有那份状态"，顶层名字和包含方式不变 |
| 11-04 | `InstanceSettingsDialog` 脏状态判据 | `ui/InstanceSettingsSignature` | 验证工程够不着对话框里的私有函数；挪成纯函数后 U9c 可测 |
| 11-04 | 内存档位判据 | `GlobalJavaSettings::save()` | 见"约定与坑" |

## 附录 B：同期顺带修的缺陷与小决策

| 日期 | 编号 | 位置 | 修了什么 |
| --- | --- | --- | --- |
| 10-31 | — | `OverlayHost` | 后台完成的事不再驱逐用户正看着的那一页（`queuePageOpen`） |
| 11-03 | R1 | `ResourceStore` | `LatestRun::replies` 改存 `QPointer<ControlGet>`，避免一发已回、一发在飞时再开批或关窗对已析构对象再 `delete` |
| 11-03 | R2 | `util/NbtTree.cpp` | 加 `kMaxNodes`（1<<20）与"项数 × 最少字节 ≤ 剩余字节"检查，`reserve` 只按 `kMaxReserve`（4096）一次要；`itemType == End` 却 count > 0 直接拒 |
| 11-03 | R3 | `ResourcePageController` | 详情页前置名字空回信死循环：问过的号记进 `dependencyAskedIds`，收页/换资源清空 |
| 11-03 | R4 | `ServerListFile.cpp` | `servers.dat` 空 `List(End, 0)` 读进来原地补成 `Compound`，避免写盘后变成 `List(End, count>0)` 谁都不认 |
| 11-03 | R5 | `FileOperationRecoveryController` | 续做按报告钥匙挡第二次；成功后摘掉原任务的恢复入口 |
| 11-03 | R6 | `ResourceInstallController` | 光影的前置一律按实例 `mods/` 算，补 `busyInstanceId` |
| 11-03 | R7 | `ModDependencies.cpp` + `ResourceInstallController` | `requiredDependenciesOf()` 按 projectId 去重；"还差几发"改成钥匙集合 `pendingKeys`（`ResourceStore` 同一钥匙第二发会掐掉第一发且不回调，计数模型会停在 1） |
| 11-03 | R8 | `AccountStore.cpp` | 凭据齐着但列表读不出/写不进时，切离线/登出直接中止（`activeCredentialWouldBeLost()`），不清唯一一份 refresh_token |
| 11-03 | R13 | `TokenStore` / `AccountStore` / `MainWindowAccount.cpp` | `TokenStore::clear()` 返回 `bool` + `errorOut`，三个键都清掉才算成功；登出/删除不再永远报成功 |
| 11-03 | R15 | `AccountStore::activate()` | 第二步失败时把微软那格原始字节放回去，放不回去单独报"请重新登录" |
| 11-03 | R14 | `MainWindowLifecycle.cpp` | 关窗记下没停下的服务端，日志照实说，不再无条件说"已经停下来" |
| 11-03 | R9 | `ServerModel.hpp` | `server.properties` 键和值按 Java Properties 转义（`\uXXXX`、`\\`、`\t\n\r\f`；键里另转 `=:#!`）；分隔符取第一个未被转义的 `=` / `:`；中文 MOTD 不再乱码 |
| 11-03 | R10–R12 | `ZipUtil`、`FileHash` | 压缩炸弹判据（单条 ≤ 2 GiB、压缩比 ≤ 1000 且只判 > 1 MiB 的条目、一批累计 ≤ 16 GiB）；`readEntry()` 默认上限 16 MiB；`readAlgorithmPrefix()` 认 `md5 <hex>` 与 `SHA-256 = xxxx` |
| 11-03 | R16 | `Properties::setValue()`、`UpdateRollback::create()`、`VersionDisplay::sanitizeComponent()`、`ReplayInfo::integerOf()` | 大小写不敏感改最后一个；清单量抄下来的副本；截断点撞代理对退一格；越界/非有限值回 -1 |
| 11-03 | O1–O4 | `ThemeSettingsController`、`ResourceStore`、`DownloadManager::onAssetIndexDownloaded`、`ModpackExport` | 不透明度滑块 60 ms 防抖（`applySpecThrottled()`，析构补落盘）；批次收尾 `std::move`；资产索引 `QSaveFile` + 256 KiB 分块写；`wtfml.meta` 走 `AppPaths::versionMetaFileName()` |
| 11-03 | — | 底栏 | 左下角把服务端也算"在跑"（`ServerWorkspaceController::runningServerIds()`），"暂无运行实例"改"暂无实例或服务端" |
| 11-03 | V2 | `ZipUtil::createZip()` | 写任何字节之前先看目标：`.NET` 的 `ZipFile.Open(...,'Create')` 是"打开并清空"，失败路径删半成品会毁掉盘上原来的好包 |
| 11-03 | V3 | zip 开头四字节判据 | 流式落盘、分段合并、本地复用三处收成一处（`PK\x03\x04`、`PK\x05\x06` 等） |
| 11-03 | V1 | `net/ControlGet.hpp` | 截止定时器不再丢弃已完成、`finished` 尚未派发的回复（原先会把一份完整正文报成"等待网络请求超时"），见 [REQUEST_BUDGETS.md](perf-notes/REQUEST_BUDGETS.md) |

回归用例：R1 `ResourceStoreLatestRunTests.cpp`、R2 `NbtLimitsTests.cpp`、R3 `runResourceDetailPageTests`、R4 `ServerListEditTests.cpp`、R7 `runDependencyAndBatchTests`、账号边界 `AccountSkinTests.cpp`、R9/R16 `ServerModelTests` `FileHashTests` `VersionNameTests` `ReplayInfoTests`。

## 附录 C：当时的"尚未收口"清单

2026-11-05 之前的待办（MainWindow 剩余协调职责、安装占用四格、实例设置脏状态、长寿命 UI 夹具、VersionManager 是否继续切）全部已收口或结论为不切，见上文"2026-11-05 复核"。原计划条目在 [主计划归档](../archive/ROADMAP_ARCHIVE_20261105.md) §4，审查记录在 [REVIEW_LOG.md](../archive/REVIEW_LOG.md)，嵌套事件循环与实例元数据盘点在 [NAV_AND_META_AUDIT.md](../archive/NAV_AND_META_AUDIT.md)。
