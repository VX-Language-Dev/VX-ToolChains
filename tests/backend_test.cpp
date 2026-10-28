// backend_test — 后端 P1 测试：VXOBJ reader 往返 + LLVM emitter 断言 + 不支持诊断。
// 自带断言宏，风格对齐 vxobj_test。
#include "vx/backend/llvm_emitter.hpp"
#include "vx/backend/vxobj_reader.hpp"
#include "vx/compiler/codegen.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/compiler/vxobj.hpp"
#include "vx/util/util.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

using namespace vx::compiler;
using vx::util::Arena;
namespace ir = vx::compiler::ir;
namespace obj = vx::compiler::obj;
namespace backend = vx::backend;

constexpr std::string_view kTriple = "arm64-apple-macosx";

// 源码 → ModuleIR（前端流水线：tokenize → parse → codegen）
std::optional<ir::ModuleIR> gen_src(std::string_view src) {
  static Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, "<test>", arena, bag);
  auto prog = parse(toks, "<test>", arena, bag);
  if (!prog) {
    std::fprintf(stderr, "FAIL parse:\n%s", render_all(bag, src, false).c_str());
    ++g_failures;
    return std::nullopt;
  }
  auto mod = generate_module(*prog, "<test>", bag);
  if (!mod) {
    std::fprintf(stderr, "FAIL codegen:\n%s",
                 render_all(bag, src, false).c_str());
    ++g_failures;
    return std::nullopt;
  }
  return std::move(*mod);
}

// 归一化返回类型："" 与 "void" 视为等价（writer 只存 has_return 位）
bool ret_type_eq(std::string_view a, std::string_view b) {
  auto norm = [](std::string_view s) -> std::string_view {
    return (s.empty() || s == "void") ? std::string_view("void") : s;
  };
  return norm(a) == norm(b);
}

// ==================== Reader ====================

void test_reader_roundtrip() {
  auto m = gen_src(
      "extern out(msg: string)\n"
      "func add(a: int, b: int) -> int:\n"
      "    return a + b\n"
      "func main() -> int:\n"
      "    out(\"hi\")\n"
      "    return 0\n");
  CHECK(m);
  const std::string data = obj::serialize(*m, "test-target");

  auto rt = backend::read_vxobj(data);
  CHECK(rt.ok);
  if (!rt.ok) {
    std::fprintf(stderr, "reader error: %s\n", rt.error.c_str());
    return;
  }
  CHECK(rt.target == "test-target");
  CHECK(rt.module.funcs.size() == m->funcs.size());

  // 逐函数比对名字 / 参数 / 返回类型 / 指令数
  for (std::size_t i = 0; i < m->funcs.size() && i < rt.module.funcs.size();
       ++i) {
    const ir::FuncIR &a = m->funcs[i];
    const ir::FuncIR &b = rt.module.funcs[i];
    CHECK(a.name == b.name);
    CHECK(a.id == b.id);
    CHECK(ret_type_eq(a.ret_type, b.ret_type));
    CHECK(a.params.size() == b.params.size());
    CHECK(a.local_count == b.local_count);
    CHECK(a.instructions.size() == b.instructions.size());
    for (std::size_t j = 0; j < a.params.size() && j < b.params.size(); ++j) {
      CHECK(a.params[j].name == b.params[j].name);
      CHECK(a.params[j].type == b.params[j].type);
    }
    for (std::size_t j = 0;
         j < a.instructions.size() && j < b.instructions.size(); ++j) {
      CHECK(a.instructions[j].op == b.instructions[j].op);
      CHECK(a.instructions[j].args == b.instructions[j].args);
    }
  }

  // 链接表：extern out 被还原
  CHECK(rt.module.linkage.size() == m->linkage.size());
  if (!rt.module.linkage.empty()) {
    CHECK(rt.module.linkage[0].name == m->linkage[0].name);
  }
}

void test_reader_bad_magic() {
  auto rt = backend::read_vxobj("NOTVXOBJ-x");
  CHECK(!rt.ok);
  CHECK(!rt.error.empty());
}

void test_reader_truncated() {
  auto m = gen_src("func main() -> int:\n    return 0\n");
  CHECK(m);
  const std::string data = obj::serialize(*m, "t");
  // 截断到一半，reader 应安全失败（不越界）
  auto rt = backend::read_vxobj(std::string_view(data).substr(0, data.size() / 2));
  CHECK(!rt.ok);
}

void test_reader_bad_version() {
  auto m = gen_src("func main() -> int:\n    return 0\n");
  CHECK(m);
  std::string data = obj::serialize(*m, "t");
  // 版本字段在 magic(5) 之后，改为 3
  data[7] = 3;
  auto rt = backend::read_vxobj(data);
  CHECK(!rt.ok);
}

// ==================== Emitter ====================

void test_emitter_simple() {
  auto m = gen_src(
      "extern out(msg: string)\n"
      "func main() -> int:\n"
      "    out(\"hello vx\")\n"
      "    return 0\n");
  CHECK(m);
  auto r = backend::emit_llvm_ir(*m, kTriple);
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "emit error: %s\n", r.error.c_str());
    return;
  }
  const std::string &ir = r.ir;
  CHECK(ir.find("target triple = \"arm64-apple-macosx\"") != std::string::npos);
  CHECK(ir.find("define i32 @vx_main()") != std::string::npos);
  CHECK(ir.find("define i32 @main()") != std::string::npos);
  CHECK(ir.find("call i32 @vx_main()") != std::string::npos);
  CHECK(ir.find("declare void @out(ptr)") != std::string::npos);
  CHECK(ir.find("call void @out(ptr") != std::string::npos);
  CHECK(ir.find("hello vx") != std::string::npos);
  CHECK(ir.find("ret i32 0") != std::string::npos);
}

void test_emitter_no_entry_wrapper() {
  // 动态库模式：main 仍发射为 @vx_main，但不生成进程入口 @main
  auto m = gen_src(
      "func main() -> int:\n"
      "    return 0\n");
  CHECK(m);
  auto r = backend::emit_llvm_ir(*m, kTriple, false);
  CHECK(r.ok);
  if (!r.ok) return;
  CHECK(r.ir.find("define i32 @vx_main()") != std::string::npos);
  CHECK(r.ir.find("define i32 @main()") == std::string::npos);
}

void test_emitter_arithmetic_and_branch() {
  auto m = gen_src(
      "extern out_int(v: int)\n"
      "func main() -> int:\n"
      "    var x = 3 + 4 * 2\n"
      "    if x > 5:\n"
      "        out_int(x)\n"
      "    return x\n");
  CHECK(m);
  auto r = backend::emit_llvm_ir(*m, kTriple);
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "emit error: %s\n", r.error.c_str());
    return;
  }
  const std::string &ir = r.ir;
  CHECK(ir.find("mul i32") != std::string::npos);
  CHECK(ir.find("add i32") != std::string::npos);
  CHECK(ir.find("icmp sgt i32") != std::string::npos);
  CHECK(ir.find("br i1") != std::string::npos);
  CHECK(ir.find("declare void @out_int(i32)") != std::string::npos);
}

void test_emitter_param_and_return() {
  auto m = gen_src(
      "func square(n: int) -> int:\n"
      "    return n * n\n"
      "func main() -> int:\n"
      "    return square(5)\n");
  CHECK(m);
  auto r = backend::emit_llvm_ir(*m, kTriple);
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "emit error: %s\n", r.error.c_str());
    return;
  }
  const std::string &ir = r.ir;
  CHECK(ir.find("define i32 @square(i32 %a0)") != std::string::npos);
  CHECK(ir.find("store i32 %a0, ptr %s0") != std::string::npos);
  CHECK(ir.find("call i32 @square(i32 5)") != std::string::npos);
}

void test_emitter_void_function() {
  auto m = gen_src(
      "func nop():\n"
      "    return\n"
      "func main():\n"
      "    nop()\n"
      "    return\n");
  CHECK(m);
  auto r = backend::emit_llvm_ir(*m, kTriple);
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "emit error: %s\n", r.error.c_str());
    return;
  }
  CHECK(r.ir.find("define void @nop()") != std::string::npos);
  CHECK(r.ir.find("call void @nop()") != std::string::npos);
}

void test_emitter_float() {
  auto m = gen_src(
      "extern out_float(v: float)\n"
      "func cmp() -> int:\n"
      "    if 1.0 < 2.5:\n"
      "        return 1\n"
      "    return 0\n"
      "func main() -> float:\n"
      "    out_float(1.5 + 2.25 * 2.0 - 0.5 / 2.0)\n"
      "    return -3.25\n");
  CHECK(m);
  auto r = backend::emit_llvm_ir(*m, kTriple);
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "emit error: %s\n", r.error.c_str());
    return;
  }
  const std::string &ir = r.ir;
  CHECK(ir.find("declare void @out_float(double)") != std::string::npos);
  CHECK(ir.find("fadd double") != std::string::npos);
  CHECK(ir.find("fsub double") != std::string::npos);
  CHECK(ir.find("fmul double") != std::string::npos);
  CHECK(ir.find("fdiv double") != std::string::npos);
  CHECK(ir.find("fcmp olt double") != std::string::npos);
  CHECK(ir.find("fneg double") != std::string::npos);
  CHECK(ir.find("define double @vx_main()") != std::string::npos);
  CHECK(ir.find("ret double") != std::string::npos);
}

void test_emitter_float_type_mismatch() {
  // F64 算术混入非 double 操作数（手工构造 IR）应报错而非生成非法 IR
  ir::ModuleIR mod;
  ir::FuncIR f;
  f.name = "f";
  f.ret_type = "void";
  ir::Inst c1;
  c1.op = ir::OP_CONST_INT;
  c1.args = "i1";
  ir::Inst c2;
  c2.op = ir::OP_CONST_FLOAT;
  c2.args = "f2.0";
  ir::Inst add;
  add.op = ir::OP_F64_ADD;
  ir::Inst ret;
  ret.op = ir::OP_RETURN_VOID;
  f.instructions = {c1, c2, add, ret};
  mod.funcs.push_back(std::move(f));

  auto r = backend::emit_llvm_ir(mod, kTriple);
  CHECK(!r.ok);
  CHECK(r.error.find("F64 算术操作数非 double") != std::string::npos);
}

void test_emitter_unsupported_opcode() {
  ir::ModuleIR mod;
  ir::FuncIR f;
  f.name = "f";
  f.ret_type = "void";
  f.local_count = 0;
  ir::Inst inst;
  inst.op = ir::OP_STRUCT_NEW; // 仍未覆盖
  inst.args = "2";
  f.instructions.push_back(inst);
  mod.funcs.push_back(std::move(f));

  auto r = backend::emit_llvm_ir(mod, kTriple);
  CHECK(!r.ok);
  CHECK(r.error.find("后端暂不支持") != std::string::npos);
  CHECK(r.error.find("OP_STRUCT_NEW") != std::string::npos);
}

void test_emitter_system_builtins() {
  // 系统能力 builtin：声明与调用发射（符号名与 VX 可见名不同）
  auto m = gen_src(
      "func main() -> int:\n"
      "    var s = file_read(\"a.txt\")\n"
      "    var n = file_write(\"b.txt\", s)\n"
      "    out_int(sys_argv_count())\n"
      "    return 0\n");
  CHECK(m);
  auto r = backend::emit_llvm_ir(*m, kTriple);
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "emit error: %s\n", r.error.c_str());
    return;
  }
  const std::string &ir = r.ir;
  CHECK(ir.find("declare ptr @vx_file_read(ptr)") != std::string::npos);
  CHECK(ir.find("call ptr @vx_file_read(ptr") != std::string::npos);
  CHECK(ir.find("declare i32 @vx_file_write(ptr, ptr)") != std::string::npos);
  CHECK(ir.find("declare i32 @vx_argv_count()") != std::string::npos);
  CHECK(ir.find("call i32 @vx_argv_count()") != std::string::npos);
}

void test_emitter_unknown_call() {
  ir::ModuleIR mod;
  ir::FuncIR f;
  f.name = "main";
  f.ret_type = "int";
  ir::Inst call;
  call.op = ir::OP_CALL;
  call.args = "f;does_not_exist";
  f.instructions.push_back(call);
  ir::Inst ret;
  ret.op = ir::OP_RETURN;
  f.instructions.push_back(ret);
  mod.funcs.push_back(std::move(f));

  auto r = backend::emit_llvm_ir(mod, kTriple);
  CHECK(!r.ok);
  CHECK(r.error.find("未知调用目标") != std::string::npos);
}

} // namespace

int main() {
  test_reader_roundtrip();
  test_reader_bad_magic();
  test_reader_truncated();
  test_reader_bad_version();

  test_emitter_simple();
  test_emitter_no_entry_wrapper();
  test_emitter_arithmetic_and_branch();
  test_emitter_param_and_return();
  test_emitter_void_function();
  test_emitter_float();
  test_emitter_float_type_mismatch();
  test_emitter_unsupported_opcode();
  test_emitter_unknown_call();
  test_emitter_system_builtins();

  if (g_failures == 0) {
    std::fprintf(stdout, "backend tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "backend tests: %d failures\n", g_failures);
  return 1;
}
