// diag_test — Compiler 层统一诊断基础设施测试（自带断言宏，不依赖第三方框架；
// MinSizeRel 下 NDEBUG 会禁用 assert，故不用 <cassert>）。
#include "vx/compiler/diag.hpp"

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

void test_severity() {
  CHECK(severity_name(Severity::Error) == "error");
  CHECK(severity_name(Severity::Warning) == "warning");
  CHECK(severity_name(Severity::Info) == "info");

  CHECK(severity_rank(Severity::Error) == 2);
  CHECK(severity_rank(Severity::Warning) == 1);
  CHECK(severity_rank(Severity::Info) == 0);
  CHECK(severity_rank(Severity::Error) > severity_rank(Severity::Warning));
}

void test_constructors() {
  Diagnostic d = make_diag(diag_code::lex_unknown_char, Severity::Error,
                           "无法识别的字符 0x40", "foo.vx", 3, 5);
  CHECK(d.code == "E2001");
  CHECK(d.severity == Severity::Error);
  CHECK(d.message == "无法识别的字符 0x40");
  CHECK(d.file == "foo.vx");
  CHECK(d.line == 3);
  CHECK(d.col == 5);
  CHECK(d.end_line == 0);
  CHECK(d.end_col == 0);

  Diagnostic s = make_diag_span(diag_code::parse_expected_token,
                                Severity::Warning, "期望 )", "a.vx", 1, 2, 1, 5);
  CHECK(s.end_line == 1);
  CHECK(s.end_col == 5);
  CHECK(s.severity == Severity::Warning);
}

void test_bag() {
  DiagBag bag;
  CHECK(bag.empty());
  CHECK(bag.size() == 0);
  CHECK(!bag.has_error());

  bag.add(make_diag(diag_code::lex_unknown_char, Severity::Warning, "w", "a.vx",
                    1, 1));
  CHECK(!bag.empty());
  CHECK(bag.size() == 1);
  CHECK(!bag.has_error());

  bag.add(make_diag(diag_code::lex_unterminated_string, Severity::Error, "e",
                    "a.vx", 2, 1));
  CHECK(bag.size() == 2);
  CHECK(bag.has_error());

  DiagBag other;
  other.add(make_diag("E9999", Severity::Info, "i", "b.vx", 1, 1));
  bag.add_all(other);
  CHECK(bag.size() == 3);

  bag.clear();
  CHECK(bag.empty());
  CHECK(!bag.has_error());

  // items() 暴露内部顺序（添加顺序）
  DiagBag ordered;
  ordered.add(make_diag(diag_code::lex_unknown_char, Severity::Error, "first",
                        "z.vx", 9, 9));
  ordered.add(make_diag(diag_code::lex_unknown_char, Severity::Error, "second",
                        "z.vx", 9, 9));
  CHECK(ordered.items().size() == 2);
  CHECK(ordered.items()[0].message == "first");
  CHECK(ordered.items()[1].message == "second");
}

void test_sort() {
  DiagBag bag;
  bag.add(make_diag("E2", Severity::Info, "b1", "b.vx", 1, 1));
  bag.add(make_diag("E2", Severity::Error, "a2", "a.vx", 2, 1));
  bag.add(make_diag("E2", Severity::Warning, "a1w", "a.vx", 1, 5));
  bag.add(make_diag("E2", Severity::Error, "a1e", "a.vx", 1, 5));
  bag.add(make_diag("E2", Severity::Error, "a1c", "a.vx", 1, 3));
  bag.sort();

  const auto &v = bag.items();
  CHECK(v.size() == 5);
  // a.vx 内部：line 1 col 3 → line 1 col 5(Error) → line 1 col 5(Warning) → line 2
  CHECK(v[0].message == "a1c");
  CHECK(v[1].message == "a1e");
  CHECK(v[2].message == "a1w");
  CHECK(v[3].message == "a2");
  CHECK(v[4].message == "b1");
  // 同级同位置的 Error 排在 Warning 之前
  CHECK(severity_rank(v[1].severity) > severity_rank(v[2].severity));
}

std::string render_expected_basic() {
  std::string e;
  e += "error[E2001]: 无法识别的字符 0x40\n";
  e += "  --> foo.vx:2:9\n";
  e += "  |\n";
  e += "2 | var x = @\n";
  e += "  | " + std::string(8, ' ') + "^\n";
  return e;
}

void test_render_basic() {
  const std::string src = "var a = 1\nvar x = @\n";
  Diagnostic d = make_diag(diag_code::lex_unknown_char, Severity::Error,
                           "无法识别的字符 0x40", "foo.vx", 2, 9);
  CHECK_EQ_STR(render_diagnostic(d, src, false), render_expected_basic());
}

void test_render_span() {
  const std::string src = "func(";
  Diagnostic d = make_diag_span(diag_code::parse_expected_token, Severity::Error,
                                "期望标识符", "a.vx", 1, 1, 1, 4);
  std::string e;
  e += "error[E2010]: 期望标识符\n";
  e += "  --> a.vx:1:1\n";
  e += "  |\n";
  e += "1 | func(\n";
  e += "  | ^^^\n";
  CHECK_EQ_STR(render_diagnostic(d, src, false), e);
}

void test_render_gutter_width() {
  // 行号 >= 10 时，gutter 宽度应为 2，使各线段对齐
  std::string src;
  for (int i = 1; i <= 10; ++i) src += "l" + std::to_string(i) + "\n";
  Diagnostic d = make_diag("E2010", Severity::Error, "msg", "a.vx", 10, 1);
  std::string e;
  e += "error[E2010]: msg\n";
  e += "  --> a.vx:10:1\n";
  e += "   |\n";
  e += "10 | l10\n";
  e += "   | ^\n";
  CHECK_EQ_STR(render_diagnostic(d, src, false), e);
}

void test_render_no_code_and_no_col() {
  const std::string src = "var x\n";
  Diagnostic d = make_diag("", Severity::Warning, "缺少初始化", "a.vx", 1, 0);
  std::string e;
  e += "warning: 缺少初始化\n";
  e += "  --> a.vx:1\n";
  e += "  |\n";
  e += "1 | var x\n";
  CHECK_EQ_STR(render_diagnostic(d, src, false), e);
}

void test_render_no_source() {
  Diagnostic d = make_diag("E2002", Severity::Error, "字符串未闭合", "a.vx", 3, 5);
  std::string e;
  e += "error[E2002]: 字符串未闭合\n";
  e += "  --> a.vx:3:5\n";
  CHECK_EQ_STR(render_diagnostic(d, "", false), e);
}

void test_render_source_without_file() {
  // 无文件名但行号有效：仍渲染源码行，但不输出 --> 位置行
  const std::string src = "abc\ndef\n";
  Diagnostic d = make_diag("", Severity::Info, "提示", "", 2, 2);
  std::string e;
  e += "info: 提示\n";
  e += "  |\n";
  e += "2 | def\n";
  e += "  |  ^\n";
  CHECK_EQ_STR(render_diagnostic(d, src, false), e);
}

void test_render_note() {
  const std::string src = "var x = @\n";
  Diagnostic d = make_diag("E2001", Severity::Error, "非法字符", "a.vx", 1, 9);
  d.note = "字符 @ 不属于 VX 词法";
  std::string e;
  e += "error[E2001]: 非法字符\n";
  e += "  --> a.vx:1:9\n";
  e += "  |\n";
  e += "1 | var x = @\n";
  e += "  | " + std::string(8, ' ') + "^\n";
  e += "  = note: 字符 @ 不属于 VX 词法\n";
  CHECK_EQ_STR(render_diagnostic(d, src, false), e);
}

void test_render_color() {
  const std::string src = "var x = @\n";
  Diagnostic d = make_diag("E2001", Severity::Error, "非法字符", "a.vx", 1, 9);
  const std::string plain = render_diagnostic(d, src, false);
  const std::string colored = render_diagnostic(d, src, true);
  CHECK(plain.find("\x1b[") == std::string::npos);
  CHECK(colored.find("\x1b[31m") != std::string::npos); // error 红
  CHECK(colored.find("\x1b[0m") != std::string::npos);  // reset

  Diagnostic w = make_diag("", Severity::Warning, "w", "a.vx", 1, 1);
  CHECK(render_diagnostic(w, src, true).find("\x1b[33m") != std::string::npos);
  Diagnostic i = make_diag("", Severity::Info, "i", "a.vx", 1, 1);
  CHECK(render_diagnostic(i, src, true).find("\x1b[36m") != std::string::npos);
}

void test_render_all_sorts_and_summarizes() {
  const std::string src = "var a = 1\nvar x = @\n";
  DiagBag bag;
  // 逆序添加：render_all 内部排序，且不改动原 bag 的顺序
  bag.add(make_diag(diag_code::lex_unknown_char, Severity::Error, "第二个错误",
                    "foo.vx", 2, 9));
  bag.add(make_diag(diag_code::lex_unknown_char, Severity::Error, "第一个错误",
                    "foo.vx", 2, 5));
  bag.add(make_diag("", Severity::Warning, "一条警告", "foo.vx", 1, 1));

  const std::string out = render_all(bag, src, false);
  // 原 bag 顺序未被改动
  CHECK(bag.items()[0].message == "第二个错误");
  CHECK(bag.items()[1].message == "第一个错误");

  // 输出按位置排序：line1col1(warning) → line2col5 → line2col9
  const std::size_t w = out.find("warning: 一条警告");
  const std::size_t e1 = out.find("第一个错误");
  const std::size_t e2 = out.find("第二个错误");
  CHECK(w != std::string::npos);
  CHECK(w < e1 && e1 < e2);

  // 末尾汇总行
  CHECK(out.find("error: 因 2 个错误中止编译（另有 1 个警告）") !=
        std::string::npos);
}

void test_render_all_warning_only() {
  const std::string src = "var x\n";
  DiagBag bag;
  bag.add(make_diag("", Severity::Warning, "w1", "a.vx", 1, 1));
  bag.add(make_diag("", Severity::Warning, "w2", "a.vx", 1, 1));
  const std::string out = render_all(bag, src, false);
  CHECK(out.find("warning: 2 个警告") != std::string::npos);
  CHECK(out.find("中止编译") == std::string::npos);
}

void test_render_all_no_summary_when_clean() {
  const std::string src = "var x\n";
  DiagBag bag;
  bag.add(make_diag("", Severity::Info, "提示", "a.vx", 1, 1));
  const std::string out = render_all(bag, src, false);
  CHECK(out.find("info: 提示") != std::string::npos);
  CHECK(out.find("中止编译") == std::string::npos);
  CHECK(out.find("个警告") == std::string::npos);
}

void test_render_span_multiline() {
  // P1-5：跨行 span 退化——高亮到起始行的行尾，而非只画 1 个 ^
  const std::string src = "abcdef\nghij\n";
  Diagnostic d =
      make_diag_span("E2010", Severity::Error, "跨行", "a.vx", 1, 3, 2, 2);
  std::string e;
  e += "error[E2010]: 跨行\n";
  e += "  --> a.vx:1:3\n";
  e += "  |\n";
  e += "1 | abcdef\n";
  e += "  | " + std::string(2, ' ') + "^^^^\n"; // col3 → 行尾 = 4 列
  CHECK_EQ_STR(render_diagnostic(d, src, false), e);
}

void test_render_caret_clamped_to_line_end() {
  // P1-6：col 越出行尾时，起始缩进被夹到行尾，插入符不再浮到远处
  const std::string src = "ab\n";
  Diagnostic d = make_diag("E2001", Severity::Error, "越界", "a.vx", 1, 99);
  std::string e;
  e += "error[E2001]: 越界\n";
  e += "  --> a.vx:1:99\n";
  e += "  |\n";
  e += "1 | ab\n";
  e += "  | " + std::string(2, ' ') + "^\n";
  CHECK_EQ_STR(render_diagnostic(d, src, false), e);
}

void test_render_span_clamped_to_line_end() {
  // P1-6：同行 span 的 end_col 越出行尾时，caret 被夹断到行尾
  const std::string src = "ab\n";
  Diagnostic d =
      make_diag_span("E2010", Severity::Error, "越界span", "a.vx", 1, 1, 1, 99);
  std::string e;
  e += "error[E2010]: 越界span\n";
  e += "  --> a.vx:1:1\n";
  e += "  |\n";
  e += "1 | ab\n";
  e += "  | ^^\n";
  CHECK_EQ_STR(render_diagnostic(d, src, false), e);
}

} // namespace

int main() {
  test_severity();
  test_constructors();
  test_bag();
  test_sort();
  test_render_basic();
  test_render_span();
  test_render_gutter_width();
  test_render_no_code_and_no_col();
  test_render_no_source();
  test_render_source_without_file();
  test_render_note();
  test_render_color();
  test_render_all_sorts_and_summarizes();
  test_render_all_warning_only();
  test_render_all_no_summary_when_clean();
  test_render_span_multiline();
  test_render_caret_clamped_to_line_end();
  test_render_span_clamped_to_line_end();

  if (g_failures == 0) {
    std::fprintf(stdout, "diag_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "diag_test: %d check(s) failed\n", g_failures);
  return 1;
}