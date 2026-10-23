// link_driver.hpp — 后端链接驱动：LLVM IR 文本 → 原生可执行文件。
// 通过子进程调用 host clang，编译 .ll 并注入内置 vxrt 运行时后链接。
#pragma once

#include "vx/compiler/typeir.hpp"

#include <string>
#include <string_view>

namespace vx::backend {

struct LinkResult {
  bool ok = false;
  std::string error;
  std::string command; // 实际执行的命令（便于诊断）
};

// 当前 host 的平台三元组（.ll 头 / 链接目标），如 "arm64-apple-macosx"。
// 实现转发 vx::util 的平台自动检测（arch + OS 宏检测）。
std::string host_triple();

// 定位 clang 可执行文件：优先环境变量 VX_CLANG，其次 PATH 中的 clang。
// 未找到返回空串。
std::string find_clang();

// 将 LLVM IR 文本编译+链接为可执行文件 exe_path（注入内置 vxrt 运行时）。
// target 为空（默认）时按 host 平台链接；非空时给 clang 追加 -target（交叉编译）。
LinkResult link_executable(std::string_view ll_ir, std::string_view exe_path,
                           std::string_view target = {});

// 模块感知链接：在 link_executable 基础上，依 ModuleIR::libs 并入导入库的
// 实现文件（@imp 的 .c/.a/.dylib），并为 @cppimp 生成 C++ shim 一并编译。
LinkResult link_module(std::string_view ll_ir,
                       const vx::compiler::ir::ModuleIR &mod,
                       std::string_view exe_path,
                       std::string_view target = {});

// 生成 @cppimp 的 C++ shim 源码：为每个 C++ 导入符号生成 extern "C" 包装，
// 把 C++ ABI 转换为 VX ABI（符号 _vx_<lib>_<name>，类型映射见 vxabi.hpp）。
std::string generate_cpp_shim(const vx::compiler::ir::ModuleIR &mod);

} // namespace vx::backend
