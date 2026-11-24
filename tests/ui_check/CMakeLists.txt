# UI 回归验证：offscreen 起一个 QMainWindow，断言布局几何和控件行为。

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# assets/icons.qrc（四个加载器的真图）也要走 RCC。产品那份 CMakeLists
# 早就是开的，这三个验证工程只开了 AUTOMOC —— 不开的话 .qrc 会被**静默忽略**：
# 编译链接全都过，只是 `:/assets/loader/*.png` 全是空图（症状见下面那行的注释）。
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets GuiPrivate)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
get_filename_component(REPO_ROOT ${REPO_ROOT} REALPATH)

# GroupDialog.cpp 也编进来：它和 MainWindowUI 同一个层次（纯界面、不依赖 src/），
# 而且 MainWindowUI 里那条"组在界面上写成什么"的规则就是从这个类取的。
add_library(wtfml_ui_runtime STATIC
                                # Z11 那两条原生消息的契约断言要拼 MSG：单独一个 TU，
                                # 免得 main.cpp 去 include <windows.h>（理由见那个文件头）。

                                ${REPO_ROOT}/ui/AccountSkinDialog.cpp
                                ${REPO_ROOT}/ui/MainWindowUI.cpp
                                ${REPO_ROOT}/ui/PresetInstallPicker.cpp
                                # 预设管理页：加载态走查截图（--loading）要把它摆上来；纯界面，不认识业务。
                                ${REPO_ROOT}/ui/PresetManagePage.cpp
                                ${REPO_ROOT}/ui/PresetEditorPage.cpp
                                ${REPO_ROOT}/ui/PresetOptionsEditor.cpp
                                ${REPO_ROOT}/ui/PresetModEditor.cpp
                                ${WTFML_UI_PAGE_SOURCES}
                                # 「最大内存」那一行那根条：MainWindowUI.cpp 和
                                # InstanceSettingsDialog.cpp 都直接 new 它，少了这一个文件会
                                # 在链接那一步报一串 undefined reference to MemorySlider::…
                                ${REPO_ROOT}/ui/MemorySlider.cpp
                                ${REPO_ROOT}/ui/GroupDialog.cpp
                                ${REPO_ROOT}/ui/GroupListDelegate.cpp
                                ${REPO_ROOT}/ui/ListRowDelegate.cpp
                                # 内容区覆盖层 + 内嵌提示面板：设置界面和所有提示
                                # 都开在这一层里，不再另开独立窗口。它们是纯界面（不认识业务），
                                # 所以归这一层验。
                                ${REPO_ROOT}/ui/OverlayHost.cpp
                                ${REPO_ROOT}/ui/InlineMessage.cpp
                                ${REPO_ROOT}/ui/InstanceSettingsDialog.cpp
${REPO_ROOT}/ui/InstanceSettingsSignature.cpp
                                ${REPO_ROOT}/ui/InstanceUpdateWizard.cpp
    # 迁移式升级页的骨架：只画状态、只转发动作（判断在 operations/Upgrade*）。
    ${REPO_ROOT}/ui/InstanceUpgradePage.cpp
    ${REPO_ROOT}/ui/UpgradeReportDialog.cpp
    ${REPO_ROOT}/ui/UpgradeHistoryDialog.cpp
    ${REPO_ROOT}/ui/InstanceFilesPage.cpp
    ${REPO_ROOT}/ui/ShellOpenWith.hpp
    ${REPO_ROOT}/ui/ShellOpenWith.cpp
    ${REPO_ROOT}/ui/InstanceFilesPageRows.cpp
    ${REPO_ROOT}/ui/InstanceFilesSelection.cpp
    ${REPO_ROOT}/ui/InstanceFilesPageFilter.cpp
    ${REPO_ROOT}/ui/InstanceFilesPageScroll.cpp
    ${REPO_ROOT}/ui/InstanceFilesPageInfoPanel.cpp
    ${REPO_ROOT}/ui/InstanceFilesPageStructures.cpp
    ${REPO_ROOT}/ui/InstanceFilesPageCapabilities.cpp
    ${REPO_ROOT}/ui/InstanceFilesPageDirectoryStats.cpp
    ${REPO_ROOT}/ui/InstanceFilesPageRefresh.cpp
    ${REPO_ROOT}/ui/InstanceFilesPageState.cpp
    ${REPO_ROOT}/ui/InstanceFilesPageEventFilter.cpp
    ${REPO_ROOT}/ui/ResourcePackPage.cpp
    # 「配置文件」那一页：编辑器那一格和它那套纯函数（认 BOM / 认行尾 /
    # 认是不是 UTF-8）都是 ui/ 的东西，InstanceSettingsDialog 直接 new 它。少了这两个
    # 文件，链接期报的是 `undefined reference to ConfigFileEditorPage::…`。
    ${REPO_ROOT}/ui/ConfigFileText.cpp
    ${REPO_ROOT}/ui/ConfigFileEditorPage.cpp
    # 「条目」那一格：一条 `键 = 值` 是怎么从正文里认出来的（键路径、类型、
    # 值在哪几个字符上）—— ConfigFileEditorPage 直接调它，所以这一份也得编进来。
    ${REPO_ROOT}/ui/ConfigEntries.cpp
    # 「世界设置」那一页：和「配置文件」那一页同一个来路 ——
    # InstanceSettingsDialog 直接 new 它，少了这一个文件链接期报一串
    # `undefined reference to WorldSettingsPage::…`。
    ${REPO_ROOT}/ui/WorldSettingsPage.cpp
    # 规则那一格名字上盖的官方中文：WorldSettingsPage 直接调它。
    ${REPO_ROOT}/ui/GameRuleNames.cpp

                                ${REPO_ROOT}/ui/JavaMismatchDialog.cpp
                                ${REPO_ROOT}/ui/JavaManageDialog.cpp
                                ${REPO_ROOT}/ui/SkinModel.cpp
                                ${REPO_ROOT}/ui/VersionIcons.cpp
                                # 四个加载器的标志那几张真图：qrc 不列在这儿，
                                # `:/assets/loader/*.png` 就是空图，loaderMark() 一路退回草方块
                                # （ui_check 那条「四张标志互不相同」会红，产品自己看着一切正常）。
                                ${REPO_ROOT}/ui/AgentMarkdown.cpp
                                ${REPO_ROOT}/ui/AgentMessageView.cpp
                                ${REPO_ROOT}/ui/AgentComposer.cpp
                                ${REPO_ROOT}/ui/AgentPage.cpp
                                ${REPO_ROOT}/ui/AgentSecretEdit.cpp
                                ${REPO_ROOT}/ui/AgentSettingsPage.cpp
                                ${REPO_ROOT}/ui/AgentToolCard.cpp
                                ${REPO_ROOT}/ui/AgentApprovalCard.cpp
                                ${REPO_ROOT}/ui/AgentMcpPage.cpp
                                ${REPO_ROOT}/ui/AgentViewTypes.hpp
                                ${REPO_ROOT}/assets/icons.qrc
                                # 「启用 / 禁用」那颗开关和 MOTD 那套配色
                                # （MotdText 被 VersionTreeDelegate 引到）。
                                ${REPO_ROOT}/ui/ToggleSwitch.cpp
                                ${REPO_ROOT}/ui/MotdText.cpp
                                ${REPO_ROOT}/ui/VersionTreeDelegate.cpp
                                ${REPO_ROOT}/ui/LoadingState.cpp
                                ${REPO_ROOT}/ui/ResourceDetailPage.cpp
                                ${REPO_ROOT}/ui/InstanceGroupDelegate.cpp
                                # 下载页那四页的"点的是哪一类"和"去问哪一类"是靠一个字符串对
                                # 起来的（MainWindowUI::downloadCategoryKeys() ==
                                # ResourceCatalog::kindKey()）。这条等号只有把两边都编进同一个
                                # 可执行文件里才钉得住 —— 主验证工程不编 UI，钉不了。
                                # ResourceStore 不在其列：那是异步 I/O，还要一个真的
                                # QGuiApplication 才画得出图标。
                                ${REPO_ROOT}/src/download/ResourceCatalog.cpp
                                # 前置模组 / 更新检测那两套判据的**字**。这一层
                                # 只读它那几个 *Text()（"说不清"这三个字的唯一出处）——
                                # 界面上的 tooltip 和它必须是同一句话，这一条两边都在同一个
                                # 可执行文件里才钉得住。它自己只依赖 ResourceCatalog，所以
                                # 跟着那一份进来就行。
                                ${REPO_ROOT}/src/download/ModDependencies.cpp
                                # ModpackImport 是**因为 ResourceCatalog 才进来的**：
                                # 加了 CurseForge 这个来源之后，它拼接口地址要问
                                # ModpackImport::curseForgeApiBase()（那条社区反代地址的唯一
                                # 出处），于是这一层的"纯函数"这话不再成立 —— 少了这两个文件
                                # 链接就过不去，而且报的是 `undefined reference to
                                # ModpackImport::curseForgeApiBase` 这种看着跟界面毫无关系的
                                # 一行。InstanceFiles 是 ModpackImport 自己要的（和 mw_check /
                                # ui_shot 那两份清单一致）。
                                ${REPO_ROOT}/src/manage/ModpackImport.cpp
                                ${REPO_ROOT}/src/manage/ModpackImport.hpp
                                ${REPO_ROOT}/src/manage/InstanceFiles.cpp
                                # 「游戏日志」窗。它是纯界面（不认识"会话"，
                                # 见它的文件头），所以归这一层验最合适：给它几行字，看它
                                # 写没写上去、跟不跟到底、清空/关闭两颗钮管不管用。
                                ${REPO_ROOT}/ui/GameLogWindow.cpp
                                # 运行输出共用模型：GameLogWindow 和
                                # ServerConsolePage 都直接用它。
                                ${REPO_ROOT}/ui/RuntimeOutputModel.cpp
                                ${REPO_ROOT}/ui/SkinViewer.cpp
                                ${REPO_ROOT}/ui/TitleBar.cpp
                                ${REPO_ROOT}/ui/BrandMark.cpp
                                ${REPO_ROOT}/ui/DesignDefaults.cpp
                                ${REPO_ROOT}/ui/Theme.cpp
                                # 「预设管理」与「原理图信息」这两页：表面巡检要真的把它们摆出来。
                                # 产品里它们是 MainWindow 直接 new 的，这一层原先没编进来。
                                ${REPO_ROOT}/ui/PresetManagePage.cpp
                                ${REPO_ROOT}/ui/SchematicInfoDialog.cpp
                                ${REPO_ROOT}/ui/ThemeAdvancedEditor.cpp
                                # 动效那一层：按钮按下 / 换页 / 展开折叠。
                                # MainWindowUI.cpp 和 OverlayHost.cpp 都直接调它。
                                ${REPO_ROOT}/ui/Animations.cpp
                                ${REPO_ROOT}/ui/WindowMotion.cpp
                                # 「先铺底色、再铺一张图」的容器：四个页面和主页
                                # 左栏现在直接 new 它，少了这一个文件会在链接那一步报
                                # `undefined reference to ThemeBackground::ThemeBackground(…)`。
                                ${REPO_ROOT}/ui/ThemeBackground.cpp
                                # 磨砂玻璃：场景底与绘制过滤器（Theme::apply() 装它）。
                                ${REPO_ROOT}/ui/Popups.cpp
                                ${REPO_ROOT}/ui/Glass.cpp
                                # 任务中心那块面板：`MainWindowUI.cpp` 直接
                                # new 它（底栏中间那格点开那一下），少了这一个文件会在链接
                                # 那一步报 `undefined reference to TaskCenterPanel::…`。
                                ${REPO_ROOT}/ui/TaskCenterPanel.cpp
                                ${REPO_ROOT}/ui/ActivityPopover.cpp
                                ${REPO_ROOT}/ui/TaskDetailPage.cpp
                                ${REPO_ROOT}/ui/TaskProgressBar.cpp
                                # 完整性检查报告页：MainWindowUI.cpp 用到它那几个名字。
                                ${REPO_ROOT}/ui/IntegrityReportPage.cpp
                                # 工具箱那一页：MainWindowUI 不再直接用它，但验证工程里
                                # 工具箱各套件 new 它，少了这一个文件会在
                                # 链接那一步报一串 `undefined reference to ToolboxPage::…`。
                                ${REPO_ROOT}/ui/ToolboxPage.cpp
                                # 更新中心那一页：U33 直接 new 它。
                                ${REPO_ROOT}/ui/UpdateCenterPage.cpp
                                # 「新建服务端」那一页。
                                ${REPO_ROOT}/ui/NewServerPage.cpp
                                ${REPO_ROOT}/ui/ServerConsolePage.cpp
                                # 「服务端设置」那一页。
                                ${REPO_ROOT}/ui/ServerSettingsPage.cpp
                                ${REPO_ROOT}/ui/SystemBackdrop.cpp
                                ${REPO_ROOT}/ui/WindowChrome.cpp
                                ${REPO_ROOT}/ui/WindowEdge.cpp
                                ${REPO_ROOT}/ui/ChromeDialog.cpp)

# InstanceFiles 那一层要的（`VersionInheritance::parentIdOf` / `maxDepth`）——
# 少了它，链接期报的是 `undefined reference to VersionInheritance::…`。和 mw_check /
# ui_shot 那两份清单同一个形状：**有才加**，别的分支上没有这个文件时这一句不生效。
if(EXISTS ${REPO_ROOT}/src/launcher/VersionInheritance.cpp)
    target_sources(wtfml_ui_runtime PRIVATE ${REPO_ROOT}/src/launcher/VersionInheritance.cpp
                                    ${REPO_ROOT}/src/launcher/VersionInheritance.hpp)
endif()

# 给 Java 8 换 ASCII 路径那一套。和上面 VersionInheritance 同一个形状：
# **有才加**。少了它，链接期报的是 `undefined reference to AsciiPathAlias::…` ——
# 而它自己又要 ZipUtil（读 jar 里的 class 文件头）。
if(EXISTS ${REPO_ROOT}/src/launcher/AsciiPathAlias.cpp)
    target_sources(wtfml_ui_runtime PRIVATE ${REPO_ROOT}/src/launcher/AsciiPathAlias.cpp
                                    ${REPO_ROOT}/src/launcher/AsciiPathAlias.hpp)
endif()
# ZipUtil 是 InstanceFiles.cpp 要的（`mainClassJavaMajor()` 要读 jar 里
# 那个 class 文件的头 6、7 字节来判这个主类编给哪个 Java）。少了它链接期报的是
# `undefined reference to ZipUtil::readEntry(QString const&, QString const&, …)` ——
# 一行看着和界面毫无关系的错。
if(EXISTS ${REPO_ROOT}/src/util/ZipUtil.cpp)
    target_sources(wtfml_ui_runtime PRIVATE ${REPO_ROOT}/src/util/ZipUtil.cpp
                                    ${REPO_ROOT}/src/util/ZipUtil.hpp)
endif()

target_include_directories(wtfml_ui_runtime PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/ui)
target_link_libraries(wtfml_ui_runtime PRIVATE Qt6::Widgets)
# Shared production code is built for correctness checks, not performance claims.
# Keep the existing unoptimized verification configuration.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(wtfml_ui_runtime PRIVATE -O0)
endif()
# dwmapi 是 ui/WindowChrome.cpp 要的（DwmSetWindowAttribute，把圆角明说一遍），
# 见主工程 CMakeLists.txt 里同一段。少了它会在链接那一步报 undefined reference。
target_link_libraries(wtfml_ui_runtime PRIVATE dwmapi Qt6::GuiPrivate)


# Production code is shared by independently buildable test executables.
wtfml_exclude_shared_sources(wtfml_ui_runtime wtfml_core_runtime)
target_link_libraries(wtfml_ui_runtime PUBLIC wtfml_core_runtime Qt6::Widgets)
target_include_directories(wtfml_ui_runtime PUBLIC ${REPO_ROOT}/src ${REPO_ROOT}/ui ${REPO_ROOT}/tests ${CMAKE_CURRENT_SOURCE_DIR})
add_library(wtfml_ui_fixture STATIC modules/Support.cpp modules/Support.hpp chrome_messages.cpp chrome_native.cpp)
# ccache 的 `CCACHE_BASEDIR` 会把 `__FILE__` 改写成相对路径 —— 那几个"扫源码"的断言
# 靠 `__FILE__` 推仓库根会算到构建目录上去。这里把真路径直接传进来（正斜杠，免转义）。
file(TO_CMAKE_PATH "${REPO_ROOT}" WTFML_REPO_ROOT_POSIX)
target_compile_definitions(wtfml_ui_fixture PUBLIC WTFML_REPO_ROOT="${WTFML_REPO_ROOT_POSIX}")
target_link_libraries(wtfml_ui_fixture PUBLIC wtfml_ui_runtime)
file(GLOB _cases CONFIGURE_DEPENDS modules/*.cpp)
list(REMOVE_ITEM _cases "${CMAKE_CURRENT_SOURCE_DIR}/modules/Support.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/modules/ModuleMain.cpp")
list(FILTER _cases EXCLUDE REGEX "/Ui[^/]*Check\\.cpp$")
add_library(ui_legacy_cases STATIC ${_cases})
target_link_libraries(ui_legacy_cases PUBLIC wtfml_ui_fixture)
add_executable(ui_check main.cpp)
# 「English 界面下导航轨是英文」那几条要看真的 .qm，所以这一份 exe 也带一份资源
# （和产品同一个生成物，见顶层 CMakeLists 里 wtfml_embed_translations）。
if(COMMAND wtfml_embed_translations)
    wtfml_embed_translations(ui_check)
endif()
target_link_libraries(ui_check PRIVATE ui_legacy_cases)
if(WTFML_COMPACT_TEST_RUNNERS)
 add_executable(ui_check_modules modules/ModuleMain.cpp)
 if(COMMAND wtfml_embed_translations)
  wtfml_embed_translations(ui_check_modules)
 endif()
 target_link_libraries(ui_check_modules PRIVATE wtfml_ui_fixture)
 target_include_directories(ui_check_modules PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
 target_compile_definitions(ui_check_modules PRIVATE WTFML_COMPACT_RUNNER)
endif()
function(add_ui_module target source)
 if(source MATCHES "\\.hpp$")
  add_executable(${target} modules/ModuleMain.cpp ${source})
 else()
  add_library(${target}_case OBJECT ${source})
  target_link_libraries(${target}_case PRIVATE wtfml_ui_fixture)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
   target_compile_options(${target}_case PRIVATE -O0 -Wa,-mbig-obj)
  endif()
  if(WTFML_COMPACT_TEST_RUNNERS)
   wtfml_compact_module(${target} ${ARGN} ui_check_modules)
  else()
   add_executable(${target} modules/ModuleMain.cpp $<TARGET_OBJECTS:${target}_case>)
  endif()
  target_sources(ui_legacy_cases PRIVATE $<TARGET_OBJECTS:${target}_case>)
 endif()
 if(NOT WTFML_COMPACT_TEST_RUNNERS)
  target_compile_definitions(${target} PRIVATE WTFML_MODULE_ENTRY=${ARGN})
  target_link_libraries(${target} PRIVATE wtfml_ui_fixture)
 endif()
 add_dependencies(wtfml_specialist_modules ${target})
endfunction()
add_ui_module(ui_check_install_layout modules/UiInstallLayoutCheck.cpp UiInstallLayoutCheck)
add_ui_module(ui_check_agent modules/UiAgentCheck.cpp UiAgentCheck)
add_ui_module(ui_check_toolbox_polish modules/UiToolboxPolishCheck.cpp UiToolboxPolishCheck)
add_ui_module(ui_check_toolbox_layout modules/UiToolboxLayoutCheck.cpp UiToolboxLayoutCheck)
add_ui_module(ui_check_window_size modules/UiWindowSizeCheck.cpp UiWindowSizeCheck)
add_ui_module(ui_check_input_widths modules/UiInputWidthCheck.cpp UiInputWidthCheck)
# 子页面 / 弹框 / 浮层巡检：计入的残留不为 0 就失败（守卫，见模块文件头）。
add_ui_module(ui_check_surface_sweep modules/UiSurfaceSweepCheck.cpp UiSurfaceSweepCheck)
if(NOT WTFML_COMPACT_TEST_RUNNERS AND COMMAND wtfml_embed_translations)
 wtfml_embed_translations(ui_check_surface_sweep)
endif()
add_ui_module(ui_check_resource_packs modules/UiResourcePacksCheck.cpp UiResourcePacksCheck)
add_ui_module(ui_check_lifecycle modules/UiLifecycleCheck.cpp UiLifecycleCheck)
add_ui_module(ui_check_ui_refresh modules/UiUiRefreshCheck.cpp UiUiRefreshCheck)
add_ui_module(ui_check_files_rows modules/UiFilesRowsCheck.cpp UiFilesRowsCheck)
add_ui_module(ui_check_theme modules/UiThemeCheck.cpp UiThemeCheck)
add_ui_module(ui_check_fold_motion modules/UiFoldMotionChecks.cpp UiFoldMotionChecks)
add_ui_module(ui_check_loading_state modules/UiLoadingStateChecks.cpp UiLoadingStateChecks)
add_ui_module(ui_check_design_defaults modules/UiDesignDefaultsChecks.cpp UiDesignDefaultsChecks)
add_ui_module(ui_check_group_editor modules/UiGroupEditorChecks.cpp UiGroupEditorChecks)
add_ui_module(ui_check_daily_quote modules/UiDailyQuoteCheck.cpp UiDailyQuoteCheck)
add_ui_module(ui_check_daily_quote_shots modules/UiDailyQuoteShots.cpp UiDailyQuoteShots)  # 样张，不进 ctest
add_ui_module(ui_check_long_instance_name modules/UiLongInstanceNameChecks.cpp UiLongInstanceNameChecks)
add_ui_module(ui_check_launch_card modules/UiLaunchCardChecks.cpp UiLaunchCardChecks)
add_ui_module(ui_check_long_instance_name_shots modules/UiLongInstanceNameShots.cpp UiLongInstanceNameShots)  # 样张，不进 ctest
add_ui_module(ui_check_loading_state_shots modules/UiLoadingStateShots.cpp UiLoadingStateShots)  # 样张，不进 ctest
add_ui_module(ui_check_detail_chip_shots modules/UiDetailChipShots.cpp UiDetailChipShots)  # 样张，不进 ctest
add_ui_module(ui_check_home_greeting_shots modules/UiHomeGreetingShots.cpp UiHomeGreetingShots)  # 样张，不进 ctest
add_ui_module(ui_check_account_kind_shots modules/UiAccountKindShots.cpp UiAccountKindShots)  # 样张，不进 ctest
add_ui_module(ui_check_popups modules/UiPopupCheck.cpp UiPopupCheck)
add_ui_module(ui_check_window_chrome modules/UiWindowChromeCheck.cpp UiWindowChromeCheck)
add_ui_module(ui_check_window_motion modules/UiWindowMotionCheck.cpp UiWindowMotionCheck)
add_ui_module(ui_check_armor_trim ArmorTrimChecks.cpp runArmorTrimChecks)
add_ui_module(ui_check_background_shots modules/UiBackgroundShots.cpp UiBackgroundShots)  # 背景图样张，不进 ctest
add_ui_module(ui_check_settings_shots modules/UiSettingsShotsCheck.cpp UiSettingsShotsCheck)  # 设置页验收截图，不进 ctest
add_ui_module(ui_check_review_shots modules/UiReviewShotsCheck.cpp UiReviewShotsCheck)  # 走查验收截图，不进 ctest
wtfml_write_compact_entries()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
 foreach(_target wtfml_ui_fixture ui_legacy_cases ui_check)
  target_compile_options(${_target} PRIVATE -O0 -Wa,-mbig-obj)
 endforeach()
endif()

wtfml_add_test(NAME wtfml.ui-agent COMMAND ui_check_agent)
set_tests_properties(wtfml.ui-agent PROPERTIES
    LABELS "ui;agent"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.ui COMMAND ui_check)
wtfml_add_test(NAME wtfml.ui-bootstrap COMMAND ui_check --bootstrap-check)
if(WTFML_I18N)
    wtfml_add_test(NAME wtfml.ui-language COMMAND ui_check --language-check)
    set_tests_properties(wtfml.ui-language PROPERTIES LABELS "ui;i18n" TIMEOUT 30
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}")
endif()
set_tests_properties(wtfml.ui-bootstrap PROPERTIES LABELS "ui;compile" TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}")
wtfml_add_test(NAME wtfml.ui-refresh COMMAND ui_check_ui_refresh)
wtfml_add_test(NAME wtfml.files-rows COMMAND ui_check_files_rows)
set_tests_properties(wtfml.files-rows PROPERTIES
    LABELS "ui"
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.ui-daily-quote COMMAND ui_check_daily_quote)
set_tests_properties(wtfml.ui-daily-quote PROPERTIES
    LABELS "ui;theme"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.launch-card COMMAND ui_check_launch_card)
set_tests_properties(wtfml.launch-card PROPERTIES
    LABELS "ui;theme"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.ui-long-instance-name COMMAND ui_check_long_instance_name)
set_tests_properties(wtfml.ui-long-instance-name PROPERTIES
    LABELS "ui;theme"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.install-layout COMMAND ui_check_install_layout)
set_tests_properties(wtfml.install-layout PROPERTIES
    LABELS "ui;install"
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.toolbox-polish COMMAND ui_check_toolbox_polish)
wtfml_add_test(NAME wtfml.input-widths COMMAND ui_check_input_widths)
set_tests_properties(wtfml.input-widths PROPERTIES
    LABELS "ui;layout"
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
# 表面巡检守卫：计入的残留不为 0 就失败（例外只认 SurfaceScan.hpp 的 checkBoxException()）。
# 单跑实测 8–21 秒（验证构建、offscreen）；超时留到 180 秒是给 CI 慢机。
wtfml_add_test(NAME wtfml.surface-sweep COMMAND ui_check_surface_sweep)
set_tests_properties(wtfml.surface-sweep PROPERTIES
    LABELS "ui;theme"
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.toolbox-layout COMMAND ui_check_toolbox_layout)
set_tests_properties(wtfml.toolbox-layout PROPERTIES
    LABELS "ui;toolbox"
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.window-size COMMAND ui_check_window_size)
set_tests_properties(wtfml.window-size PROPERTIES
    LABELS "ui"
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.armor-trim COMMAND ui_check_armor_trim)
set_tests_properties(wtfml.armor-trim PROPERTIES
    LABELS "ui;toolbox"
    TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
set_tests_properties(wtfml.toolbox-polish PROPERTIES
    LABELS "ui;toolbox"
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
# 下拉弹层高度（最后一项被裁那条）：同一套断言跑两遍 —— 本机缩放一遍、125% 一遍
# （用户那台就是 125%，弹层少 12 逻辑 px 只在有断言时看得见）。见 UiPopupCheck 的 ③b。
wtfml_add_test(NAME wtfml.popups COMMAND ui_check_popups)
wtfml_add_test(NAME wtfml.popups-scale COMMAND ui_check_popups)
set_tests_properties(wtfml.popups wtfml.popups-scale PROPERTIES
    LABELS "ui;popups"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
set_tests_properties(wtfml.popups-scale PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.25;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.lifecycle COMMAND ui_check_lifecycle)
wtfml_add_test(NAME wtfml.window-motion COMMAND ui_check_window_motion)
set_tests_properties(wtfml.window-motion PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
    TIMEOUT 60
    LABELS "ui"
)
wtfml_add_test(NAME wtfml.resource-packs COMMAND ui_check_resource_packs)
set_tests_properties(wtfml.resource-packs PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
    TIMEOUT 60
    LABELS "ui;files"
)
set_tests_properties(wtfml.lifecycle PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
    TIMEOUT 60
    LABELS "ui"
)
set_tests_properties(wtfml.ui-refresh PROPERTIES
    LABELS "ui"
    TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
# 真 windows 平台插件：窗口摆到屏外、不激活，量真 HWND 上的 WM_NCHITTEST / 标题栏 / 原生子窗口。
wtfml_add_test(NAME wtfml.window-chrome COMMAND ui_check_window_chrome)
set_tests_properties(wtfml.window-chrome PROPERTIES
    LABELS "ui;window"
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=windows;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.fold-motion COMMAND ui_check_fold_motion)
set_tests_properties(wtfml.fold-motion PROPERTIES
    LABELS "ui;settings"
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.loading-state COMMAND ui_check_loading_state)
set_tests_properties(wtfml.loading-state PROPERTIES
    LABELS "ui;loading"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.design-defaults COMMAND ui_check_design_defaults)
set_tests_properties(wtfml.design-defaults PROPERTIES
    LABELS "ui;theme"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.group-editor COMMAND ui_check_group_editor)
set_tests_properties(wtfml.group-editor PROPERTIES
    LABELS "ui"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
wtfml_add_test(NAME wtfml.theme COMMAND ui_check_theme)
set_tests_properties(wtfml.theme PROPERTIES
    LABELS "ui;theme"
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)
set_tests_properties(wtfml.ui PROPERTIES
    LABELS "ui;visual"
    TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;${WTFML_TEST_RUNTIME_ENVIRONMENT}"
)

get_property(WTFML_UI_TESTS DIRECTORY PROPERTY TESTS)
set_property(TEST ${WTFML_UI_TESTS} APPEND PROPERTY LABELS ci-ui)



