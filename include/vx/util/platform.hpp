// platform.hpp — 宿主平台自动检测：架构 + 操作系统 → 规范 LLVM target triple。
// 编译期宏检测，前端（VXOBJ target 字段）与后端（IR 头 / clang -target）共用。
#pragma once

#include <string>

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

} // namespace vx::util
