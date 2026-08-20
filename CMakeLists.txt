cmake_minimum_required(VERSION 3.10)
project(proxied)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加可执行文件，包含资源文件
add_executable(proxied WIN32
    proxied/proxied.cpp
    proxied/pch.cpp
    proxied/proxied.rc
    proxied/proxied_dialogs.rc
)

# 设置预编译头文件
target_precompile_headers(proxied PRIVATE
    proxied/pch.h
)

# 包含目录
target_include_directories(proxied PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/proxied
)

# 链接必要的库
target_link_libraries(proxied PRIVATE
    comctl32
    shell32
    shlwapi
    advapi32
    user32
    gdi32
    ole32
)

# 设置 Windows 子系统
set_target_properties(proxied PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:WINDOWS"
)

# 设置字符集为 Unicode
target_compile_definitions(proxied PRIVATE
    UNICODE
    _UNICODE
    WIN32
    _WINDOWS
)

# 禁用特定警告（如果需要）
# target_compile_options(proxied PRIVATE /W3)

# 如果需要，可以设置输出目录
# set_target_properties(proxied PROPERTIES
#     RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
# )
