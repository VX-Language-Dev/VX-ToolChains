# vxls — VX 语言服务器（LSP over stdio），独立 CMake 工程。
# 集成 vx-tidy：默认对文档跑静态检查并合并发布诊断（--no-tidy 关闭）；
# --only-tidy 仅运行 vx-tidy 后退出。依赖 vxtidy 动态库。
# 单独配置（cmake -S tools/vxls）时反向引入 vx-toolkit 根工程的库目标
#（置位 VX_TOOLS_SUBBUILD 防止根工程递归纳入 tools/）。
cmake_minimum_required(VERSION 3.23)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vxls LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
    if(NOT TARGET vx_compiler)
        set(VX_TOOLS_SUBBUILD ON CACHE BOOL "工具子项目反向引入根工程")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.."
                         "${CMAKE_CURRENT_BINARY_DIR}/vx-toolkit")
    endif()
endif()

# vxtidy 动态库来自 sibling 子项目（主构建已纳入；独立配置时补引入）
if(NOT TARGET vxtidy)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vx-tidy"
                     "${CMAKE_CURRENT_BINARY_DIR}/vx-tidy")
endif()

add_executable(vxls
    src/main.cpp
    src/analysis.cpp
    src/transport.cpp
    src/json.cpp
)
target_include_directories(vxls PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vxls PRIVATE vx_compiler vxtidy)
target_compile_options(vxls PRIVATE -Wall -Wextra -Wpedantic)

# 单元测试（注册到所属构建的 CTest）
add_executable(vxls_tests
    tests/vxls_test.cpp
    src/analysis.cpp
    src/json.cpp
)
target_include_directories(vxls_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vxls_tests PRIVATE vx_compiler vxtidy)
target_compile_options(vxls_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vxls_tests COMMAND vxls_tests)
