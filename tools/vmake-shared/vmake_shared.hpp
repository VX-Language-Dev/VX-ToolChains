// vmake_shared.hpp — VMakeSet.vmake 共享词法与规则（vmake / vmake-lsp 复用）。
// 语法（Python 风格函数调用）：
//   exe("hello", sources=["src/main.vx", "src/util.vx"])
//   dylib("mylib", sources=["lib.vx"], output="libmylib.dylib"])
// 支持注释、尾随逗号、括号内换行、\uXXXX 转义。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vmake_shared {

// ---- 词法 ----
enum class TokType {
  Ident,
  Num,
  Str,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Comma,
  Equals,
  Newline,
  End,
  Bad, // 非法字符（如未闭合字符串）
};

// 词法单元：字符串同时保留原始文本与解码后的值
struct Token {
  TokType type = TokType::End;
  std::string text;  // 原始源文本（字符串含引号）
  std::string value; // 字符串：未转义值；标识符/数字：同 text
  int line = 0;      // 0 起始行号
  int column = 0;    // 0 起始列（行内字节偏移）
  int end_line = 0;
  int end_column = 0; // 不含
};

// 词法分析：注释、字符串（\n \t \r \" \\ \uXXXX）、标识符、数字与分隔符。
// 字符串解码为 UTF-8（\uXXXX 超 ASCII 时按码点编码）。
std::vector<Token> lex(std::string_view text);

// ---- 共享校验规则 ----
// 目标名仅允许字母、数字、下划线与连字符。
bool valid_target_name(std::string_view name);

// 绝对路径判定：POSIX 的 `/` 开头，以及 Windows 盘符前缀（如 `C:\`）。
bool is_absolute_path(std::string_view p);

// 路径分量中是否含 `..`（按 `/` 与 `\` 切分）。
bool has_dotdot_component(std::string_view p);

// ---- 源文件分类（.vx → vxc；.c → clang -c；.cpp/.cc/.cxx → clang -c C++）----
bool is_vx_source(std::string_view s);
bool is_c_source(std::string_view s);
bool is_cxx_source(std::string_view s);

} // namespace vmake_shared
