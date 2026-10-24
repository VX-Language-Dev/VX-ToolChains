// llvm_emitter.hpp — TypeIR（虚拟栈机）→ LLVM IR 文本（SSA 直接发射）。
// 发射期维护「编译期操作数栈」，标签处隐式开新基本块；不引入运行时栈 helper。
#pragma once

#include "vx/compiler/typeir.hpp"

#include <string>
#include <string_view>

namespace vx::backend {

struct EmitResult {
  bool ok = false;
  std::string error;
  std::string ir; // LLVM IR 文本（.ll）
};

// 将 ModuleIR 降为 LLVM IR 文本。
// - target_triple 写入 IR 头（供 clang 选择目标）。
// - 函数 main 发射为 @vx_main；emit_entry 为 true 时额外生成 i32 @main()
//   包装作为可执行入口，动态库目标传 false 以避免导出进程入口。
// - 遇到 MVP 未覆盖的 opcode 或无法解析的调用时 ok=false，error 含中文原因。
EmitResult emit_llvm_ir(const vx::compiler::ir::ModuleIR &mod,
                        std::string_view target_triple,
                        bool emit_entry = true);

} // namespace vx::backend
