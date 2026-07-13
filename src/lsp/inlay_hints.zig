const std = @import("std");
const Allocator = std.mem.Allocator;

const Stmt = @import("../parser/ast.zig").Stmt;
const Expr = @import("../parser/ast.zig").Expr;

pub const InlayHint = struct {
    position: Position,
    label: InlayHintLabel,
    kind: ?InlayHintKind,
    tooltip: ?[]u8,
    paddingLeft: bool,
    paddingRight: bool,
};

pub const InlayHintLabel = union(enum) {
    String: []u8,
    Parts: []InlayHintLabelPart,
};

pub const InlayHintLabelPart = struct {
    value: []u8,
    tooltip: ?[]u8,
    location: ?Location,
    command: ?Command,
};

pub const Location = struct {
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

pub const InlayHintKind = enum(u8) {
    Type = 1,
    Parameter = 2,
};

pub const Command = struct {
    title: []u8,
    command: []u8,
    arguments: ?[][]u8,
};

pub fn collectInlayHints(allocator: Allocator, ast: []const Stmt, source: []const u8) ![]InlayHint {
    var hints = std.ArrayList(InlayHint).init(allocator);
    defer hints.deinit();

    for (ast) |*stmt| {
        try collectFromStmt(stmt, source, &hints, allocator);
    }

    return hints.toOwnedSlice();
}

fn collectFromStmt(stmt: *const Stmt, source: []const u8, hints: *std.ArrayList(InlayHint), allocator: Allocator) !void {
    _ = stmt;
    _ = source;
    _ = hints;
    _ = allocator;
    // 实现从语句收集内联提示
}

fn collectFromExpr(expr: *const Expr, source: []const u8, hints: *std.ArrayList(InlayHint), allocator: Allocator) !void {
    _ = expr;
    _ = source;
    _ = hints;
    _ = allocator;
    // 实现从表达式收集内联提示
}

fn tryInferTypeFromExpr(expr: *const Expr) ?[]const u8 {
    _ = expr;
    // 实现类型推断
    return null;
}
