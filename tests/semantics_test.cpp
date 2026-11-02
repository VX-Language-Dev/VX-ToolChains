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
                    "func main() -> int:\n"
                    "    var o = new C()\n"
                    "    o.x = 1\n"
                    "    out_int(o.get())\n"
                    "    return C.NAME == \"c\"\n",
                    &ok);
  // 字段赋值 / 方法调用 / 类常量访问全部通过
  CHECK(ok);
  CHECK(codes.empty());
}

} // namespace

int main() {
  test_ok_program();
  test_undefined_var();
  test_redefined();
  test_shadowing();
  test_unknown_type();
  test_flow();
  test_call_checks();
  test_class_checks();

  if (g_failures == 0) {
    std::fprintf(stdout, "semantics tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "semantics tests: %d failure(s)\n", g_failures);
  return 1;
}
