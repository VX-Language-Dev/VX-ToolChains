const std = @import("std");
const Allocator = std.mem.Allocator;

const Stmt = @import("../parser/ast.zig").Stmt;
const Expr = @import("../parser/ast.zig").Expr;
const Token = @import("../token.zig").Token;
const TokenType = @import("../token.zig").TokenType;

pub const Hover = struct {
    contents: HoverContents,
    range: ?Range,
};

pub const HoverContents = union(enum) {
    Scalar: MarkupContent,
    Array: []MarkupContent,
};

pub const MarkupContent = struct {
    kind: MarkupKind,
    value: []u8,
};

pub const MarkupKind = enum {
    PlainText,
    Markdown,
};

pub const Range = struct {
    start: Position,
    end: Position,
};

pub const Position = struct {
    line: u32,
    character: u32,
};

fn builtinHover(allocator: Allocator, name: []const u8) ?[]u8 {
    _ = allocator;
    if (std.mem.eql(u8, name, "out")) {
        return "**out(value: int)**\n\n内置输出函数，将整数打印到标准输出并追加换行。";
    }
    if (std.mem.eql(u8, name, "sys_argv")) {
        return "**sys_argv() -> [string]**\n\n返回命令行参数数组。";
    }
    if (std.mem.eql(u8, name, "len")) {
        return "**len(collection) -> int**\n\n返回数组或字符串的长度。";
    }
    if (std.mem.eql(u8, name, "panic")) {
        return "**panic(message: string)**\n\n触发运行时 panic 并终止程序。";
    }
    return null;
}

pub fn hover(
    allocator: Allocator,
    ast: []const Stmt,
    tokens: []const Token,
    source: []const u8,
    position: Position,
) !?Hover {
    _ = ast;
    _ = source;
    const line = position.line + 1;
    const col = position.character + 1;

    // 查找光标处的token
    const token = findTokenAt(tokens, line, col) orelse return null;
    const range = tokenRange(&token);

    // 关键字悬停
    if (isKeywordToken(token.kind)) {
        if (try keywordHover(allocator, token.kind)) |h| {
            return Hover{
                .contents = HoverContents{ .Scalar = h },
                .range = range,
            };
        }
    }

    // 标识符悬停
    if (token.kind == .Identifier) {
        // 实现标识符悬停逻辑
    }

    return null;
}

fn findTokenAt(tokens: []const Token, line: usize, col: usize) ?Token {
    for (tokens) |token| {
        if (token.line == line and token.col <= col and col < token.col + token.value.len) {
            return token;
        }
    }
    return null;
}

fn tokenRange(token: *const Token) ?Range {
    const line0 = if (token.line > 0) @as(u32, @intCast(token.line - 1)) else 0;
    const col0 = if (token.col > 0) @as(u32, @intCast(token.col - 1)) else 0;
    return Range{
        .start = Position{ .line = line0, .character = col0 },
        .end = Position{ .line = line0, .character = col0 + @as(u32, @intCast(token.value.len)) },
    };
}

fn isKeywordToken(kind: TokenType) bool {
    return switch (kind) {
        .If, .Elif, .Else, .For, .While, .Break, .Continue, .Func, .Return, .Import, .As, .Struct, .Class, .Enum, .Union, .New, .Move, .IntT, .FloatT, .DoubleT, .VarT, .BoolT, .VoidT, .And, .Or, .Not, .In, .True, .False, .Nil => true,
        else => false,
    };
}

fn keywordHover(allocator: Allocator, kind: TokenType) !?MarkupContent {
    const desc = switch (kind) {
        .If => "**if 条件判断**\n\n```vx\nif condition:\n    body\n```",
        .For => "**for 循环**\n\n```vx\nfor x in collection:\n    body\n```",
        .While => "**while 循环**\n\n```vx\nwhile condition:\n    body\n```",
        .Func => "**func 函数声明**\n\n```vx\nfunc name(param: type) -> ret:\n    body\n```",
        .Return => "**return 函数返回**",
        .Import => "**import 导入模块**\n\n```vx\nimport module as alias\n```",
        .Struct => "**struct 结构体**\n\n```vx\nstruct Name:\n    field: type\n```",
        .Class => "**class 类**",
        .Enum => "**enum 枚举**\n\n```vx\nenum Name:\n    Variant = 0\n```",
        .IntT => "**int 整数类型**\n\n64位有符号整数",
        .FloatT => "**float 单精度浮点**",
        .DoubleT => "**double 双精度浮点**",
        .BoolT => "**bool 布尔类型**",
        .VoidT => "**void 空类型**",
        .True => "**true 布尔值真**",
        .False => "**false 布尔值假**",
        .Nil => "**nil 空值**",
        else => return null,
    };

    var content = std.ArrayList(u8).init(allocator);
    defer content.deinit();
    content.appendSlice(desc) catch return null;

    return MarkupContent{
        .kind = .Markdown,
        .value = try content.toOwnedSlice(),
    };
}

fn makeHover(allocator: Allocator, content: []const u8) !HoverContents {
    var value = std.ArrayList(u8).init(allocator);
    defer value.deinit();
    value.appendSlice(content) catch {};

    return HoverContents{
        .Scalar = MarkupContent{
            .kind = .Markdown,
            .value = try value.toOwnedSlice(),
        },
    };
}
