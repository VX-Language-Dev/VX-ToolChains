# ─────────────────────────────────────────────────────────────────────
# toolchain-linux-x86_64.cmake
# ─────────────────────────────────────────────────────────────────────
# 交叉构建目标：x86_64-unknown-linux-musl（静态 musl，ELF）
# 交叉编译器：zig c++（wrapper 见 scripts/toolchains/）
# ─────────────────────────────────────────────────────────────────────
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   "${CMAKE_CURRENT_LIST_DIR}/zig-cxx-linux-x86_64")
set(CMAKE_CXX_COMPILER "${CMAKE_CURRENT_LIST_DIR}/zig-cxx-linux-x86_64")
set(CMAKE_AR           "${CMAKE_CURRENT_LIST_DIR}/zig-ar")
set(CMAKE_RANLIB       "${CMAKE_CURRENT_LIST_DIR}/zig-ranlib")
set(CMAKE_C_COMPILER_WORKS ON)
set(CMAKE_CXX_COMPILER_WORKS ON)
set(CMAKE_C_COMPILER_ID Clang)
set(CMAKE_CXX_COMPILER_ID Clang)
set(CMAKE_CXX_COMPILER_VERSION 17.0.0)
# 位置无关代码：libvx_util.a / libvx_compiler.a 需被 libvxtidy.so 动态库链接
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)