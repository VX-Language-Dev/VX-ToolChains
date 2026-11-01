// verify.hpp — AST 结构指纹：用于校验格式化前后语义结构未被改写。
//
// 指纹只反映语法结构与字面量，忽略空白与注释（注释由格式化器单独处理）。
// 格式化前后指纹一致，即 AST 结构等价。
#pragma once

#include "vx/compiler/ast.hpp"

#include <span>
#include <string>

namespace vfmt {

// 生成一组顶层语句的 AST 结构指纹。
std::string ast_fingerprint(std::span<const vx::compiler::Stmt* const> stmts);

} // namespace vfmt
