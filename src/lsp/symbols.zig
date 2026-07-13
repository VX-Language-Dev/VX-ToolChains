const std = @import("std");
const Allocator = std.mem.Allocator;

const Stmt = @import("../parser/ast.zig").Stmt;
const Expr = @import("../parser/ast.zig").Expr;

pub const DocumentSymbol = struct {
    name: []u8,
    detail: ?[]u8,
    kind: SymbolKind,
    range: Range,
    selection_range: Range,
    children: ?[]DocumentSymbol,
};

pub const SymbolInformation = struct {
    name: []u8,
    kind: SymbolKind,
    location: Location,
    containerName: ?[]u8,
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

pub const SymbolKind = enum(u16) {
    File = 1,
    Module = 2,
    Namespace = 3,
    Package = 4,
    Class = 5,
    Method = 6,
    Property = 7,
    Field = 8,
    Constructor = 9,
    Enum = 10,
    Interface = 11,
    Function = 12,
    Variable = 13,
    Constant = 14,
    String = 15,
    Number = 16,
    Boolean = 17,
    Array = 18,
    Object = 19,
    Key = 20,
    Null = 21,
    EnumMember = 22,
    Struct = 23,
    Event = 24,
    Operator = 25,
    TypeParameter = 26,
};

pub fn documentSymbols(allocator: Allocator, ast: []const Stmt) ![]DocumentSymbol {
    var symbols = std.ArrayList(DocumentSymbol).init(allocator);
    defer symbols.deinit();

    for (ast) |*stmt| {
        if (try buildDocumentSymbol(allocator, stmt)) |sym| {
            try symbols.append(sym);
        }
    }

    return symbols.toOwnedSlice();
}

fn buildDocumentSymbol(allocator: Allocator, stmt: *const Stmt) !?DocumentSymbol {
    _ = allocator;
    _ = stmt;
    // 实现从AST构建文档符号
    return null;
}

fn makeVarSymbol(allocator: Allocator, name: []const u8, type_info: ?[]const u8, line: usize, col: usize) !DocumentSymbol {
    return DocumentSymbol{
        .name = try allocator.dupe(u8, name),
        .detail = if (type_info) |t| try allocator.dupe(u8, t) else null,
        .kind = .Variable,
        .range = Range{
            .start = Position{ .line = @intCast(line), .character = @intCast(col) },
            .end = Position{ .line = @intCast(line), .character = @intCast(col + name.len) },
        },
        .selection_range = Range{
            .start = Position{ .line = @intCast(line), .character = @intCast(col) },
            .end = Position{ .line = @intCast(line), .character = @intCast(col + name.len) },
        },
        .children = null,
    };
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
