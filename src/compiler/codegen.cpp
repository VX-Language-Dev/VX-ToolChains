// codegen.cpp — 类型化 AST → TypeIR 虚拟指令序列（主入口；CodeGen 实现拆分于 codegen_decl.hpp）。
#include "vx/compiler/codegen.hpp"

#include "codegen_decl.hpp"

#include <optional>

namespace vx::compiler {

optional<ir::ModuleIR> generate_module(const Program& prog, std::string_view filename, DiagBag& bag,
                                       const ImportTable* imports, const Analysis* analysis) {
  return CodeGen(filename, bag, imports, analysis).run(prog);
}

} // namespace vx::compiler
