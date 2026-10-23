// tidy_test — vx-tidy 静态分析引擎测试（自带断言宏，对齐主仓测试风格）。
#include "tidy.hpp"

#include <cstdio>
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

using vxtidy::Finding;

// 在发现中查找指定规则的条目；返回指针或 nullptr
const Finding *find_rule(const std::vector<Finding> &fs, std::string_view rule) {
  for (const Finding &f : fs) {
    if (f.rule == rule) return &f;
  }
  return nullptr;
}

std::size_t count_rule(const std::vector<Finding> &fs, std::string_view rule) {
  std::size_t n = 0;
  for (const Finding &f : fs) {
    if (f.rule == rule) ++n;
  }
  return n;
}

// ---- 用例 ----

void test_clean_source() {
  // 完全干净的源码：无任何发现
  const auto fs = vxtidy::check(
      "func add(a: int, b: int) -> int:\n"
      "    return a + b\n"
      "\n"
      "func main() -> int:\n"
      "    var x = add(1, 2)\n"
      "    out_int(x)\n"
      "    return 0\n",
      "clean.vx");
  CHECK(fs.empty());
  if (!fs.empty()) {
    for (const auto &f : fs) {
      std::fprintf(stderr, "  unexpected: %s\n",
                   vxtidy::format_text(f, "clean.vx").c_str());
    }
  }
}

void test_undeclared_call() {
  const auto fs = vxtidy::check(
      "func main():\n"
      "    nosuchfn(1)\n"
      "    return\n",
      "u.vx");
  const Finding *f = find_rule(fs, "VXTIDY001");
  CHECK(f != nullptr);
  if (f) {
    CHECK(f->line == 2);
    CHECK(f->message.find("nosuchfn") != std::string::npos);
    CHECK(f->severity == 1);
  }
  // 内置函数不算未声明
  const auto fs2 = vxtidy::check(
      "func main():\n"
      "    out_int(42)\n"
      "    return\n",
      "b.vx");
  CHECK(find_rule(fs2, "VXTIDY001") == nullptr);
  // extern 声明后调用合法
  const auto fs3 = vxtidy::check(
      "extern puts(s: string)\n"
      "func main():\n"
      "    puts(\"hi\")\n"
      "    return\n",
      "e.vx");
  CHECK(find_rule(fs3, "VXTIDY001") == nullptr);
}

void test_unused_variable() {
  const auto fs = vxtidy::check(
      "func main():\n"
      "    var unused = 1\n"
      "    return\n",
      "v.vx");
  const Finding *f = find_rule(fs, "VXTIDY002");
  CHECK(f != nullptr);
  if (f) {
    CHECK(f->line == 2);
    CHECK(f->severity == 2); // warning
  }
  // 读取过的变量不报
  const auto fs2 = vxtidy::check(
      "func main() -> int:\n"
      "    var used = 1\n"
      "    return used\n",
      "v2.vx");
  CHECK(find_rule(fs2, "VXTIDY002") == nullptr);
}

void test_redeclared() {
  // 函数重复声明
  const auto fs = vxtidy::check(
      "func dup():\n"
      "    return\n"
      "func dup():\n"
      "    return\n",
      "d.vx");
  CHECK(count_rule(fs, "VXTIDY003") == 1);
  // 局部变量重复声明
  const auto fs2 = vxtidy::check(
      "func f():\n"
      "    var a = 1\n"
      "    var a = 2\n"
      "    out_int(a)\n"
      "    return\n",
      "d2.vx");
  CHECK(find_rule(fs2, "VXTIDY003") != nullptr);
}

void test_shadowed_param() {
  const auto fs = vxtidy::check(
      "func f(x: int) -> int:\n"
      "    var x = 1\n"
      "    return x\n",
      "s.vx");
  const Finding *f = find_rule(fs, "VXTIDY004");
  CHECK(f != nullptr);
  if (f) {
    CHECK(f->message.find("遮蔽") != std::string::npos);
  }
}

void test_missing_return() {
  const auto fs = vxtidy::check(
      "func f() -> int:\n"
      "    var x = 1\n"
      "    out_int(x)\n",
      "m.vx");
  const Finding *f = find_rule(fs, "VXTIDY005");
  CHECK(f != nullptr);
  // void 函数不要求 return
  const auto fs2 = vxtidy::check(
      "func g():\n"
      "    return\n",
      "m2.vx");
  CHECK(find_rule(fs2, "VXTIDY005") == nullptr);
  // 有 return 不报
  const auto fs3 = vxtidy::check(
      "func h() -> int:\n"
      "    return 0\n",
      "m3.vx");
  CHECK(find_rule(fs3, "VXTIDY005") == nullptr);
}

void test_import_prefix() {
  // @imp 导入库前缀调用不报未声明
  const auto fs = vxtidy::check(
      "@imp(math)\n"
      "func main() -> int:\n"
      "    return math.add(1, 2)\n",
      "i.vx");
  CHECK(find_rule(fs, "VXTIDY001") == nullptr);
  // 未导入的前缀报 VXTIDY001
  const auto fs2 = vxtidy::check(
      "func main() -> int:\n"
      "    return unknownlib.f(1)\n",
      "i2.vx");
  CHECK(find_rule(fs2, "VXTIDY001") != nullptr);
}

void test_syntax_error_passthrough() {
  const auto fs = vxtidy::check(
      "func main():\n"
      "    var x = \n",
      "sy.vx");
  const Finding *f = find_rule(fs, "VXTIDY900");
  CHECK(f != nullptr);
  if (f) CHECK(f->severity == 1);
}

void test_format() {
  const Finding f{"VXTIDY001", 1, 3, 5, "调用了未声明的函数 'foo'"};
  const std::string line = vxtidy::format_text(f, "a.vx");
  CHECK(line == "a.vx:3:5: error: 调用了未声明的函数 'foo' [VXTIDY001]");

  const std::string js = vxtidy::format_json({f}, "a.vx");
  CHECK(js.find("\"rule\":\"VXTIDY001\"") != std::string::npos);
  CHECK(js.find("\"file\":\"a.vx\"") != std::string::npos);
  CHECK(js.front() == '[');
  CHECK(js.back() == ']');
}

void test_rules_registry() {
  const auto &rules = vxtidy::all_rules();
  CHECK(rules.size() >= 6);
  // 编号唯一
  for (std::size_t i = 0; i < rules.size(); ++i) {
    for (std::size_t j = i + 1; j < rules.size(); ++j) {
      CHECK(rules[i].rule != rules[j].rule);
    }
  }
}

} // namespace

int main() {
  test_clean_source();
  test_undeclared_call();
  test_unused_variable();
  test_redeclared();
  test_shadowed_param();
  test_missing_return();
  test_import_prefix();
  test_syntax_error_passthrough();
  test_format();
  test_rules_registry();

  if (g_failures == 0) {
    std::fputs("vx-tidy tests: all passed\n", stdout);
    return 0;
  }
  std::fprintf(stderr, "vx-tidy tests: %d failures\n", g_failures);
  return 1;
}
