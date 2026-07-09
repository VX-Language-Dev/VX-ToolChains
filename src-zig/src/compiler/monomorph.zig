const std = @import("std");
const Expr = @import("../parser/ast.zig").Expr;

/// AST 级泛型单态化（placeholder）。
/// 返回的新 AST 中，所有带类型参数的 struct/class/func 模板都会被替换为
/// 实际使用到的具体变体；未使用的模板会被移除。
/// 当前实现为占位符，直接返回输入的 AST。
pub fn monomorphizeAst(ast: std.ArrayList(*Expr), allocator: std.mem.Allocator) std.ArrayList(*Expr) {
    _ = allocator;
    return ast; // placeholder
}
