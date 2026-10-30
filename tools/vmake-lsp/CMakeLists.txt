cmake_minimum_required(VERSION 3.23)

# vmake-lsp — VMakeSet.vmake 语言服务器（LSP over stdio），C++23。
# 单独配置（cmake -S tools/vmake-lsp）时反向引入根工程的 vmake_core 与 vx_util。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vmake-lsp LANGUAGES CXX)
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

# 共享 JSON 库（主构建已纳入；独立配置时补引入）
if(NOT TARGET vxjson)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../json"
                     "${CMAKE_CURRENT_BINARY_DIR}/json")
endif()

# 核心库：传输层 / 解析器（供主程序与单元测试复用）；JSON 来自共享 vxjson
add_library(vmake_lsp_core STATIC
    src/transport.cpp
    src/parser.cpp
)
target_include_directories(vmake_lsp_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vmake_lsp_core PUBLIC vxjson)
target_compile_options(vmake_lsp_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(vmake-lsp src/main.cpp)
target_link_libraries(vmake-lsp PRIVATE vmake_lsp_core)
target_compile_options(vmake-lsp PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS vmake-lsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# 单元测试：解析器行为（诊断规则、位置、命中测试）
add_executable(vmake_lsp_tests tests/parser_test.cpp)
target_link_libraries(vmake_lsp_tests PRIVATE vmake_lsp_core)
target_compile_options(vmake_lsp_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME vmake_lsp_tests COMMAND vmake_lsp_tests)

# 端到端：真实 vmake-lsp 进程的 LSP 会话（initialize → 诊断 → 补全 → 格式化 …）
add_executable(vmake_lsp_e2e_tests tests/lsp_e2e_test.cpp)
target_link_libraries(vmake_lsp_e2e_tests PRIVATE vmake_lsp_core)
target_compile_options(vmake_lsp_e2e_tests PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(vmake_lsp_e2e_tests PRIVATE
    VMAKE_LSP_BIN_PATH="$<TARGET_FILE:vmake-lsp>")
add_dependencies(vmake_lsp_e2e_tests vmake-lsp)
add_test(NAME vmake_lsp_e2e_tests COMMAND vmake_lsp_e2e_tests)
set_tests_properties(vmake_lsp_e2e_tests PROPERTIES SKIP_RETURN_CODE 77)
