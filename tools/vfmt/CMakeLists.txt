# vfmt — VX 源码格式化工具，独立 CMake 工程。
# 基于 Lexer token 流重建缩进与空格布局，保留注释；格式化前后比对 AST
# 结构指纹，确保格式化不改变语义。
# 单独配置（cmake -S tools/vfmt）时反向引入 vx-toolkit 根工程的库目标
#（置位 VX_TOOLS_SUBBUILD 防止根工程递归纳入 tools/）。
cmake_minimum_required(VERSION 3.23)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vfmt LANGUAGES CXX)
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

# 核心库：格式化 + AST 指纹校验（供 CLI 与测试复用）
add_library(vfmt_core STATIC
    src/format.cpp
    src/verify.cpp
)
target_include_directories(vfmt_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vfmt_core PUBLIC vx_compiler)
target_compile_options(vfmt_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(vfmt src/main.cpp)
target_link_libraries(vfmt PRIVATE vfmt_core)
target_compile_options(vfmt PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS vfmt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# 单元测试（注册到所属构建的 CTest）
add_executable(vfmt_tests tests/vfmt_test.cpp)
target_link_libraries(vfmt_tests PRIVATE vfmt_core)
target_compile_options(vfmt_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vfmt_tests COMMAND vfmt_tests)
