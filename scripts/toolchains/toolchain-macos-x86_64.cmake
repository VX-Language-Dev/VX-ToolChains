# ─────────────────────────────────────────────────────────────────────
# toolchain-macos-x86_64.cmake
# ─────────────────────────────────────────────────────────────────────
# macOS 目标：x86_64（Intel Mac，Mach-O）
#
# 注意：macOS SDK 仅在 macOS 主机上可用，此 toolchain 依赖宿主 Apple clang。
# 二进制为 Mach-O x86_64，只能在 macOS (x86_64) 上运行。
# ─────────────────────────────────────────────────────────────────────
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 使用宿主 Apple clang（不覆盖编译器），通过 CMAKE_OSX_ARCHITECTURES 定架构
set(CMAKE_OSX_ARCHITECTURES "x86_64")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)