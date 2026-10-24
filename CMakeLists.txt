cmake_minimum_required(VERSION 3.23)

# VX-ToolKit C++ 重写工程：Util / Compiler / Backend 三层 + vxc002 / vlnk 二进制
# 构建由 CMake + Ninja 驱动（见 Makefile），同时导出 compile_commands.json 供 clangd 使用
project(vx-toolkit LANGUAGES CXX C)

# C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出编译数据库，供 clangd 等工具使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 构建种类默认 MinSizeRel
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING
        "Build type: Debug, Release, MinSizeRel, RelWithDebInfo" FORCE)
endif()

# MinSizeRel：追加 -g（MSR 要求带调试符号）
if(CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(CMAKE_CXX_FLAGS_MINSIZE_REL "${CMAKE_CXX_FLAGS_MINSIZE_REL} -g")
    set(CMAKE_C_FLAGS_MINSIZE_REL "${CMAKE_C_FLAGS_MINSIZE_REL} -g")
endif()

# ==================== Util 层 ====================
add_library(vx_util
    src/util/arena.cpp
    src/util/str.cpp
    src/util/fs.cpp
    src/util/platform.cpp
)
target_include_directories(vx_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(vx_util PRIVATE -Wall -Wextra -Wpedantic)

# ==================== Compiler 层 ====================
# 模块分层：diag（统一诊断基础设施）先于 Lexer/Parser/Codegen 存在。
add_library(vx_compiler
    src/compiler/diag.cpp
    src/compiler/vxabi.cpp
    src/compiler/lexer.cpp
    src/compiler/parser.cpp
    src/compiler/importer.cpp
    src/compiler/codegen.cpp
    src/compiler/vxobj.cpp
)
target_link_libraries(vx_compiler PUBLIC vx_util)
target_include_directories(vx_compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(vx_compiler PRIVATE -Wall -Wextra -Wpedantic)

# ==================== 编译器前端二进制 ====================
# vxc002 = Lexer + Parser + Codegen（read → lex → parse → codegen）。
# VXOBJ writer 后续版本补齐。
add_executable(vxc002 src/driver/main.cpp)
target_link_libraries(vxc002 PRIVATE vx_compiler vx_util)
target_compile_options(vxc002 PRIVATE -Wall -Wextra -Wpedantic)

# ==================== Runtime 层 ====================
# 预编译的 VX 运行时（vxrt.c），供 vlnk 链接期直接并入，避免每次重新编译 C 源。
add_library(vx_runtime STATIC src/runtime/vxrt.c)
target_include_directories(vx_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime)
set_target_properties(vx_runtime PROPERTIES OUTPUT_NAME vxrt)

# ==================== Backend 层 ====================
# 后端 vlnk：VXOBJ reader（字节流 → ModuleIR）+ LLVM emitter（ModuleIR → .ll 文本）。
add_library(vx_backend
    src/backend/vxobj_reader.cpp
    src/backend/llvm_emitter.cpp
    src/backend/link_driver.cpp
)
target_link_libraries(vx_backend PUBLIC vx_compiler)
target_include_directories(vx_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(vx_backend PRIVATE -Wall -Wextra -Wpedantic)

# ==================== 后端二进制 ====================
# vlnk = VXOBJ reader + LLVM emitter + clang 链接（.vxobj → 原生可执行文件）。
add_executable(vlnk src/driver/vlnk_main.cpp)
target_link_libraries(vlnk PRIVATE vx_backend)
target_compile_options(vlnk PRIVATE -Wall -Wextra -Wpedantic)

# ==================== 测试 ====================
enable_testing()

# ==================== 工具子项目（tools/）====================
# 自动检测 tools/ 下含 CMakeLists.txt 的子项目（如 vxls、vx-tidy）并纳入主构建；
# 子项目各自为独立 CMake 工程，也可单独配置构建。
# 须置于 enable_testing() 之后，子项目内 add_test 才会被 CTest 收集。
# VX_TOOLS_SUBBUILD：由工具子项目反向引入本工程时置位，避免递归纳入。
if(NOT VX_TOOLS_SUBBUILD)
    file(GLOB vx_tool_cmakelists "${CMAKE_CURRENT_SOURCE_DIR}/tools/*/CMakeLists.txt")
    foreach(tool_cmake IN LISTS vx_tool_cmakelists)
        get_filename_component(tool_dir "${tool_cmake}" DIRECTORY)
        get_filename_component(tool_name "${tool_dir}" NAME)
        message(STATUS "检测到工具子项目: tools/${tool_name}")
        add_subdirectory("${tool_dir}" "${CMAKE_CURRENT_BINARY_DIR}/tools/${tool_name}")
    endforeach()
endif()

add_executable(vx_util_tests tests/util_test.cpp)
target_link_libraries(vx_util_tests PRIVATE vx_util)
target_compile_options(vx_util_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME util_tests COMMAND vx_util_tests)

add_executable(vx_diag_tests tests/diag_test.cpp)
target_link_libraries(vx_diag_tests PRIVATE vx_compiler)
target_compile_options(vx_diag_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME diag_tests COMMAND vx_diag_tests)

add_executable(vx_lexer_tests tests/lexer_test.cpp)
target_link_libraries(vx_lexer_tests PRIVATE vx_compiler)
target_compile_options(vx_lexer_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME lexer_tests COMMAND vx_lexer_tests)

add_executable(vx_parser_tests tests/parser_test.cpp)
target_link_libraries(vx_parser_tests PRIVATE vx_compiler)
target_compile_options(vx_parser_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME parser_tests COMMAND vx_parser_tests)

add_executable(vx_codegen_tests tests/codegen_test.cpp)
target_link_libraries(vx_codegen_tests PRIVATE vx_compiler)
target_compile_options(vx_codegen_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME codegen_tests COMMAND vx_codegen_tests)

add_executable(vx_vxobj_tests tests/vxobj_test.cpp)
target_link_libraries(vx_vxobj_tests PRIVATE vx_compiler vx_util)
target_compile_options(vx_vxobj_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vxobj_tests COMMAND vx_vxobj_tests)

add_executable(vx_e2e_tests tests/vxc_e2e_test.cpp)
target_link_libraries(vx_e2e_tests PRIVATE vx_compiler vx_util)
target_compile_options(vx_e2e_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME e2e_tests COMMAND vx_e2e_tests)

add_executable(vx_backend_tests tests/backend_test.cpp)
target_link_libraries(vx_backend_tests PRIVATE vx_backend vx_compiler vx_util)
target_compile_options(vx_backend_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME backend_tests COMMAND vx_backend_tests)

add_executable(vx_vlnk_e2e_tests tests/vlnk_e2e_test.cpp)
target_link_libraries(vx_vlnk_e2e_tests PRIVATE vx_backend vx_compiler vx_util)
target_compile_options(vx_vlnk_e2e_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vlnk_e2e_tests COMMAND vx_vlnk_e2e_tests)
# 找不到 clang 时测试返回 77 → CTest 记为跳过
set_tests_properties(vlnk_e2e_tests PROPERTIES SKIP_RETURN_CODE 77)

# @imp / @cppimp 导入：ABI 命名、headers 解析、codegen 调用与 emitter 声明
add_executable(vx_import_tests tests/import_test.cpp)
target_link_libraries(vx_import_tests PRIVATE vx_backend vx_compiler vx_util)
target_compile_options(vx_import_tests PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(vx_import_tests PRIVATE
    VX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME import_tests COMMAND vx_import_tests)
# 测试工作目录设为 build 根，便于统一使用相对路径
set_tests_properties(import_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# @imp / @cppimp 端到端（需 clang；缺失时返回 77 → 跳过）
add_executable(vx_import_e2e_tests tests/import_e2e_test.cpp)
target_link_libraries(vx_import_e2e_tests PRIVATE vx_backend vx_compiler vx_util)
target_compile_options(vx_import_e2e_tests PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(vx_import_e2e_tests PRIVATE
    VX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME import_e2e_tests COMMAND vx_import_e2e_tests)
set_tests_properties(import_e2e_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" SKIP_RETURN_CODE 77)
