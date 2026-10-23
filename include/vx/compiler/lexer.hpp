// lexer.hpp — VX 源码词法分析器：.vx 源码 → Token 流。
// 词法规格与自举编译器 compiler/lexer.vx 保持一致：
//   - 缩进敏感（Python 风格 INDENT/DEDENT），空格计 1、制表符计 4
//   - 括号/花括号/方括号内换行不产生 NL，也不触发缩进处理
//   - 注释以 # 起始至行尾；字符串支持 "..." 与 '...' 及转义序列
//   - ; 产生 Semi token（括号内忽略），由 Parser 作为单行语句分隔符处理
#pragma once

#include "vx/compiler/diag.hpp"
#include "vx/util/arena.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace vx::compiler {

// Token 种类（语义对齐自举编译器 T_* 常量）
enum class Tok : std::uint8_t {
  Eof,     // 流结束（始终存在）
  Nl,      // 逻辑换行
  Semi,    // ; 单行多语句分隔符
  Indent,  // 缩进加深
  Dedent,  // 缩进回退（可连续多个）
  Ident,   // 标识符
  Int,     // 整数字面量
  Float,   // 浮点字面量
  Str,     // 字符串字面量（text 为解码后的值）
  Kw,      // 关键字（text 为关键字名）
  Plus,    // +
  Minus,   // -
  Star,    // *
  Slash,   // /
  Percent, // %
  Power,   // ^
  Eq,      // ==
  Ne,      // !=
  Lt,      // <
  Gt,      // >
  Le,      // <=
  Ge,      // >=
  Assign,  // =
  PlusEq,  // +=
  MinusEq, // -=
  StarEq,  // *=
  SlashEq, // /=
  Arrow,   // ->
  And,     // &&（等价关键字 and）
  Or,      // ||（等价关键字 or）
  Not,     // !（等价关键字 not）
  LParen,  // (
  RParen,  // )
  LBracket,// [
  RBracket,// ]
  LBrace,  // {
  RBrace,  // }
  Colon,   // :
  Comma,   // ,
  Dot,     // .
  At,      // @（导入注解前缀：@imp / @cppimp）
};

// 判断单词是否为 VX 关键字
bool is_keyword(std::string_view word);

// Token 种类名（诊断输出用）
std::string_view tok_name(Tok kind);

struct Token {
  Tok kind;
  std::string_view text; // 词素：字面量/运算符原文；结构与标点 token 为空
  int line;              // 1 起始行号
  int col;               // 1 起始列号（按 Unicode 码点计，制表符算 1 列）。
                         // 结构性 token（Indent/Dedent）恒定位到逻辑行首 col=1。
};

// 词法分析：返回以 Tok::Eof 结尾的 token 流。
// 采用 fail-soft 策略：遇到非法字符/未闭合字符串时推入 bag 并做最小恢复
// （跳过 1 个字符 / 跳到文件结束），继续产出 token，不中断整个流程。
// 返回值恒非空（至少含 Eof）；调用方用 bag.has_error() 判定是否有词法错误。
// Token 数组存入 arena；字符串解码结果同样存入 arena（text 指向其中）。
// filename 仅用于诊断定位（写入 Diagnostic::file），可为空。
std::span<const Token> tokenize(std::string_view src, std::string_view filename,
                                vx::util::Arena &arena, DiagBag &bag);

} // namespace vx::compiler
