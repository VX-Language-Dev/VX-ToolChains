// codegen_test — 代码生成测试：AST → TypeIR 指令序列（自带断言宏，无第三方依赖）。
#include "vx/compiler/codegen.hpp"

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

// 词法 + 语法 + 代码生成一体（arena 静态，string_view 在断言期间有效）
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

// 期望 codegen 失败：返回错误消息（不额外计失败）
std::string gen_expect_error(std::string_view src) {
  static Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, "<test>", arena, bag);
  auto prog = parse(toks, "<test>", arena, bag);
  if (!prog) {
    std::fprintf(stderr, "FAIL parse:\n%s", render_all(bag, src, false).c_str());
    ++g_failures;
    return "";
  }
  auto mod = generate_module(*prog, "<test>", bag);
  if (!mod) {
    return bag.items().back().message;
  }
  std::fprintf(stderr, "FAIL expected codegen error, got success\n");
  ++g_failures;
  return "";
}

// 第 f 个函数第 i 条指令的操作码
std::uint8_t op_at(const ir::ModuleIR &m, std::size_t f, std::size_t i) {
  return m.funcs[f].instructions[i].op;
}

std::string args_at(const ir::ModuleIR &m, std::size_t f, std::size_t i) {
  return m.funcs[f].instructions[i].args;
}

// ---- 用例 ----

void test_arith() {
  auto m = gen_src("func main() -> int:\n    return 1 + 2 * 3\n");
  CHECK(m);
  const auto &f = m->funcs[0];
  CHECK(f.name == "main");
  const std::size_t expect_ops[] = {
      ir::OP_CONST_INT, ir::OP_CONST_INT, ir::OP_CONST_INT,
      ir::OP_I32_MUL,   ir::OP_I32_ADD,   ir::OP_RETURN};
  CHECK(f.instructions.size() == 6);
  for (std::size_t i = 0; i < 6 && i < f.instructions.size(); ++i) {
    CHECK(f.instructions[i].op == expect_ops[i]);
  }
  CHECK(args_at(*m, 0, 0) == "i1");
  CHECK(args_at(*m, 0, 5) == "");
}

void test_vars_assign() {
  auto m = gen_src("func f():\n    var x = 1\n    x = x + 1\n    return x\n");
  CHECK(m);
  const auto &f = m->funcs[0];
  CHECK(f.local_count == 1);
  const std::size_t expect_ops[] = {ir::OP_CONST_INT, ir::OP_STORE_VAR,
                                    ir::OP_LOAD_VAR,  ir::OP_CONST_INT,
                                    ir::OP_I32_ADD,   ir::OP_STORE_VAR,
                                    ir::OP_LOAD_VAR,  ir::OP_RETURN};
  CHECK(f.instructions.size() == 8);
  for (std::size_t i = 0; i < 8 && i < f.instructions.size(); ++i) {
    CHECK(f.instructions[i].op == expect_ops[i]);
  }
  CHECK(args_at(*m, 0, 1) == "0");
  CHECK(args_at(*m, 0, 5) == "0");
}

void test_if_elif_else() {
  auto m = gen_src(
      "func g(n: int) -> int:\n"
      "    if n < 0:\n"
      "        return 0 - 1\n"
      "    elif n == 0:\n"
      "        return 0\n"
      "    else:\n"
      "        return n\n");
  CHECK(m);
  const auto &f = m->funcs[0];
  // 关键控制流锚点
  CHECK(op_at(*m, 0, 2) == ir::OP_I32_LT);
  CHECK(op_at(*m, 0, 3) == ir::OP_JUMP_IF_FALSE);
  CHECK(args_at(*m, 0, 3) == "L1"); // else 标签
  // then 分支以 return 结束，其后仍是 JUMP end（死代码，对齐自举）
  CHECK(op_at(*m, 0, 7) == ir::OP_RETURN);
  CHECK(op_at(*m, 0, 8) == ir::OP_JUMP);
  CHECK(args_at(*m, 0, 8) == "L2"); // end 标签
  CHECK(op_at(*m, 0, 9) == ir::OP_NOOP);
  CHECK(args_at(*m, 0, 9) == "L1");
  // elif 条件后的 JUMP_IF_FALSE 与末尾 end 标签
  bool has_l3_jif = false, has_l3_label = false, has_end = false;
  for (const auto &inst : f.instructions) {
    if (inst.op == ir::OP_JUMP_IF_FALSE && inst.args == "L3") {
      has_l3_jif = true;
    }
    if (inst.op == ir::OP_NOOP && inst.args == "L3") {
      has_l3_label = true;
    }
    if (inst.op == ir::OP_NOOP && inst.args == "L2") {
      has_end = true;
    }
  }
  CHECK(has_l3_jif);
  CHECK(has_l3_label);
  CHECK(has_end);
}

void test_while_break_continue() {
  auto m = gen_src(
      "func w():\n"
      "    var i = 0\n"
      "    while i < 10:\n"
      "        if i == 3:\n"
      "            continue\n"
      "        if i == 8:\n"
      "            break\n"
      "        i = i + 1\n"
      "    return i\n");
  CHECK(m);
  const auto &f = m->funcs[0];
  CHECK(f.local_count == 1); // 仅 i（隐藏变量只出现在 for）
  bool jump_start = false, jump_end = false;
  for (std::size_t i = 0; i < f.instructions.size(); ++i) {
    if (f.instructions[i].op == ir::OP_JUMP && args_at(*m, 0, i) == "L1") {
      jump_start = true; // continue / 回跳
    }
    if (f.instructions[i].op == ir::OP_JUMP && args_at(*m, 0, i) == "L2") {
      jump_end = true; // break
    }
  }
  CHECK(jump_start);
  CHECK(jump_end);
}

void test_for() {
  auto m = gen_src(
      "func h(xs: var):\n"
      "    for x in xs:\n"
      "        out(x)\n");
  CHECK(m);
  const auto &f = m->funcs[0];
  // 参数 xs=0；隐藏变量 iter/idx；循环变量 x
  CHECK(f.local_count == 4);
  bool has_iter = false, has_idx = false, has_guard = false;
  for (std::size_t i = 0; i < f.instructions.size(); ++i) {
    if (f.instructions[i].op == ir::OP_STORE_VAR &&
        args_at(*m, 0, i) == "1") {
      has_iter = true;
    }
    if (f.instructions[i].op == ir::OP_CONST_INT &&
        args_at(*m, 0, i) == "i100000") {
      has_guard = true;
    }
    if (f.instructions[i].op == ir::OP_INDEX_GET) {
      has_idx = true;
    }
  }
  CHECK(has_iter);
  CHECK(has_guard);
  CHECK(has_idx);
  // 函数调用
  bool has_call = false;
  for (std::size_t i = 0; i < f.instructions.size(); ++i) {
    if (f.instructions[i].op == ir::OP_CALL && args_at(*m, 0, i) == "f;out") {
      has_call = true;
    }
  }
  CHECK(has_call);
}

void test_extern_linkage() {
  auto m = gen_src(
      "extern out(msg: string)\n"
      "func main():\n"
      "    out(\"hi\")\n"
      "    return\n");
  CHECK(m);
  CHECK(m->linkage.size() == 1);
  CHECK(m->linkage[0].name == "out");
  CHECK(m->linkage[0].ret_type.empty()); // 无显式返回类型
  CHECK(m->linkage[0].params.size() == 1);
  CHECK(op_at(*m, 0, 0) == ir::OP_CONST_STRING);
  CHECK(args_at(*m, 0, 0) == "shi");
  CHECK(op_at(*m, 0, 1) == ir::OP_CALL);
  CHECK(args_at(*m, 0, 1) == "f;out");
  CHECK(op_at(*m, 0, 2) == ir::OP_POP);
  CHECK(op_at(*m, 0, 3) == ir::OP_RETURN_VOID);
}

void test_class_method_new() {
  auto m = gen_src(
      "class P:\n"
      "    x: int\n"
      "    func get() -> int:\n"
      "        return self.x\n"
      "\n"
      "func make() -> var:\n"
      "    var p = new P()\n"
      "    return p.get()\n");
  CHECK(m);
  CHECK(m->funcs.size() == 2);
  // 方法：P.get，隐式 self 占槽 0
  const auto &get = m->funcs[0];
  CHECK(get.name == "P.get");
  CHECK(get.params.size() == 1);
  CHECK(get.params[0].name == "self");
  CHECK(get.instructions.size() == 3);
  CHECK(op_at(*m, 0, 0) == ir::OP_LOAD_VAR);
  CHECK(args_at(*m, 0, 0) == "0");
  CHECK(op_at(*m, 0, 1) == ir::OP_FIELD_LOAD);
  CHECK(args_at(*m, 0, 1) == "x");
  CHECK(op_at(*m, 0, 2) == ir::OP_RETURN);
  // 调用方：new + 方法调用 m;get + 显式 return
  const auto &make = m->funcs[1];
  CHECK(make.instructions.size() == 5);
  CHECK(op_at(*m, 1, 0) == ir::OP_NEWZ);
  CHECK(args_at(*m, 1, 0) == "P;8");
  CHECK(op_at(*m, 1, 1) == ir::OP_STORE_VAR);
  CHECK(op_at(*m, 1, 2) == ir::OP_LOAD_VAR);
  CHECK(op_at(*m, 1, 3) == ir::OP_CALL);
  CHECK(args_at(*m, 1, 3) == "m;get");
  CHECK(op_at(*m, 1, 4) == ir::OP_RETURN);
}

void test_collections_index() {
  auto m = gen_src(
      "func c():\n"
      "    var a = [1, 2]\n"
      "    var d = {\"k\": 1}\n"
      "    var v = a[0]\n"
      "    a[0] = v + 1\n"
      "    return d[\"k\"]\n");
  CHECK(m);
  const auto &f = m->funcs[0];
  const std::size_t ops[] = {ir::OP_CONST_INT,  ir::OP_CONST_INT,
                             ir::OP_ARRAY_NEW,  ir::OP_STORE_VAR,
                             ir::OP_CONST_STRING, ir::OP_CONST_INT,
                             ir::OP_MAP_NEW,    ir::OP_STORE_VAR,
                             ir::OP_LOAD_VAR,   ir::OP_CONST_INT,
                             ir::OP_INDEX_GET,  ir::OP_STORE_VAR,
                             ir::OP_LOAD_VAR,   ir::OP_CONST_INT,
                             ir::OP_LOAD_VAR,   ir::OP_CONST_INT,
                             ir::OP_I32_ADD,    ir::OP_INDEX_SET,
                             ir::OP_LOAD_VAR,   ir::OP_CONST_STRING,
                             ir::OP_INDEX_GET,  ir::OP_RETURN};
  CHECK(f.instructions.size() == 22);
  for (std::size_t i = 0; i < 22 && i < f.instructions.size(); ++i) {
    CHECK(f.instructions[i].op == ops[i]);
  }
  CHECK(args_at(*m, 0, 2) == "2"); // ARRAY_NEW count
  CHECK(args_at(*m, 0, 6) == "1"); // MAP_NEW count
  CHECK(args_at(*m, 0, 19) == "sk");
}

void test_literals_unary() {
  auto m = gen_src(
      "func k() -> var:\n"
      "    var a = true\n"
      "    var b = nil\n"
      "    var c = -1\n"
      "    var d = not (1 == 2)\n"
      "    return a\n");
  CHECK(m);
  CHECK(op_at(*m, 0, 0) == ir::OP_CONST_BOOL);
  CHECK(args_at(*m, 0, 0) == "btrue");
  CHECK(op_at(*m, 0, 2) == ir::OP_CONST_NIL);
  CHECK(op_at(*m, 0, 4) == ir::OP_CONST_INT);
  CHECK(op_at(*m, 0, 5) == ir::OP_I32_NEG);
  // not (1 == 2) → CONST i1, CONST i2, I32_EQ, BOOL_NOT
  CHECK(op_at(*m, 0, 7) == ir::OP_CONST_INT);
  CHECK(op_at(*m, 0, 9) == ir::OP_I32_EQ);
  CHECK(op_at(*m, 0, 10) == ir::OP_BOOL_NOT);
}

void test_default_return() {
  auto m = gen_src("func noret() -> int:\n    var x = 1\n");
  CHECK(m);
  const auto &f = m->funcs[0];
  CHECK(f.instructions.size() == 4);
  CHECK(op_at(*m, 0, 2) == ir::OP_CONST_INT);
  CHECK(args_at(*m, 0, 2) == "i0");
  CHECK(op_at(*m, 0, 3) == ir::OP_RETURN);
}

void test_compound_assign() {
  auto m = gen_src(
      "func c():\n"
      "    var x = 1\n"
      "    x += 2\n"
      "    var a = [1]\n"
      "    a[0] += 5\n");
  CHECK(m);
  const auto &f = m->funcs[0];
  // x += 2 → LOAD 0, CONST i2, ADD, STORE 0
  CHECK(op_at(*m, 0, 2) == ir::OP_LOAD_VAR);
  CHECK(op_at(*m, 0, 4) == ir::OP_I32_ADD);
  CHECK(op_at(*m, 0, 5) == ir::OP_STORE_VAR);
  // a[0] += 5 经隐藏临时展开，以 INDEX_SET 收尾
  bool has_index_set = false;
  for (std::size_t i = 0; i < f.instructions.size(); ++i) {
    if (f.instructions[i].op == ir::OP_INDEX_SET) {
      has_index_set = true;
    }
  }
  CHECK(has_index_set);
}

void test_globals_recorded() {
  auto m = gen_src(
      "var G = 5\n"
      "func m():\n"
      "    return G\n");
  CHECK(m);
  CHECK(m->global_vars.size() == 1);
  CHECK(m->global_vars[0] == "G");
  CHECK(m->funcs.size() == 1);
  // G 按普通局部槽位分配（自举行为：全局寻址待后续版本）
  CHECK(op_at(*m, 0, 0) == ir::OP_LOAD_VAR);
  CHECK(args_at(*m, 0, 0) == "0");
}

void test_errors() {
  // break 在循环外
  CHECK(gen_expect_error("func e():\n    break\n") ==
        "break outside loop");
  // continue 在循环外
  CHECK(gen_expect_error("func e3():\n    continue\n") ==
        "continue outside loop");
}

// 逻辑运算短路求值：右侧仅在需要时求值，结果经临时槽位合流（0/1）
void test_short_circuit() {
  // 操作数用比较表达式（不产生 CONST i0/i1），便于统计短路合流常量
  auto m = gen_src("func f() -> int:\n"
                   "    var r = 2 > 1 and 3 > 2\n"
                   "    return r\n");
  CHECK(m);
  const auto &f = m->funcs[0];
  bool has_bool_and = false, has_jump_true = false, has_store = false,
       has_load = false;
  for (std::size_t i = 0; i < f.instructions.size(); ++i) {
    const auto op = f.instructions[i].op;
    if (op == ir::OP_BOOL_AND || op == ir::OP_BOOL_OR) has_bool_and = true;
    if (op == ir::OP_JUMP_IF_TRUE) has_jump_true = true;
    if (op == ir::OP_STORE_VAR && !f.instructions[i].args.empty()) {
      // __sc_ 临时槽合流（与用户变量 r 的槽号不同，此处仅计存在性）
      has_store = true;
    }
    if (op == ir::OP_LOAD_VAR) has_load = true;
  }
  CHECK(!has_bool_and); // 不再用非短路单指令
  CHECK(has_jump_true); // 分支判定跳转
  CHECK(has_store);
  CHECK(has_load);
  // 短路结果规范化：两条出 0 路径 + 一条出 1 路径（and 形态）
  std::size_t const_i0 = 0, const_i1 = 0;
  for (const auto &inst : f.instructions) {
    if (inst.op == ir::OP_CONST_INT && inst.args == "i0") ++const_i0;
    if (inst.op == ir::OP_CONST_INT && inst.args == "i1") ++const_i1;
  }
  CHECK(const_i0 >= 2); // 两条出 0 路径（操作数不含 i0 字面量）
  CHECK(const_i1 >= 1); // 至少真路径的 i1（操作数可能本身含 i1 字面量）

  // or：左真短路（右侧函数调用不求值由运行时验证，此处验证结构对称）
  auto m2 = gen_src("func g() -> int:\n"
                    "    var r = 1 > 2 or 3 > 2\n"
                    "    return r\n");
  CHECK(m2);
  bool has_jump_false = false;
  for (const auto &inst : m2->funcs[0].instructions) {
    if (inst.op == ir::OP_JUMP_IF_FALSE) has_jump_false = true;
  }
  CHECK(has_jump_false);
}

// 字符串相等比较 → OP_STR_EQ（内容比较，!= 由 BOOL_NOT 取反）
void test_string_compare() {
  auto m = gen_src("func s() -> int:\n"
                   "    return \"ab\" == \"ab\"\n");
  CHECK(m);
  bool has_str_eq = false;
  for (const auto &inst : m->funcs[0].instructions) {
    if (inst.op == ir::OP_STR_EQ) has_str_eq = true;
  }
  CHECK(has_str_eq);

  auto m2 = gen_src("func s2() -> int:\n"
                    "    return \"a\" != \"b\"\n");
  CHECK(m2);
  bool has_str_eq2 = false, has_not = false;
  for (const auto &inst : m2->funcs[0].instructions) {
    if (inst.op == ir::OP_STR_EQ) has_str_eq2 = true;
    if (inst.op == ir::OP_BOOL_NOT) has_not = true;
  }
  CHECK(has_str_eq2);
  CHECK(has_not);

  // 字符串变量参与比较（var_kind_ 传播）
  auto m3 = gen_src("func s3() -> int:\n"
                    "    var t = \"x\"\n"
                    "    return t == \"x\"\n");
  CHECK(m3);
  bool has_str_eq3 = false;
  for (const auto &inst : m3->funcs[0].instructions) {
    if (inst.op == ir::OP_STR_EQ) has_str_eq3 = true;
  }
  CHECK(has_str_eq3);
}

// ^ 幂运算 → 调用运行时 vx_pow_i32
void test_power() {
  auto m = gen_src("func p() -> int:\n    return 2 ^ 10\n");
  CHECK(m);
  bool has_pow = false;
  for (const auto &inst : m->funcs[0].instructions) {
    if (inst.op == ir::OP_CALL && inst.args == "f;vx_pow_i32") has_pow = true;
  }
  CHECK(has_pow);
  // 嵌套：两侧先求值
  auto m2 = gen_src("func p2(a: int) -> int:\n    return (a + 1) ^ 2\n");
  CHECK(m2);
  bool has_pow2 = false, has_add = false;
  for (const auto &inst : m2->funcs[0].instructions) {
    if (inst.op == ir::OP_CALL && inst.args == "f;vx_pow_i32") has_pow2 = true;
    if (inst.op == ir::OP_I32_ADD) has_add = true;
  }
  CHECK(has_pow2);
  CHECK(has_add);
}

// 变量类型传播：浮点变量参与 F64 运算（旧版退化为 I32 → 后端类型诊断）
void test_float_var_propagation() {
  auto m = gen_src("func v() -> float:\n"
                   "    var x = 1.5\n"
                   "    return x * 2.0\n");
  CHECK(m);
  bool has_f64_mul = false, has_i32_mul = false;
  for (const auto &inst : m->funcs[0].instructions) {
    if (inst.op == ir::OP_F64_MUL) has_f64_mul = true;
    if (inst.op == ir::OP_I32_MUL) has_i32_mul = true;
  }
  CHECK(has_f64_mul);
  CHECK(!has_i32_mul);
  // 赋值更新类型：var y = 0.0; y = y + 4.5 → F64_ADD
  auto m2 = gen_src("func v2() -> float:\n"
                    "    var y = 0.0\n"
                    "    y = y + 4.5\n"
                    "    return y\n");
  CHECK(m2);
  bool has_f64_add = false;
  for (const auto &inst : m2->funcs[0].instructions) {
    if (inst.op == ir::OP_F64_ADD) has_f64_add = true;
  }
  CHECK(has_f64_add);
  // 整数变量不误判：var n = 1; n * 2 → I32_MUL
  auto m3 = gen_src("func v3() -> int:\n"
                    "    var n = 1\n"
                    "    return n * 2\n");
  CHECK(m3);
  bool has_i32_mul3 = false;
  for (const auto &inst : m3->funcs[0].instructions) {
    if (inst.op == ir::OP_I32_MUL) has_i32_mul3 = true;
  }
  CHECK(has_i32_mul3);
}

} // namespace

int main() {
  test_arith();
  test_vars_assign();
  test_if_elif_else();
  test_while_break_continue();
  test_for();
  test_extern_linkage();
  test_class_method_new();
  test_collections_index();
  test_literals_unary();
  test_default_return();
  test_compound_assign();
  test_globals_recorded();
  test_errors();
  test_short_circuit();
  test_string_compare();
  test_power();
  test_float_var_propagation();

  if (g_failures == 0) {
    std::fprintf(stdout, "codegen tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "codegen tests: %d failures\n", g_failures);
  return 1;
}
