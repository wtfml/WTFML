
# Assertion objects belong to one domain; a targeted build never builds other domains.
add_library(wtfml_core_runtime STATIC ${PROJECT_SOURCES})
target_include_directories(wtfml_core_runtime PUBLIC ${REPO_ROOT}/src ${REPO_ROOT}/ui ${REPO_ROOT}/tests)
target_link_libraries(wtfml_core_runtime PUBLIC Qt6::Core Qt6::Network Qt6::Gui Qt6::Concurrent crypt32 ole32 shlwapi windowscodecs shell32 user32 uuid)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
 target_compile_options(wtfml_core_runtime PRIVATE -O0)
endif()
function(wtfml_core_compile target)
 target_link_libraries(${target} PRIVATE wtfml_core_fixture)
 if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(${target} PRIVATE -O0 -Wa,-mbig-obj)
 endif()
endfunction()
add_library(wtfml_core_fixture STATIC core_modules/Support.cpp)
target_link_libraries(wtfml_core_fixture PUBLIC wtfml_core_runtime)
target_compile_definitions(wtfml_core_fixture PUBLIC REPO_ROOT_PATH="${REPO_ROOT}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
 target_compile_options(wtfml_core_fixture PRIVATE -O0 -Wa,-mbig-obj)
endif()
set(WTFML_CORE_CASE_OBJECTS)
set(WTFML_CORE_MODULE_TARGETS)
add_library(core_cases_downloads OBJECT
 core_modules/AssetNamesTests.cpp
 core_modules/PipelineTests.cpp
 core_modules/DownloadConcurrencyTests.cpp
 core_modules/DownloadSpeedLimitTests.cpp
 core_modules/DownloadProxyTests.cpp
 core_modules/LocalFileReuseTests.cpp
 core_modules/DownloadReceiptScaleTests.cpp
)
wtfml_core_compile(core_cases_downloads)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_downloads>)
add_executable(verify_downloads core_modules/downloads_main.cpp $<TARGET_OBJECTS:core_cases_downloads>)
wtfml_core_compile(verify_downloads)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_downloads)
wtfml_add_test(NAME wtfml.core-downloads COMMAND verify_downloads)
set_tests_properties(wtfml.core-downloads PROPERTIES LABELS "core;core-downloads;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_install OBJECT
 core_modules/VersionManagerBatch.cpp
 core_modules/VersionManagerFetchFail.cpp
 core_modules/LoaderInheritanceTests.cpp
 core_modules/LoaderInstallTests.cpp
 core_modules/LoaderInstallLive.cpp
)
wtfml_core_compile(core_cases_install)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_install>)
add_executable(verify_install core_modules/install_main.cpp $<TARGET_OBJECTS:core_cases_install>)
wtfml_core_compile(verify_install)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_install)
wtfml_add_test(NAME wtfml.core-install COMMAND verify_install)
set_tests_properties(wtfml.core-install PROPERTIES LABELS "core;core-install;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_instances OBJECT
 core_modules/VersionNameTests.cpp
 core_modules/VersionJvmArgsTests.cpp
 core_modules/InstanceFileTests.cpp
 core_modules/VersionMetaWriteTests.cpp
 core_modules/InstanceTraitsTests.cpp
 core_modules/InstallOccupancyTests.cpp
 core_modules/FileOperationOccupancyTests.cpp
core_modules/UpgradeSelectionTests.cpp
core_modules/UpgradeReportTests.cpp
core_modules/UpgradeHistoryTests.cpp
core_modules/UpgradeOptionsTests.cpp
 core_modules/RenameInstanceTests.cpp
 core_modules/InstanceGroupTests.cpp
 core_modules/InstalledScanTests.cpp
 core_modules/VersionChannelTests.cpp
 core_modules/IconCacheTests.cpp
 core_modules/ImageDecodeTests.cpp
 core_modules/CopyInstanceAndWorldTests.cpp
 core_modules/VersionIntegrityTests.cpp
 core_modules/BackupTests.cpp
 core_modules/InstanceFilterTests.cpp
 core_modules/UpdatePolicyTests.cpp
 core_modules/UpdateRollbackTests.cpp
 core_modules/FolderRemovalTests.cpp
 core_modules/TransferPlannerTests.cpp
 core_modules/OperationReportTests.cpp
 core_modules/LongPathTests.cpp
)
wtfml_core_compile(core_cases_instances)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_instances>)
add_executable(verify_instances core_modules/instances_main.cpp $<TARGET_OBJECTS:core_cases_instances>)
wtfml_core_compile(verify_instances)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_instances)
wtfml_add_test(NAME wtfml.core-instances COMMAND verify_instances)
set_tests_properties(wtfml.core-instances PROPERTIES LABELS "core;core-instances;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_java OBJECT
 core_modules/MemorySuggestionTests.cpp
 core_modules/EffectiveJavaSettingsTests.cpp
 core_modules/JavaWarnMismatchTests.cpp
 core_modules/JavaReleaseParseTests.cpp
 core_modules/AsciiAliasTests.cpp
)
wtfml_core_compile(core_cases_java)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_java>)
add_executable(verify_java core_modules/java_main.cpp $<TARGET_OBJECTS:core_cases_java>)
wtfml_core_compile(verify_java)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_java)
wtfml_add_test(NAME wtfml.core-java COMMAND verify_java)
set_tests_properties(wtfml.core-java PROPERTIES LABELS "core;core-java;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_accounts OBJECT
 core_modules/PlayerSkinTests.cpp
 core_modules/OfflineSkinTests.cpp
 core_modules/DefaultSkinTests.cpp
 core_modules/AccountSkinTests.cpp
 core_modules/OAuthEndpointTests.cpp
 core_modules/SettingsSyncRetryTests.cpp
)
wtfml_core_compile(core_cases_accounts)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_accounts>)
add_executable(verify_accounts core_modules/accounts_main.cpp $<TARGET_OBJECTS:core_cases_accounts>)
wtfml_core_compile(verify_accounts)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_accounts)
wtfml_add_test(NAME wtfml.core-accounts COMMAND verify_accounts)
set_tests_properties(wtfml.core-accounts PROPERTIES LABELS "core;core-accounts;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_resources OBJECT
 core_modules/ModpackImportTests.cpp
 core_modules/ModpackExportFormatTests.cpp
 core_modules/ModpackExportLookupTests.cpp
 core_modules/ModpackExportPipelineTests.cpp
 core_modules/ThemeImageTests.cpp
 core_modules/MultiMCPatchImportTests.cpp
 core_modules/CurseForgeTests.cpp
 core_modules/ModpackCompletionTests.cpp
 core_modules/AssetDirectorKnownSetTests.cpp
 core_modules/ResourceCatalogTests.cpp
 core_modules/ResourceDetailTests.cpp
 # DQ：主页「每日一言」的判据层（纯函数 + 假响应，不联网）。
 core_modules/DailyQuoteTests.cpp
 # HG：主页大标题的问候规则（纯函数）。
 core_modules/HomeGreetingTests.cpp
 # ZI：资源商店那几发也走共享的 Control 额度（本机假接口，不碰外网）。
 core_modules/ResourceStoreBudgetTests.cpp
 # R1：更新检查批次表在"一发已收场、一发还在飞"时的收场（R1）。
 core_modules/ResourceStoreLatestRunTests.cpp
)
wtfml_core_compile(core_cases_resources)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_resources>)
add_executable(verify_resources core_modules/resources_main.cpp $<TARGET_OBJECTS:core_cases_resources>)
wtfml_core_compile(verify_resources)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_resources)
wtfml_add_test(NAME wtfml.core-resources COMMAND verify_resources)
set_tests_properties(wtfml.core-resources PROPERTIES LABELS "core;core-resources;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_launch OBJECT
 core_modules/LaunchOptionsTests.cpp
 core_modules/ProcessPathLimitTests.cpp
 core_modules/QuickPlayTests.cpp core_modules/ErrorReportTests.cpp
 core_modules/LaunchScriptExportTests.cpp
 core_modules/NativesMarkerTests.cpp
 core_modules/GameExitDiagnosisTests.cpp
 core_modules/LaunchHistoryTests.cpp
 core_modules/StartupDiagnosisTests.cpp
 core_modules/StorageAvailabilityTests.cpp
)
wtfml_core_compile(core_cases_launch)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_launch>)
add_executable(verify_launch core_modules/launch_main.cpp $<TARGET_OBJECTS:core_cases_launch>)
wtfml_core_compile(verify_launch)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_launch)
wtfml_add_test(NAME wtfml.core-launch COMMAND verify_launch)
set_tests_properties(wtfml.core-launch PROPERTIES LABELS "core;core-launch;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_servers OBJECT
 core_modules/ServerPingTests.cpp
 core_modules/ServerListEditTests.cpp
 core_modules/ServerListControllerTests.cpp
 core_modules/ServerModelTests.cpp
 core_modules/ServerRegistryTests.cpp
 core_modules/ServerInstallRunTests.cpp
 core_modules/ServerRunTests.cpp
 core_modules/ServerSettingsTests.cpp
 # R2：NBT 读入的项数/元素上限（R2）。
 core_modules/NbtLimitsTests.cpp
 core_modules/ServerPlayersTests.cpp
 core_modules/ServerRestartTests.cpp
 core_modules/ServerResourcePackTests.cpp
 core_modules/ServerContentTests.cpp
)
wtfml_core_compile(core_cases_servers)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_servers>)
add_executable(verify_servers core_modules/servers_main.cpp $<TARGET_OBJECTS:core_cases_servers>)
wtfml_core_compile(verify_servers)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_servers)
wtfml_add_test(NAME wtfml.core-servers COMMAND verify_servers)
set_tests_properties(wtfml.core-servers PROPERTIES LABELS "core;core-servers;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_activity OBJECT
 core_modules/TaskLedgerTests.cpp
)
wtfml_core_compile(core_cases_activity)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_activity>)
add_executable(verify_activity core_modules/activity_main.cpp $<TARGET_OBJECTS:core_cases_activity>)
wtfml_core_compile(verify_activity)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_activity)
wtfml_add_test(NAME wtfml.core-activity COMMAND verify_activity)
set_tests_properties(wtfml.core-activity PROPERTIES LABELS "core;core-activity;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_presets OBJECT
 core_modules/PresetCompatibilityTests.cpp
 core_modules/PresetStressTests.cpp
 core_modules/PresetStoreTests.cpp
 core_modules/VanillaOptionsTests.cpp
 core_modules/ServerListPresetTests.cpp
 core_modules/GenericFilesTests.cpp
 core_modules/PresetPlannerTests.cpp
 core_modules/MalilibKeysTests.cpp
 core_modules/PresetCaptureApplyTests.cpp
 core_modules/ResourceAndDataPackTests.cpp
 core_modules/ModInstallPlanTests.cpp
 core_modules/PresetSnapshotScaleTests.cpp
)
wtfml_core_compile(core_cases_presets)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_presets>)
add_executable(verify_presets core_modules/presets_main.cpp $<TARGET_OBJECTS:core_cases_presets>)
wtfml_core_compile(verify_presets)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_presets)
wtfml_add_test(NAME wtfml.core-presets COMMAND verify_presets)
set_tests_properties(wtfml.core-presets PROPERTIES LABELS "core;core-presets;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_toolbox OBJECT
 core_modules/ToolboxExtraTests.cpp
 core_modules/TextFormatTests.cpp
 core_modules/CoordConvertTests.cpp
 core_modules/FileHashTests.cpp
 core_modules/AnvilPlannerTests.cpp
 core_modules/SchematicInfoTests.cpp
 core_modules/ReplayInfoTests.cpp
 core_modules/LogFileReaderTests.cpp
)
wtfml_core_compile(core_cases_toolbox)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_toolbox>)
add_executable(verify_toolbox core_modules/toolbox_main.cpp $<TARGET_OBJECTS:core_cases_toolbox>)
wtfml_core_compile(verify_toolbox)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_toolbox)
wtfml_add_test(NAME wtfml.core-toolbox COMMAND verify_toolbox)
set_tests_properties(wtfml.core-toolbox PROPERTIES LABELS "core;core-toolbox;ci-core" TIMEOUT 900 RUN_SERIAL TRUE)
add_library(core_cases_agent OBJECT
 core_modules/AgentSseParserTests.cpp
 core_modules/AgentProviderAdapterTests.cpp
 core_modules/AgentSessionStoreTests.cpp
 core_modules/AgentSessionCryptoTests.cpp
 core_modules/AgentCredentialStoreTests.cpp
 core_modules/AgentContextTests.cpp
 core_modules/AgentLoopTests.cpp
 core_modules/AgentSkillsTests.cpp
 core_modules/AgentMarkdownTests.cpp
 core_modules/AgentPathPolicyTests.cpp
 core_modules/AgentEgressPolicyTests.cpp
 core_modules/AgentApprovalTests.cpp
 core_modules/AgentArtifactTests.cpp
 core_modules/AgentInstanceLockTests.cpp
 core_modules/AgentBackupLifecycleTests.cpp
 core_modules/AgentToolTests.cpp
 core_modules/AgentOpenAiClientTests.cpp
 core_modules/AgentDiagnosticProjectionTests.cpp
 core_modules/AgentLauncherToolsTests.cpp
 core_modules/AgentWorkflowTests.cpp
 core_modules/AgentMcpProtocolTests.cpp
 core_modules/AgentMcpTrustTests.cpp
 core_modules/AgentMcpClientTests.cpp
 core_modules/AgentWebPolicyTests.cpp
 core_modules/AgentWebToolsTests.cpp
 core_modules/AgentCommandToolsTests.cpp
 core_modules/AgentPerformanceTests.cpp
)
wtfml_core_compile(core_cases_agent)
list(APPEND WTFML_CORE_CASE_OBJECTS $<TARGET_OBJECTS:core_cases_agent>)
add_executable(verify_agent core_modules/agent_main.cpp $<TARGET_OBJECTS:core_cases_agent>)
wtfml_core_compile(verify_agent)
list(APPEND WTFML_CORE_MODULE_TARGETS verify_agent)
wtfml_add_test(NAME wtfml.core-agent COMMAND verify_agent)
set_tests_properties(wtfml.core-agent PROPERTIES LABELS "core;core-agent;ci-core" TIMEOUT 300 RUN_SERIAL TRUE)
add_executable(verify_baseline main.cpp)
target_compile_definitions(verify_baseline PRIVATE WTFML_CORE_BASELINE)
wtfml_core_compile(verify_baseline)
add_custom_target(core_modules DEPENDS verify_baseline ${WTFML_CORE_MODULE_TARGETS})


