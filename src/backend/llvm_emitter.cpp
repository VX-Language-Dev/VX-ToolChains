// llvm_emitter.cpp — TypeIR（虚拟栈机）→ LLVM IR 文本（SSA 直接发射）。
// 设计：发射期维护「编译期操作数栈」（Value{val, vt}），槽位类型由写入者推断；
// 条件分支自动开 fallthrough 基本块；标签处若非终止块则补 br，保证 IR 合法。
// Emitter 实现拆分于 emitter_*.hpp；本文件为主入口（emit_llvm_ir）。
#include "vx/backend/llvm_emitter.hpp"

#include "emitter_call.hpp"
#include "emitter_decl.hpp"
#include "emitter_func.hpp"
#include "emitter_inst.hpp"
#include "emitter_scaffold.hpp"
#include "emitter_text.hpp"
#include "emitter_value.hpp"

#include <string_view>

namespace vx::backend {

EmitResult emit_llvm_ir(const ir::ModuleIR& mod, std::string_view target_triple, bool emit_entry) {
  return Emitter(mod, target_triple, emit_entry).run();
}

} // namespace vx::backend
