const std = @import("std");
const Allocator = std.mem.Allocator;

const Stmt = @import("../parser/ast.zig").Stmt;
const Expr = @import("../parser/ast.zig").Expr;

pub const GotoDefinitionResponse = struct {
    uri: []u8,
    range: Range,
};

pub const Range = struct {
    start: Position,
    end: Position,
};

pub const Position = struct {
    line: u32,
    character: u32,
};

pub fn gotoDefinition(
    allocator: Allocator,
    ast: []const Stmt,
    source: []const u8,
    position: Position,
    uri: []const u8,
) !?GotoDefinitionResponse {
    const line = position.line + 1;
    const col = position.character + 1;

    // 查找标识符名称
    const name = identifierNameAt(source, line, col) orelse return null;

    // 首先找到光标所在的函数
    if (try findEnclosingFunction(allocator, ast, line)) |enclosing| {
        // 在该函数内搜索定义
        if (try findLocalDefinition(enclosing, &name, line)) |loc| {
            return locationToResponse(allocator, uri, loc, &name);
        }
    }

    // 全局搜索
    var defs = std.ArrayList([2]usize).init(allocator);
    defer defs.deinit();

    try collectDefinitions(ast, &name, &defs);

    if (defs.items.len > 0) {
        const def = defs.items[0];
        return locationToResponse(allocator, uri, [2]usize{ def[0], def[1] }, &name);
    }

    return null;
}

fn identifierNameAt(source: []const u8, line: usize, col: usize) ?[]u8 {
    var lines = std.mem.split(u8, source, "\n");
    var current_line: usize = 0;

    while (lines.next()) |l| : (current_line += 1) {
        if (current_line + 1 == line) {
            if (col <= l.len) {
                var start = col;
                while (start > 0 and isIdentChar(l[start - 1])) : (start -= 1) {}
                var end = col;
                while (end < l.len and isIdentChar(l[end])) : (end += 1) {}

                if (start < end) {
                    return l[start..end];
                }
            }
        }
    }
    return null;
}

fn isIdentChar(c: u8) bool {
    return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or
        (c >= '0' and c <= '9') or c == '_';
}

fn findEnclosingFunction(allocator: Allocator, ast: []const Stmt, target_line: usize) !?*const Stmt {
    _ = allocator;
    for (ast) |*stmt| {
        if (try findEnclosingInStmt(stmt, target_line)) |found| {
            return found;
        }
    }
    return null;
}

fn findEnclosingInStmt(stmt: *const Stmt, target_line: usize) !?*const Stmt {
    _ = stmt;
    _ = target_line;
    // 实现查找包含指定行的最内层函数
    return null;
}

fn findLocalDefinition(func_stmt: *const Stmt, name: *const u8, target_line: usize) ?[2]usize {
    _ = func_stmt;
    _ = name;
    _ = target_line;
    // 实现在函数内搜索局部定义
    return null;
}

fn collectDefinitions(ast: []const Stmt, name: *const u8, defs: *std.ArrayList([2]usize)) !void {
    _ = ast;
    _ = name;
    _ = defs;
    // 实现全局搜索定义
}

fn locationToResponse(allocator: Allocator, uri: []const u8, loc: [2]usize, name: *const u8) !GotoDefinitionResponse {
    _ = name;
    return GotoDefinitionResponse{
        .uri = try allocator.dupe(u8, uri),
        .range = Range{
            .start = Position{
                .line = if (loc[0] > 0) @as(u32, @intCast(loc[0] - 1)) else 0,
                .character = if (loc[1] > 0) @as(u32, @intCast(loc[1] - 1)) else 0,
            },
            .end = Position{
                .line = if (loc[0] > 0) @as(u32, @intCast(loc[0] - 1)) else 0,
                .character = if (loc[1] > 0) @as(u32, @intCast(loc[1])) else 0,
            },
        },
    };
}
