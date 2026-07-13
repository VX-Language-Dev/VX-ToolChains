const std = @import("std");
const Allocator = std.mem.Allocator;

const Stmt = @import("../parser/ast.zig").Stmt;
const Expr = @import("../parser/ast.zig").Expr;
const Token = @import("../token.zig").Token;
const TokenType = @import("../token.zig").TokenType;

pub const ScopeDepth = struct {
    value: usize,

    pub fn init(value: usize) ScopeDepth {
        return ScopeDepth{ .value = value };
    }
};

pub const SymbolInfo = struct {
    name: []u8,
    kind: SymbolKind,
    detail: ?[]u8,
};

pub const SymbolKind = enum {
    Function,
    Variable,
    Struct,
    Class,
    Enum,
    Union,
    TypeAlias,
    LoopVar,
    Param,
};

pub const ScopedSymbolInfo = struct {
    info: SymbolInfo,
    depth: ScopeDepth,
};

pub const CompletionItem = struct {
    label: []u8,
    kind: CompletionItemKind,
    detail: ?[]u8,
    documentation: ?[]u8,
};

pub const CompletionItemKind = enum(u8) {
    Text = 1,
    Method = 2,
    Function = 3,
    Constructor = 4,
    Field = 5,
    Variable = 6,
    Class = 7,
    Interface = 8,
    Module = 9,
    Property = 10,
    Unit = 11,
    Value = 12,
    Enum = 13,
    Keyword = 14,
    Snippet = 15,
    Color = 16,
    File = 17,
    Reference = 18,
    Folder = 19,
    EnumMember = 20,
    Constant = 21,
    Struct = 22,
    Event = 23,
    Operator = 24,
    TypeParameter = 25,
};

pub fn collectSymbols(allocator: Allocator, ast: []const Stmt) ![]SymbolInfo {
    var symbols = std.ArrayList(SymbolInfo).init(allocator);
    defer symbols.deinit();

    for (ast) |stmt| {
        try collectFromExpr(stmt, &symbols, allocator);
    }

    return symbols.toOwnedSlice();
}

fn collectFromExpr(expr: *const Stmt, symbols: *std.ArrayList(SymbolInfo), allocator: Allocator) !void {
    // 实现符号收集逻辑
    _ = expr;
    _ = symbols;
    _ = allocator;
}

pub fn formatFuncSignature(
    allocator: Allocator,
    name: []const u8,
    params: []const struct { []u8, []u8 },
    ret_type: ?[]const u8,
) ![]u8 {
    var result = std.ArrayList(u8).init(allocator);
    defer result.deinit();

    result.appendSlice("func ") catch {};
    result.appendSlice(name) catch {};
    result.append('(') catch {};

    for (params, 0..) |param, i| {
        if (i > 0) {
            result.appendSlice(", ") catch {};
        }
        result.appendSlice(param[0]) catch {};
        result.appendSlice(": ") catch {};
        result.appendSlice(param[1]) catch {};
    }

    result.append(')') catch {};

    if (ret_type) |ret| {
        result.appendSlice(" -> ") catch {};
        result.appendSlice(ret) catch {};
    }

    return result.toOwnedSlice();
}
