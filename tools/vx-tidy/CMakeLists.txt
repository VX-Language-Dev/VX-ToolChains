# vx-tidy — VX 静态分析引擎，独立 CMake 工程。
# 架构：引擎编译为动态库 libvxtidy（SHARED），CLI（vx-tidy）与
# 外部工具（如 vxls 的 --no-tidy/默认集成）均通过该 dylib 使用。
# 单独配置（cmake -S tools/vx-tidy）时反向引入 vx-toolkit 根工程的库目标
#（置位 VX_TOOLS_SUBBUILD 防止根工程递归纳入 tools/）。
cmake_minimum_required(VERSION 3.23)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vx-tidy LANGUAGES CXX)
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

# 引擎动态库（libvxtidy.dylib）：vx-tidy CLI 与 vxls 共同依赖。
# vx_compiler 用 PRIVATE 链接：tidy.hpp 公共接口不暴露编译器类型，
# 避免下游（vxls）同时链接 vx_compiler 产生重复库告警。
add_library(vxtidy SHARED src/tidy.cpp)
target_include_directories(vxtidy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vxtidy PRIVATE vx_compiler)
target_compile_options(vxtidy PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(vxtidy PROPERTIES POSITION_INDEPENDENT_CODE ON)

# CLI：vx-tidy（链接 vxtidy 动态库；自身用 vx_util::read_file，需链 vx_util）
add_executable(vx-tidy src/main.cpp)
target_link_libraries(vx-tidy PRIVATE vxtidy vx_util)
target_include_directories(vx-tidy PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_compile_options(vx-tidy PRIVATE -Wall -Wextra -Wpedantic)

# 引擎测试（注册到所属构建的 CTest）
add_executable(vx_tidy_tests tests/tidy_test.cpp)
target_link_libraries(vx_tidy_tests PRIVATE vxtidy)
target_compile_options(vx_tidy_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vx_tidy_tests COMMAND vx_tidy_tests)
