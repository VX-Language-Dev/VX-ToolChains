# ─────────────────────────────────────────────────────────────────────
# toolchain-linux-riscv32.cmake
# ─────────────────────────────────────────────────────────────────────
# Linux 目标：RISC-V 32 位（riscv32-unknown-linux-gnu）
#
# 依赖宿主已安装的 riscv32-linux-gnu-gcc / clang 交叉工具链。
# ─────────────────────────────────────────────────────────────────────
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

set(CMAKE_C_COMPILER riscv32-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER riscv32-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)