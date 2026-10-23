// codegen.hpp — 代码生成：类型化 AST → TypeIR 虚拟指令序列。
// 指令发射语义对齐自举 compiler/codegen.vx，并补齐其缺失的结构：
//   - 方法调用（Property 调用 → OP_CALL "m;<method>"，接收者已在栈上）
//   - 方法生成为 "<Class>.<method>" 具名函数，隐式 self 占首槽位
//   - 列表/字典字面量、下标读/写、属性赋值、复合赋值（+= 等）
// 已知简化（与自举一致，待类型推断后细化）：
//   - 二元运算一律发射 I32 族指令；and/or 不做短路
#pragma once

#include "vx/compiler/ast.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/importer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/compiler/typeir.hpp"

#include <optional>
#include <string_view>

namespace vx::compiler {

// 遍历整个程序生成模块 IR。
// FuncDecl → funcs；ExternDecl → linkage；@imp/@cppimp → 导入符号的 linkage
// 条目（VX ABI 符号名）与 ModuleIR::libs；ClassDecl/StructDecl 注册字段并为
// 每个方法生成具名函数；顶层 VarDecl 仅记录进 ModuleIR::global_vars。
// imports 为已解析的导入表（空则忽略 @imp/@cppimp，调用点按库内名解析）。
// 失败时把诊断推入 bag 并返回 std::nullopt（fail-fast）。
std::optional<ir::ModuleIR> generate_module(const Program &prog,
                                            std::string_view filename,
                                            DiagBag &bag,
                                            const ImportTable *imports = nullptr);

} // namespace vx::compiler
