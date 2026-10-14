// parser.hpp — VX 语法分析器：Token 流 → AST。
// 递归下降 + 运算符优先级爬升，语法覆盖对齐自举 compiler/parser.vx，
// 并补齐真实源码需要而自举缺失的结构：列表/字典字面量、下标、
// 属性/下标赋值目标、复合赋值、class/struct/extern/export、单行多语句。
#pragma once

#include "vx/compiler/ast.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/util/arena.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace vx::compiler {

struct ParseError {
  std::string message;
  int line;
};

struct Program {
  std::span<const Stmt *const> stmts;
};

// 解析整个程序；AST 节点全部在 arena 上分配，生命周期随 arena。
std::expected<Program, ParseError> parse(std::span<const Token> tokens,
                                         vx::util::Arena &arena);

} // namespace vx::compiler
