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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 版本号，默认 "dev"，CI 通过 -DAPP_VERSION=v1.0.0 传入
set(APP_VERSION "dev" CACHE STRING "Application version string")

# 1. 编译注入 DLL qtcore_qm.dll
add_library(qtcore_qm SHARED
    src/qtcore_qm.cpp
    src/hook_manager.cpp
    src/pe_patcher.cpp
    src/dictionary_compiler.cpp
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(qtcore_qm PRIVATE
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
    src/mainwindow.cpp
    src/cs2_detector.cpp
    src/pe_patcher.cpp
    src/fgd_translator.cpp
    src/backup_manager.cpp
    src/dictionary_compiler.cpp
)

set_target_properties(CS2WorkshopToolsLocalizerCN PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    AUTOUIC ON
)

target_compile_definitions(CS2WorkshopToolsLocalizerCN PRIVATE
    APP_VERSION="${APP_VERSION}"
)

target_link_libraries(CS2WorkshopToolsLocalizerCN PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    advapi32
    user32
    shell32
    version
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

# 3. 单元与集成测试程序
add_executable(test_components
    tests/test_components.cpp
    src/cs2_detector.cpp
    src/pe_patcher.cpp
    src/fgd_translator.cpp
    src/backup_manager.cpp
    src/hook_manager.cpp
    src/dictionary_compiler.cpp
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(test_components PRIVATE
    third_party/minhook/include
    third_party/minhook/src
)
target_link_libraries(test_components PRIVATE
    Qt6::Core
    advapi32
    user32
    shell32
    version
    psapi
)
