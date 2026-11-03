// semantics_test — 语义分析测试：符号表 / 类型环境 / 诊断（自带断言宏，无第三方依赖）。
#include "vx/compiler/semantics.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                             \
      ++g_failures;                                                                                                    \
    }                                                                                                                  \
  } while (0)

using namespace vx::compiler;

// 语义检查源码，返回诊断码集合（按出现顺序）
std::vector<std::string> check_src(std::string_view src, bool* ok = nullptr) {
  static vx::util::Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, "<test>", arena, bag);
  auto prog = parse(toks, "<test>", arena, bag);
  if (!prog || bag.has_error()) {
    std::fprintf(stderr, "FAIL parse:\n%s", render_all(bag, src, false).c_str());
    ++g_failures;
    if (ok)
      *ok = false;
    return {};
  }
  bool pass = check_module(*prog, "<test>", bag);
  if (ok)
    *ok = pass;
  std::vector<std::string> codes;
  for (const Diagnostic& d : bag.items()) {
    if (!d.code.empty()) {
      codes.push_back(d.code);
    }
  }
  return codes;
}

bool has_code(const std::vector<std::string>& codes, std::string_view code) {
  for (const std::string& c : codes) {
    if (c == code) {
      return true;
    }
  }
  return false;
}

// 语义检查源码，返回全部诊断（用于断言 note/help 内容）
std::vector<Diagnostic> check_full(std::string_view src) {
  static vx::util::Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, "<test>", arena, bag);
  auto prog = parse(toks, "<test>", arena, bag);
  if (!prog || bag.has_error()) {
    std::fprintf(stderr, "FAIL parse:\n%s", render_all(bag, src, false).c_str());
    ++g_failures;
    return {};
  }
  check_module(*prog, "<test>", bag);
  return bag.items();
}

// 是否存在 code匹配且 note 含子串的诊断
bool has_note(const std::vector<Diagnostic>& diags, std::string_view code, std::string_view note_sub) {
  for (const Diagnostic& d : diags) {
    if (d.code == code && d.note.find(note_sub) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// ---- 用例 ----

// 合法程序：函数/前向引用/类/方法/类常量/内置函数/控制流全部通过
void test_ok_program() {
  bool ok = true;
  auto codes = check_src("func helper(n: int) -> int:\n"
                         "    return n * 2\n"
                         "\n"
                         "func main() -> int:\n"
                         "    var total = helper(21)      # 前向引用已声明的函数\n"
                         "    var items = [1, 2, 3]\n"
                         "    var conf = {\"a\": 1}\n"
                         "    for it in items:\n"
                         "        total = total + it      # for 循环变量与重赋值合法\n"
                         "    while total > 0:\n"
                         "        total = total - 1\n"
                         "        if total < 5:\n"
                         "            break\n"
                         "    out_int(total)\n"
                         "    file_read(\"x.txt\")\n"
                         "    return total\n",
                         &ok);
  CHECK(ok);
  CHECK(codes.empty());
}

// 未定义变量（E0010）
void test_undefined_var() {
  auto codes = check_src("func main() -> int:\n"
                         "    x = 1\n"
                         "    return y\n");
  CHECK(has_code(codes, diag_code::sem_undefined));
}

// 重复定义（E0011）：同作用域变量 / 顶层函数 / 同名方法
void test_redefined() {
  auto codes = check_src("func main():\n"
                         "    var a = 1\n"
                         "    var a = 2\n");
  CHECK(has_code(codes, diag_code::sem_redefined));

  codes = check_src("func f():\n"
                    "    return\n"
                    "func f():\n"
                    "    return\n");
  CHECK(has_code(codes, diag_code::sem_redefined));

  codes = check_src("class C:\n"
                    "    func m():\n"
                    "        return\n"
                    "    func m():\n"
                    "        return\n");
  CHECK(has_code(codes, diag_code::sem_redefined));
}

// 内层遮蔽外层合法，不报重复定义
void test_shadowing() {
  bool ok = true;
  auto codes = check_src("func main():\n"
                         "    var a = 1\n"
                         "    if a > 0:\n"
                         "        var a = 2\n"
                         "    return\n",
                         &ok);
  CHECK(ok);
  CHECK(codes.empty());
}

// 未知类型标注（E0003）
void test_unknown_type() {
  auto codes = check_src("func main():\n"
                         "    var x: bogus = 1\n");
  CHECK(has_code(codes, diag_code::sem_unknown_type));
}

// break/continue/return 位置（E0005）
void test_flow() {
  auto codes = check_src("func main():\n"
                         "    break\n");
  CHECK(has_code(codes, diag_code::sem_flow));

  codes = check_src("func main():\n"
                    "    while true:\n"
                    "        break\n"
                    "    continue\n");
  CHECK(has_code(codes, diag_code::sem_flow));
}

// 调用检查：参数个数（E0006）/ 内置函数放行。
// 注：本编译单元未声明的函数不报错 —— vmake 多文件工程允许跨文件调用，
// 符号在链接期解析。
void test_call_checks() {
  auto codes = check_src("func add(a: int, b: int) -> int:\n"
                         "    return a + b\n"
                         "func main() -> int:\n"
                         "    return add(1)\n");
  CHECK(has_code(codes, diag_code::sem_arity));

  bool ok = true;
  codes = check_src("func main() -> int:\n"
                    "    out_int(1)\n"
                    "    return sys_argv_count()\n",
                    &ok);
  CHECK(ok);
  CHECK(codes.empty());
}

// new：类必须已定义（E0010）；字段/方法/类常量访问
void test_class_checks() {
  auto codes = check_src("func main():\n"
                         "    var o = new Missing()\n");
  CHECK(has_code(codes, diag_code::sem_undefined));

  codes = check_src("class C:\n"
                    "    x: int\n"
                    "    func get(self) -> int:\n"
                    "        return self.x\n"
                    "func main() -> int:\n"
                    "    var o = new C()\n"
                    "    o.y = 1\n"
                    "    return o.get()\n");
  CHECK(has_code(codes, diag_code::sem_undefined)); // o.y 字段不存在

  bool ok = true;
  codes = check_src("class C:\n"
                    "    x: int\n"
                    "    NAME = \"c\"\n"
                    "    func get(self) -> int:\n"
                    "        return self.x\n"
                    "func main():\n"
                    "    var o = new C()\n"
                    "    o.x = 1\n"
                    "    out_int(o.get())\n"
                    "    if C.NAME == \"c\":\n"
                    "        out_int(1)\n",
                    &ok);
  // 字段赋值 / 方法调用 / 类常量访问全部通过
  CHECK(ok);
  CHECK(codes.empty());
}

// 字段类型检查：赋值类型须与字段声明一致（无隐式转换）
void test_field_type() {
  auto codes = check_src("class P:\n"
                         "    x: int\n"
                         "func main():\n"
                         "    var p = new P()\n"
                         "    p.x = 1.5\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));

  bool ok = true;
  codes = check_src("class P:\n"
                    "    x: float\n"
                    "func main():\n"
                    "    var p = new P()\n"
                    "    p.x = 1.5\n"
                    "    var y = p.x\n"
                    "    y = y * 2.0\n",
                    &ok);
  CHECK(ok);
  CHECK(codes.empty());
}

// 变量初始化 / 赋值 / 运算的无隐式转换策略
void test_no_implicit_conversion() {
  // 标注与初始化不一致
  auto codes = check_src("func main():\n"
                         "    var f: float = 1\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));

  // 已推断类型的变量赋不同值
  codes = check_src("func main():\n"
                    "    var n = 1\n"
                    "    n = 1.5\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));

  // int 与 float 混合运算
  codes = check_src("func main():\n"
                    "    var n = 1\n"
                    "    var x = 2.0\n"
                    "    n + x\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));

  // 比较两侧类型不一致
  codes = check_src("func main():\n"
                    "    if 1 == \"1\":\n"
                    "        out_int(1)\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));

  // 复合赋值按 target op value 展开校验
  codes = check_src("func main():\n"
                    "    var n = 1\n"
                    "    n += 1.5\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));
}

// 返回值类型须与声明一致（void 不可返回值）
void test_return_type() {
  auto codes = check_src("func f() -> int:\n"
                         "    return \"s\"\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));

  codes = check_src("func f() -> void:\n"
                    "    return 1\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));

  // 非 void 函数的裸 return 允许（codegen 补默认返回值）
  bool ok = true;
  codes = check_src("func f() -> int:\n"
                    "    return\n",
                    &ok);
  CHECK(ok);
  CHECK(codes.empty());
}

// var 推断：初始化表达式 → 变量类型；首次赋值细化未定类型
void test_var_inference() {
  bool ok = true;
  auto codes = check_src("func main():\n"
                         "    var a = 1\n"
                         "    var b = a * 2\n"
                         "    var c = nil\n"
                         "    c = \"x\"\n"
                         "    c = c + \"y\"\n"
                         "    var list = [1.5, 2.5]\n"
                         "    var e = list[0]\n"
                         "    e = e + 1.0\n"
                         "    for it in [1, 2]:\n"
                         "        b = b + it\n",
                         &ok);
  CHECK(ok);
  CHECK(codes.empty());

  // 列表元素类型不一致
  codes = check_src("func main():\n"
                    "    var list = [1, \"a\"]\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));
}

// 方法重载解析：按参数类型匹配；无匹配 / 歧义分别报错
void test_method_overloads() {
  bool ok = true;
  auto codes = check_src("class C:\n"
                         "    func m(a: int) -> int:\n"
                         "        return a\n"
                         "    func m(a: float) -> float:\n"
                         "        return a\n"
                         "func main():\n"
                         "    var o = new C()\n"
                         "    out_int(o.m(3))\n"
                         "    out_float(o.m(1.5))\n",
                         &ok);
  CHECK(ok);
  CHECK(codes.empty());

  // 实参类型不匹配任何重载
  codes = check_src("class C:\n"
                    "    func m(a: int) -> int:\n"
                    "        return a\n"
                    "func main():\n"
                    "    var o = new C()\n"
                    "    o.m(\"s\")\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));

  // 个数不匹配
  codes = check_src("class C:\n"
                    "    func m(a: int) -> int:\n"
                    "        return a\n"
                    "func main():\n"
                    "    var o = new C()\n"
                    "    o.m()\n");
  CHECK(has_code(codes, diag_code::sem_arity));

  // var 实参同时匹配多个重载 → 歧义
  codes = check_src("class C:\n"
                    "    func m(a: int) -> int:\n"
                    "        return a\n"
                    "    func m(a: var) -> int:\n"
                    "        return 1\n"
                    "func main():\n"
                    "    var o = new C()\n"
                    "    var v = 1\n"
                    "    o.m(v)\n");
  CHECK(has_code(codes, diag_code::sem_ambiguous));

  // 相同参数签名的重复声明仍报 E0011
  codes = check_src("class C:\n"
                    "    func m(a: int) -> int:\n"
                    "        return a\n"
                    "    func m(a: int) -> int:\n"
                    "        return a\n");
  CHECK(has_code(codes, diag_code::sem_redefined));
}

// 函数实参类型检查（extern 为 FFI 边界，只查个数）
void test_call_arg_types() {
  auto codes = check_src("func add(a: int, b: int) -> int:\n"
                         "    return a + b\n"
                         "func main() -> int:\n"
                         "    return add(1, \"x\")\n");
  CHECK(has_code(codes, diag_code::sem_type_mismatch));

  bool ok = true;
  codes = check_src("extern puts(s: string)\n"
                    "func main():\n"
                    "    var buf = malloc(32)\n"
                    "    puts(buf)\n",
                    &ok);
  CHECK(ok);
  CHECK(codes.empty());
}

} // namespace

// 诊断 note/help：E0010 help 建议、E0011 指向首次定义、E0012 作用域泄漏
void test_diag_notes() {
  // E0010 未定义变量带 help 建议
  auto diags = check_full("func main():\n"
                          "    x = 1\n");
  CHECK(has_note(diags, diag_code::sem_undefined, "help:"));

  // E0011 重复定义 note 指向首次定义行
  diags = check_full("func main():\n"
                     "    var a = 1\n"
                     "    var a = 2\n");
  CHECK(has_note(diags, diag_code::sem_redefined, "第 2 行"));

  // E0012 作用域泄漏：内层声明的变量在外层使用
  diags = check_full("func main():\n"
                     "    if true:\n"
                     "        var inner = 1\n"
                     "    inner = 2\n");
  CHECK(has_note(diags, diag_code::sem_scope_leak, "第 3 行"));
  CHECK(has_note(diags, diag_code::sem_scope_leak, "help:"));

  // 函数重复定义 note 指向首次声明行
  diags = check_full("func dup():\n"
                     "    return\n"
                     "func dup():\n"
                     "    return\n");
  CHECK(has_note(diags, diag_code::sem_redefined, "第 1 行"));
}

int main() {
  test_ok_program();
  test_undefined_var();
  test_redefined();
  test_shadowing();
  test_unknown_type();
  test_flow();
  test_call_checks();
  test_class_checks();
  test_field_type();
  test_no_implicit_conversion();
  test_return_type();
  test_var_inference();
  test_method_overloads();
  test_call_arg_types();
  test_diag_notes();

  if (g_failures == 0) {
    std::fprintf(stdout, "semantics tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "semantics tests: %d failure(s)\n", g_failures);
  return 1;
}
