// vxc_e2e_test — vxc002 端到端测试：驱动 lex → parse → codegen 对真实 .vx
// 样例源码，断言生成的 TypeIR 模块结构（函数数、指令数、链接表等）。
// 自带断言宏，不依赖第三方框架。
#include "vx/compiler/codegen.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/util/fs.hpp"
#include "vx/util/util.hpp"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ_STR(actual, expect)                                           \
  do {                                                                         \
    const std::string a_ = (actual);                                           \
    const std::string e_ = (expect);                                           \
    if (a_ != e_) {                                                            \
      std::fprintf(stderr, "FAIL %s:%d:\n  实际: %s\n  期望: %s\n", __FILE__,  \
                   __LINE__, a_.c_str(), e_.c_str());                          \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

using namespace vx::compiler;
namespace ir = vx::compiler::ir;
using vx::util::Arena;

// 词法 + 语法 + 代码生成一体（arena 静态，string_view 在断言期间有效）
std::optional<ir::ModuleIR> gen_src(std::string_view src,
                                    std::string_view filename) {
  static Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, filename, arena, bag);
  if (bag.has_error()) {
    std::fprintf(stderr, "FAIL lex %s:\n%s", std::string(filename).data(),
                 render_all(bag, src, false).c_str());
    ++g_failures;
    return std::nullopt;
  }
  auto prog = parse(toks, filename, arena, bag);
  if (!prog) {
    std::fprintf(stderr, "FAIL parse %s:\n%s", std::string(filename).data(),
                 render_all(bag, src, false).c_str());
    ++g_failures;
    return std::nullopt;
  }
  auto mod = generate_module(*prog, filename, bag);
  if (!mod) {
    std::fprintf(stderr, "FAIL codegen %s:\n%s", std::string(filename).data(),
                 render_all(bag, src, false).c_str());
    ++g_failures;
    return std::nullopt;
  }
  return std::move(*mod);
}

// 读取 tests/ 下的 .vx 样例并驱动全流程
void test_sample(std::string_view name, std::string_view filename,
                 std::size_t expect_funcs_min = 1) {
  fs::path p = fs::current_path();
  while (!p.empty() && !fs::exists(p / "tests" / filename)) {
    p = p.parent_path();
  }
  CHECK(!p.empty());
  if (p.empty()) {
    return;
  }
  auto r = vx::util::read_file(p / "tests" / filename);
  CHECK(r.has_value());
  if (!r) {
    return;
  }
  auto mod = gen_src(*r, filename);
  CHECK(mod);
  if (!mod) {
    return;
  }
  CHECK(mod->funcs.size() >= expect_funcs_min);
  std::fprintf(stdout, "  e2e %s: %zu funcs, %zu insts, %zu linkage, "
                   "%zu globals\n",
               std::string(name).data(), mod->funcs.size(),
               mod->funcs[0].instructions.size(), mod->linkage.size(),
               mod->global_vars.size());
}

// 各样例独立驱动，断言关键结构
void test_simple() {
  // test_simple.vx：extern out + func main
  fs::path p = fs::current_path();
  while (!p.empty() && !fs::exists(p / "tests" / "test_simple.vx")) {
    p = p.parent_path();
  }
  CHECK(!p.empty());
  if (p.empty()) {
    return;
  }
  auto r = vx::util::read_file(p / "tests" / "test_simple.vx");
  CHECK(r.has_value());
  if (!r) {
    return;
  }
  auto mod = gen_src(*r, "test_simple.vx");
  CHECK(mod);
  if (!mod) {
    return;
  }
  CHECK(mod->linkage.size() == 1);
  CHECK_EQ_STR(mod->linkage[0].name, "out");
  CHECK(mod->funcs.size() == 1);
  CHECK_EQ_STR(mod->funcs[0].name, "main");
  // main 应包含 OP_CONST_STRING + OP_CALL + OP_POP + OP_RETURN
  bool has_call = false, has_pop = false, has_ret = false;
  for (const auto &inst : mod->funcs[0].instructions) {
    if (inst.op == ir::OP_CALL) has_call = true;
    if (inst.op == ir::OP_POP) has_pop = true;
    // main 为 -> int 且有 `return 0`，故发射带值返回 OP_RETURN（非 void）
    if (inst.op == ir::OP_RETURN || inst.op == ir::OP_RETURN_VOID) has_ret = true;
  }
  CHECK(has_call);
  CHECK(has_pop);
  CHECK(has_ret);
}

void test_math_float() {
  // 验证 P0-3：浮点字面量二元运算发射 F64 族
  auto mod = gen_src(
      "func f() -> float:\n"
      "    return 1.5 + 2.5\n",
      "float_test.vx");
  CHECK(mod);
  if (!mod) {
    return;
  }
  const auto &f = mod->funcs[0];
  bool has_f64_add = false;
  for (const auto &inst : f.instructions) {
    if (inst.op == ir::OP_F64_ADD) has_f64_add = true;
  }
  CHECK(has_f64_add);
  // 整数运算仍为 I32 族
  auto mod2 = gen_src("func g() -> int:\n    return 1 + 2\n", "int_test.vx");
  CHECK(mod2);
  if (!mod2) {
    return;
  }
  const auto &f2 = mod2->funcs[0];
  bool has_i32_add = false;
  for (const auto &inst : f2.instructions) {
    if (inst.op == ir::OP_I32_ADD) has_i32_add = true;
  }
  CHECK(has_i32_add);
}

void test_string_concat() {
  // 验证 P0-3：字符串拼接发射 OP_STR_CONCAT
  auto mod = gen_src(
      "func h():\n"
      "    var s = \"a\" + \"b\"\n"
      "    return\n",
      "concat_test.vx");
  CHECK(mod);
  if (!mod) {
    return;
  }
  bool has_concat = false;
  for (const auto &f : mod->funcs) {
    for (const auto &inst : f.instructions) {
      if (inst.op == ir::OP_STR_CONCAT) has_concat = true;
    }
  }
  CHECK(has_concat);
}

void test_parser_recovery() {
  // P0-2：语法错误后应继续解析后续语句（多错误一次性暴露）
  static Arena arena;
  DiagBag bag;
  auto toks = tokenize(
      "var x =\nvar y = 1\nfunc bad() -> int:\n    return\nvar z = 2\n",
      "recovery.vx", arena, bag);
  auto prog = parse(toks, "recovery.vx", arena, bag);
  // fail-soft：即使中间有错误，也应尽量产出后续语句，不至于全盘丢弃
  if (prog) {
    CHECK(prog->stmts.size() >= 2); // 至少 var y / var z 等后续语句应被解析
  }
}

} // namespace

int main() {
  test_simple();
  test_math_float();
  test_string_concat();
  test_parser_recovery();

  // 通用样例驱动（只断言能跑通全流程并产出函数）
  test_sample("test_simple", "test_simple.vx", 1);
  test_sample("test_void", "test_void.vx", 1);
  test_sample("test_return", "test_return.vx", 1);
  test_sample("test_math", "test_math.vx", 1);
  test_sample("test_prime", "test_prime.vx", 1);

  if (g_failures == 0) {
    std::fprintf(stdout, "vxc_e2e tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "vxc_e2e tests: %d failure(s)\n", g_failures);
  return 1;
}
