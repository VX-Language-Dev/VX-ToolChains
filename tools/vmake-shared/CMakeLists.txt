# vmake-shared — VMakeSet.vmake 共享词法 / 校验规则静态库。
# vmake 与 vmake-lsp 共用同一套 .vmake 解析底层。
cmake_minimum_required(VERSION 3.23)

# 单独配置（cmake -S tools/vmake-shared）时自举工程；被主构建或其它工具引入时跳过
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vmake_shared LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# 幂等保护：根构建 glob 会先纳入本目录，vmake / vmake-lsp 再引入时跳过
if(NOT TARGET vmake_shared)
    add_library(vmake_shared STATIC
        vmake_shared.cpp
    )
    target_include_directories(vmake_shared PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(vmake_shared PRIVATE -Wall -Wextra -Wpedantic)
endif()
