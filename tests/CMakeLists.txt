# Every test records its build owner; isolated processes may share a runner.
# Tooling tests use '-'.
set_property(GLOBAL PROPERTY WTFML_TEST_BUILD_OWNERS "test|target\n")
option(WTFML_COMPACT_TEST_RUNNERS "Share specialist runners while keeping separate CTest processes" OFF)
add_custom_target(wtfml_specialist_modules)
function(wtfml_add_test)
 cmake_parse_arguments(T "" "NAME" "COMMAND" ${ARGN})
 list(GET T_COMMAND 0 owner)
 if(TARGET "${owner}")
  get_target_property(runner ${owner} WTFML_COMPACT_RUNNER)
  if(runner)
   set(command ${T_COMMAND})
   list(REMOVE_AT command 0)
   list(PREPEND command ${runner} ${owner})
   add_test(NAME ${T_NAME} COMMAND ${command})
   set(owner ${runner})
  else()
   add_test(${ARGN})
  endif()
 else()
  add_test(${ARGN})
 endif()
 if(NOT TARGET "${owner}")
  set(owner "-")
 endif()
 # Non-audited checks remain exclusive even when CTest has parallel slots.
 set_tests_properties(${T_NAME} PROPERTIES RUN_SERIAL TRUE)
 set_property(GLOBAL APPEND_STRING PROPERTY WTFML_TEST_BUILD_OWNERS "${T_NAME}|${owner}\n")
endfunction()
function(wtfml_compact_module target entry runner)
 add_custom_target(${target} DEPENDS ${runner})
 set_property(TARGET ${target} PROPERTY WTFML_COMPACT_RUNNER ${runner})
 target_sources(${runner} PRIVATE $<TARGET_OBJECTS:${target}_case>)
 set_property(DIRECTORY APPEND_STRING PROPERTY WTFML_COMPACT_ENTRIES
  "if (name == QStringLiteral(\"${target}\")) return invoke(${entry}, app);\n")
endfunction()
function(wtfml_write_compact_entries)
 if(WTFML_COMPACT_TEST_RUNNERS)
  get_property(entries DIRECTORY PROPERTY WTFML_COMPACT_ENTRIES)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/CompactEntries.inc" CONTENT "${entries}")
 endif()
endfunction()
function(wtfml_write_test_owners)
 get_property(owners GLOBAL PROPERTY WTFML_TEST_BUILD_OWNERS)
 if(TARGET WTFMLNetworkDiag)
  string(APPEND owners "wtfml.network-diag|WTFMLNetworkDiag\n")
 endif()
 file(WRITE "${CMAKE_BINARY_DIR}/test-targets.txt" "${owners}")
endfunction()
function(wtfml_exclude_shared_sources target)
    set(owned)
    foreach(library IN LISTS ARGN)
        get_target_property(sources ${library} SOURCES)
        foreach(source IN LISTS sources)
            get_filename_component(path "${source}" REALPATH)
            list(APPEND owned "${path}")
            if(path MATCHES "\\.cpp$")
                string(REGEX REPLACE "\\.cpp$" ".hpp" header "${path}")
                list(APPEND owned "${header}")
                string(REGEX REPLACE "\\.cpp$" ".h" header "${path}")
                list(APPEND owned "${header}")
            endif()
        endforeach()
    endforeach()
    get_target_property(sources ${target} SOURCES)
    set(unique)
    foreach(source IN LISTS sources)
        get_filename_component(path "${source}" REALPATH)
        if(NOT path IN_LIST owned)
            list(APPEND unique "${source}")
        endif()
    endforeach()
    set_property(TARGET ${target} PROPERTY SOURCES "${unique}")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL wtfml_write_test_owners)

# 核心回归验证：直接编译生产代码，用控制台断言覆盖文件、网络解析和启动规则。

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Gui Concurrent)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
get_filename_component(REPO_ROOT ${REPO_ROOT} REALPATH)

set(PROJECT_SOURCES
    # auth/ 这一段是「多账号 + 换皮肤披风」那次改动加进来的。
    #   - TokenStore / AccountStore：凭据落盘的字节格式（含 DPAPI）和多账号列表。
    #     这一层**必须**在真 Qt 下验：盘上那份字节的最外层是 @ByteArray(...) 那种
    #     包装，手搓一个"看起来对"的字节串等于自证（见 memory 里那条「令牌存盘的
    #     字节格式」）。
    #   - SkinApi：换皮肤披风那几条接口的解析与话术，全是纯函数（不联网）。
    #   - AuthManager：只为 SkinApi 那句 403 话术转发（appRegistrationHint 只能有
    #     一个出处），verify 自己不构造它。
    ${REPO_ROOT}/src/auth/AccountStore.cpp
    ${REPO_ROOT}/src/auth/AccountStore.hpp
    ${REPO_ROOT}/src/auth/AuthManager.cpp
    ${REPO_ROOT}/src/auth/AuthManager.hpp
    ${REPO_ROOT}/src/auth/SkinApi.cpp
    ${REPO_ROOT}/src/auth/SkinApi.hpp
    ${REPO_ROOT}/src/auth/TokenStore.cpp
    ${REPO_ROOT}/src/auth/TokenStore.hpp
    ${REPO_ROOT}/src/util/SecretProtector.cpp
    ${REPO_ROOT}/src/util/SecretProtector.hpp
    ${REPO_ROOT}/src/agent/AgentTypes.cpp
    ${REPO_ROOT}/src/agent/AgentTypes.hpp
    ${REPO_ROOT}/src/agent/AgentSettings.cpp
    ${REPO_ROOT}/src/agent/AgentSettings.hpp
    ${REPO_ROOT}/src/agent/AgentPaths.cpp
    ${REPO_ROOT}/src/agent/AgentPaths.hpp
    ${REPO_ROOT}/src/agent/AgentRedaction.cpp
    ${REPO_ROOT}/src/agent/AgentRedaction.hpp
    ${REPO_ROOT}/src/agent/AgentSecretStore.cpp
    ${REPO_ROOT}/src/agent/AgentSecretStore.hpp
    ${REPO_ROOT}/src/agent/AgentSessionCrypto.cpp
    ${REPO_ROOT}/src/agent/AgentSessionCrypto.hpp
    ${REPO_ROOT}/src/agent/AgentSession.cpp
    ${REPO_ROOT}/src/agent/AgentSession.hpp
    ${REPO_ROOT}/src/agent/AgentSessionStore.cpp
    ${REPO_ROOT}/src/agent/AgentSessionStore.hpp
    ${REPO_ROOT}/src/agent/AgentSseParser.cpp
    ${REPO_ROOT}/src/agent/AgentSseParser.hpp
    ${REPO_ROOT}/src/agent/AgentProviderAdapters.cpp
    ${REPO_ROOT}/src/agent/AgentProviderAdapters.hpp
    ${REPO_ROOT}/src/agent/AgentModelClient.hpp
    ${REPO_ROOT}/src/agent/OpenAiCompatibleClient.cpp
    ${REPO_ROOT}/src/agent/OpenAiCompatibleClient.hpp
    ${REPO_ROOT}/src/agent/AgentContextBuilder.cpp
    ${REPO_ROOT}/src/agent/AgentContextBuilder.hpp
    ${REPO_ROOT}/src/agent/AgentLoop.cpp
    ${REPO_ROOT}/src/agent/AgentLoop.hpp
    ${REPO_ROOT}/src/agent/AgentSkill.cpp
    ${REPO_ROOT}/src/agent/AgentSkill.hpp
    ${REPO_ROOT}/src/agent/AgentWinPath.cpp
    ${REPO_ROOT}/src/agent/AgentWinPath.hpp
    ${REPO_ROOT}/src/agent/AgentEgressPolicy.cpp
    ${REPO_ROOT}/src/agent/AgentEgressPolicy.hpp
    ${REPO_ROOT}/src/agent/AgentArtifactStore.cpp
    ${REPO_ROOT}/src/agent/AgentArtifactStore.hpp
    ${REPO_ROOT}/src/agent/AgentBackupIndex.cpp
    ${REPO_ROOT}/src/agent/AgentBackupIndex.hpp
    ${REPO_ROOT}/src/agent/AgentInstanceLock.cpp
    ${REPO_ROOT}/src/agent/AgentInstanceLock.hpp
    ${REPO_ROOT}/src/agent/AgentFileTools.cpp
    ${REPO_ROOT}/src/agent/AgentFileTools.hpp
    ${REPO_ROOT}/src/agent/AgentTool.hpp
    ${REPO_ROOT}/src/agent/AgentToolRegistry.cpp
    ${REPO_ROOT}/src/agent/AgentApprovalPolicy.cpp
    ${REPO_ROOT}/src/agent/AgentApprovalPolicy.hpp
    ${REPO_ROOT}/src/agent/AgentFileToolEngine.cpp
    ${REPO_ROOT}/src/agent/AgentFileToolEngine.hpp
    ${REPO_ROOT}/src/agent/AgentDiagnosticProjection.cpp
    ${REPO_ROOT}/src/agent/AgentDiagnosticProjection.hpp
    ${REPO_ROOT}/src/agent/AgentLauncherHost.hpp
    ${REPO_ROOT}/src/agent/AgentLauncherTools.cpp
    ${REPO_ROOT}/src/agent/AgentLauncherTools.hpp
    ${REPO_ROOT}/src/agent/AgentWorkflow.cpp
    ${REPO_ROOT}/src/agent/AgentWorkflow.hpp
    ${REPO_ROOT}/src/agent/AgentWorkflowEngine.cpp
    ${REPO_ROOT}/src/agent/AgentWorkflowEngine.hpp
    ${REPO_ROOT}/src/agent/AgentCompositeToolEngine.cpp
    ${REPO_ROOT}/src/agent/AgentCompositeToolEngine.hpp
    ${REPO_ROOT}/src/agent/McpTypes.cpp
    ${REPO_ROOT}/src/agent/McpTypes.hpp
    ${REPO_ROOT}/src/agent/McpProtocol.cpp
    ${REPO_ROOT}/src/agent/McpProtocol.hpp
    ${REPO_ROOT}/src/agent/McpTrustStore.cpp
    ${REPO_ROOT}/src/agent/McpTrustStore.hpp
    ${REPO_ROOT}/src/agent/McpClient.cpp
    ${REPO_ROOT}/src/agent/McpClient.hpp
    ${REPO_ROOT}/src/agent/McpManager.cpp
    ${REPO_ROOT}/src/agent/McpManager.hpp
    ${REPO_ROOT}/src/agent/AgentMcpToolEngine.cpp
    ${REPO_ROOT}/src/agent/AgentMcpToolEngine.hpp
    ${REPO_ROOT}/src/agent/AgentWebPolicy.cpp
    ${REPO_ROOT}/src/agent/AgentWebPolicy.hpp
    ${REPO_ROOT}/src/agent/AgentWebTransport.hpp
    ${REPO_ROOT}/src/agent/QtAgentWebTransport.cpp
    ${REPO_ROOT}/src/agent/QtAgentWebTransport.hpp
    ${REPO_ROOT}/src/agent/AgentWebTools.cpp
    ${REPO_ROOT}/src/agent/AgentWebTools.hpp
    ${REPO_ROOT}/src/agent/AgentCommandTools.cpp
    ${REPO_ROOT}/src/agent/AgentCommandTools.hpp
    ${REPO_ROOT}/ui/AgentMarkdown.cpp
    ${REPO_ROOT}/ui/AgentMarkdown.hpp
    ${REPO_ROOT}/src/net/HttpClient.cpp
    ${REPO_ROOT}/src/net/HttpClient.hpp
    ${REPO_ROOT}/src/download/DownloadManager.cpp
    ${REPO_ROOT}/src/download/DownloadManagerRecovery.cpp
    ${REPO_ROOT}/src/download/DownloadManagerLocalReuse.cpp
    ${REPO_ROOT}/src/download/DownloadManagerMirror.cpp
    ${REPO_ROOT}/src/download/DownloadManagerThrottle.cpp
    ${REPO_ROOT}/src/download/DownloadManagerSegmented.cpp
    ${REPO_ROOT}/src/download/DownloadManagerMetadata.cpp
    # 「从别的游戏文件夹里借文件」：DownloadManager 会调它
    # （来源为空 = 一个文件都不看）。纯值进结论出那一层直接编进 verify 逐格念。
    ${REPO_ROOT}/src/download/LocalFileReuse.cpp
    ${REPO_ROOT}/src/download/LocalFileReuse.hpp
    # GTNH 首启资源：AssetDirector 的已知集合清单、复用判据与下载计划（2.1.6a/2.1.6b）。
    # 生成的那份 Data.cpp 只提供嵌入的字节，判据全在 KnownSet.cpp 里。
    ${REPO_ROOT}/src/download/AssetDirectorKnownSet.cpp
    ${REPO_ROOT}/src/download/AssetDirectorKnownSet.hpp
    ${REPO_ROOT}/src/download/AssetDirectorKnownSetData.cpp
    ${REPO_ROOT}/src/download/ResumableDownload.cpp
    ${REPO_ROOT}/src/download/ResumableDownload.hpp
    ${REPO_ROOT}/src/download/DownloadManager.hpp
    # 下载诊断层：DownloadManager / ResumableDownload 无条件调它
    # （普通构建 enabled() 是编译期 false，一个字段都不多写）。不放进来源表就是链接期
    # 一片 undefined reference。
    ${REPO_ROOT}/src/util/DownloadDiagnostics.cpp
    ${REPO_ROOT}/src/util/DownloadDiagnostics.hpp
    ${REPO_ROOT}/src/launcher/LauncherManager.cpp
    ${REPO_ROOT}/src/launcher/LauncherManager.hpp
    ${REPO_ROOT}/src/launcher/LibraryRules.hpp
    # 起进程那条 260 字符的路径墙：四个启动点都要调它，验证工程那一节只喂纯值
    # （见 core_modules/ProcessPathLimitTests.cpp）。
    ${REPO_ROOT}/src/launcher/ProcessPathLimit.cpp
    ${REPO_ROOT}/src/launcher/ProcessPathLimit.hpp
    # DefaultSkin：离线名字 → 官方默认皮肤那张图（Java 那个 UUID.hashCode() 的实现、
    # 18 元表的下标、两套规矩各自的路径、缓存文件名怎么拼）。**这一份跑的是真 JAR** ——
    # 工程自己造一个假的版本目录 + 假 JAR（Compress-Archive），把"判据是 JAR、不是版本号"
    # 这条也钉住了。真值表来自本机 JVM（见那边的注释）。
    ${REPO_ROOT}/src/manage/DefaultSkin.cpp
    ${REPO_ROOT}/src/manage/DefaultSkin.hpp
    ${REPO_ROOT}/src/manage/InstanceFiles.cpp
    # FolderManager.cpp 是**被 VersionManager.cpp 拽进来的**：删实例那条路
    # （VersionManager::deleteInstance）用的是 FolderManager.hpp 里那个自由函数
    # removeTreeWithoutFollowingLinks()，实现就在这个 .cpp 里。少了它，链接期报的是
    # 一条 `undefined reference to removeTreeWithoutFollowingLinks(QString const&)`
    # —— 编译过得去，别以为只是头文件的事。
    ${REPO_ROOT}/src/manage/FolderManager.cpp
    ${REPO_ROOT}/src/manage/InstanceFiles.hpp
    ${REPO_ROOT}/src/manage/InstanceGroupStore.cpp
    ${REPO_ROOT}/src/manage/InstanceGroupStore.hpp
    ${REPO_ROOT}/src/manage/DroppedResource.cpp
    ${REPO_ROOT}/src/util/SevenZip.cpp
    ${REPO_ROOT}/src/manage/ModpackImport.cpp
    ${REPO_ROOT}/src/manage/ModpackInstallReport.cpp
    ${REPO_ROOT}/src/manage/ModpackCompletion.cpp
    ${REPO_ROOT}/src/manage/MultiMCPatchImport.cpp
    ${REPO_ROOT}/src/manage/DroppedResource.hpp
    ${REPO_ROOT}/src/manage/ModpackImport.hpp
    ${REPO_ROOT}/src/manage/ModpackExport.cpp
    ${REPO_ROOT}/src/manage/InstanceExport.cpp
    ${REPO_ROOT}/src/manage/ModpackExport.hpp
    ${REPO_ROOT}/src/manage/PlayerSkinStore.cpp
    ${REPO_ROOT}/src/manage/PlayerSkinStore.hpp
    # OfflineSkin：离线账号自己挑的那张图存在哪、模型那一行怎么归一化、撤掉之后
    # 是不是真的一张都不剩。纯文件读写，不联网。
    ${REPO_ROOT}/src/manage/OfflineSkin.cpp
    ${REPO_ROOT}/src/manage/OfflineSkin.hpp
    ${REPO_ROOT}/src/manage/InstanceMetaFile.cpp
    ${REPO_ROOT}/src/manage/VersionManager.cpp
    # 同一个类按职责分开的那三份（核心那一份就在上面；理由见各文件头）。
    # 少一份不会在编译期报错，只在链接期报一串 `VersionManager::…` 的 undefined reference。
    ${REPO_ROOT}/src/manage/VersionManagerLoaderInstall.cpp
    ${REPO_ROOT}/src/manage/VersionManagerModpack.cpp
    ${REPO_ROOT}/src/manage/VersionManagerExport.cpp
    ${REPO_ROOT}/src/manage/VersionManager.hpp
    ${REPO_ROOT}/src/manage/VersionDisplay.cpp
    ${REPO_ROOT}/src/manage/VersionDisplay.hpp
    # 体检那一层（ZN 节）：纯值进、结论出 —— 一个文件都不下，所以能直接对着一个搭出来的
    # 临时游戏文件夹跑。
    ${REPO_ROOT}/src/manage/VersionIntegrity.cpp
    ${REPO_ROOT}/src/manage/VersionIntegrity.hpp
    # 更新中心那两层：判据（忽略 / 固定那一档，纯值 + 一次文件读写）和
    # 回滚点（真拷文件、真还原）。都放进来 —— ZX / ZT 两节直接对着临时目录钉。
    ${REPO_ROOT}/src/manage/UpdatePolicy.cpp
    ${REPO_ROOT}/src/manage/UpdatePolicy.hpp
    ${REPO_ROOT}/src/manage/UpdateRollback.cpp
    ${REPO_ROOT}/src/manage/UpdateRollback.hpp
    # 备份那一层：打一份包、读清单、还原。和 WorldFiles 同一个理由放进验证工程 ——
    # 「清单读得回来吗」「还原之前那份安全快照做出来没有」「解压到一半失败时旧的那一份还在不在」
    # 这三件事都能绕开界面直接钉死，而且全是真的建 zip、真的解回来。
    ${REPO_ROOT}/src/manage/BackupFiles.cpp
    ${REPO_ROOT}/src/manage/BackupFiles.hpp
    ${REPO_ROOT}/src/manage/ManualRecovery.cpp
    ${REPO_ROOT}/src/manage/ManualRecovery.hpp
    # WorldFiles：「导入世界」和存档改名那两个改盘的动作。放进来是为了绕开原生选择框
    # （测试驱动不了它）直接调函数 —— 重名挡住没有、拷一半失败会不会留半份，都在这里钉。
    ${REPO_ROOT}/src/manage/WorldFiles.cpp
    ${REPO_ROOT}/src/manage/WorldFiles.hpp
    # 世界只读概况：头部表解析、种子/玩家位置读取、目录扫描 —— 纯函数那两截
    # 直接喂假数据就能逐格念，不用造一个真存档。
    ${REPO_ROOT}/src/manage/WorldMap.cpp
    ${REPO_ROOT}/src/manage/WorldMap.hpp
    # 「上一次启动」那本账：格式怎么读写、时间怎么说成人话、目标没了怎么降级、
    # 上限怎么截 —— 全是纯函数，**一条都不用起界面**就能逐格念。
    ${REPO_ROOT}/src/manage/LaunchHistory.cpp
    ${REPO_ROOT}/src/manage/LaunchHistory.hpp
    # 实例预设：值那一层（格式标记、schema 版本、
    # 枚举⇄词、段与整份的读回）与盘那一层（下标、单份、增删改）。★ 两个都收一个 `root`
    # 参数，所以 `verify` 的 **PA** 一节拿 `QTemporaryDir` 一喂就能逐格念 —— 包括最难查的
    # 那两格：「一份坏了另外几份照旧列得出来」和「名字里带 `../` 也逃不出预设目录」。
    ${REPO_ROOT}/src/presets/PresetTypes.hpp
    ${REPO_ROOT}/src/presets/PresetWorkspace.cpp
    ${REPO_ROOT}/src/presets/PresetStore.cpp
    ${REPO_ROOT}/src/presets/PresetStore.hpp
    # 原版设置那一段：`options.txt` 的保序读写（门槛就是
    # **原地往返**，测试里那几条逐字节比的就是它）、认得的键那张表、按键冲突，以及"这一段
    # 怎么落到目标上"的计划与执行。`verify` 的 **PB** 一节拿两份假 `options.txt` 一喂就能
    # 逐格念 —— 包括最容易做错的那两格：「目标版本新增的键一个都不许丢」和「认不出来的行
    # 任何一档都不删」。
    ${REPO_ROOT}/src/presets/VanillaOptions.cpp
    ${REPO_ROOT}/src/presets/VanillaOptions.hpp
    # 服务器列表那一段：归一化地址、Append / Replace / Skip
    # 的计划与报告，以及真的落在一份 `servers.dat` 上的那一趟（读回来还是不是游戏认得出来
    # 的那棵树、没碰过的条目是不是逐字节没变）。`verify` 的 **PC** 一节拿 `QTemporaryDir`
    # 造一份真文件（gzip / 裸的两种都有）就能逐格念。
    ${REPO_ROOT}/src/presets/ServerListPreset.cpp
    ${REPO_ROOT}/src/presets/ServerListPreset.hpp
    # 通用文件组那一段：`config/` / `defaultconfigs/`
    # / 资源包 / 光影 —— 只做整份的动作（缺了才拷 / 覆盖 / 跳过）+ 那三道路径闸。`verify`
    # 的 **PD** 一节在两个临时目录之间真拷真读（含 junction 逃逸那两条：源侧和目标的）。
    ${REPO_ROOT}/src/presets/GenericFiles.cpp
    ${REPO_ROOT}/src/presets/GenericFiles.hpp
    # 应用计划那一层：一段一行地算"这一趟会怎么样"。
    # `verify` 的 **PE** 一节：手搓几段假段 + 一份快照，念表上那一列字和那几个数（那一层
    # 只读盘、一个字节都不写，所以除了通用文件组的干跑之外全都是纯值）。
    ${REPO_ROOT}/src/presets/PresetPlanner.cpp
    ${REPO_ROOT}/src/presets/PresetPlanner.hpp
    # 提取与落到实例上：`verify` 的 **PG**
    # 一节拿真实例形状的临时目录走一遍 —— 收一份出来、再落回另一个实例、覆盖那几样
    # 备份在不在、游戏在跑时是不是一个字节都没写。
    ${REPO_ROOT}/src/presets/PresetCapture.cpp
    ${REPO_ROOT}/src/presets/PresetCapture.hpp
    ${REPO_ROOT}/src/presets/PresetApply.cpp
    ${REPO_ROOT}/src/presets/PresetExecutionReport.cpp
    ${REPO_ROOT}/src/presets/PresetApply.hpp
    # 资源包次序 / 启用 + 存档数据包：`verify` 的 **PL**
    # 一节 —— 资源包按 options.txt 那一行摆启用与次序（内置包不动）、数据包落进真存档 /
    # 没那个存档时进 `datapacks-pending/` 并记进 `wtfml.datapacks`（**从不冒充已启用**）。
    ${REPO_ROOT}/src/presets/ResourcePackPreset.cpp
    ${REPO_ROOT}/src/presets/ResourcePackPreset.hpp
    ${REPO_ROOT}/src/presets/DataPackPreset.cpp
    ${REPO_ROOT}/src/presets/DataPackPreset.hpp
    # 模组「真正去下」之前的判据层：
    # `verify` 的 **PM** 一节 —— 必要/可选失败分开、依赖按 `平台:项目号` 去重展开、
    # optional 只列不下、embedded 无视、incompatible 挡住、替代项目不确认就不换、
    # 落盘大小/sha1 校验。全拿手搭的假答复，**一条都不用联网**。
    ${REPO_ROOT}/src/presets/ModInstallPlan.cpp
    ${REPO_ROOT}/src/presets/InstalledModInventory.cpp
    ${REPO_ROOT}/src/presets/ModInstallPlan.hpp
    # 模组预设的兼容检查：本地筛选、去重、
    # 有限并发、generation 隔离、缓存命中/过期、超时 ⇒ `Unknown` —— 全部拿假提供方
    # 逐格念，**一条都不用联网**。`verify` 的 **PH** 一节。
    ${REPO_ROOT}/src/presets/PresetCompatibility.cpp
    ${REPO_ROOT}/src/presets/PresetCompatibility.hpp
    ${REPO_ROOT}/src/presets/ModrinthCompatibility.cpp
    ${REPO_ROOT}/src/presets/ModrinthCompatibility.hpp
    # MaLiLib 那一族的热键：按**结构**认这一族
    # （不看文件名）、一格一格地读/写（认三档：整格是热键 / 一格装开关+热键 / 别的值）、
    # 冲突分析、以及在两份真形状的 `config/*.json` 之间真写一遍。`verify` 的 **PF** 一节拿
    # `QTemporaryDir` 造几份真形状的配置就能逐格念 —— 最要紧的那几格：「目标上没绑的那些
    # 热键一个字不动」（合并不许退化成覆盖）、「值那一格类型变了就不转」、「覆盖档解绑但
    # **不删格子**」。
    ${REPO_ROOT}/src/presets/MalilibKeys.cpp
    ${REPO_ROOT}/src/presets/MalilibKeys.hpp
    # 结构 / 原理图的元数据与方块统计。`verify` 的 **SG**
    # 一节拿 `NbtTree::serialize()` 手搭三种格式的树（含 gzip 那一层）逐项念。
    ${REPO_ROOT}/src/schematic/SchematicInfo.cpp
    ${REPO_ROOT}/src/schematic/SchematicInfo.hpp
    # Replay 录像（`.mcpr`）的元数据。`verify` 的 **RP**
    # 一节拿 `ZipUtil::createZip()` 造几份真形状的 `.mcpr` 逐项念（缺字段 / 不是 zip /
    # 少了 metaData.json 各一路）。
    ${REPO_ROOT}/src/replay/ReplayInfo.cpp
    ${REPO_ROOT}/src/replay/ReplayInfo.hpp
    # 日志文件读行 / 数行（含 gzip）。
    ${REPO_ROOT}/src/util/LogFileReader.cpp
    ${REPO_ROOT}/src/util/LogFileReader.hpp
    # 启动诊断。`verify` 的 **DX** 一节造临时
    # mods/日志/崩溃报告，逐条念「问题 / 提醒 / 信息」的判据。
    ${REPO_ROOT}/src/diagnose/StartupDiagnosis.cpp
    ${REPO_ROOT}/src/diagnose/StartupDiagnosis.hpp
    # 迁移 / 升级副本。`verify` 的 **TR** 一节
    # 自己摆一份源实例（存档 / 模组 / 日志），逐条念兼容结论、默认勾选与"只增不删"。
    ${REPO_ROOT}/src/operations/TransferPlanner.cpp
    ${REPO_ROOT}/src/operations/TransferPlanner.hpp
    # 迁移式升级的九类选择：稳定键 / 默认值 / 指纹 / 依赖提示（纯逻辑，UP 段直接钉）。
    ${REPO_ROOT}/src/operations/UpgradeSelection.cpp
    ${REPO_ROOT}/src/operations/UpgradePageState.cpp
    ${REPO_ROOT}/src/operations/UpgradeReport.cpp
    ${REPO_ROOT}/src/operations/UpgradeHistory.cpp
    ${REPO_ROOT}/src/operations/UpgradeOptions.cpp
    ${REPO_ROOT}/src/operations/UpgradeSelection.hpp
    ${REPO_ROOT}/src/operations/UpgradePageState.hpp
    ${REPO_ROOT}/src/operations/UpgradeReport.hpp
    ${REPO_ROOT}/src/operations/UpgradeHistory.hpp
    ${REPO_ROOT}/src/operations/UpgradeOptions.hpp
    ${REPO_ROOT}/src/operations/TransferApply.cpp
    ${REPO_ROOT}/src/operations/TransferApply.hpp
    # 原地升级事务：`verify_instances` 里那几条人工大实例探针直接调用
    # prepareAndCommit / recoverInterrupted；普通测试仍然只跑临时目录夹具。
    ${REPO_ROOT}/src/operations/InstanceUpgrade.cpp
    ${REPO_ROOT}/src/operations/InstanceUpgrade.hpp
    ${REPO_ROOT}/src/operations/OperationReport.cpp
    ${REPO_ROOT}/src/operations/OperationReport.hpp
    ${REPO_ROOT}/src/settings/EffectiveJavaSettings.cpp
    ${REPO_ROOT}/src/settings/EffectiveJavaSettings.hpp
    ${REPO_ROOT}/src/settings/GlobalJavaSettings.cpp
    ${REPO_ROOT}/src/settings/GameWindowSettings.cpp
    ${REPO_ROOT}/src/settings/GameWindowSettings.hpp
    ${REPO_ROOT}/src/settings/GlobalJavaSettings.hpp
    ${REPO_ROOT}/src/settings/LauncherSettings.cpp
    ${REPO_ROOT}/src/settings/LauncherSettings.hpp
    ${REPO_ROOT}/src/util/AppPaths.hpp
    ${REPO_ROOT}/src/util/SystemMemory.cpp
    # 「这块盘还在不在」：启动点、启动前检查、下载入口都要问一句。
    ${REPO_ROOT}/src/util/StorageAvailability.cpp
    ${REPO_ROOT}/src/util/StorageAvailability.hpp
    # ServerPing：服务器列表那个"连上去问一句"（状态协议）。ZH 节**真起一台假服务器**
    # 连一次 —— 纯函数那半只证明"我按我写的规则拆了包"，协议那头对不对得看那一趟。
    ${REPO_ROOT}/src/util/ServerPing.cpp
    ${REPO_ROOT}/src/util/ServerPing.hpp
    # 工具箱那四件：纯函数，判据在 tests/main.cpp 里那几节（彩色文本三种写法
    # 的往返、坐标 8:1 与 F3+C 那行 tp、哈希的"已知值"归一化与流式读取、铁砧那套惩罚机制）。
    ${REPO_ROOT}/src/tools/TextFormat.cpp
    ${REPO_ROOT}/src/tools/TextFormat.hpp
    ${REPO_ROOT}/src/tools/CoordConvert.cpp
    ${REPO_ROOT}/src/tools/CoordConvert.hpp
    ${REPO_ROOT}/src/tools/FileHash.cpp
    ${REPO_ROOT}/src/tools/FileHash.hpp
    ${REPO_ROOT}/src/tools/AnvilPlanner.cpp
    ${REPO_ROOT}/src/tools/AnvilPlanner.hpp
    # 信标计算 / 酿造助手：ZAI 那一节钉那几组数
    # （累计方块数、效果范围、效果解锁表）与酿造路线（链式前驱、二选一冲突、形态后处理）。
    ${REPO_ROOT}/src/tools/Beacon.cpp
    ${REPO_ROOT}/src/tools/Beacon.hpp
    ${REPO_ROOT}/src/tools/Brewing.cpp
    ${REPO_ROOT}/src/tools/Brewing.hpp
    ${REPO_ROOT}/src/tools/GameVersion.hpp
    # 旗帜设计 / 纹饰搭配：ZAJ 那一节钉图案表、层数上限、
    # 材料合并与纹饰的海龟壳限制。
    ${REPO_ROOT}/src/tools/Banner.cpp
    ${REPO_ROOT}/src/tools/Banner.hpp
    ${REPO_ROOT}/src/tools/ArmorTrim.cpp
    ${REPO_ROOT}/src/tools/ArmorTrim.hpp
    # 运行输出共用模型：只依赖 QtCore（字符串 + 容器），
    # 所以 ZAM 那一节能直接驱动它：认级别 / 计数 / 档案筛选 / 搜索 / 两条上限 / 导出。
    ${REPO_ROOT}/ui/RuntimeOutputModel.cpp
    ${REPO_ROOT}/ui/RuntimeOutputModel.hpp
    # ServerListFile：servers.dat 的读（转 NbtUtil）和「只动 name / ip
    # 两格」的写。它要 NbtUtil（读）和 ZipUtil（gzip 那一层）—— 那两样都在上面。
    ${REPO_ROOT}/src/manage/ServerListFile.cpp
    ${REPO_ROOT}/src/manage/ServerListFile.hpp
    # ServerListController：上面那三个写入口**谁在指挥**（改 / 加 / 删那三条
    # 链路上的四道闸）。它收成一个假 Host 就能驱动的形状 —— ZK13 起那一节正是靠
    # 这一点才验得了（那些闸不夹在 OverlayHost 中间，不必开真窗口）。
    # 它只要 ServerListFile（在上面）和 QtCore。
    ${REPO_ROOT}/src/controllers/ServerListController.cpp
    ${REPO_ROOT}/src/controllers/ServerListController.hpp
    # InstanceMaintenanceController：世界 level.dat 的世代备份落在哪个格子 ——
    # ZQ9 那一节拿两个实例下**同名**的存档钉住"各占一格、清理互不挤代数、最近一份不串门"。
    # 它只要 FolderManager（在上面）、WorldFiles（在上面）、ConfigFileText（头文件）、QtCore。
    ${REPO_ROOT}/src/controllers/InstanceMaintenanceController.cpp
    ${REPO_ROOT}/src/controllers/InstanceMaintenanceController.hpp
    # ConfigFileText 的**备份那一半**（`backupPathFor` / `backupFolderName` / `isBackupFolderName`
    # / `backupFoldersToRemove`）上面那个控制器要用。整份 .cpp 只依赖 QtCore（文本往返那半也在，
    # 但这里只驱动备份这几个纯函数）。
    ${REPO_ROOT}/ui/ConfigFileText.cpp
    ${REPO_ROOT}/ui/ConfigFileText.hpp
    ${REPO_ROOT}/src/launcher/QuickPlay.hpp
    ${REPO_ROOT}/src/util/ZipUtil.cpp
    ${REPO_ROOT}/src/util/ZipUtil.hpp
    ${REPO_ROOT}/src/util/NbtUtil.cpp
    ${REPO_ROOT}/src/util/NbtUtil.hpp
    # NbtTree：`level.dat` 那一棵"能改的树"（世界设置那条路的判据层）。放进来是为了能
    # 拿手工拼的字节去喂它 —— "改一格别的地方一个字节不变""越界的长度当场拒绝"这些
    # 全靠断言钉死，造一个真存档来验是验不到这些角落的。
    ${REPO_ROOT}/src/util/NbtTree.cpp
    ${REPO_ROOT}/src/util/NbtTree.hpp
    # ErrorReport：日志窗底下那颗「导出错误报告…」那一份文本怎么拼（纯函数：时间由调用方
    # 传进来，所以报告头部那几行能被断言钉死）。
    ${REPO_ROOT}/src/util/ErrorReport.cpp
    ${REPO_ROOT}/src/util/ErrorReport.hpp
    # GameExitDiagnosis：游戏异常退出之后那几句话。**纯函数**（值进、
    # 字符串出，不看时钟、不读盘、不起进程）—— "哪条提示是从哪个字段推出来的"这件事只能
    # 在这里逐格念一遍（真机上要真崩一次才走得到，见下面 ZP 节）。
    ${REPO_ROOT}/src/diag/GameExitDiagnosis.cpp
    ${REPO_ROOT}/src/diag/GameExitDiagnosis.hpp
)

# VersionArguments.hpp 是"启动参数按 version.json 展开"那次改动引入的。这个文件在各
# 分支上不一定存在（例如只含下载修复的分支），所以存在才加进来 —— 否则 CMake 会因为找不到
# 文件直接配置失败，而不是让对应用例在运行时如实地报 FAIL（那才是想要的信号）。
if(EXISTS ${REPO_ROOT}/src/launcher/VersionArguments.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/launcher/VersionArguments.hpp)
endif()

# LaunchScriptExport 是「导出启动脚本」那份 .bat 的正文：哪些字符要加引号、`%` 写两遍、
# 令牌换成环境变量引用、带已知凭据就不写。理由同上：别的分支上没有它，存在才加。
if(EXISTS ${REPO_ROOT}/src/launcher/LaunchScriptExport.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/launcher/LaunchScriptExport.hpp)
endif()

# VersionInheritance 是"带模组加载器的版本（inheritsFrom）"那次改动引入的。理由同上：
# 别的分支上没有这两个文件，存在才加，缺了就让对应用例在运行时报 FAIL。
if(EXISTS ${REPO_ROOT}/src/launcher/VersionInheritance.cpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/launcher/VersionInheritance.cpp
                                ${REPO_ROOT}/src/launcher/VersionInheritance.hpp)
endif()

# AsciiPathAlias 是"Java 8 打不开非 ASCII 路径"那次修复引入的（真机上 `20w14∞` 和中文名的
# 1.8 整合包起不来）。理由同上：别的分支上没有这两个文件，存在才加。
if(EXISTS ${REPO_ROOT}/src/launcher/AsciiPathAlias.cpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/launcher/AsciiPathAlias.cpp
                                ${REPO_ROOT}/src/launcher/AsciiPathAlias.hpp)
endif()

# LoaderMeta.hpp 是"Fabric 安装"那次改动引入的（纯头文件：地址怎么拼、回复怎么挑）。
# 理由同上：别的分支上没有它，存在才加。
if(EXISTS ${REPO_ROOT}/src/download/LoaderMeta.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/download/LoaderMeta.hpp)
endif()

# ServerInstall 是"服务端安装链路"那一层纯函数：八档服务端各要
# 下什么、要不要跑安装器、装完的入口是什么、启动参数怎么拼。理由同 LoaderMeta ——
# 一整个文件都是判据，离线喂数据就能钉住；它只依赖 LoaderMeta，不拖任何网络进来。
if(EXISTS ${REPO_ROOT}/src/download/ServerInstall.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/download/ServerInstall.hpp)
endif()

# ResourceCatalog 是"下载页搜模组/光影/资源包/整合包"那次改动引入的（纯函数：地址怎么拼、
# 接口回的那段 JSON 怎么读）。理由同上：别的分支上没有它，存在才加。
if(EXISTS ${REPO_ROOT}/src/download/ResourceCatalog.cpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/download/ResourceCatalog.cpp
                                ${REPO_ROOT}/src/download/ResourceCatalog.hpp)
endif()

# ModDependencies 是"前置模组一键下载 + 实例更新检测"那一层纯函数：
# 缺哪些前置 / 去哪问 / 挑哪一版 / 盘上这一条是不是最新。理由同 ResourceCatalog ——
# 一整个文件都是判据，离线喂数据就能钉住（这一节里的是 R1/R2 两带）。
# **它只依赖 ResourceCatalog**（不含 QNetwork 那一套），所以放在这里不会把网络拖进来。
if(EXISTS ${REPO_ROOT}/src/download/ModDependencies.cpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/download/ModDependencies.cpp
                                ${REPO_ROOT}/src/download/ModDependencies.hpp)
endif()

# ResourceStore 是"下载页取数 + 图标缓存"那一层。这里**只链、不构造**：它一构造就建
# QNetworkAccessManager，而这一节验的全是纯函数/文件层（缓存路径怎么拼、过期怎么判、
# 存了读得回来不回来、哪几种失败算"主机级"）。图标那条网络路仍然只在 mw_check 的
# resource-live 里真跑（那一条要联外网）。
if(EXISTS ${REPO_ROOT}/src/download/ResourceStore.cpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/download/ResourceStore.cpp
                                ${REPO_ROOT}/src/download/ResourceStore.hpp)
endif()

# ImageDecode 是"列表图标走系统 WIC 解 webp"那次改动引入的（Qt 先试，Qt 认不得的交给
# WIC）。这条能离线验：喂一段真 webp 字节进去看解不解得出来（webpAvailable() 是"这台机器
# 现在能不能解"，不是常量，所以那条断言要按它分支，见 main.cpp 的 ZC 段）。
# 理由同上：别的分支上没有它，存在才加。
if(EXISTS ${REPO_ROOT}/src/util/DailyQuote.cpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/util/DailyQuote.cpp
                                ${REPO_ROOT}/src/util/DailyQuote.hpp)
endif()
# HomeGreeting：主页大标题的问候规则（纯函数）。verify 单测它，mw_check 经 MainWindow 用它。
if(EXISTS ${REPO_ROOT}/src/util/HomeGreeting.cpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/util/HomeGreeting.cpp
                                ${REPO_ROOT}/src/util/HomeGreeting.hpp)
endif()
if(EXISTS ${REPO_ROOT}/src/util/ImageDecode.cpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/util/ImageDecode.cpp
                                ${REPO_ROOT}/src/util/ImageDecode.hpp)
endif()

# 任务账本那一套。它**一个 QWidget 都不碰、一个网络都
# 不开**，所以 verify 这一节能直接 new 一个、喂几条假任务进去，把那几条纯规则钉死：
# 「终态只落一次」「号从盘上续」「没跑完的读回来算已中断」。
# 理由同上：别的分支上没有它，存在才加。
if(EXISTS ${REPO_ROOT}/src/tasks/TaskManager.cpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/tasks/TaskManager.cpp
                                ${REPO_ROOT}/src/tasks/TaskManager.hpp
                                ${REPO_ROOT}/src/tasks/TaskHistory.cpp
                                ${REPO_ROOT}/src/tasks/TaskHistory.hpp
                                ${REPO_ROOT}/src/tasks/TaskSnapshot.hpp
                                ${REPO_ROOT}/src/tasks/TaskId.hpp)
endif()

# ThemeImage 是设置页「背景图」那两行收图的那一层（用户点名要的"能换背景图"）。
# 它能离线验：V5s 那一段在一个临时数据根目录里真存真删，只看盘上少了哪一个文件。
# 理由同上：别的分支上没有它，存在才加。
if(EXISTS ${REPO_ROOT}/src/util/ThemeImage.cpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/util/ThemeImage.cpp
                                ${REPO_ROOT}/src/util/ThemeImage.hpp)
endif()

# 服务端工作区那一层：判据层（`ServerModel`，
# 纯值进、结构出：状态那七档、`eula.txt` 算不算接受、`server.properties` 解/写、我们记的
# 那一份的字节格式）+ 扫盘那一层（`ServerRegistry`）。两样都能离线钉死 —— 后者在一个**临时
# 数据根目录**里真建真读真删（见 main.cpp 的 ZW / ZX 两节）。
# 理由同上：别的分支上没有它，存在才加。
if(EXISTS ${REPO_ROOT}/src/server/ServerModel.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/server/ServerModel.hpp
                                ${REPO_ROOT}/src/server/ServerRegistry.cpp
                                ${REPO_ROOT}/src/server/ServerRegistry.hpp)
endif()

# 服务端安装那条路：`ServerInstallRun.hpp` 是
# **纯头**（还差哪一份元数据、取回来的那份怎么读、跑工具的命令行、分成几步）—— 全是纯函数，
# 喂一份 json 就出结论，不联网不起进程。所以 `verify` 的 **ZY** 一节能把它整片钉死。
# 驱动层（`ServerInstallController`）不在这儿：它要 QObject / QProcess，归 mw_check。
if(EXISTS ${REPO_ROOT}/src/server/ServerInstallRun.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/server/ServerInstallRun.hpp)
endif()

# 服务端的"跑"那条路：
# `ServerRun.hpp` 同样是**纯头**（那条启动命令怎么拼、起来没有、退了算哪一档状态、宽限多久
# 才允许强杀）—— 不联网不起进程不看时钟，所以 `verify` 的 **ZZ** 一节能把它整片钉死。
# 驱动层（`ServerProcess`，一条真 QProcess）不在这儿：归 mw_check（T41 拿它自己的 exe 当
# "服务端"跑一整趟）。
if(EXISTS ${REPO_ROOT}/src/server/ServerRun.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/server/ServerRun.hpp)
endif()

# 「服务端设置」那几层：
#   * `ServerPropertiesForm` —— `server.properties` 常用字段那张表（每一项的值域、"这个值
#     收不收"、把补丁叠回一份原文上、文件级的那几处不对）；
#   * `ServerPortCheck` —— 那一件**真去占一次端口**的小事（它要 Qt6::Network，本工程已链）；
#   * `ServerCheck` —— 体检（纯值进、结论出）。
# 所以 `verify` 的 **ZV** 一节能把它们整片钉死（真占端口的只有那一发，占的是临时挑的号）。
if(EXISTS ${REPO_ROOT}/src/server/ServerPropertiesForm.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/server/ServerPropertiesForm.hpp
                                ${REPO_ROOT}/src/server/ServerPropertiesForm.cpp
                                ${REPO_ROOT}/src/server/ServerPortCheck.hpp
                                ${REPO_ROOT}/src/server/ServerPortCheck.cpp
                                ${REPO_ROOT}/src/server/ServerCheck.hpp)
endif()

# 服务端那四份玩家名单：纯解析 / 纯拼 JSON，一个字节的盘都不碰。
# verify 的 **ZAE** 一节直接钉它（外加离线 UUID 那个已知向量）。
if(EXISTS ${REPO_ROOT}/src/server/ServerPlayers.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/server/ServerPlayers.hpp
                                ${REPO_ROOT}/src/server/ServerPlayers.cpp)
endif()

# 崩溃自动重启那条策略：纯判据，verify 的 **ZAF** 一节钉它。
if(EXISTS ${REPO_ROOT}/src/server/ServerRestart.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/server/ServerRestart.hpp
                                ${REPO_ROOT}/src/server/ServerRestart.cpp)
endif()

# 服务端资源包：条目表判据是纯的；`inspect()` 会借 ZipUtil（真 zip）。
# verify 的 **ZAG** 一节钉它。
if(EXISTS ${REPO_ROOT}/src/server/ServerResourcePack.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/server/ServerResourcePack.hpp
                                ${REPO_ROOT}/src/server/ServerResourcePack.cpp)
endif()

# 服务端内容（模组 / 数据包）：名字 / 分类 / 改名全是纯函数。
# verify 的 **ZAH** 一节钉它。
if(EXISTS ${REPO_ROOT}/src/server/ServerContent.hpp)
    list(APPEND PROJECT_SOURCES ${REPO_ROOT}/src/server/ServerContent.hpp
                                ${REPO_ROOT}/src/server/ServerContent.cpp)
endif()

include(core_modules.cmake)
add_executable(verify main.cpp ${WTFML_CORE_CASE_OBJECTS})
target_link_libraries(verify PRIVATE wtfml_core_fixture)
# core_modules 是「所有核心验证程序」的入口。兼容驱动也归它：ctest 的
# wtfml.core-bootstrap 跑的就是 verify --bootstrap-check，漏了这条依赖时
# 只建 core_modules 会留下上一次的旧 exe，自检看起来"超时"其实是在跑旧的全套。
add_dependencies(core_modules verify)

target_include_directories(verify PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/ui)
target_link_libraries(verify PRIVATE Qt6::Core Qt6::Network Qt6::Gui Qt6::Concurrent)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(verify PRIVATE -O0)
    # Keep MinGW's large-object support for template-heavy assertion modules.
    # verify is a compatibility driver; routine CI builds independent domains.
    target_compile_options(verify PRIVATE -Wa,-mbig-obj)
endif()

# TokenStore 的 DPAPI（CryptProtectData）在这个库里；Gui 是 AuthManager.cpp 里的
# QDesktopServices（verify 自己不构造 AuthManager，只是把它的一个静态函数链进来）。
target_link_libraries(verify PRIVATE crypt32 advapi32)

# ImageDecode.cpp 那三个（见主工程 CMakeLists.txt 里同一段）：ole32 起 COM、shlwapi 要
# SHCreateMemStream、windowscodecs 是 WIC 本体。MinGW 上这一句不加的话，链接期报的是一串
# `undefined reference to IID_IWICImagingFactory` 之类 —— 不是运行期，别指望跑起来才发现。
if(WIN32)
    target_link_libraries(verify PRIVATE ole32 shlwapi windowscodecs)
endif()

# 验证代码按仓库根定位 tests/meta。
target_compile_definitions(verify PRIVATE REPO_ROOT_PATH="${REPO_ROOT}")

wtfml_add_test(NAME wtfml.core COMMAND verify_baseline)
wtfml_add_test(NAME wtfml.core-bootstrap COMMAND verify --bootstrap-check)
set_tests_properties(wtfml.core-bootstrap PROPERTIES LABELS "core;compile" TIMEOUT 30)
wtfml_add_test(NAME wtfml.test-runner COMMAND pwsh -NoProfile -File "${REPO_ROOT}/tools/test-runner-check.ps1")
wtfml_add_test(NAME wtfml.test-modules COMMAND pwsh -NoProfile -File "${REPO_ROOT}/tools/check-test-modules.ps1")
set_tests_properties(wtfml.test-modules PROPERTIES LABELS "tooling" TIMEOUT 60)
# 表面登记：新写的对话框 / showPage 页面没登记进 sweep 的表面清单就红（见 tools/check-surface-registry.ps1）。
wtfml_add_test(NAME wtfml.surface-registry COMMAND pwsh -NoProfile -File "${REPO_ROOT}/tools/check-surface-registry.ps1")
set_tests_properties(wtfml.surface-registry PROPERTIES LABELS "tooling" TIMEOUT 120)
if(NOT DEFINED WTFML_BUILD_NETWORK_DIAG OR WTFML_BUILD_NETWORK_DIAG)
    wtfml_add_test(NAME wtfml.ci-partitions COMMAND pwsh -NoProfile -File "${REPO_ROOT}/tools/check-ci-partitions.ps1"
        -BuildDir "${CMAKE_BINARY_DIR}")
    set_tests_properties(wtfml.ci-partitions PROPERTIES LABELS "tooling" TIMEOUT 60)
endif()
set_tests_properties(wtfml.test-runner PROPERTIES
    LABELS "tooling"
    TIMEOUT 60
)
# TIMEOUT 900：300 秒在 CI 上不够。GitHub 那个 windows runner 上，300 秒
# 只跑到 V5z（源码里大约四成），而被杀的那一刻还在稳定出 PASS —— 不是挂住，是慢
# （O0 的验证工程 + 临时目录里一堆真文件 + 那几段等 10/30 秒超时的用例）。本机同样
# 这一份大约 100 秒。放到 900 给 CI 留三倍富余；工作流自己还有 45 分钟的总闸。
set_tests_properties(wtfml.core PROPERTIES
    LABELS "core"
    TIMEOUT 900
)

# Windows resolves libstdc++ from PATH.  Developers often have another MinGW
# installed globally; loading that runtime beside a Qt built with MinGW 13 can
# crash before a test reaches main().  Prepend the runtime that belongs to the
# compiler CMake actually selected, and the directory containing Qt6Core.dll.
get_filename_component(WTFML_TEST_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_target_property(WTFML_TEST_QT_CORE Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT WTFML_TEST_QT_CORE)
    get_target_property(WTFML_TEST_QT_CORE Qt6::Core IMPORTED_LOCATION)
endif()
get_filename_component(WTFML_TEST_QT_BIN "${WTFML_TEST_QT_CORE}" DIRECTORY)
set(WTFML_TEST_PATH "${WTFML_TEST_COMPILER_BIN};${WTFML_TEST_QT_BIN};$ENV{PATH}")
string(REPLACE ";" "\\;" WTFML_TEST_PATH "${WTFML_TEST_PATH}")
set_property(TEST wtfml.core wtfml.core-bootstrap APPEND PROPERTY
    ENVIRONMENT "PATH=${WTFML_TEST_PATH}"
)
set(WTFML_TEST_RUNTIME_ENVIRONMENT "PATH=${WTFML_TEST_PATH}")
foreach(domain downloads install instances java accounts resources launch servers activity presets toolbox agent)
    set_property(TEST wtfml.core-${domain} APPEND PROPERTY ENVIRONMENT "PATH=${WTFML_TEST_PATH}")
endforeach()

get_property(WTFML_CORE_TESTS DIRECTORY PROPERTY TESTS)
set_property(TEST ${WTFML_CORE_TESTS} APPEND PROPERTY LABELS ci-core)

add_subdirectory(ui_check)
add_executable(wtfml_modpack_boundary EXCLUDE_FROM_ALL ${REPO_ROOT}/tests/acceptance/modpack_boundary/main.cpp)
target_link_libraries(wtfml_modpack_boundary PRIVATE wtfml_core_runtime Qt6::Network)
add_subdirectory(mw_check)


# Audited modules use independent settings, default/current data roots and temporary fixtures.
# They do not launch shared helper processes. Expand this list only with overlap evidence.
set_tests_properties(wtfml.core-accounts wtfml.core-activity wtfml.core-toolbox
    PROPERTIES RUN_SERIAL FALSE)
set_property(TEST wtfml.core-accounts wtfml.core-activity wtfml.core-toolbox
    APPEND PROPERTY LABELS isolated-parallel)

# Override a single import in this test process only; the production binary has no fault hook.
if(MINGW AND WIN32)
    add_executable(token_protection_fault token_protection_fault/main.cpp)
    target_link_libraries(token_protection_fault PRIVATE wtfml_core_runtime crypt32)
    wtfml_add_test(NAME wtfml.token-protection-fault COMMAND token_protection_fault)
    set_tests_properties(wtfml.token-protection-fault PROPERTIES
        LABELS "core;security;ci-core" TIMEOUT 30 ENVIRONMENT "${WTFML_TEST_RUNTIME_ENVIRONMENT}")
endif()
