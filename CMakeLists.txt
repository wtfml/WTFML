cmake_minimum_required(VERSION 3.16)
project(WTFMinecraftLauncher VERSION 0.6.2 LANGUAGES CXX)
include(${CMAKE_CURRENT_SOURCE_DIR}/ui/MainWindowPages.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/Zc:__cplusplus)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Core Gui Concurrent GuiPrivate)

# ---------------------------------------------------------------------------
# 界面多语言（英文）：assets/i18n/wtfml_en.ts → lrelease → 编进资源
# ---------------------------------------------------------------------------
# 源语言是中文，英文那一份是译文（流程见 docs/dev/I18N.md）。这一段的规矩：
#   * 有 Qt6LinguistTools 就用它给的 lrelease；
#   * 没有（静态 Qt 只编了 qtbase，没有 LinguistTools）就用缓存变量 WTFML_LRELEASE 指的
#     那一个，本机默认位置见下面的 find_program；
#   * 两个都找不到 → **配置阶段直接失败**。悄悄出一个没有英文的版本比配置失败更糟：
#     用户切了 English、界面还是中文，谁也说不清是哪一层坏了。
#   * 只有显式 -DWTFML_I18N=OFF 才允许跳过。
option(WTFML_I18N "Build and embed UI translations (wtfml_en.qm)" ON)

if(WTFML_I18N)
    # 这一个只是"我自己指一个"的口子（-DWTFML_LRELEASE=<path>）。不设它会自己找：
    # 先看 Qt6LinguistTools 给的 lrelease 目标，再在几个默认位置里找（见下面 find_program）。
    # NOTE: 别把它先建成一个空缓存项 —— find_program 见到该变量已定义就不搜了，
    # 静态 Qt 那一趟会直接报"找不到 lrelease"。
    set(_wtfml_lrelease "")
    if(WTFML_LRELEASE)
        set(_wtfml_lrelease "${WTFML_LRELEASE}")
    else()
        find_package(Qt6 QUIET COMPONENTS LinguistTools)
        if(TARGET Qt6::lrelease)
            set(_wtfml_lrelease "$<TARGET_FILE:Qt6::lrelease>")
        else()
            # 本机默认查找位置：**当前这套 Qt 自己的 bin**（qmake 在哪、lrelease 就在哪）、
            # QTDIR，以及这台开发机上那份动态 Qt —— 静态 Qt（static-6.10.1-mingw13）只编了
            # qtbase，没有 lrelease。
            get_target_property(_wtfml_qmake Qt6::qmake IMPORTED_LOCATION)
            if(_wtfml_qmake)
                get_filename_component(_wtfml_qt_bin "${_wtfml_qmake}" DIRECTORY)
            endif()
            find_program(_wtfml_lrelease_found NAMES lrelease lrelease.exe
                HINTS "${_wtfml_qt_bin}" "${QT6_INSTALL_PREFIX}/bin" "$ENV{QTDIR}/bin"
                PATHS "D:/Qt/Qt/6.10.1/mingw_64/bin")
            if(_wtfml_lrelease_found)
                set(_wtfml_lrelease "${_wtfml_lrelease_found}")
            endif()
        endif()
    endif()
    if(NOT _wtfml_lrelease)
        message(FATAL_ERROR
            "找不到 lrelease，界面英文翻译无法生成。三条路选一条：
"
            "  1. 装 Qt6 的 LinguistTools（动态 Qt 自带）；
"
            "  2. 用 -DWTFML_LRELEASE=<lrelease.exe 的完整路径> 指一个；
"
            "  3. 确实不要英文界面时显式 -DWTFML_I18N=OFF。")
    endif()

    set(WTFML_TS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/i18n/wtfml_en.ts")
    set(WTFML_I18N_QM_DIR "${CMAKE_CURRENT_BINARY_DIR}/i18n")
    set(WTFML_QM_FILE "${WTFML_I18N_QM_DIR}/wtfml_en.qm")
    add_custom_command(OUTPUT "${WTFML_QM_FILE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${WTFML_I18N_QM_DIR}"
        COMMAND "${_wtfml_lrelease}" "${WTFML_TS_FILE}" -qm "${WTFML_QM_FILE}"
        DEPENDS "${WTFML_TS_FILE}"
        COMMENT "lrelease: wtfml_en.qm"
        VERBATIM)
    add_custom_target(wtfml_translations DEPENDS "${WTFML_QM_FILE}")

    # 把那一份 .qm 编进指定目标。产品挂一次，验证工程要验英文界面的再挂一次
    # （AUTORCC 漏了的表现是"编过了、界面上没有英文"，见 ui/ 那份 CMakeLists 里的同一条）。
    function(wtfml_embed_translations target)
        if(NOT WTFML_I18N)
            return()
        endif()
        qt_add_resources(${target} "wtfml_i18n"
            PREFIX "/i18n"
            BASE "${WTFML_I18N_QM_DIR}"
            FILES "${WTFML_QM_FILE}")
        add_dependencies(${target} wtfml_translations)
    endfunction()
endif()

# The launcher used to keep its verification programs under build/, which meant
# they were invisible to source control and to CTest.  Keep normal developer
# builds lean, but provide one switch that enables the complete regression suite.
option(WTFML_BUILD_TESTS "Build the WTFML regression test executables" OFF)
option(WTFML_STRICT_WARNINGS "Enable additional compiler diagnostics" OFF)
option(WTFML_WARNINGS_AS_ERRORS "Treat launcher warnings as build errors" OFF)
# 特供版：把下载链路的每一条请求 / 每一次换源与重试的**决定**都记进
# `<数据目录>/logs/wtfml-download.log`（见 src/util/DownloadDiagnostics.hpp）。
# 默认关：普通构建一行都不多写（enabled() 缓存成常量 false）。
option(WTFML_DIAG_DOWNLOAD "Always-on verbose download diagnostics (special diagnostic build)" OFF)
# 测量构建：只用来量界面的真实耗时（真 Release）。默认 OFF —— 普通构建里
# `PerfProbe::attach()` 是内联空函数，`src/perf/PerfProbe.cpp` 不参与编译。
# 为什么不能拿验证工程量：那几个工程按设计是 -O0（见 tests/mw_check/CMakeLists.txt），
# 数字不能代表产品。量法与既有基线见 docs/dev/PERFORMANCE.md。
option(WTFML_PERF_PROBE "Build the UI perf probe (measurement-only Release build)" OFF)
# 并发下载基准：本机起 HTTP 服务器，用真的 DownloadManager 下一批文件，量调度/写盘/
# 主线程空档。和上面那条一样**只做测量**：默认 OFF、不进 ctest、不进发布物。
# （产品 exe 没有假网络注入点，验证工程又是 -O0 —— 所以另开一个只含下载引擎的 Release 工具。）
option(WTFML_PERF_DOWNLOAD "Build the concurrent-download benchmark tool (measurement-only)" OFF)
# 预编译头：把每个 TU 都要重新解析的那一截 Qt 头只解析一次（量法与取舍见
# docs/dev/PERFORMANCE.md「共享头扇出的时间成本与预编译头」那一节）。默认 ON —— 它只影响
# 构建耗时，不影响产物：PCH 只是"提前把这份头的解析结果存盘"，编出来的代码和不用它时相同。
# 关掉它：`-DWTFML_USE_PCH=OFF`。什么时候要关：换编译器/工具链排查奇怪的警告，
# 或者盘太紧（这份 .gch 本机 177 MB，在构建目录里、不进仓库）。
option(WTFML_USE_PCH "Precompile the shared Qt header for every launcher TU (ignored under ccache)" ON)
# 编译缓存：本机装了 ccache 就默认自动套上，不用每次配置都记得加参数。
# CI 通过环境变量 CMAKE_CXX_COMPILER_LAUNCHER=sccache 指定缓存，那种情况这里不动。
# 套着 ccache 时，主程序不再用上面那份 PCH：GCC 的 .gch 每个构建目录各编一份、内容都不一样
# （11-14 实测两个工作树的 .gch md5 不同），带着它，主程序在别的工作树里一个 TU 都命中不了。
# 想退回：-DWTFML_USE_CCACHE=OFF。
option(WTFML_USE_CCACHE "Use ccache as the compiler launcher when it is installed" ON)
if(WTFML_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(WTFML_CCACHE_PROGRAM ccache)
    if(WTFML_CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${WTFML_CCACHE_PROGRAM}")
    endif()
endif()
set(WTFML_LAUNCHER_NAME "")
if(CMAKE_CXX_COMPILER_LAUNCHER)
    list(GET CMAKE_CXX_COMPILER_LAUNCHER 0 WTFML_LAUNCHER_PROGRAM)
    get_filename_component(WTFML_LAUNCHER_NAME "${WTFML_LAUNCHER_PROGRAM}" NAME_WE)
endif()
if(WTFML_LAUNCHER_NAME STREQUAL "ccache")
    # base_dir 设成本仓库根目录，直接写在 ccache 的命令行上：仓库里的路径都改写成相对构建目录的
    # 路径，主仓和各工作树才能互相命中。不靠环境变量 CCACHE_BASEDIR，因为长期开着的进程
    # （同席）拉起的成员只带启动那一刻的旧值（11-14 就是这样：旧值不含 D:\AI，跨工作树全不命中）。
    list(INSERT CMAKE_CXX_COMPILER_LAUNCHER 1 "base_dir=${CMAKE_SOURCE_DIR}")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(HEADER_FILES
    src/MainWindow.hpp
    src/perf/PerfProbe.hpp
    src/perf/StallSampler.hpp
    src/controllers/JavaRuntimeController.hpp
    src/controllers/JavaSettingsController.hpp
    # 实例预设管理页的胶水。
    src/controllers/PresetController.hpp
    src/controllers/LauncherPreferencesController.hpp
    src/controllers/CrashReportController.hpp
    src/controllers/IntegrityController.hpp
    src/controllers/BackupController.hpp
    src/controllers/ServerPingController.hpp
    src/controllers/FolderController.hpp
    src/controllers/ModpackExportController.hpp
    src/controllers/InstanceFileController.hpp
    src/controllers/InstanceTransferController.hpp
    # 「继续未完成操作」：按 OperationReport 把没做完的文件操作补完。
    src/controllers/FileOperationRecoveryController.hpp
    src/controllers/InstanceDialogController.hpp
    src/controllers/InstanceEditorController.hpp
    src/controllers/ImportController.hpp
    src/controllers/GameSessionController.hpp
    src/controllers/LaunchController.hpp
    src/controllers/ConnectionsController.hpp
    src/controllers/InstanceIconController.hpp
    src/controllers/ActivityCenterController.hpp
    src/controllers/TaskDetailController.hpp
    src/controllers/HomeQuickController.hpp
    src/controllers/DailyQuoteController.hpp
    src/controllers/InstanceMaintenanceController.hpp
    src/controllers/ServerWorkspaceController.hpp
    src/controllers/ServerConsoleController.hpp
    src/controllers/ServerSettingsController.hpp
    src/controllers/ThemeSettingsController.hpp
    src/controllers/PlayerSkinController.hpp
    src/controllers/LaunchHistoryController.hpp
    src/controllers/InstanceSettingsController.hpp
    src/controllers/InstancePagesController.hpp
    src/controllers/ServerListController.hpp
    src/launcher/LauncherManager.hpp
    src/launcher/LibraryRules.hpp
    # 「导出启动脚本」那份 .bat 的正文（纯值进、文本出，验证工程逐条钉住）。
    src/launcher/LaunchScriptExport.hpp
    src/launcher/ProcessPriority.hpp
    src/launcher/VersionInheritance.hpp
    src/auth/AuthManager.hpp
    src/auth/AccountStore.hpp
    src/auth/OAuthEndpoints.hpp
    src/auth/SkinApi.hpp
    src/auth/TokenStore.hpp
    src/util/SecretProtector.hpp
    src/agent/AgentTypes.hpp
    src/agent/AgentSettings.hpp
    src/agent/AgentPaths.hpp
    src/agent/AgentRedaction.hpp
    src/agent/AgentSecretStore.hpp
    src/agent/AgentSessionCrypto.hpp
    src/agent/AgentSession.hpp
    src/agent/AgentSessionStore.hpp
    src/agent/AgentSseParser.hpp
    src/agent/AgentProviderAdapters.hpp
    src/agent/AgentModelClient.hpp
    src/agent/OpenAiCompatibleClient.hpp
    src/agent/AgentContextBuilder.hpp
    src/agent/AgentLoop.hpp
    src/agent/AgentSkill.hpp
    src/agent/AgentPromptLibrary.hpp
    src/agent/AgentWorkspaceStore.hpp
    src/agent/AgentLaunchHost.hpp
    src/agent/AgentTestLaunch.hpp
    src/agent/AgentTestLaunchToolEngine.hpp
    src/agent/AgentRepairHost.hpp
    src/agent/AgentRepairTransaction.hpp
    src/agent/AgentRepairToolEngine.hpp
    src/agent/AgentWinPath.hpp
    src/agent/AgentEgressPolicy.hpp
    src/agent/AgentArtifactStore.hpp
    src/agent/AgentBackupIndex.hpp
    src/agent/AgentInstanceLock.hpp
    src/agent/AgentFileTools.hpp
    src/agent/AgentTool.hpp
    src/agent/AgentApprovalPolicy.hpp
    src/agent/AgentFileToolEngine.hpp
    src/agent/AgentDiagnosticProjection.hpp
    src/agent/AgentLauncherHost.hpp
    src/agent/AgentLauncherTools.hpp
    src/agent/AgentWorkflow.hpp
    src/agent/AgentWorkflowEngine.hpp
    src/agent/AgentCompositeToolEngine.hpp
    src/agent/McpTypes.hpp
    src/agent/McpProtocol.hpp
    src/agent/McpTrustStore.hpp
    src/agent/McpClient.hpp
    src/agent/McpManager.hpp
    src/agent/AgentMcpToolEngine.hpp
    src/agent/AgentInstancePaths.hpp
    src/agent/AgentKnowledgeRules.hpp
    src/agent/AgentLogIndex.hpp
    src/agent/AgentCrashSnapshot.hpp
    src/agent/AgentMixinOwnership.hpp
    src/agent/AgentWebPolicy.hpp
    src/agent/AgentWebTransport.hpp
    src/agent/QtAgentWebTransport.hpp
    src/agent/AgentWebTools.hpp
    src/agent/AgentCommandTools.hpp
    src/controllers/AgentController.hpp
    src/controllers/AgentLauncherHostImpl.hpp
    src/controllers/AgentTestLaunchRunner.hpp
    src/controllers/AgentRepairRunner.hpp
    ui/AgentMarkdown.hpp
    ui/AgentViewTypes.hpp
    ui/AgentMessageView.hpp
    ui/AgentComposer.hpp
    ui/AgentPage.hpp
    ui/AgentSecretEdit.hpp
    ui/InputSizing.hpp
    ui/AgentSettingsPage.hpp
    ui/AgentToolCard.hpp
    ui/AgentPanelPaint.hpp
    ui/AgentApprovalCard.hpp
    ui/AgentMcpPage.hpp
    src/net/HttpClient.hpp
    src/manage/DroppedResource.hpp
    src/manage/FolderManager.hpp
    src/manage/InstanceFiles.hpp
    src/manage/InstanceIconLookup.hpp
    src/manage/ModpackImport.hpp
    src/manage/ModpackExport.hpp
    src/manage/InstanceExport.hpp
    src/manage/ServerListFile.hpp
    src/manage/ServerStatusCache.hpp
    src/manage/VersionManager.hpp
    src/manage/VersionManagerInternals.hpp
    src/manage/VersionDisplay.hpp
    src/manage/VersionIntegrity.hpp
    src/manage/UpdatePolicy.hpp
    src/manage/UpdateRollback.hpp
    src/manage/BackupFiles.hpp
    # 「人工恢复目录」：人挑一份旧目录把现在这一份换掉，两边都保留。
    src/manage/ManualRecovery.hpp
    src/manage/WorldFiles.hpp
    # 世界只读概况：种子 / 玩家位置 / 已生成区域 / 大小。
    src/manage/WorldMap.hpp
    # 启动历史账本：主页“最近游玩”排与统计的数据源。
    src/manage/LaunchHistory.hpp
    # 实例页那张树的搜索与筛选：判据层，纯函数。
    src/manage/InstanceFilter.hpp
    # 实例预设：先落「值」和「账」两层，
    # 段里那点载荷归各个适配器，界面那一半等 P1 的管理页上线时再接。
    src/presets/PresetTypes.hpp
    src/presets/PresetStore.hpp
    # 原版设置那一段：`options.txt` 的保序模型 + 认得的
    # 键那张表 + 按键冲突 + "这一段怎么落到实例上"的计划。纯值层（字节进、字节出）。
    src/presets/VanillaOptions.hpp
    # 服务器列表那一段：`servers.dat` 的判据 —— 地址怎么算
    # 同一台、Append / Replace / Skip 各是什么、这一趟会动哪几条。写盘那一半仍旧只有一处
    # 实现（`src/manage/ServerListFile` 露出来的那几口），这一层一个 QFile 都不开。
    src/presets/ServerListPreset.hpp
    # 通用文件组那一段：`config/` / `defaultconfigs/` /
    # 资源包 / 光影 —— 我们看不懂里面的东西，所以只做整份的动作（缺了才拷 / 覆盖 / 跳过），
    # 三道路径闸和"这一项只能复制或覆盖"那记号都在这一份里。
    src/presets/GenericFiles.hpp
    # MaLiLib 那一族的热键：`config/` 底下
    # `malilib.json` / `litematica.json` / `tweakeroo.json` / `minihud.json` / `itemscroller.json`
    # 那一族 —— **按结构认**（不看文件名，所以 TweakerMore / Masa Gadget / OMMC 这些扩展照样认
    # 得出来），身份是 `modId/groupId/optionName`，默认只搬热键、可选连开关和别的值一起搬。
    src/presets/MalilibKeys.hpp
    # 把一份预设落到一个实例上这件事的**计划**那一层：一段一行，
    # 按段的种类分派给上面三个适配器，再把各自的报告翻成同一张表。**只读，一个字节都不写。**
    src/presets/PresetPlanner.hpp
    # 提取那一趟：四个适配器各会收自己那
    # 一摊，但"先收哪几样、收不出来怎么报"是一条流程，只在这一处。
    src/presets/PresetCapture.hpp
    # 落到实例上那一趟：快照 → 计划 → 备份 → 执行。
    # 两趟确认协议照 `PresetPlanner` 那一份实现，别的层不再立第二套。
    src/presets/PresetApply.hpp
    # 模组预设的兼容检查：先本地判、再查缓存、
    # 最后有限并发问平台。★ 网络失败 / 429 / 超时只算「暂无法确认」，**不计入缺失**。
    src/presets/PresetCompatibility.hpp
    # 真网络版的平台查询（Modrinth 异步；CurseForge 要 key，没配置就一律「暂无法确认」）。
    src/presets/ModrinthCompatibility.hpp
    # 结构 / 原理图的元数据与方块统计：
    # Litematica / Sponge / 原版结构三种格式，只读盘、只算。
    src/schematic/SchematicInfo.hpp
    # Replay 录像（`.mcpr`）的元数据：
    # 只读 zip 里的 metaData.json，不读那份几百 MB 的 recording.tmcpr。
    src/replay/ReplayInfo.hpp
    # 日志文件（含 .log.gz）的读行与数行，给内置日志查看器用。
    src/util/LogFileReader.hpp
    # 启动诊断：
    # 版本 / 加载器 / Java / 内存 / 模组 / 日志 / 崩溃报告，只读盘、只算。
    src/diagnose/StartupDiagnosis.hpp
    # 迁移 / 升级副本：计划只读盘，落盘在
    # TransferApply。目标版本 / 加载器不一致时如实说不一致，不替用户猜。
    src/operations/TransferPlanner.hpp
    # 迁移式升级的九类选择：稳定键、默认值、指纹与依赖提示（纯逻辑）。
    src/operations/UpgradeSelection.hpp
    src/operations/UpgradePageState.hpp
    src/operations/UpgradeReport.hpp
    src/operations/UpgradeHistory.hpp
    src/operations/UpgradeOptions.hpp
    src/operations/TransferApply.hpp
    # 一次文件操作的统一报告（逐项结果 / 冲突策略 / 可续暂存 / 失败原因）。
    src/operations/OperationReport.hpp
    src/settings/GlobalJavaSettings.hpp
    src/settings/GameWindowSettings.hpp
    src/settings/LauncherSettings.hpp
    src/download/DownloadManager.hpp
    src/download/LocalFileReuse.hpp
    src/download/AssetDirectorKnownSet.hpp
    src/download/DownloadSpeedLimit.hpp
    src/download/DownloadProxy.hpp
    src/download/LoaderMeta.hpp
    src/download/ServerInstall.hpp
    src/download/ResourceCatalog.hpp
    src/download/ModDependencies.hpp
    src/download/ResourceStore.hpp
    # 任务中心那一套：账本的形状（TaskSnapshot / TaskId，
    # 纯值 + 几条纯函数）、账本自己（TaskManager）、以及它的落盘（TaskHistory）。
    src/tasks/TaskId.hpp
    src/tasks/TaskSnapshot.hpp
    src/tasks/TaskManager.hpp
    src/tasks/TaskHistory.hpp
    src/diag/GameExitDiagnosis.hpp
    # 服务端工作区：一个服务端是什么（判据层，
    # 纯函数）、盘上那一份谁在住（扫目录 + 读写 .wtfml-server）。
    src/server/ServerModel.hpp
    src/server/ServerRegistry.hpp
    # 服务端安装那四段：判据层（纯头：还差哪
    # 一份元数据、取回来怎么读、分成几步）+ 驱动层（取元数据 → 下载 → 跑安装器 → 核对）。
    src/server/ServerInstallRun.hpp
    src/server/ServerInstallController.hpp
    # 服务端的启动 / 停止那一套：判据层（纯头：那条命令怎么拼、起来没有、怎么停、宽限多久）+ 驱动层
    # （一条 QProcess，一个服务端一份）。
    src/server/ServerRun.hpp
    src/server/ServerProcess.hpp
    # 「服务端设置」那几层的判据：
    # `server.properties` 常用字段那张表与它的值域（纯）、端口占用那一次真监听（薄）、
    # 体检（纯值进、结论出）。
    src/server/ServerPropertiesForm.hpp
    src/server/ServerPortCheck.hpp
    src/server/ServerCheck.hpp
    # 服务端那四份玩家名单：纯解析 / 纯拼 JSON。
    src/server/ServerPlayers.hpp
    # 异常退出后自动重启那一条策略：纯判据。
    src/server/ServerRestart.hpp
    # 服务端资源包那一份本地 zip 的判据 + SHA-1。
    src/server/ServerResourcePack.hpp
    # 服务端内容（模组 / 数据包）的名字与分类判据：纯函数。
    src/server/ServerContent.hpp
    src/util/AppPaths.hpp
    src/util/ThemeImage.hpp
    src/util/DailyQuote.hpp
    src/util/HomeGreeting.hpp
    src/util/ImageDecode.hpp
    src/util/SystemMemory.hpp
    # 掉盘时那句话：卷认得出来且已就绪才算可用（见头文件）。
    src/util/StorageAvailability.hpp
    src/util/MemoryBudget.hpp
    src/util/FileAssociations.hpp
    src/util/SevenZip.hpp
    src/util/ZipUtil.hpp
    src/util/PowerShell.hpp
    src/util/NbtUtil.hpp
    src/util/NbtTree.hpp
    src/util/ServerPing.hpp
    src/util/ErrorReport.hpp
    src/util/DownloadDiagnostics.hpp
    # 几个完整句子接成一段：中文直接相接，英文那一个空格在译文里。
    src/util/SentenceJoin.hpp
    # 工具箱那五件：纯逻辑层，各自的"照什么规矩、不做什么"
    # 都写在头文件开头那一段里。
    src/tools/TextFormat.hpp
    src/tools/CoordConvert.hpp
    src/tools/FileHash.hpp
    src/tools/AnvilPlanner.hpp
    # 工具箱另外四件（信标计算 / 酿造助手 / 旗帜设计 / 纹饰搭配）：同一层纯逻辑。
    src/tools/Beacon.hpp
    src/tools/Brewing.hpp
    src/tools/Banner.hpp
    src/tools/ArmorTrim.hpp
    # 「目标版本」那一栏的共用版本表与比较规则（旗帜 / 酿造共用这一份）。
    src/tools/GameVersion.hpp
    ui/ConfigFileText.hpp
    ui/ConfigFileEditorPage.hpp
    ui/WorldSettingsPage.hpp
    ui/GameRuleNames.hpp
    # 运行输出共用模型：游戏日志窗和服务端控制台
    # 共用"认级别 / 计数 / 搜索 / 行数上限 / 导出"这一份。
    ui/RuntimeOutputModel.hpp
    ui/Theme.hpp
    ui/ThemeBackground.hpp
    ui/Glass.hpp
    ui/ConfigEntries.hpp
    ui/MotdText.hpp
    ui/ToggleSwitch.hpp
    # 工具箱那一整页（五件小工具共用一页 + 顶上那排 chip，见它文件头那一段）。
    ui/ToolboxPage.hpp
    # 「服务端设置」那一整页。
    ui/ServerSettingsPage.hpp
    ui/JavaManageDialog.hpp
    # 实例预设管理页：
    # 左侧列表 + 右侧摘要 / 提取 / 应用计划，全部在覆盖层里，不另开窗。
    ui/PresetManagePage.hpp
    # 安装页「预设」那一行点开的紧凑弹窗：
    # 四类各挑一份、计划与原因可展开，**不跳页**。
    ui/PresetInstallPicker.hpp
    # 底栏中间那格点开之后弹出来的任务中心面板（页面/行的形状见它的文件头）。
    ui/TaskCenterPanel.hpp
    # 自绘的进度条：
    # 底栏 / 任务面板 / 详情页三处共用这一条，不再各自用原生 QProgressBar。
    ui/TaskProgressBar.hpp
)

set(SOURCE_FILES
    src/main.cpp
    src/MainWindow.cpp
    # 「设置」选项卡那一片接线：同一个类，另一个翻译单元（见那个文件的文件头）。
    src/MainWindowSettings.cpp
    src/MainWindowServer.cpp
    src/MainWindowTasks.cpp
    src/MainWindowSkin.cpp
    src/MainWindowInstanceTree.cpp
    src/MainWindowAccount.cpp
    src/MainWindowGroup.cpp
    src/MainWindowInstanceActions.cpp
    src/MainWindowInstanceFiles.cpp
    src/MainWindowSessionExport.cpp
    src/MainWindowInstanceSettingsWiring.cpp
    src/MainWindowDownloadOrigin.cpp
    src/MainWindowLaunch.cpp
    src/MainWindowFolderColumn.cpp
    src/MainWindowLaunchJava.cpp
    src/MainWindowRepair.cpp
    src/MainWindowIntegrityBackup.cpp
    src/MainWindowFileResource.cpp
    src/MainWindowInstanceEntry.cpp
    src/MainWindowOverlayGate.cpp
    src/MainWindowSessionEvents.cpp
    src/MainWindowDropImport.cpp
    src/MainWindowUpdateCenter.cpp
    src/MainWindowBottomBar.cpp
    src/MainWindowEventFilter.cpp
    src/MainWindowLifecycle.cpp
    src/MainWindowResourceDependency.cpp
    src/controllers/JavaRuntimeController.cpp
    src/controllers/JavaSettingsController.cpp
    src/controllers/PresetControllerEditor.cpp
    src/controllers/PresetController.cpp
    src/controllers/LauncherPreferencesController.cpp
    src/controllers/CrashReportController.cpp
    src/controllers/IntegrityController.cpp
    src/controllers/ToolboxController.cpp
    src/controllers/DirectDownloadController.cpp
    src/controllers/DirectDownloadRecovery.cpp
    src/controllers/DownloadBatchController.cpp
    src/controllers/DownloadInstallContinuation.cpp
    src/controllers/BackupController.cpp
    src/controllers/ServerPingController.cpp
    src/controllers/FolderController.cpp
    src/controllers/ModpackExportController.cpp
    src/controllers/InstanceFileController.cpp
    src/controllers/InstanceTransferController.cpp
    src/controllers/InstanceUpgradeController.cpp
    src/controllers/InstanceDialogController.cpp
    src/controllers/UpgradeAssessment.cpp
    src/controllers/InstanceUpdateFlow.cpp
    src/controllers/InstanceEditorController.cpp
    src/controllers/ImportController.cpp
    src/controllers/GameSessionController.cpp
    src/controllers/LaunchController.cpp
    src/controllers/ConnectionsController.cpp
    src/controllers/InstanceIconController.cpp
    src/controllers/ActivityCenterController.cpp
    src/controllers/TaskDetailController.cpp
    src/controllers/HomeQuickController.cpp
    # 主页「每日一言」：取句 / 当天缓存 / 句子池（判据在 src/util/DailyQuote.cpp）。
    src/controllers/DailyQuoteController.cpp
    src/controllers/InstanceMaintenanceController.cpp
    src/controllers/ServerWorkspaceController.cpp
    src/controllers/ServerConsoleController.cpp
    src/controllers/ServerSettingsController.cpp
    src/controllers/ServerSettingsPageController.cpp
    src/controllers/DownloadCompletionController.cpp
    src/controllers/DownloadTaskLedgerController.cpp
    src/controllers/ResourceInstallController.cpp
    src/controllers/InstallPageController.cpp
    src/controllers/ServerRunController.cpp
    src/controllers/ServerMenuController.cpp
    src/controllers/ServerManagementActions.cpp
    src/controllers/ThemeSettingsController.cpp
    src/controllers/PlayerSkinController.cpp
    src/controllers/LaunchHistoryController.cpp
    src/controllers/InstanceSettingsController.cpp
    src/controllers/MainWindowViewData.cpp
    src/controllers/ResourceSearchController.cpp
    src/controllers/InstanceUpdateCheckController.cpp
    src/controllers/UpdateCenterController.cpp
    src/controllers/DownloadFilterController.cpp
    src/controllers/ResourcePageController.cpp
    src/controllers/InstancePagesController.cpp
    src/controllers/ServerListController.cpp
    src/launcher/LauncherManager.cpp
    src/launcher/AsciiPathAlias.cpp
    src/launcher/ProcessPathLimit.cpp
    src/launcher/VersionInheritance.cpp
    src/auth/AuthManager.cpp
    src/auth/AccountStore.cpp
    src/auth/SkinApi.cpp
    src/auth/TokenStore.cpp
    src/util/SecretProtector.cpp
    src/agent/AgentTypes.cpp
    src/agent/AgentSettings.cpp
    src/agent/AgentPaths.cpp
    src/agent/AgentRedaction.cpp
    src/agent/AgentSecretStore.cpp
    src/agent/AgentSessionCrypto.cpp
    src/agent/AgentSession.cpp
    src/agent/AgentSessionStore.cpp
    src/agent/AgentSseParser.cpp
    src/agent/AgentProviderAdapters.cpp
    src/agent/OpenAiCompatibleClient.cpp
    src/agent/AgentContextBuilder.cpp
    src/agent/AgentLoop.cpp
    src/agent/AgentSkill.cpp
    src/agent/AgentPromptLibrary.cpp
    src/agent/AgentWorkspaceStore.cpp
    src/agent/AgentTestLaunch.cpp
    src/agent/AgentTestLaunchToolEngine.cpp
    src/agent/AgentRepairTransaction.cpp
    src/agent/AgentRepairToolEngine.cpp
    src/agent/AgentWinPath.cpp
    src/agent/AgentEgressPolicy.cpp
    src/agent/AgentArtifactStore.cpp
    src/agent/AgentBackupIndex.cpp
    src/agent/AgentInstanceLock.cpp
    src/agent/AgentFileTools.cpp
    src/agent/AgentToolRegistry.cpp
    src/agent/AgentApprovalPolicy.cpp
    src/agent/AgentFileToolEngine.cpp
    src/agent/AgentDiagnosticProjection.cpp
    src/agent/AgentLauncherTools.cpp
    src/agent/AgentWorkflow.cpp
    src/agent/AgentWorkflowEngine.cpp
    src/agent/AgentCompositeToolEngine.cpp
    src/agent/McpTypes.cpp
    src/agent/McpProtocol.cpp
    src/agent/McpTrustStore.cpp
    src/agent/McpClient.cpp
    src/agent/McpManager.cpp
    src/agent/AgentMcpToolEngine.cpp
    src/agent/AgentInstancePaths.cpp
    src/agent/AgentKnowledgeRules.cpp
    src/agent/AgentLogIndex.cpp
    src/agent/AgentCrashSnapshot.cpp
    src/agent/AgentMixinOwnership.cpp
    src/agent/AgentWebPolicy.cpp
    src/agent/QtAgentWebTransport.cpp
    src/agent/AgentWebTools.cpp
    src/agent/AgentCommandTools.cpp
    src/controllers/AgentController.cpp
    src/controllers/AgentLauncherHostImpl.cpp
    src/controllers/AgentTestLaunchRunner.cpp
    src/controllers/AgentRepairRunner.cpp
    src/net/HttpClient.cpp
    src/manage/DroppedResource.cpp
    src/manage/FolderManager.cpp
    src/manage/InstanceFiles.cpp
    src/manage/InstanceIconLookup.cpp
    src/manage/InstanceGroupStore.cpp
    src/manage/ModpackImport.cpp
    src/manage/ModpackInstallReport.cpp
    src/manage/ModpackCompletion.cpp
    src/manage/MultiMCPatchImport.cpp
    src/manage/ModpackExport.cpp
    src/manage/InstanceExport.cpp
    src/manage/ServerStatusCache.cpp
    src/manage/ServerListFile.cpp
    src/manage/DefaultSkin.cpp
    src/manage/PlayerSkinStore.cpp
    # 离线账号自己挑的那张皮肤。
    src/manage/OfflineSkin.cpp
    src/manage/InstanceMetaFile.cpp
    src/manage/VersionManager.cpp
    # 这四份是同一个类的实现，按职责分文件（理由见各自文件头与 VersionManagerInternals.hpp）。
    src/manage/VersionManagerLoaderInstall.cpp
    src/manage/VersionManagerModpack.cpp
    src/manage/VersionManagerExport.cpp
    src/manage/VersionDisplay.cpp
    src/manage/VersionIntegrity.cpp
    src/manage/UpdatePolicy.cpp
    src/manage/UpdateRollback.cpp
    src/manage/BackupFiles.cpp
    src/manage/ManualRecovery.cpp
    src/manage/WorldFiles.cpp
    # 世界只读概况：只读 level.dat 的那几格 + region/*.mca 的 8 KiB 头。
    src/manage/WorldMap.cpp
    # 「上一次启动」那本账。
    src/manage/LaunchHistory.cpp
    # 实例预设的盘那一层：下标 + 单份 + 那几件增删改。
    src/presets/PresetWorkspace.cpp
    src/presets/PresetStore.cpp
    # 原版设置那一段的判据：见头文件那一位。
    src/presets/VanillaOptions.cpp
    # 服务器列表那一段的判据 + 落到实例上：见头文件那一位。
    src/presets/ServerListPreset.cpp
    # 通用文件组那一段的判据 + 落盘：见头文件那一位。
    src/presets/GenericFiles.cpp
    # 应用计划那一层：见头文件那一位。
    src/presets/PresetPlanner.cpp
    # 提取与落到实例上。
    src/presets/PresetCapture.cpp
    src/presets/PresetApply.cpp
    src/presets/PresetExecutionReport.cpp
    src/presets/PresetModInstaller.cpp
    src/presets/PresetModDeployment.cpp
    # 资源包次序 / 启用 + 存档数据包：见头文件那两位。
    src/presets/ResourcePackPreset.cpp
    src/presets/DataPackPreset.cpp
    # 模组「真正去下」之前的判据层：
    # 必要/可选、依赖展开、冲突、替代确认与落盘校验，见头文件那一位。
    src/presets/ModInstallPlan.cpp
    src/presets/InstalledModInventory.cpp
    # 模组预设的兼容检查：见头文件那一位。
    src/presets/PresetCompatibility.cpp
    src/presets/ModrinthCompatibility.cpp
    # MaLiLib 那一族的热键：见头文件那一位。
    src/presets/MalilibKeys.cpp
    # 结构 / 原理图的元数据与方块统计。
    src/schematic/SchematicInfo.cpp
    # Replay 录像的元数据。
    src/replay/ReplayInfo.cpp
    src/util/LogFileReader.cpp
    # 启动诊断。
    src/diagnose/StartupDiagnosis.cpp
    # 迁移 / 升级副本。
    src/operations/TransferPlanner.cpp
    src/operations/UpgradeSelection.cpp
    src/operations/UpgradePageState.cpp
    src/operations/UpgradeReport.cpp
    src/operations/UpgradeHistory.cpp
    src/operations/UpgradeOptions.cpp
    src/operations/TransferApply.cpp
    src/operations/InstanceUpgrade.cpp
    src/operations/OperationReport.cpp
    src/controllers/FileOperationRecoveryController.cpp
    src/settings/EffectiveJavaSettings.cpp
    src/settings/GlobalJavaSettings.cpp
    src/settings/GameWindowSettings.cpp
    src/settings/LauncherSettings.cpp
    src/download/DownloadManager.cpp
    src/download/DownloadManagerRecovery.cpp
    src/download/DownloadManagerLocalReuse.cpp
    src/download/DownloadManagerMirror.cpp
    src/download/DownloadManagerThrottle.cpp
    src/download/DownloadManagerSegmented.cpp
    src/download/DownloadManagerMetadata.cpp
    src/download/LocalFileReuse.cpp
    src/download/AssetDirectorKnownSet.cpp
    src/download/AssetDirectorKnownSetData.cpp
    src/download/ResumableDownload.cpp
    src/download/ResumableDownload.hpp
    src/download/ZipMagic.hpp
    src/download/ResourceCatalog.cpp
    src/download/ModDependencies.cpp
    src/download/ResourceStore.cpp
    # 任务中心那一套：账本 + 它的落盘。
    src/tasks/TaskManager.cpp
    src/tasks/TaskHistory.cpp
    # 游戏异常退出那几句话：纯规则，一张表。
    src/diag/GameExitDiagnosis.cpp
    src/server/ServerRegistry.cpp
    # 服务端安装的驱动层：它带 QObject + QNetwork + QProcess
    # （判据那一半在 `ServerInstallRun.hpp`，是纯头）。
    src/server/ServerInstallController.cpp
    # 服务端的"跑"：一个服务端一条 QProcess
    # （判据那一半在 `ServerRun.hpp`，是纯头）。
    src/server/ServerProcess.cpp
    # 服务端设置那两张表 + 那一次真监听（判据那一半在各自的头里）。
    src/server/ServerPropertiesForm.cpp
    src/server/ServerPortCheck.cpp
    # 玩家名单的 JSON 读写判据。
    src/server/ServerPlayers.cpp
    # 崩溃自动重启的判据。
    src/server/ServerRestart.cpp
    # 服务端资源包：条目表判据 + SHA-1（借 ZipUtil / FileHash）。
    src/server/ServerResourcePack.cpp
    # 服务端内容（模组 / 数据包）的名字与分类。
    src/server/ServerContent.cpp
    src/util/DailyQuote.cpp
    # 主页大标题那句问候（按时段 / 上次打开日期 / 玩家名）。
    src/util/HomeGreeting.cpp
    src/util/ImageDecode.cpp
    src/util/ThemeImage.cpp
    src/util/SystemMemory.cpp
    src/util/StorageAvailability.cpp
    src/util/FileAssociations.cpp
    src/util/SevenZip.cpp
    src/util/ZipUtil.cpp
    src/util/NbtUtil.cpp
    src/util/NbtTree.cpp
    src/util/ServerPing.cpp
    src/util/ErrorReport.cpp
    src/util/DownloadDiagnostics.cpp
    src/tools/TextFormat.cpp
    src/tools/CoordConvert.cpp
    src/tools/FileHash.cpp
    src/tools/AnvilPlanner.cpp
    src/tools/Beacon.cpp
    src/tools/Brewing.cpp
    src/tools/Banner.cpp
    src/tools/ArmorTrim.cpp
    src/controllers/ArmorTrimPreviewController.cpp
    ui/AccountSkinDialog.cpp
    ui/GameLogWindow.cpp
    # 运行输出共用模型：上面那一窗和 ServerConsolePage 都直接用它。
    ui/RuntimeOutputModel.cpp
    ui/MainWindowUI.cpp
    ui/AgentMarkdown.cpp
    ui/AgentMessageView.cpp
    ui/AgentComposer.cpp
    ui/AgentPage.cpp
    ui/AgentSecretEdit.cpp
    ui/AgentSettingsPage.cpp
    ui/AgentToolCard.cpp
    ui/AgentApprovalCard.cpp
    ui/AgentMcpPage.cpp
    ${WTFML_UI_PAGE_SOURCES}
    ui/MemorySlider.cpp
    ui/MotdText.cpp
    ui/ToolboxPage.cpp
    ui/ToggleSwitch.cpp
    ui/TaskCenterPanel.cpp
    ui/ActivityPopover.cpp
    ui/TaskDetailPage.cpp
    ui/TaskProgressBar.cpp
    ui/IntegrityReportPage.cpp
    ui/UpdateCenterPage.cpp
    ui/InstanceUpgradePage.cpp
    ui/UpgradeReportDialog.cpp
    ui/UpgradeHistoryDialog.cpp
    ui/ShellOpenWith.hpp
    ui/ShellOpenWith.cpp
    ui/SchematicInfoDialog.hpp
    ui/SchematicInfoDialog.cpp
    ui/NewServerPage.cpp
    ui/ServerConsolePage.cpp
    ui/ServerSettingsPage.cpp
    ui/GroupDialog.cpp
    ui/GroupListDelegate.cpp
    ui/ListRowDelegate.cpp
    ui/InstanceSettingsDialog.cpp
    ui/InstanceSettingsSignature.cpp
    ui/InstanceUpdateWizard.cpp
    ui/InstanceFilesPage.cpp
    ui/InstanceFilesPageRows.cpp
    ui/InstanceFilesSelection.cpp
    ui/InstanceFilesPageFilter.cpp
    ui/InstanceFilesPageScroll.cpp
    ui/InstanceFilesPageInfoPanel.cpp
    ui/InstanceFilesPageStructures.cpp
    ui/InstanceFilesPageCapabilities.cpp
    ui/InstanceFilesPageDirectoryStats.cpp
    ui/InstanceFilesPageRefresh.cpp
    ui/InstanceFilesPageState.cpp
    ui/InstanceFilesPageEventFilter.cpp
    ui/ResourcePackPage.cpp
    ui/ConfigFileText.cpp
    ui/ConfigFileEditorPage.cpp
    ui/WorldSettingsPage.cpp
    ui/GameRuleNames.cpp
    ui/ConfigEntries.cpp
    ui/InlineMessage.cpp
    ui/JavaMismatchDialog.cpp
    ui/JavaManageDialog.cpp
    ui/PresetEditorPage.cpp
    ui/PresetManagePage.cpp
    ui/PresetModEditor.cpp
    ui/PresetOptionsEditor.cpp
    ui/PresetInstallPicker.cpp
    ui/OverlayHost.cpp
    ui/SkinModel.cpp
    ui/VersionIcons.cpp
    # 四个加载器的标志那几张真图。CMAKE_AUTORCC 是开的，所以一个 .qrc
    # 列在这儿就够了、不用再写 qt_add_resources。验证工程也要各自列一份（见那几个
    # CMakeLists 里同一行的注释）：漏了的表现是那几张图**静默变回草方块**。
    assets/icons.qrc
    # 技能提示词与后续诊断知识。提示词正文不在 AgentSkill.cpp 里；改 .md 后
    # 只重编这份资源。验证工程通过 wtfml_core_runtime 带同一份 qrc。
    assets/agent/agent.qrc
    ui/VersionTreeDelegate.cpp
    ui/LoadingState.cpp
    ui/ResourceDetailPage.cpp
    ui/InstanceGroupDelegate.cpp
    ui/SkinViewer.cpp
    ui/TitleBar.cpp
    ui/BrandMark.cpp
    ui/DesignDefaults.cpp
    ui/Theme.cpp
    ui/ThemeAdvancedEditor.cpp
    # 动效那一层：按下按钮 / 换页 / 展开折叠。
    ui/Animations.cpp
    ui/WindowMotion.cpp
    ui/ThemeBackground.cpp
    ui/Glass.cpp
    # 浮层那一层：自绘 tooltip / 下拉弹层 / 菜单共用的磨砂外壳（不碰窗口与失焦磨砂）。
    ui/Popups.cpp
    ui/SystemBackdrop.cpp
    ui/WindowChrome.cpp
    ui/WindowEdge.cpp
    ui/ChromeDialog.hpp
    ui/ChromeDialog.cpp
    ${HEADER_FILES}
)

qt_add_executable(WTFMinecraftLauncher
    ${SOURCE_FILES}
    # 清单里只有一条：longPathAware（>260 字符的路径）。由 windres 编成 RT_MANIFEST 资源，
    # 见 res/wtfml.rc 与 res/wtfml.manifest 的注释。
    $<$<PLATFORM_ID:Windows>:${CMAKE_CURRENT_SOURCE_DIR}/res/wtfml.rc>
)

# 英文翻译编进 exe（资源路径 `:/i18n/wtfml_en.qm`，见 ui/UiLanguage.hpp）。
wtfml_embed_translations(WTFMinecraftLauncher)

# exe 图标（assets/app/wtfml.ico，由上面那份 .rc 编进去）。windres 只看 .rc、不看它引用的
# .ico，所以换图标时**必须让这份 .rc 重编**：OBJECT_DEPENDS 是唯一能把这两条接上的地方
# （AUTORCC / 依赖扫描都不管 .rc 里引用的文件）。漏了这一条的表现是换了图标、exe 里还是旧的。
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/res/wtfml.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/app/wtfml.ico")

# 测量构建（-DWTFML_PERF_PROBE=ON）：只给真 Release 量界面耗时用。
# 默认不加进来 —— 普通构建里连这个 .cpp 都不编，`PerfProbe::attach()` 是内联空函数。
# 只给主程序挂宏，验证工程不受影响（它们本来也不该跑探针）。
if(WTFML_PERF_PROBE)
    target_sources(WTFMinecraftLauncher PRIVATE src/perf/PerfProbe.cpp src/perf/StallSampler.cpp)
    target_compile_definitions(WTFMinecraftLauncher PRIVATE WTFML_PERF_PROBE=1)
    # 「卡在哪一行」要能解析：栈里本程序的帧只记 `exe+0xRVA`，名字由
    # tools/symbolize-stalls.ps1 用 addr2line 从 DWARF 里取 —— 没有 -g 就只有地址。
    # -g 不改代码生成，只让 exe 变大；这一档本来就是测量用的。
    target_compile_options(WTFMinecraftLauncher PRIVATE -g)
    # StackWalk64 / SymFromAddr。
    target_link_libraries(WTFMinecraftLauncher PRIVATE dbghelp)
endif()

# GUI 子系统（**不弹控制台窗口**）。
#
# 不设这一条的话，MinGW 出来的默认是 CUI（Windows 控制台程序），而双击一个 CUI 程序时
# Windows 会给它开一个黑框，里面刷的是 [WTFML] 的启动日志和 Qt 自己那几句警告。
# 验证工程的 exe（verify / ui_check /
# mw_check / ui_shot）**不受影响**：它们是各自 CMakeLists 里单独的 target，本来就该是
# 控制台程序（那一跑就是"看它打什么字"）。
#
# 代价是"那些话没人看得见"，由 src/main.cpp 的 installFileLog() 补上：
# <数据目录>/logs/wtfml.log，每次启动截断重写。
set_target_properties(WTFMinecraftLauncher PROPERTIES WIN32_EXECUTABLE TRUE)

# 还得把 Qt 自己那个入口（Qt6EntryPoint）关掉，否则 MinGW 这一套链不过去：
# 它那个 `WinMain → main(__argc, __argv)` 要 `__argc`/`__argv`，而这个工具链上它们
# 是 dllimport 的、没人提供 —— 症状是链接期一句
# `undefined reference to __imp___argc`（不是运行期，所以别指望跑起来才发现）。
#
# qt_no_entrypoint 是 Qt 自己认的那个开关（Qt6CoreTargets.cmake 里那段 genex 就是按它
# 决定要不要链 EntryPointPrivate），链上 MinGW 自带的 WinMain 之后 `int main()` 照旧是
# 入口，GUI 子系统这件事只由上面那条 WIN32_EXECUTABLE 决定。
set_target_properties(WTFMinecraftLauncher PROPERTIES qt_no_entrypoint TRUE)

# 版本号**只有一个出处 = 上面 `project(... VERSION ...)` 那一位**，由这儿递给源码。
#
# src/main.cpp 的 `setApplicationVersion(WTFML_VERSION)` 读它，值最后会走到两处：
#   * `src/launcher/LauncherManager.cpp` —— version.json 里 `${launcher_version}` 展开成
#     `-Dlauncher_version=`（盘上那一位空的时候回落到字面量 `1.0`）；
#   * `src/MainWindow.cpp` 的错误报告（`facts.launcherVersion`）。
# 两处必须都从这里取，不要再写第二份字面量。
target_compile_definitions(WTFMinecraftLauncher PRIVATE WTFML_VERSION="${PROJECT_VERSION}")

# 预编译头（WTFML_USE_PCH，默认 ON）。
#
# 为什么要它：本机量过，一个**空** TU 只 `#include <QString> + <QWidget>` 就要 1427 ms
# 语法解析（空 TU 66 ms）—— 产品里每一个 TU 都在重复付这一笔。把这份头预编译一次之后，
# 同一个 TU 337 ms（.gch 本机 177 MB，读回来的固定成本约 270 ms）。
#
# 为什么只给 `QWidget` 这一条，不给 `<QtWidgets>` 整个模块：整模块那份 .gch 本机 425 MB、
# 每个 TU 读回来要 3445 ms —— 比不用 PCH 还慢。加条目前先照第十三节的办法量一遍。
#
# 只挂在主程序上：验证工程是几十个各自独立的 target，每个都要自己那份 .gch（几十份
# 177 MB），换不来时间。它们本来也不是日常改动的那一份。
#
# 编译器外面套的是 ccache 时不用 PCH（原因见文件开头 WTFML_USE_CCACHE 那段）。
if(WTFML_USE_PCH AND WTFML_LAUNCHER_NAME STREQUAL "ccache")
    message(STATUS "WTFML: compiler launcher is ccache, precompiled header skipped so cache hits carry across worktrees")
elseif(WTFML_USE_PCH)
    target_precompile_headers(WTFMinecraftLauncher PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:<QtWidgets/QWidget>>")
endif()

# 特供版（-DWTFML_DIAG_DOWNLOAD=ON）：详细下载日志恒开。
#
# ★ 宏**只给 DownloadDiagnostics.cpp 这一个文件**，不是 target 级的：别处的调用点一律走
#   `DownloadDiag::enabled()`（普通构建里它回 false，一个字段都不多写）。这样切换这个开关
#   只重编那一个文件 + 重新链接，不用把整份 300 多个 .o 全推翻重来。
if(WTFML_DIAG_DOWNLOAD)
    set_source_files_properties(src/util/DownloadDiagnostics.cpp PROPERTIES
        COMPILE_DEFINITIONS "WTFML_DIAG_DOWNLOAD=1;WTFML_BUILD_FLAVOR=\"download-diagnostics\"")
endif()

# 正版登录默认使用 WTFML 自己的 Microsoft Entra 应用。客户端 ID 是 OAuth 的公开标识。
set(WTFML_CLIENT_ID "b24d0030-cedb-454a-96f4-690ba85fcf98" CACHE STRING
    "Microsoft OAuth 客户端 ID（空 = 禁用在线授权）")
set(WTFML_REDIRECT_URI "" CACHE STRING "OAuth 本地回调基址（空 = http://localhost/，端口由系统分配）")

if(WTFML_CLIENT_ID)
    target_compile_definitions(WTFMinecraftLauncher PRIVATE WTFML_CLIENT_ID="${WTFML_CLIENT_ID}")
endif()
if(WTFML_REDIRECT_URI)
    target_compile_definitions(WTFMinecraftLauncher PRIVATE WTFML_REDIRECT_URI="${WTFML_REDIRECT_URI}")
endif()

# MainWindow.cpp is still large while its controllers are being extracted.  A
# debug build can exceed PE/COFF's default section count under MinGW.
if(MINGW)
    target_compile_options(WTFMinecraftLauncher PRIVATE -Wa,-mbig-obj)
endif()

target_include_directories(WTFMinecraftLauncher PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/ui      
)

target_link_libraries(WTFMinecraftLauncher PRIVATE
    Qt6::Widgets
    Qt6::Network
    Qt6::Core
    Qt6::Gui
    Qt6::Concurrent
    # ui/WindowChrome.cpp：QNativeInterface::Private::QWindowsWindow::setCustomMargins（去掉系统标题栏）。
    Qt6::GuiPrivate
)

if(WTFML_STRICT_WARNINGS)
    if(MSVC)
        target_compile_options(WTFMinecraftLauncher PRIVATE /W4 /permissive-)
    else()
        target_compile_options(WTFMinecraftLauncher PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
if(WTFML_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(WTFMinecraftLauncher PRIVATE /WX)
    else()
        target_compile_options(WTFMinecraftLauncher PRIVATE -Werror)
    endif()
endif()

# 令牌落盘用的 DPAPI（CryptProtectData）在这个库里。见 src/auth/TokenStore.cpp 的 protect()。
#
# 另外三个是 src/util/ImageDecode.cpp 要的（**只有 Windows 上有**，所以整段套在 WIN32 里）：
#   * ole32          —— CoInitializeEx / CoCreateInstance（起 COM、拿 WIC 工厂）
#   * shlwapi        —— SHCreateMemStream（把内存里那串图标字节变成一个 IStream，不落盘）
#   * windowscodecs  —— WIC 本体（IWICImagingFactory 那些）
# 三个都是系统 DLL（Vista 起就在），**不发任何东西**：见 ImageDecode 文件头那段。
#
# dwmapi 是外框那一份要的（ui/WindowChrome.cpp 里的 DwmSetWindowAttribute，把圆角明说一遍）。
# 同样是系统 DLL（Vista 起就有）。
#
# shell32 + uuid 是「删除走回收站」那一份要的（FolderManager.cpp 的 moveToRecycleBin：
# IFileOperation / SHCreateItemFromParsingName / IID_IFileOperation 这些符号）。
# 两个都是系统库，**不发任何东西**。
if(WIN32)
    target_link_libraries(WTFMinecraftLauncher PRIVATE crypt32 advapi32 ole32 shlwapi windowscodecs dwmapi shell32 uuid)
endif()

if(WTFML_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(WTFML_BUILD_NETWORK_DIAG "Build the standalone network diagnostic tool" ON)
if(WTFML_BUILD_NETWORK_DIAG)
    add_subdirectory(tools/network_diag)
endif()

if(WTFML_PERF_DOWNLOAD)
    add_subdirectory(tools/perf_download)
endif()




