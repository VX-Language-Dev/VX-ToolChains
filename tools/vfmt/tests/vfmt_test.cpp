// vfmt_test — vfmt 格式化器测试（自带断言宏，对齐主仓测试风格）。
#include "format.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                             \
      ++g_failures;                                                                                                    \
    }                                                                                                                  \
  } while (0)

// 格式化并断言成功；失败时打印错误并返回空串。
std::string fmt(std::string_view src, const vfmt::Options& opts = {}) {
  const auto res = vfmt::format(src, opts);
  if (!res) {
    std::fprintf(stderr, "  format failed: %s\n", res.error().c_str());
    return {};
  }
  return res->text;
}

// 断言格式化结果等于期望。
void expect_eq(std::string_view src, std::string_view expected, const vfmt::Options& opts = {}) {
  const std::string got = fmt(src, opts);
  if (got != expected) {
    std::fprintf(stderr, "FAIL input:\n%.*s\n  expected:\n%.*s\n  got:\n%s\n", static_cast<int>(src.size()), src.data(),
                 static_cast<int>(expected.size()), expected.data(), got.c_str());
    ++g_failures;
  }
}

// ---- 用例 ----

// 基本缩进规范化：统一为 4 空格，制表符转空格
void test_indent() {
  expect_eq("func f():\n  return 1\n", "func f():\n    return 1\n");
  expect_eq("func f():\n\treturn 1\n", "func f():\n    return 1\n");
  expect_eq("func f():\n      return 1\n", "func f():\n    return 1\n");
}

// 嵌套块的缩进层级（用制表符缩进的合法嵌套）
void test_nested_indent() {
  expect_eq("func f():\n\tif x:\n\t\treturn 1\n", "func f():\n    if x:\n        return 1\n");
}

// 自定义缩进宽度
void test_indent_width() {
  vfmt::Options opts;
  opts.indent_width = 2;
  expect_eq("func f():\n    return 1\n", "func f():\n  return 1\n", opts);
}

// 二元运算符两侧留空格
void test_binary_operator_spacing() {
  expect_eq("var x = a+b\n", "var x = a + b\n");
  expect_eq("var x = a==b\n", "var x = a == b\n");
  expect_eq("var x = a+b*c\n", "var x = a + b * c\n");
}

// 一元运算符与操作数紧贴
void test_unary_spacing() {
  expect_eq("var x = -a\n", "var x = -a\n");
  expect_eq("var x = -  a\n", "var x = -a\n");
  expect_eq("var x = not  a\n", "var x = not a\n");
  expect_eq("var x = !a\n", "var x = !a\n");
  expect_eq("f(-1)\n", "f(-1)\n");
  expect_eq("var x = a * -b\n", "var x = a * -b\n");
}

// 括号、逗号、下标内部不留空格
void test_call_and_subscript_spacing() {
  expect_eq("var x = f( a , b )\n", "var x = f(a, b)\n");
  expect_eq("var x = a [ 0 ]\n", "var x = a[0]\n");
  expect_eq("var x = a . b\n", "var x = a.b\n");
}

// 类型注解的冒号：前不留空格，后留一个空格
void test_colon_spacing() {
  expect_eq("var x : int = 1\n", "var x: int = 1\n");
  expect_eq("func f(a : int) -> int:\n    return a\n", "func f(a: int) -> int:\n    return a\n");
}

// 控制流关键字后的括号保留空格，函数调用不留
void test_control_keyword_paren() {
  expect_eq("if (x):\n    return 1\n", "if (x):\n    return 1\n");
  expect_eq("while (x):\n    return 1\n", "while (x):\n    return 1\n");
  expect_eq("var x = f ( 1 )\n", "var x = f(1)\n");
}

// const 关键字必须保留（AST 的 VarDecl 不区分 const 与 var）
void test_const_preserved() {
  expect_eq("const PI = 3\n", "const PI = 3\n");
  expect_eq("mut  x = 1\n", "mut x = 1\n");
}

// import 的 as 别名必须保留（AST 的 Import 节点不含 alias）
void test_import_alias_preserved() {
  expect_eq("import std.mem.allocator as alloc\n", "import std.mem.allocator as alloc\n");
  expect_eq("@imp(collections/vec) as v\n", "@imp(collections/vec) as v\n");
}

// @imp 库引用路径紧凑书写
void test_import_path_compact() {
  expect_eq("@imp( collections / vec )\n", "@imp(collections/vec)\n");
  expect_eq("@cppimp( cpp / greet.hpp )\n", "@cppimp(cpp/greet.hpp)\n");
}

// 字符串转义往返：解码后再编码须保持等价
void test_string_escape_roundtrip() {
  expect_eq("var s = \"a\\nb\"\n", "var s = \"a\\nb\"\n");
  expect_eq("var s = \"say \\\"hi\\\"\"\n", "var s = \"say \\\"hi\\\"\"\n");
  expect_eq("var s = 'single'\n", "var s = \"single\"\n");
  expect_eq("var s = \"tab\\there\"\n", "var s = \"tab\\there\"\n");
}

// 独立注释行与行尾注释均须保留
void test_comments_preserved() {
  expect_eq("# top comment\nfunc f():\n    # inner comment\n    return 1\n",
            "# top comment\nfunc f():\n    # inner comment\n    return 1\n");
  expect_eq("func f():\n    return 1  # trailing\n", "func f():\n    return 1  # trailing\n");
}

// 空行：顶层最多保留 2 个，块内最多 1 个
void test_blank_lines() {
  expect_eq("func a():\n    return 1\n\n\n\n\nfunc b():\n    return 2\n",
            "func a():\n    return 1\n\n\nfunc b():\n    return 2\n");
  expect_eq("func f():\n    x = 1\n\n\n\n    y = 2\n", "func f():\n    x = 1\n\n    y = 2\n");
}

// 文件开头与结尾的空行被裁剪
void test_leading_trailing_blanks() {
  expect_eq("\n\nfunc f():\n    return 1\n\n\n", "func f():\n    return 1\n");
}

// 同一物理行的多条语句拆分为多行
void test_semicolon_split() {
  expect_eq("func f():\n    a(); b()\n", "func f():\n    a()\n    b()\n");
}

// 块构造的同行体不得拆分（否则语句会脱离所属块）
void test_inline_block_body_kept() {
  expect_eq("if x: a(); b()\n", "if x: a(); b()\n");
  expect_eq("func f(): return 1\n", "func f(): return 1\n");
}

// 幂等性：格式化结果再次格式化应保持不变
void test_idempotent() {
  const std::string src = "# header\nfunc f(a: int, b: int) -> int:\n"
                          "    var x = a+b\n"
                          "    if x > 0:\n"
                          "        return x\n"
                          "    return 0\n";
  const std::string once = fmt(src);
  CHECK(!once.empty());
  const std::string twice = fmt(once);
  CHECK(!twice.empty());
  if (once != twice) {
    std::fprintf(stderr, "FAIL not idempotent:\n%s\n---\n%s\n", once.c_str(), twice.c_str());
    ++g_failures;
  }
}

// 格式化不得改变语义结构（AST 指纹自检通过即等价）
void test_structure_preserved() {
  const std::string src = "@imp(math)\n"
                          "func main() -> int:\n"
                          "    var x = math.add(1,2)\n"
                          "    var s = \"hi\"\n"
                          "    if x == 3 and not s == \"\":\n"
                          "        out(s)\n"
                          "    return x\n";
  const std::string got = fmt(src);
  CHECK(!got.empty());
  if (got.empty()) {
    return;
  }
  // 关键语义 token 必须仍在结果中
  CHECK(got.find("@imp(math)") != std::string::npos);
  CHECK(got.find("math.add(1, 2)") != std::string::npos);
  CHECK(got.find("and not") != std::string::npos);
}

// 语法错误应返回失败而非产出结果
void test_syntax_error_rejected() {
  const auto res = vfmt::format("func f(:\n", {});
  CHECK(!res.has_value());
}

// 词法错误应返回失败
void test_lex_error_rejected() {
  const auto res = vfmt::format("var x = @\n", {});
  CHECK(!res.has_value());
}

// class / struct / 成员访问的规范化
void test_class_and_struct() {
  expect_eq("class P:\n  x: int\n  y: int\n", "class P:\n    x: int\n    y: int\n");
  expect_eq("struct S:\n  a: int\n", "struct S:\n    a: int\n");
}

// 字典与列表字面量的空格规范化
void test_literals() {
  expect_eq("var d = {\"a\" : 1}\n", "var d = {\"a\": 1}\n");
  expect_eq("var l = [ 1 , 2 ]\n", "var l = [1, 2]\n");
}

// 空文件与仅含空行的文件：产出空结果
void test_empty_file() {
  expect_eq("", "");
  expect_eq("\n\n\n", "");
}

// 仅含注释的文件：注释须保留
void test_comment_only_file() {
  expect_eq("# just a comment\n", "# just a comment\n");
}

// 文件末尾缺少换行：补齐换行
void test_missing_final_newline() {
  expect_eq("func f():\n    return 1", "func f():\n    return 1\n");
}

// 字符串内的 '#' 不是注释
void test_hash_inside_string() {
  expect_eq("var s = \"hello # world\"\n", "var s = \"hello # world\"\n");
}

// 跨行字符串：解码后重新编码为转义形式，语义等价
void test_multiline_string() {
  expect_eq("var s = \"line1\nline2\"\n", "var s = \"line1\\nline2\"\n");
}

// CRLF 源文件：统一为 LF
void test_crlf() {
  expect_eq("func f():\r\n    return 1\r\n", "func f():\n    return 1\n");
}

// 深层嵌套的缩进层级
void test_deep_nesting() {
  expect_eq("func f():\n  if a:\n    if b:\n      return 1\n",
            "func f():\n    if a:\n        if b:\n            return 1\n");
}

// 多行字面量的换行结构须保留（不被折叠为单行）
void test_multiline_literal_preserved() {
  expect_eq("var d = {\n  \"a\": 1,\n  \"b\": 2,\n}\n", "var d = {\n    \"a\": 1,\n    \"b\": 2,\n}\n");
}

// 括号内续行的缩进：元素缩进一级，右闭合符与前缀对齐
void test_continuation_indent() {
  expect_eq("func f():\n  var x = g({\n    \"a\": 1,\n  })\n",
            "func f():\n    var x = g({\n        \"a\": 1,\n    })\n");
}

// 注释在多行字面量内部：须保留
void test_comment_inside_literal() {
  expect_eq("var d = {\n  # 说明\n  \"a\": 1,\n}\n", "var d = {\n    # 说明\n    \"a\": 1,\n}\n");
}

} // namespace

int main() {
  test_indent();
  test_nested_indent();
  test_indent_width();
  test_binary_operator_spacing();
  test_unary_spacing();
  test_call_and_subscript_spacing();
  test_colon_spacing();
  test_control_keyword_paren();
  test_const_preserved();
  test_import_alias_preserved();
  test_import_path_compact();
  test_string_escape_roundtrip();
  test_comments_preserved();
  test_blank_lines();
  test_leading_trailing_blanks();
  test_semicolon_split();
  test_inline_block_body_kept();
  test_idempotent();
  test_structure_preserved();
  test_syntax_error_rejected();
  test_lex_error_rejected();
  test_class_and_struct();
  test_literals();
  test_empty_file();
  test_comment_only_file();
  test_missing_final_newline();
  test_hash_inside_string();
  test_multiline_string();
  test_crlf();
  test_deep_nesting();
  test_multiline_literal_preserved();
  test_continuation_indent();
  test_comment_inside_literal();

  if (g_failures != 0) {
    std::fprintf(stderr, "\n%d check(s) failed\n", g_failures);
    return 1;
  }
  std::fprintf(stdout, "all vfmt tests passed\n");
  return 0;
}
