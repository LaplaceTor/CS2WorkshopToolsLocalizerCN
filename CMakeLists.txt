cmake_minimum_required(VERSION 3.20)
project(CS2HammerTranslateCN LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

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

# 1. 编译注入 DLL qtcore_qm.dll
add_library(qtcore_qm SHARED
    src/qtcore_qm.cpp
)
target_link_libraries(qtcore_qm PRIVATE user32)
set_target_properties(qtcore_qm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}"
)

# 2. 编译主启动器图形界面程序
add_executable(CS2HammerTranslateCN WIN32
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/cs2_detector.h
    src/cs2_detector.cpp
    src/pe_patcher.h
    src/pe_patcher.cpp
    src/fgd_translator.h
    src/fgd_translator.cpp
    src/backup_manager.h
    src/backup_manager.cpp
)

target_link_libraries(CS2HammerTranslateCN PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    advapi32
    user32
    shell32
)

# 复制字典文件和 DLL 到输出目录方便调试运行
add_custom_command(TARGET CS2HammerTranslateCN POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/fgd_translations.json"
        "$<TARGET_FILE_DIR:CS2HammerTranslateCN>/fgd_translations.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/qt_translations.json"
        "$<TARGET_FILE_DIR:CS2HammerTranslateCN>/qt_translations.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:qtcore_qm>"
        "$<TARGET_FILE_DIR:CS2HammerTranslateCN>/qtcore_qm.dll"
)

# 3. 单元与集成测试程序
add_executable(test_components
    tests/test_components.cpp
    src/cs2_detector.h
    src/cs2_detector.cpp
    src/pe_patcher.h
    src/pe_patcher.cpp
    src/fgd_translator.h
    src/fgd_translator.cpp
    src/backup_manager.h
    src/backup_manager.cpp
)
target_link_libraries(test_components PRIVATE
    advapi32
    user32
    shell32
)
