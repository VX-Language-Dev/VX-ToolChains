// platform.hpp — 宿主平台自动检测：架构 + 操作系统 → 规范 LLVM target triple。
// 编译期宏检测，前端（VXOBJ target 字段）与后端（IR 头 / clang -target）共用。
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

namespace vx::util {

// 宿主架构名："arm64" | "x86_64" | "unknown"
std::string host_arch();

// 宿主系统名："macosx" | "linux" | "windows" | "unknown"
std::string host_os();

// 宿主平台三元组（LLVM triple 规范形式）：
//   macOS arm64  → "arm64-apple-macosx"
//   Linux x86_64 → "x86_64-unknown-linux-gnu"
//   Windows x64  → "x86_64-pc-windows-msvc"
std::string host_triple();

// 人读平台名（诊断/verbose 输出），如 "macOS arm64"、"Linux x86_64"。
std::string host_name();

// 在 $PATH 各目录中依次查找候选可执行文件名（如 {"ar", "llvm-ar"}），
// 返回第一个可执行（access X_OK）命中的完整路径；全部未命中返回空串。
// 环境变量覆盖（VX_CLANG / VX_AR 等）由调用方先行处理，此处只查 PATH。
std::string find_in_path(std::initializer_list<std::string_view> candidates);

} // namespace vx::util
