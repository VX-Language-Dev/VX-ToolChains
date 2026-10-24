# vmake — VX 项目构建编排器（VMakeLists.toml），独立 CMake 工程。
# 自身不编译 VX 代码，只按顺序调用 vxc / vlnk；输出风格对齐 Cargo。
# 单独配置（cmake -S tools/vmake）时反向引入 vx-toolkit 根工程的库与工具目标
#（置位 VX_TOOLS_SUBBUILD 防止根工程递归纳入 tools/）。
cmake_minimum_required(VERSION 3.23)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vmake LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
    if(NOT TARGET vx_util)
        set(VX_TOOLS_SUBBUILD ON CACHE BOOL "工具子项目反向引入根工程")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.."
                         "${CMAKE_CURRENT_BINARY_DIR}/vx-toolkit")
    endif()
endif()

# 核心库：配置解析 / 工具链定位 / 子进程执行 / 构建编排（供单元测试复用）
add_library(vmake_core STATIC
    src/config.cpp
    src/toolchain.cpp
    src/process.cpp
    src/build.cpp
)
target_include_directories(vmake_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vmake_core PUBLIC vx_util)
target_compile_options(vmake_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(vmake src/main.cpp)
target_link_libraries(vmake PRIVATE vmake_core)
target_compile_options(vmake PRIVATE -Wall -Wextra -Wpedantic)

# 单元测试：TOML 子集、目标校验、构建顺序、产物命名
add_executable(vmake_tests tests/vmake_test.cpp)
target_link_libraries(vmake_tests PRIVATE vmake_core)
target_compile_options(vmake_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vmake_tests COMMAND vmake_tests)

# 端到端：真实 vxc + vlnk + clang；无 clang 时测试返回 77（CTest SKIP）
add_executable(vmake_e2e_tests tests/vmake_e2e_test.cpp)
target_link_libraries(vmake_e2e_tests PRIVATE vx_backend)
target_compile_options(vmake_e2e_tests PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(vmake_e2e_tests PRIVATE
    VMAKE_BIN_PATH="$<TARGET_FILE:vmake>"
    VXC_BIN_PATH="$<TARGET_FILE:vxc002>"
    VLNK_BIN_PATH="$<TARGET_FILE:vlnk>")
add_dependencies(vmake_e2e_tests vmake vxc002 vlnk)
add_test(NAME vmake_e2e_tests COMMAND vmake_e2e_tests)
set_tests_properties(vmake_e2e_tests PROPERTIES SKIP_RETURN_CODE 77)
