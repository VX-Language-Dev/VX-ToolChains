// lexer_test — Compiler 层词法分析器测试（自带断言宏，不依赖第三方框架；
// MinSizeRel 下 NDEBUG 会禁用 assert，故不用 <cassert>）。
#include "vx/compiler/lexer.hpp"

#include "vx/util/util.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
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

using vx::compiler::Tok;
using vx::compiler::Token;
using vx::compiler::tokenize;

// 词法分析辅助：返回 token 流；出现词法错误时输出诊断并返回空。
// 用静态 arena 承载字符串解码结果，保证返回后 text 仍然有效。
std::vector<Token> lex(std::string_view src) {
  static vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = tokenize(src, "<test>", arena, bag);
  if (bag.has_error()) {
    std::fprintf(stderr, "unexpected lex error:\n%s",
                 vx::compiler::render_all(bag, src, false).c_str());
    ++g_failures;
    return {};
  }
  return {toks.begin(), toks.end()};
}

const Token *tok_at(const std::vector<Token> &toks, std::size_t i) {
  return i < toks.size() ? &toks[i] : nullptr;
}

#define CHECK_TOK(toks, i, kind_, text_)                                       \
  do {                                                                         \
    const Token *t = tok_at(toks, i);                                          \
    if (t == nullptr) {                                                        \
      std::fprintf(stderr, "FAIL %s:%d: token %zu 不存在\n", __FILE__,          \
                   __LINE__, static_cast<std::size_t>(i));                     \
      ++g_failures;                                                            \
    } else if (t->kind != (kind_) || t->text != (text_)) {                     \
      std::fprintf(stderr, "FAIL %s:%d: token[%zu] = %s(\"%s\"), 期望 %s(\"%s\")\n", \
                   __FILE__, __LINE__, static_cast<std::size_t>(i),            \
                   std::string(vx::compiler::tok_name(t->kind)).c_str(),       \
                   std::string(t->text).c_str(), #kind_, text_);               \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

// 断言 token 的行列位置（越界或行列不符时报告）
#define CHECK_POS(toks, i, expect_line, expect_col)                            \
  do {                                                                         \
    if ((i) >= (toks).size()) {                                                \
      std::fprintf(stderr, "FAIL %s:%d: token %zu 不存在\n", __FILE__,          \
                   __LINE__, static_cast<std::size_t>(i));                     \
      ++g_failures;                                                            \
    } else if ((toks)[i].line != (expect_line) ||                              \
               (toks)[i].col != (expect_col)) {                                \
      std::fprintf(stderr, "FAIL %s:%d: token[%zu] 位置 = %d:%d, 期望 %d:%d\n", \
                   __FILE__, __LINE__, static_cast<std::size_t>(i),            \
                   (toks)[i].line, (toks)[i].col, (expect_line), (expect_col));\
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

void test_basic() {
  auto toks = lex("var x = 1\n");
  CHECK_TOK(toks, 0, Tok::Kw, "var");
  CHECK_TOK(toks, 1, Tok::Ident, "x");
  CHECK_TOK(toks, 2, Tok::Assign, "=");
  CHECK_TOK(toks, 3, Tok::Int, "1");
  CHECK_TOK(toks, 4, Tok::Nl, "");
  CHECK_TOK(toks, 5, Tok::Eof, "");
  CHECK(toks.size() == 6);
  CHECK(toks[0].line == 1);
  CHECK(toks[3].line == 1);
}

void test_keywords_idents() {
  auto toks = lex("func main():\n    return 0\n");
  CHECK_TOK(toks, 0, Tok::Kw, "func");
  CHECK_TOK(toks, 1, Tok::Ident, "main");
  CHECK_TOK(toks, 2, Tok::LParen, "(");
  CHECK_TOK(toks, 3, Tok::RParen, ")");
  CHECK_TOK(toks, 4, Tok::Colon, ":");
  CHECK_TOK(toks, 5, Tok::Nl, "");
  CHECK_TOK(toks, 6, Tok::Indent, "");
  CHECK_TOK(toks, 7, Tok::Kw, "return");
  CHECK_TOK(toks, 8, Tok::Int, "0");
  CHECK_TOK(toks, 9, Tok::Nl, "");
  CHECK_TOK(toks, 10, Tok::Dedent, "");
  CHECK_TOK(toks, 11, Tok::Eof, "");

  // 关键字判定
  CHECK(vx::compiler::is_keyword("if"));
  CHECK(vx::compiler::is_keyword("move"));
  CHECK(!vx::compiler::is_keyword("ifx"));
  CHECK(!vx::compiler::is_keyword(""));

  // 标识符允许下划线与 UTF-8 字节
  auto id_toks = lex("_a1 世界\n");
  CHECK_TOK(id_toks, 0, Tok::Ident, "_a1");
  CHECK_TOK(id_toks, 1, Tok::Ident, "世界");
}

void test_indent_rules() {
  // 缩进加深与连续回退
  auto toks = lex("a = 1\nif a:\n    if a:\n        b = 2\n    c = 3\nd = 4\n");
  std::size_t indents = 0, dedents = 0;
  for (const auto &t : toks) {
    if (t.kind == Tok::Indent) {
      ++indents;
    }
    if (t.kind == Tok::Dedent) {
      ++dedents;
    }
  }
  CHECK(indents == 2);
  CHECK(dedents == 2);

  // 空行与整行注释不产生 INDENT/DEDENT
  auto blank = lex("if a:\n\n# 注释\n    b = 1\n");
  indents = dedents = 0;
  for (const auto &t : blank) {
    if (t.kind == Tok::Indent) {
      ++indents;
    }
    if (t.kind == Tok::Dedent) {
      ++dedents;
    }
  }
  CHECK(indents == 1);
  CHECK(dedents == 1);

  // 括号内换行不产生 NL，也不产生 INDENT
  auto cont = lex("f(1,\n  2)\n");
  for (std::size_t i = 0; i < cont.size(); ++i) {
    CHECK(cont[i].kind != Tok::Nl || i >= 6); // NL 只允许出现在 ')'/')' 之后
    CHECK(cont[i].kind != Tok::Indent);
  }
}

void test_operators_numbers() {
  auto toks = lex("a += 1 - 2 * 3 / 4 % 5 ^ 6\n"
                  "x = a == b != c <= d >= e < f > g\n"
                  "p = q and r or !s && t || u\n"
                  "h = a -> b\n");
  // 只验证双字符运算符切分正确
  bool saw_arrow = false, saw_and = false, saw_or = false, saw_plus_eq = false;
  for (const auto &t : toks) {
    if (t.kind == Tok::Arrow && t.text == "->") {
      saw_arrow = true;
    }
    if (t.kind == Tok::And && t.text == "and") {
      saw_and = true;
    }
    if (t.kind == Tok::Or && t.text == "or") {
      saw_or = true;
    }
    if (t.kind == Tok::PlusEq && t.text == "+=") {
      saw_plus_eq = true;
    }
  }
  CHECK(saw_arrow);
  CHECK(saw_and);
  CHECK(saw_or);
  CHECK(saw_plus_eq);

  auto nums = lex("1 1.5 1. .5\n");
  CHECK_TOK(nums, 0, Tok::Int, "1");
  CHECK_TOK(nums, 1, Tok::Float, "1.5");
  CHECK_TOK(nums, 2, Tok::Int, "1");
  CHECK_TOK(nums, 3, Tok::Dot, ".");
  CHECK_TOK(nums, 4, Tok::Dot, "."); // ".5" 不构成浮点（与自举实现一致）
  CHECK_TOK(nums, 5, Tok::Int, "5");
}

void test_strings_comments() {
  // 单行多语句分隔符 ; → Semi（不是 Nl，不触发缩进处理）
  auto semi = lex("a = 1; b = 2\n");
  CHECK_TOK(semi, 3, Tok::Semi, "");
  CHECK_TOK(semi, 4, Tok::Ident, "b");
  CHECK_TOK(semi, 7, Tok::Nl, "");

  auto toks = lex("s = \"a\\n\\t\\\"b\"\n");
  CHECK_TOK(toks, 0, Tok::Ident, "s");
  CHECK_TOK(toks, 1, Tok::Assign, "=");
  CHECK(toks.size() >= 4);
  if (toks.size() >= 4) {
    CHECK(toks[2].kind == Tok::Str);
    CHECK(toks[2].text == "a\n\t\"b");
  }

  auto single = lex("c = 'x\\'y'\n");
  if (single.size() >= 3) {
    CHECK(single[2].kind == Tok::Str);
    CHECK(single[2].text == "x'y");
  }

  auto comment = lex("a = 1 # 行尾注释 \"未闭合\nb = 2\n");
  CHECK_TOK(comment, 3, Tok::Nl, "");
  CHECK_TOK(comment, 4, Tok::Ident, "b");
}

void test_errors() {
  // 未闭合字符串（fail-soft：诊断入 bag，token 流仍以 Eof 收尾）
  {
    vx::util::Arena arena;
    vx::compiler::DiagBag bag;
    auto toks = tokenize("\"abc", "<test>", arena, bag);
    CHECK(bag.has_error());
    CHECK(toks.back().kind == Tok::Eof); // 流仍可用
    CHECK(bag.size() == 1);
    if (bag.size() == 1) {
      const auto &d = bag.items()[0];
      CHECK(d.code == "E2002");
      CHECK(d.severity == vx::compiler::Severity::Error);
      CHECK(d.file == "<test>");
      CHECK(d.line == 1);
      CHECK(d.col == 1);
      CHECK(d.end_line == 1);
      CHECK(d.end_col == 5); // span 覆盖 "abc 到 EOF
      CHECK(d.message == "字符串未闭合");
    }
  }
  // 无法识别的字符（恢复：跳过该字符，后续 token 仍产出）
  {
    vx::util::Arena arena;
    vx::compiler::DiagBag bag;
    auto toks = tokenize("a = 1\n$\nb = 2\n", "<test>", arena, bag);
    CHECK(bag.has_error());
    CHECK(bag.size() == 1);
    if (bag.size() == 1) {
      const auto &d = bag.items()[0];
      CHECK(d.code == "E2001");
      CHECK(d.line == 2);
      CHECK(d.col == 1);
      CHECK(d.end_line == 2);
      CHECK(d.end_col == 2); // 高亮该非法字符本身（1 列）
      CHECK(!d.message.empty());
    }
    // 恢复后仍能看到后继标识符 b（位于第 3 行）
    bool found_b = false;
    for (const auto &t : toks) {
      if (t.kind == Tok::Ident && t.text == "b") {
        found_b = true;
        CHECK(t.line == 3);
      }
    }
    CHECK(found_b);
  }
  // span 渲染联动：未闭合字符串的插入符覆盖到行尾（真实触发 P1-5 跨行退化）
  {
    vx::util::Arena arena;
    vx::compiler::DiagBag bag;
    const std::string_view src = "\"abc\n";
    (void)tokenize(src, "<test>", arena, bag);
    const std::string out = vx::compiler::render_all(bag, src, false);
    CHECK(out.find("1 | \"abc\n") != std::string::npos);
    CHECK(out.find("^^^^") != std::string::npos);
  }
}

// token 列号：普通 token 定位到词素首字符，Indent/Dedent 定位到行首
void test_token_columns() {
  auto toks = lex("var x = 1\n    y = 2\n");
  CHECK_TOK(toks, 0, Tok::Kw, "var");
  CHECK_POS(toks, 0, 1, 1);  // var
  CHECK_POS(toks, 1, 1, 5);  // x
  CHECK_POS(toks, 2, 1, 7);  // =
  CHECK_POS(toks, 3, 1, 9);  // 1
  CHECK_POS(toks, 4, 1, 10); // Nl（行尾换行符所在列）
  CHECK_POS(toks, 5, 2, 1);  // Indent（结构性 token → 行首）
  CHECK_POS(toks, 6, 2, 5);  // y（缩进 4 空格后）
  CHECK_POS(toks, 7, 2, 7);  // =
  CHECK_POS(toks, 8, 2, 9);  // 2
  CHECK_POS(toks, 9, 2, 10); // Nl
  CHECK_POS(toks, 10, 3, 1); // Dedent
  CHECK_POS(toks, 11, 3, 1); // Eof
}

// 列按 Unicode 码点计：UTF-8 标识符的后续字节不推进列号
void test_token_columns_utf8() {
  auto toks = lex("var 变量 = 1\n");
  CHECK_TOK(toks, 0, Tok::Kw, "var");
  CHECK_POS(toks, 0, 1, 1);  // var
  CHECK_POS(toks, 1, 1, 5);  // 变量（6 字节但占 2 列）
  CHECK_POS(toks, 2, 1, 8);  // =
  CHECK_POS(toks, 3, 1, 10); // 1
  CHECK_POS(toks, 4, 1, 11); // Nl
}

// 跨行字符串：换行同时推进行号（回归：不可重复计数）
void test_multiline_string_positions() {
  auto toks = lex("s = \"a\nb\"\nx = 1\n");
  CHECK_TOK(toks, 0, Tok::Ident, "s");
  CHECK_POS(toks, 0, 1, 1);
  CHECK_POS(toks, 1, 1, 3); // =
  CHECK_TOK(toks, 2, Tok::Str, "a\nb");
  CHECK_POS(toks, 2, 1, 5); // 起始引号
  CHECK_POS(toks, 3, 2, 3); // 字符串后的 Nl（第 2 行）
  CHECK_TOK(toks, 4, Tok::Ident, "x");
  CHECK_POS(toks, 4, 3, 1); // x 位于第 3 行（而非漂移到第 4 行）
  CHECK_POS(toks, 6, 3, 5); // 1
  CHECK_POS(toks, 7, 3, 6); // Nl
}

// 导入注解前缀 '@'：应为独立 At token（供 @imp / @cppimp 使用）
void test_at_directive() {
  auto toks = lex("@imp(math)\n");
  CHECK_TOK(toks, 0, Tok::At, "@");
  CHECK_TOK(toks, 1, Tok::Ident, "imp");
  CHECK_TOK(toks, 2, Tok::LParen, "(");
  CHECK_TOK(toks, 3, Tok::Ident, "math");
  CHECK_TOK(toks, 4, Tok::RParen, ")");
  CHECK_TOK(toks, 5, Tok::Nl, "");
  CHECK_TOK(toks, 6, Tok::Eof, "");
  CHECK_POS(toks, 0, 1, 1); // '@' 位于行首第 1 列

  // 子目录库引用（路径片段）与字符串形式引用
  auto path = lex("@cppimp(cpp/greet.hpp)\n");
  CHECK_TOK(path, 0, Tok::At, "@");
  CHECK_TOK(path, 1, Tok::Ident, "cppimp");
  CHECK_TOK(path, 2, Tok::LParen, "(");
  CHECK_TOK(path, 3, Tok::Ident, "cpp");
  CHECK_TOK(path, 4, Tok::Slash, "/");
  CHECK_TOK(path, 5, Tok::Ident, "greet");
  CHECK_TOK(path, 6, Tok::Dot, ".");
  CHECK_TOK(path, 7, Tok::Ident, "hpp");
  CHECK_TOK(path, 8, Tok::RParen, ")");
}

// 用仓库内全部真实 VX 源码驱动词法分析器
void test_vx_sources() {
  namespace fs = std::filesystem;

  fs::path root = fs::current_path();
  while (!root.empty() && !fs::exists(root / "compiler" / "lexer.vx")) {
    root = root.parent_path();
  }
  CHECK(!root.empty());
  if (root.empty()) {
    return;
  }

  std::size_t total_files = 0;
  std::size_t total_tokens = 0;
  for (const char *dir : {"compiler", "std", "tests"}) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root / dir, ec), end; it != end;
         it.increment(ec)) {
      if (ec) {
        break;
      }
      if (!it->is_regular_file() || it->path().extension() != ".vx") {
        continue;
      }
      // 跳过旧反编译器输出（旧语法，含分号），不属于当前语法的源码
      if (it->path().filename().string().find(".decompiled.vx") !=
          std::string::npos) {
        continue;
      }
      auto src = vx::util::read_file(it->path());
      CHECK(src.has_value());
      if (!src) {
        continue;
      }

      vx::util::Arena arena;
      vx::compiler::DiagBag bag;
      auto toks = tokenize(*src, it->path().string(), arena, bag);
      CHECK(!bag.has_error());
      if (bag.has_error()) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n%s\n", __FILE__, __LINE__,
                     it->path().string().c_str(),
                     vx::compiler::render_all(bag, *src, false).c_str());
        ++g_failures;
        continue;
      }
      CHECK(!toks.empty());
      if (toks.empty()) {
        continue;
      }
      // 流以 Eof 结尾，行号单调不减
      CHECK(toks.back().kind == Tok::Eof);
      int last_line = 1;
      for (const auto &t : toks) {
        CHECK(t.line >= last_line);
        last_line = t.line;
      }
      total_tokens += toks.size();
      ++total_files;
    }
  }
  CHECK(total_files >= 10);
  CHECK(total_tokens > 1000);

  // 抽查 compiler/lexer.vx 首个 token：行首注释应被跳过，import 为关键字
  auto src = vx::util::read_file(root / "compiler" / "lexer.vx");
  CHECK(src.has_value());
  if (src) {
    auto toks = lex(*src);
    CHECK_TOK(toks, 0, Tok::Kw, "import");
    CHECK_TOK(toks, 1, Tok::Ident, "std");
    CHECK_TOK(toks, 2, Tok::Dot, ".");
    CHECK_TOK(toks, 3, Tok::Ident, "error");
    CHECK_TOK(toks, 4, Tok::Nl, "");
  }
}

} // namespace

int main() {
  test_basic();
  test_keywords_idents();
  test_indent_rules();
  test_operators_numbers();
  test_strings_comments();
  test_errors();
  test_token_columns();
  test_token_columns_utf8();
  test_multiline_string_positions();
  test_at_directive();
  test_vx_sources();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d 项检查失败\n", g_failures);
    return 1;
  }
  std::puts("Lexer 测试全部通过");
  return 0;
}
