cmake_minimum_required(VERSION 3.23)

# VX-ToolKit C++ 重写工程（当前仅实现 Util 层，其余模块后续补齐）
# 配置方式：cmake -G Ninja -B build -S . -DCMAKE_BUILD_TYPE=MinSizeRel
project(vx-toolkit LANGUAGES CXX)

# C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 构建种类默认 MinSizeRel
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING
        "Build type: Debug, Release, MinSizeRel, RelWithDebInfo" FORCE)
endif()

# ==================== Util 层 ====================
add_library(vx_util
    src/util/arena.cpp
    src/util/str.cpp
    src/util/fs.cpp
)
target_include_directories(vx_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(vx_util PRIVATE -Wall -Wextra -Wpedantic)

# ==================== Compiler 层 ====================
add_library(vx_compiler
    src/compiler/lexer.cpp
    src/compiler/parser.cpp
)
target_link_libraries(vx_compiler PUBLIC vx_util)
target_include_directories(vx_compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(vx_compiler PRIVATE -Wall -Wextra -Wpedantic)

# ==================== 测试 ====================
enable_testing()
add_executable(vx_util_tests tests/util_test.cpp)
target_link_libraries(vx_util_tests PRIVATE vx_util)
target_compile_options(vx_util_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME util_tests COMMAND vx_util_tests)

add_executable(vx_lexer_tests tests/lexer_test.cpp)
target_link_libraries(vx_lexer_tests PRIVATE vx_compiler)
target_compile_options(vx_lexer_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME lexer_tests COMMAND vx_lexer_tests)

add_executable(vx_parser_tests tests/parser_test.cpp)
target_link_libraries(vx_parser_tests PRIVATE vx_compiler)
target_compile_options(vx_parser_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME parser_tests COMMAND vx_parser_tests)
