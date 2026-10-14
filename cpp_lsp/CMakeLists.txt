cmake_minimum_required(VERSION 3.14)
project(vx-lsp-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(vx-lsp-server
    src/json_utils.cpp
    src/vx_lexer.cpp
    src/lsp_server.cpp
)

target_include_directories(vx-lsp-server PRIVATE src)
target_compile_options(vx-lsp-server PRIVATE -Wall -Wextra -O2)
