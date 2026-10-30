// vmake_lsp_test — vmake-lsp 解析器单元测试（自带断言宏）。
// 覆盖：合法样例的解析结果与位置、vmake 权威规则的诊断、错误恢复、命中测试。
#include "parser.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

using vmake_lsp::ParseResult;
using vmake_lsp::TargetKind;
using vmake_lsp::VTarget;

ParseResult parse(std::string_view text) {
  return vmake_lsp::parse_vmake(text);
}

std::size_t diag_count(const ParseResult &r) { return r.diagnostics.size(); }

// 断言恰好一条诊断且文案包含 needle
void expect_one_diag(const ParseResult &r, std::string_view needle,
                     const char *file, int line) {
  if (r.diagnostics.size() != 1) {
    std::fprintf(stderr, "FAIL %s:%d: 期望 1 条诊断，实际 %zu\n", file, line,
                 r.diagnostics.size());
    ++g_failures;
    return;
  }
  if (r.diagnostics[0].message.find(needle) == std::string::npos) {
    std::fprintf(stderr, "FAIL %s:%d: 诊断文案不含 '%.*s'，实际 '%s'\n", file,
                 line, (int)needle.size(), needle.data(),
                 r.diagnostics[0].message.c_str());
    ++g_failures;
  }
}

#define EXPECT_DIAG(r, needle) expect_one_diag((r), (needle), __FILE__, __LINE__)

void test_valid_target() {
  ParseResult r = parse("exe(\"app\", sources=[\"src/main.vx\", \"src/util.vx\"])\n");
  CHECK(r.ok);
  CHECK(diag_count(r) == 0);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() != 1) return;

  const VTarget &t = r.targets[0];
  CHECK(t.kind == TargetKind::Exe);
  CHECK(t.name == "app");
  CHECK(t.sources.size() == 2);
  CHECK(t.sources[0] == "src/main.vx");
  CHECK(t.sources[1] == "src/util.vx");
  CHECK(!t.output.has_value());

  // 位置：名字值不含引号
  CHECK(t.name_range.start.line == 0);
  CHECK(t.name_range.start.character == 5);
  CHECK(t.name_range.end.character == 8);
  // 关键字范围
  CHECK(t.kind_range.start.character == 0);
  CHECK(t.kind_range.end.character == 3);
  // 调用范围覆盖整条语句
  CHECK(t.call_range.start.character == 0);
  CHECK(t.call_range.end.character == 50);
}

void test_dylib_and_output() {
  ParseResult r = parse("dylib(\"calc\", sources=[\"a.vx\"], output=\"libcalc.dylib\")\n");
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() != 1) return;
  const VTarget &t = r.targets[0];
  CHECK(t.kind == TargetKind::Dylib);
  CHECK(t.output.has_value());
  if (t.output) CHECK(*t.output == "libcalc.dylib");
}

void test_keyword_name() {
  // name 也可以写成关键字参数
  ParseResult r = parse("exe(name=\"app\", sources=[\"a.vx\"])\n");
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() != 1) return;
  CHECK(r.targets[0].name == "app");
}

void test_multiline_and_comments() {
  ParseResult r = parse("# 顶部注释\nexe(\"app\", sources=[\n"
                        "  \"src/main.vx\",   # 入口\n"
                        "  \"src/util.vx\",\n"
                        "])\n");
  CHECK(r.ok);
  CHECK(diag_count(r) == 0);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() != 1) return;
  const VTarget &t = r.targets[0];
  CHECK(t.sources.size() == 2);
  CHECK(t.source_ranges.size() == 2);
  // 第一个源文件位于第 2 行
  CHECK(t.source_ranges[0].start.line == 2);
  CHECK(t.source_ranges[1].start.line == 3);
  // 调用范围跨越多行
  CHECK(t.call_range.start.line == 1);
  CHECK(t.call_range.end.line == 4);
}

void test_escapes() {
  // \uXXXX 解码为 UTF-8（ASCII 用例），\t 等转义生效
  ParseResult r = parse("exe(\"a\\u0042c\", sources=[\"c\\u0041.vx\", \"d\\t.vx\"])\n");
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() != 1) return;
  CHECK(r.targets[0].name == "aBc");
  CHECK(r.targets[0].sources.size() == 2);
  CHECK(r.targets[0].sources[0] == "cA.vx");
  CHECK(r.targets[0].sources[1] == "d\t.vx");

  // \n 正常解码为换行，但换行使目标名非法（与 vmake 的 valid_target_name 一致）
  ParseResult r2 = parse("exe(\"a\\nb\", sources=[\"a.vx\"])\n");
  EXPECT_DIAG(r2, "目标名 'a\nb' 非法");
  CHECK(r2.targets.size() == 1);
}

void test_unicode_escape_utf8() {
  // \uXXXX 超出 ASCII 范围时编码为多字节 UTF-8（源文件路径允许非 ASCII）
  ParseResult r = parse("exe(\"app\", sources=[\"\\u4e2d.vx\"])\n");
  CHECK(r.ok);
  CHECK(diag_count(r) == 0);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() != 1) return;
  CHECK(r.targets[0].sources[0] == "\u4e2d.vx");
}

void test_single_quotes() {
  // 单引号字符串不处理转义
  ParseResult r = parse("exe('app', sources=['a.vx'])\n");
  CHECK(r.ok);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() != 1) return;
  CHECK(r.targets[0].name == "app");
  CHECK(r.targets[0].sources[0] == "a.vx");
}

void test_empty_document() {
  ParseResult r = parse("");
  EXPECT_DIAG(r, "未声明任何目标");
  ParseResult r2 = parse("# 只有注释\n");
  EXPECT_DIAG(r2, "未声明任何目标");
}

void test_syntax_diagnostics() {
  {
    ParseResult r = parse("foo()\n");
    EXPECT_DIAG(r, "未知目标类型");
  }
  {
    ParseResult r = parse("exe\n");
    EXPECT_DIAG(r, "需要 '('");
  }
  {
    ParseResult r = parse("exe(\"app\", sources=[\"a.vx\"]\n");
    EXPECT_DIAG(r, "缺少 ')'");
  }
  {
    ParseResult r = parse("exe(sources=[\"a.vx\"])\n");
    EXPECT_DIAG(r, "目标缺少 name 字段");
  }
  {
    ParseResult r = parse("exe(\"app\")\n");
    EXPECT_DIAG(r, "缺少 sources 字段");
  }
  {
    ParseResult r = parse("exe(\"app\", sources=[\"a.vx\"], version=1)\n");
    EXPECT_DIAG(r, "未知字段");
  }
  {
    ParseResult r = parse("exe(\"app\", name=\"b\", sources=[\"a.vx\"])\n");
    EXPECT_DIAG(r, "字段 name 重复");
  }
  {
    ParseResult r = parse("exe(\"app\", sources=[\"a.vx\"], sources=[\"b.vx\"])\n");
    EXPECT_DIAG(r, "字段 sources 重复");
  }
  {
    ParseResult r = parse("exe(\"app\", sources=[\"a.vx\"], output=\"x\", output=\"y\")\n");
    EXPECT_DIAG(r, "字段 output 重复");
  }
  {
    ParseResult r = parse("exe(\"app\", sources=\"a.vx\")\n");
    EXPECT_DIAG(r, "sources 期望字符串数组");
  }
  {
    ParseResult r = parse("exe(\"app\", sources=[\n");
    EXPECT_DIAG(r, "sources 数组未闭合");
  }
  {
    ParseResult r = parse("exe(\"app\", sources=[1])\n");
    EXPECT_DIAG(r, "数组元素必须是字符串");
  }
  {
    ParseResult r = parse("exe(\"app\", sources=[\"a.vx\" \"b.vx\"])\n");
    EXPECT_DIAG(r, "缺少 ','");
  }
  {
    ParseResult r = parse("exe(\"app\", sources [\"a.vx\"])\n");
    EXPECT_DIAG(r, "后缺少 '='");
  }
}

void test_semantic_diagnostics() {
  {
    ParseResult r = parse("exe(\"bad name\", sources=[\"a.vx\"])\n");
    EXPECT_DIAG(r, "目标名 'bad name' 非法");
    // 语义错误不影响目标本身被解析出来
    CHECK(r.targets.size() == 1);
  }
  {
    ParseResult r = parse("exe(\"a\", sources=[\"a.vx\"])\nexe(\"a\", sources=[\"b.vx\"])\n");
    EXPECT_DIAG(r, "目标名 'a' 重复");
    CHECK(r.targets.size() == 2);
  }
  {
    ParseResult r = parse("exe(\"a\", sources=[])\n");
    EXPECT_DIAG(r, "sources 不能为空");
  }
  {
    ParseResult r = parse("exe(\"a\", sources=[\"\"])\n");
    EXPECT_DIAG(r, "含空源文件路径");
  }
  {
    ParseResult r = parse("exe(\"a\", sources=[\"a.txt\"])\n");
    EXPECT_DIAG(r, "源文件必须是 .vx 文件: a.txt");
  }
  {
    ParseResult r = parse("exe(\"a\", sources=[\"a.vx\"], output=\"\")\n");
    EXPECT_DIAG(r, "output 不能为空");
  }
  {
    ParseResult r = parse("exe(\"a\", sources=[\"a.vx\"], output=\"/abs/out\")\n");
    EXPECT_DIAG(r, "output 必须是相对构建目录的路径: /abs/out");
  }
  {
    ParseResult r = parse("exe(\"a\", sources=[\"a.vx\"], output=\"../out\")\n");
    EXPECT_DIAG(r, "output 不得包含 '..': ../out");
  }
  {
    // 相对路径中的子目录是允许的
    ParseResult r = parse("exe(\"a\", sources=[\"a.vx\"], output=\"bin/custom\")\n");
    CHECK(r.ok);
  }
}

void test_error_recovery() {
  // 出错的语句不应影响后续语句的解析
  ParseResult r = parse("foo()\nexe(\"app\", sources=[\"a.vx\"])\n");
  CHECK(diag_count(r) == 1);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() == 1) CHECK(r.targets[0].name == "app");
}

void test_hit_test() {
  const std::string text =
      "exe(\"app\", sources=[\"a.vx\"], output=\"out\")\n";
  ParseResult r = parse(text);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() != 1) return;

  auto hit = vmake_lsp::hit_test(r.targets, 0, 1);
  CHECK(hit.part == vmake_lsp::Hit::Part::Kind);

  hit = vmake_lsp::hit_test(r.targets, 0, 6);
  CHECK(hit.part == vmake_lsp::Hit::Part::Name);

  // 源文件字符串 "a.vx"（第 21~24 列）
  hit = vmake_lsp::hit_test(r.targets, 0, 22);
  CHECK(hit.part == vmake_lsp::Hit::Part::SourceElem);

  // output 字符串值 "out"（第 37~39 列）
  hit = vmake_lsp::hit_test(r.targets, 0, 38);
  CHECK(hit.part == vmake_lsp::Hit::Part::Output);

  // 字段名关键字
  hit = vmake_lsp::hit_test(r.targets, 0, 15);
  CHECK(hit.part == vmake_lsp::Hit::Part::Sources);
  hit = vmake_lsp::hit_test(r.targets, 0, 30);
  CHECK(hit.part == vmake_lsp::Hit::Part::Output);

  // 空白处不属于任何部分
  hit = vmake_lsp::hit_test(r.targets, 0, 10);
  CHECK(hit.part == vmake_lsp::Hit::Part::None);
}

void test_deps_field() {
  // 解析 + 位置
  const std::string text =
      "exe(\"app\", sources=[\"a.vx\"], deps=[\"calc\", \"util\"])\n";
  ParseResult r = parse(text);
  CHECK(r.ok);
  CHECK(diag_count(r) == 0);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() != 1) return;
  const VTarget &t = r.targets[0];
  CHECK(t.has_deps);
  CHECK(t.deps.size() == 2);
  if (t.deps.size() == 2) {
    CHECK(t.deps[0] == "calc");
    CHECK(t.deps[1] == "util");
    CHECK(t.dep_ranges.size() == 2);
    // 第一个依赖名 "calc" 的值范围（不含引号）
    CHECK(t.dep_ranges[0].start.line == 0);
    CHECK(t.dep_ranges[0].start.character == 36);
    CHECK(t.dep_ranges[0].end.character == 40);
  }

  // hit_test：deps 字段名 / 元素 / 数组
  auto hit = vmake_lsp::hit_test(r.targets, 0, 31); // "deps" 关键字
  CHECK(hit.part == vmake_lsp::Hit::Part::Deps);
  hit = vmake_lsp::hit_test(r.targets, 0, 37); // "calc" 值
  CHECK(hit.part == vmake_lsp::Hit::Part::DepElem);

  // 未声明 deps 的目标：deps 范围不得误命中
  ParseResult plain = parse("exe(\"app\", sources=[\"a.vx\"])\n");
  CHECK(plain.ok);
  if (plain.targets.size() == 1) {
    CHECK(!plain.targets[0].has_deps);
    const auto hit0 =
        vmake_lsp::hit_test(plain.targets, 0, 0);
    CHECK(hit0.part != vmake_lsp::Hit::Part::Deps);
  }

  // 诊断：重复 / 非数组 / 未闭合
  ParseResult dup = parse("exe(\"app\", sources=[\"a.vx\"], deps=[\"b\"], deps=[\"c\"])\n");
  EXPECT_DIAG(dup, "字段 deps 重复");
  ParseResult nonarr = parse("exe(\"app\", sources=[\"a.vx\"], deps=\"b\")\n");
  EXPECT_DIAG(nonarr, "deps 期望字符串数组");
  ParseResult unclosed = parse("exe(\"app\", sources=[\"a.vx\"], deps=[\n");
  EXPECT_DIAG(unclosed, "deps 数组未闭合");
  ParseResult unknown = parse("exe(\"app\", sources=[\"a.vx\"], version=1)\n");
  EXPECT_DIAG(unknown, "未知字段");
}

void test_kind_name() {
  CHECK(std::string(vmake_lsp::kind_name(TargetKind::Exe)) == "exe");
  CHECK(std::string(vmake_lsp::kind_name(TargetKind::Dylib)) == "dylib");
  CHECK(std::string(vmake_lsp::kind_name(TargetKind::StaticLib)) == "staticlib");
}

void test_flags_and_tests_fields() {
  // cflags / defines / ldflags / tests：解析 + 位置（列号由 find 计算，稳健）
  const std::string text =
      "exe(\"app\", sources=[\"a.vx\"], cflags=[\"-Wall\"],\n"
      "    defines=[\"NDEBUG\", \"FOO=1\"], ldflags=[\"-s\"],\n"
      "    tests=[\"tests/app_test.vx\"])\n";
  ParseResult r = parse(text);
  CHECK(r.ok);
  CHECK(diag_count(r) == 0);
  CHECK(r.targets.size() == 1);
  if (r.targets.size() != 1) return;
  const VTarget &t = r.targets[0];
  CHECK(t.extra_arrays.size() == 4);
  if (t.extra_arrays.size() != 4) return;

  auto col = [&](std::string_view needle, std::size_t off = 0) {
    return static_cast<int>(text.find(needle, off));
  };

  const vmake_lsp::ArrayField &cflags = t.extra_arrays[0];
  CHECK(cflags.key == "cflags");
  CHECK(cflags.values.size() == 1);
  if (cflags.values.size() == 1) CHECK(cflags.values[0] == "-Wall");
  CHECK(cflags.kw_range.start.character == col("cflags"));
  CHECK(cflags.value_ranges[0].start.character == col("\"-Wall\"") + 1);

  const vmake_lsp::ArrayField &defines = t.extra_arrays[1];
  CHECK(defines.key == "defines");
  CHECK(defines.values.size() == 2);
  if (defines.values.size() == 2) {
    CHECK(defines.values[0] == "NDEBUG");
    CHECK(defines.values[1] == "FOO=1");
    CHECK(defines.value_ranges[0].start.line == 1);
    CHECK(defines.value_ranges[1].start.line == 1);
  }

  const vmake_lsp::ArrayField &ldflags = t.extra_arrays[2];
  CHECK(ldflags.key == "ldflags");
  CHECK(ldflags.values.size() == 1);
  if (ldflags.values.size() == 1) CHECK(ldflags.values[0] == "-s");

  const vmake_lsp::ArrayField &tests = t.extra_arrays[3];
  CHECK(tests.key == "tests");
  CHECK(tests.values.size() == 1);
  if (tests.values.size() == 1) CHECK(tests.values[0] == "tests/app_test.vx");

  // hit_test：字段名 / 元素
  auto hit = vmake_lsp::hit_test(r.targets, 0, col("cflags") + 1);
  CHECK(hit.part == vmake_lsp::Hit::Part::ExtraField);
  CHECK(hit.extra_index == 0);
  hit = vmake_lsp::hit_test(r.targets, 0, col("\"-Wall\"") + 2);
  CHECK(hit.part == vmake_lsp::Hit::Part::ExtraElem);
  CHECK(hit.extra_index == 0);
  CHECK(hit.elem_index == 0);
  hit = vmake_lsp::hit_test(r.targets, 2, 5); // 第 2 行 "tests" 关键字
  CHECK(hit.part == vmake_lsp::Hit::Part::ExtraField);
  CHECK(hit.extra_index == 3);
  hit = vmake_lsp::hit_test(r.targets, 2, 12); // 值 "tests/app_test.vx" 内部
  CHECK(hit.part == vmake_lsp::Hit::Part::ExtraElem);
  CHECK(hit.extra_index == 3);
  CHECK(hit.elem_index == 0);
}

void test_flags_tests_diagnostics() {
  // 重复字段
  ParseResult dup = parse(
      "exe(\"a\", sources=[\"a.vx\"], cflags=[\"-O2\"], cflags=[\"-g\"])\n");
  EXPECT_DIAG(dup, "字段 cflags 重复");
  ParseResult dup2 = parse(
      "exe(\"a\", sources=[\"a.vx\"], tests=[\"t.vx\"], tests=[\"u.vx\"])\n");
  EXPECT_DIAG(dup2, "字段 tests 重复");

  // 类型错误 / 未闭合
  ParseResult nonarr = parse("exe(\"a\", sources=[\"a.vx\"], ldflags=\"-s\")\n");
  EXPECT_DIAG(nonarr, "ldflags 期望字符串数组");
  ParseResult unclosed = parse("exe(\"a\", sources=[\"a.vx\"], defines=[\n");
  EXPECT_DIAG(unclosed, "defines 数组未闭合");

  // 空元素
  ParseResult empty_flag = parse("exe(\"a\", sources=[\"a.vx\"], cflags=[\"\"])\n");
  EXPECT_DIAG(empty_flag, "含空 cflags 条目");
  ParseResult empty_def = parse("exe(\"a\", sources=[\"a.vx\"], defines=[\"\"])\n");
  EXPECT_DIAG(empty_def, "含空 defines 条目");
  ParseResult empty_ld = parse("exe(\"a\", sources=[\"a.vx\"], ldflags=[\"\"])\n");
  EXPECT_DIAG(empty_ld, "含空 ldflags 条目");

  // tests 校验：不能为空 / 空路径 / 必须 .vx
  ParseResult empty_tests = parse("exe(\"a\", sources=[\"a.vx\"], tests=[])\n");
  EXPECT_DIAG(empty_tests, "tests 不能为空");
  ParseResult empty_path = parse("exe(\"a\", sources=[\"a.vx\"], tests=[\"\"])\n");
  EXPECT_DIAG(empty_path, "含空测试文件路径");
  ParseResult bad_ext = parse("exe(\"a\", sources=[\"a.vx\"], tests=[\"t.txt\"])\n");
  EXPECT_DIAG(bad_ext, "测试文件必须是 .vx 文件: t.txt");

  // staticlib：禁止 cflags / defines / ldflags，但允许 tests
  ParseResult sl_flags =
      parse("staticlib(\"u\", sources=[\"u.vx\"], cflags=[\"-O2\"])\n");
  EXPECT_DIAG(sl_flags, "staticlib 目标不允许 cflags / defines / ldflags");
  ParseResult sl_tests =
      parse("staticlib(\"u\", sources=[\"u.vx\"], tests=[\"t.vx\"])\n");
  CHECK(sl_tests.ok);
  CHECK(diag_count(sl_tests) == 0);
  // 非 staticlib 目标不受限
  ParseResult exe_flags = parse("exe(\"a\", sources=[\"a.vx\"], cflags=[\"-O2\"])\n");
  CHECK(diag_count(exe_flags) == 0);
}

} // namespace

int main() {
  test_valid_target();
  test_dylib_and_output();
  test_keyword_name();
  test_multiline_and_comments();
  test_escapes();
  test_unicode_escape_utf8();
  test_single_quotes();
  test_empty_document();
  test_syntax_diagnostics();
  test_semantic_diagnostics();
  test_error_recovery();
  test_hit_test();
  test_deps_field();
  test_kind_name();
  test_flags_and_tests_fields();
  test_flags_tests_diagnostics();

  if (g_failures == 0) {
    std::fprintf(stdout, "vmake-lsp tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "vmake-lsp tests: %d failures\n", g_failures);
  return 1;
}
