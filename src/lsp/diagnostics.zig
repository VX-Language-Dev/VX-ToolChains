const std = @import("std");
const Allocator = std.mem.Allocator;

const Stmt = @import("../parser/ast.zig").Stmt;
const Token = @import("../token.zig").Token;
const VXError = @import("../token.zig").VXError;

pub const Diagnostic = struct {
    range: Range,
    severity: Severity,
    code: ?[]u8,
    source: []u8,
    message: []u8,
};

pub const Range = struct {
    start: Position,
    end: Position,
};

pub const Position = struct {
    line: u32,
    character: u32,
};

pub const Severity = enum(u8) {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4,
};

pub const DiagnosticResult = struct {
    tokens: []Token,
    ast: []Stmt,
    diagnostics: []Diagnostic,

    pub fn deinit(self: *DiagnosticResult, allocator: Allocator) void {
        for (self.diagnostics) |diag| {
            diag.deinit(allocator);
        }
        allocator.free(self.diagnostics);
    }
};

pub fn runDiagnostics(
    allocator: Allocator,
    uri: []const u8,
    source: []const u8,
    memory_safety_as_warning: bool,
) !DiagnosticResult {
    _ = uri;
    _ = source;
    _ = memory_safety_as_warning;
    var tokens = std.ArrayList(Token).init(allocator);
    var ast = std.ArrayList(Stmt).init(allocator);
    var diagnostics = std.ArrayList(Diagnostic).init(allocator);
    errdefer {
        tokens.deinit();
        ast.deinit();
        diagnostics.deinit();
    }

    // 阶段一：词法分析
    // (这里应该调用Lexer，简化处理)

    // 阶段二：语法分析
    // (这里应该调用Parser，简化处理)

    // 阶段三：所有权检查
    // (这里应该调用OwnershipChecker，简化处理)

    return DiagnosticResult{
        .tokens = try tokens.toOwnedSlice(),
        .ast = try ast.toOwnedSlice(),
        .diagnostics = try diagnostics.toOwnedSlice(),
    };
}

fn vxErrorToDiagnostic(allocator: Allocator, err: VXError) !Diagnostic {
    var message = std.ArrayList(u8).init(allocator);
    defer message.deinit();

    message.appendSlice(err.msg) catch {};

    return Diagnostic{
        .range = Range{
            .start = Position{
                .line = if (err.line > 0) @intCast(err.line - 1) else 0,
                .character = if (err.col > 0) @intCast(err.col - 1) else 0,
            },
            .end = Position{
                .line = if (err.end_line > 0) @intCast(err.end_line - 1) else 0,
                .character = @intCast(err.end_col),
            },
        },
        .severity = switch (err.severity) {
            .Error => .Error,
            .Warning => .Warning,
            .Note => .Information,
            .Help => .Hint,
        },
        .code = null,
        .source = "vx",
        .message = try message.toOwnedSlice(),
    };
}
