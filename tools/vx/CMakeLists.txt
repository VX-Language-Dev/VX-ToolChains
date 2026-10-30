# vx — VX toolchain meta-command (dispatches subcommands to vxc/vlnk/vmake/etc.)
cmake_minimum_required(VERSION 3.23)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vx LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

add_executable(vx main.cpp)
target_compile_options(vx PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS vx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})