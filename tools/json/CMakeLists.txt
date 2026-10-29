# json — 极简 JSON 编解码静态库（vx 工具链 LSP 共享组件，vxls / vmake-lsp 复用）。
# 无外部依赖。
cmake_minimum_required(VERSION 3.23)

# 单独配置（cmake -S tools/json）时自举工程；被主构建或其它工具引入时跳过
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vxjson LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

add_library(vxjson STATIC
    json.cpp
)
target_include_directories(vxjson PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(vxjson PRIVATE -Wall -Wextra -Wpedantic)
