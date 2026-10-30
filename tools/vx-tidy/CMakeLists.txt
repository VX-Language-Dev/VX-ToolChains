# vx-tidy — VX 静态分析引擎，独立 CMake 工程。
# 架构：引擎编译为纯动态库 libvxtidy（SHARED），无 CLI 可执行文件；
# 外部工具（如 vxls 的 --no-tidy/默认集成/--only-tidy）与嵌入方通过该 dylib 使用，
# 命令行形态的检查可由 `vxls --only-tidy` 承担。
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

# 引擎动态库（libvxtidy.dylib）：vxls 等消费方依赖的唯一产物。
# vx_compiler 用 PRIVATE 链接：tidy.hpp 公共接口不暴露编译器类型，
# 避免下游（vxls）同时链接 vx_compiler 产生重复库告警。
add_library(vxtidy SHARED src/tidy.cpp)
target_include_directories(vxtidy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vxtidy PRIVATE vx_compiler)
target_compile_options(vxtidy PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(vxtidy PROPERTIES POSITION_INDEPENDENT_CODE ON)

install(TARGETS vxtidy
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# 引擎测试（注册到所属构建的 CTest）
add_executable(vx_tidy_tests tests/tidy_test.cpp)
target_link_libraries(vx_tidy_tests PRIVATE vxtidy)
target_compile_options(vx_tidy_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vx_tidy_tests COMMAND vx_tidy_tests)
