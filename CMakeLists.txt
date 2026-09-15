cmake_minimum_required(VERSION 3.20)
project(CS2WorkshopToolsLocalizerCN LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()


# 支持从环境变量或 CMake 参数动态检索 Qt6 路径
if(NOT DEFINED CMAKE_PREFIX_PATH)
    if(DEFINED ENV{Qt6_DIR})
        set(CMAKE_PREFIX_PATH "$ENV{Qt6_DIR}")
    elseif(DEFINED ENV{QTDIR})
        set(CMAKE_PREFIX_PATH "$ENV{QTDIR}")
    elseif(DEFINED ENV{Qt6_ROOT})
        set(CMAKE_PREFIX_PATH "$ENV{Qt6_ROOT}")
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 版本号，默认 "dev"，CI 通过 -DAPP_VERSION=v1.0.0 传入
set(APP_VERSION "dev" CACHE STRING "Application version string")

# 源码按 ui / service / core 三层组织，src/ 作为项目内部 include 根：
#   #include "core/xxx.h" / "service/xxx.h" / "ui/xxx.h"
# 依赖方向只允许 UI -> Service -> Core，Core 不得反向依赖 UI。
set(LAYERED_INCLUDE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 1. 编译注入 DLL qtcore_qm.dll
add_library(qtcore_qm SHARED
    src/core/qtcore_qm.cpp
    src/core/hook_manager.cpp
    src/core/pe_patcher.cpp
    src/core/dictionary_compiler.cpp
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(qtcore_qm PRIVATE
    ${LAYERED_INCLUDE_ROOT}
    third_party/minhook/include
    third_party/minhook/src
)
target_link_libraries(qtcore_qm PRIVATE user32 psapi)
set_target_properties(qtcore_qm PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# 2. 编译主启动器图形界面程序
add_executable(CS2WorkshopToolsLocalizerCN WIN32
    src/main.cpp

    src/ui/mainwindow.cpp
    src/ui/debug_window.cpp

    src/service/localization_service.cpp
    src/service/dictionary_service.cpp

    src/core/cs2_detector.cpp
    src/core/pe_patcher.cpp
    src/core/fgd_translator.cpp
    src/core/backup_manager.cpp
    src/core/dictionary_compiler.cpp
    src/core/launcher_config.cpp
    src/core/process_monitor.cpp
    src/core/hammer_ipc.cpp
)

set_target_properties(CS2WorkshopToolsLocalizerCN PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
)

target_include_directories(CS2WorkshopToolsLocalizerCN PRIVATE
    ${LAYERED_INCLUDE_ROOT}
)

target_compile_definitions(CS2WorkshopToolsLocalizerCN PRIVATE
    APP_VERSION="${APP_VERSION}"
)

target_link_libraries(CS2WorkshopToolsLocalizerCN PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    advapi32
    user32
    shell32
    version
    psapi
)

# 复制字典文件目录和 DLL 到输出目录方便调试运行
add_custom_command(TARGET CS2WorkshopToolsLocalizerCN POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:CS2WorkshopToolsLocalizerCN>/translations"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/translations"
        "$<TARGET_FILE_DIR:CS2WorkshopToolsLocalizerCN>/translations"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:qtcore_qm>"
        "$<TARGET_FILE_DIR:CS2WorkshopToolsLocalizerCN>/qtcore_qm.dll"
)

# 说明：本项目不设单元测试目标。
# 注入型改动的行为正确性只能靠实际 HOOK 到 Hammer 进程验证，
# 脱离宿主进程的单测无法覆盖真实时序，反而会给出虚假的安全感。
# 验证方式见 README「验证」章节。
