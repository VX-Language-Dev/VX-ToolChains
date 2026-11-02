# lsp — LSP 传输层共享静态库（vx 工具链 LSP 共享组件，vxls / vmake-lsp 复用）。
# 仅提供 Content-Length 帧的收发；JSON 编解码来自共享 vxjson。
cmake_minimum_required(VERSION 3.23)

# 单独配置（cmake -S tools/lsp）时自举工程；被主构建或其它工具引入时跳过
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vxlsp LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

# 共享 JSON 库（主构建已纳入；独立配置时补引入）
if(NOT TARGET vxjson)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../json"
                     "${CMAKE_CURRENT_BINARY_DIR}/json")
endif()

add_library(vxlsp STATIC
    lsp.cpp
)
target_include_directories(vxlsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vxlsp PUBLIC vxjson)
target_compile_options(vxlsp PRIVATE -Wall -Wextra -Wpedantic)
