add_library(wtfml_network_diag_core STATIC
    ../../src/download/DownloadManager.cpp ../../src/download/DownloadManager.hpp
    ../../src/download/DownloadManagerRecovery.cpp
    ../../src/download/DownloadManagerLocalReuse.cpp
    ../../src/download/DownloadManagerMirror.cpp
    ../../src/download/DownloadManagerThrottle.cpp
    ../../src/download/DownloadManagerSegmented.cpp
    ../../src/download/DownloadManagerMetadata.cpp
    # Recovery receipts use TokenStore's byte protection. Its storage helpers
    # also require the shared directory/path implementation; keep this target
    # complete when built without the launcher or verification runtimes.
    ../../src/auth/TokenStore.cpp
    ../../src/util/SecretProtector.cpp ../../src/util/SecretProtector.hpp
    ../../src/manage/InstanceFiles.cpp
    ../../src/launcher/VersionInheritance.cpp
    ../../src/util/ZipUtil.cpp
    # InstanceFiles 的封面解码走 ImageDecode；漏了它这两个目标链接不过。
    ../../src/util/ImageDecode.cpp ../../src/util/ImageDecode.hpp
    # 「从别的游戏文件夹里借文件」：DownloadManager 会调它。
    ../../src/download/LocalFileReuse.cpp ../../src/download/LocalFileReuse.hpp
    ../../src/download/ResumableDownload.cpp ../../src/download/ResumableDownload.hpp
    ../../src/util/DownloadDiagnostics.cpp)
target_include_directories(wtfml_network_diag_core PUBLIC ../../src)
target_link_libraries(wtfml_network_diag_core PUBLIC Qt6::Core Qt6::Network Qt6::Gui)
if(WIN32)
    # ImageDecode 走 WIC：ole32 起 COM、shlwapi 的 SHCreateMemStream、windowscodecs 是 WIC 本体。
    # 和主工程 CMakeLists.txt 里同一段（见那里的说明）。
    target_link_libraries(wtfml_network_diag_core PRIVATE crypt32 ole32 shlwapi windowscodecs)
endif()
set_target_properties(wtfml_network_diag_core PROPERTIES AUTOMOC ON)
add_executable(WTFMLNetworkDiag WIN32 main.cpp NetworkMonitor.hpp)
target_link_libraries(WTFMLNetworkDiag PRIVATE wtfml_network_diag_core Qt6::Widgets)
if(MINGW)
    target_compile_options(WTFMLNetworkDiag PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(WTFML_BUILD_TESTS)
    add_test(NAME wtfml.network-diag COMMAND WTFMLNetworkDiag --self-test)
    # 顶层 CMakeLists 把测试运行时 PATH 算在父作用域；这里和 tests/ 一样取用，
    # 否则在没有 Qt bin 的终端里会先报 0xc0000139，根本走不到 --self-test。
    set(_network_diag_environment "QT_QPA_PLATFORM=offscreen")
    if(DEFINED WTFML_TEST_RUNTIME_ENVIRONMENT AND NOT WTFML_TEST_RUNTIME_ENVIRONMENT STREQUAL "")
        list(APPEND _network_diag_environment "${WTFML_TEST_RUNTIME_ENVIRONMENT}")
    endif()
    # 子场景在 CI 上会慢；单个 worker 已放宽到 15/30 秒，这里给整组留足时间。
    set_tests_properties(wtfml.network-diag PROPERTIES RUN_SERIAL TRUE TIMEOUT 120 ENVIRONMENT "${_network_diag_environment}" LABELS "network;diagnostics;ci-app")
endif()
