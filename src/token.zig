const std = @import("std");
const Allocator = std.mem.Allocator;

// ==================== 错误处理 ====================

/// 诊断严重程度
pub const Severity = enum {
    Error,
    Warning,
    Note,
    Help,

    pub fn format(self: Severity, comptime fmt: []const u8, options: std.fmt.FormatOptions, writer: anytype) !void {
        _ = fmt;
        _ = options;
        switch (self) {
            .Error => try writer.writeAll("error"),
            .Warning => try writer.writeAll("warning"),
            .Note => try writer.writeAll("note"),
            .Help => try writer.writeAll("help"),
        }
    }
};

/// 错误/诊断类别
pub const VXErrorKind = enum {
    Lexical,
    Syntax,
    Type,
    Ownership,
    Compile,
    Link,
    Io,
    Other,

    /// 返回对应的错误代码字符串
    pub fn code(self: VXErrorKind) []const u8 {
        return switch (self) {
            .Lexical => "E0001",
            .Syntax => "E0002",
            .Type => "E0003",
            .Ownership => "E0004",
            .Compile => "E0005",
            .Link => "E0006",
            .Io => "E0007",
            .Other => "E9999",
        };
    }

    pub fn format(self: VXErrorKind, comptime fmt: []const u8, options: std.fmt.FormatOptions, writer: anytype) !void {
        _ = fmt;
        _ = options;
        try writer.writeAll(self.code());
    }
};

/// 源代码位置区间（结构化 span）
pub const VXSpan = struct {
    start_line: usize,
    start_col: usize,
    end_line: usize,
    end_col: usize,

    pub fn new(start_line: usize, start_col: usize, end_line: usize, end_col: usize) VXSpan {
        return VXSpan{
            .start_line = start_line,
            .start_col = start_col,
            .end_line = end_line,
            .end_col = end_col,
        };
    }

    pub fn point(line: usize, col: usize) VXSpan {
        return VXSpan.new(line, col, line, col);
    }
};

/// 相关位置信息（用于辅助诊断）
pub const VXRelatedInfo = struct {
    span: VXSpan,
    message: []const u8,
};

/// VX 结构化错误类型
///
/// 注意：此结构体的字符串字段均为借用切片（不拥有内存），
/// 调用者需确保其指向的数据在 VXError 使用期间有效。
pub const VXError = struct {
    msg: []const u8,
    line: usize,
    col: usize,
    end_line: usize,
    end_col: usize,
    source: ?[]const u8 = null,
    severity: Severity = .Error,
    kind: VXErrorKind = .Compile,
    code: ?[]const u8 = null,
    related: []const VXRelatedInfo = &.{},

    /// 构造一个编译错误，默认位于单点 span
    pub fn new(msg: []const u8, line: usize, col: usize) VXError {
        return VXError{
            .msg = msg,
            .line = line,
            .col = col,
            .end_line = line,
            .end_col = col,
        };
    }

    /// 指定完整的 span 区间
    pub fn withSpan(self: VXError, span: VXSpan) VXError {
        var result = self;
        result.line = span.start_line;
        result.col = span.start_col;
        result.end_line = span.end_line;
        result.end_col = span.end_col;
        return result;
    }

    /// 指定源码上下文
    pub fn withSource(self: VXError, source: []const u8) VXError {
        var result = self;
        result.source = source;
        return result;
    }

    /// 指定严重程度
    pub fn withSeverity(self: VXError, severity: Severity) VXError {
        var result = self;
        result.severity = severity;
        return result;
    }

    /// 指定错误类别和代码
    pub fn withKind(self: VXError, kind: VXErrorKind) VXError {
        var result = self;
        result.kind = kind;
        result.code = kind.code();
        return result;
    }

    /// 添加相关位置信息
    pub fn withRelated(self: VXError, related: []const VXRelatedInfo) VXError {
        var result = self;
        result.related = related;
        return result;
    }

    /// 转换为可读字符串（不含源码上下文）
    pub fn message(self: VXError) []const u8 {
        return self.msg;
    }

    /// 格式化输出，类似 Rust 的 Display 实现
    pub fn format(self: VXError, comptime fmt_str: []const u8, options: std.fmt.FormatOptions, writer: anytype) !void {
        _ = fmt_str;
        _ = options;
        try writer.print("VX {} [{}] [line {}, col {}]: {}", .{
            self.severity,
            self.kind,
            self.line,
            self.col,
            self.msg,
        });
        if (self.source) |src| {
            if (self.line > 0) {
                var line_iter = std.mem.splitScalar(u8, src, '\n');
                var i: usize = 1;
                while (line_iter.next()) |line_str| : (i += 1) {
                    if (i == self.line) {
                        const underline_len = if (self.end_line == self.line and self.end_col >= self.col)
                            @max(self.end_col - self.col, 1)
                        else
                            1;
                        try writer.print("\n", .{});
                        try writer.print("{} | ", .{self.line});
                        try writer.writeAll(line_str);
                        try writer.print("\n | ", .{});
                        try writer.writeByteNTimes(' ', if (self.col > 0) self.col - 1 else 0);
                        try writer.writeByteNTimes('^', underline_len);
                        break;
                    }
                }
            }
        }
    }
};

/// 创建 VXError（替代 vx_error! 宏的构造功能）
/// 返回一个 Lexer 类型的编译错误，用于词法分析阶段。
pub fn vxError(msg: []const u8, line: usize, col: usize, source: []const u8) VXError {
    return VXError{
        .msg = msg,
        .line = line,
        .col = col,
        .end_line = line,
        .end_col = col,
        .source = source,
        .severity = .Error,
        .kind = .Lexical,
        .code = VXErrorKind.Lexical.code(),
        .related = &.{},
    };
}

// ==================== Token 类型 ====================
pub const TokenType = enum {
    // 控制流
    If,
    Elif,
    Else,
    For,
    While,
    Break,
    Continue,
    // 函数
    Func,
    Return,
    // 循环
    Loop,
    Match,
    // 所有权
    Move,
    Mut,
    // 字面量
    Int,
    Float,
    String,
    Identifier,
    // 运算符
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Power,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    Assign,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    PercentAssign,
    PowerAssign,
    Ampersand,
    Arrow,
    // 逻辑运算符 (自举兼容: and/or/not 关键字 + 符号形式 &&/||/! 并存)
    And,
    Or,
    Not,
    // 分隔符
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Colon,
    Semicolon,
    Comma,
    Dot,
    Newline,
    Indent,
    Dedent,
    EOF,
    // 布尔/零值
    True,
    False,
    Nil,
    // 迭代/导入
    In,
    Import,
    As,
    // 原生标量类型 (硬件基础类型, 必留)
    IntT,
    FloatT,
    DoubleT,
    VarT,
    BoolT,
    VoidT,
    // 复合类型声明
    Struct,
    Class,
    Enum,
    Union,
    // 内存分配/构造
    New,
    // 宏系统
    Macro,
    Hash,
    // 外部/FFI
    Extern,
};

pub const Token = struct {
    kind: TokenType,
    value: []const u8,
    line: usize,
    col: usize,
};

// ==================== 关键字表 ====================

/// 关键字条目类型
pub const KeywordEntry = struct { name: []const u8, kind: TokenType };

/// 关键字常量表（静态数组，与 Rust 版 KEYWORDS 完全对应）
pub const KEYWORDS = &[_]KeywordEntry{
    // 22 核心骨架关键字 (底层 OpCode 绑定, 永久保留)
    .{ .name = "if", .kind = .If },
    .{ .name = "else", .kind = .Else },
    .{ .name = "elif", .kind = .Elif },
    .{ .name = "for", .kind = .For },
    .{ .name = "while", .kind = .While },
    .{ .name = "break", .kind = .Break },
    .{ .name = "continue", .kind = .Continue },
    .{ .name = "func", .kind = .Func },
    .{ .name = "return", .kind = .Return },
    .{ .name = "loop", .kind = .Loop },
    .{ .name = "match", .kind = .Match },
    .{ .name = "true", .kind = .True },
    .{ .name = "false", .kind = .False },
    .{ .name = "nil", .kind = .Nil },
    .{ .name = "in", .kind = .In },
    .{ .name = "import", .kind = .Import },
    .{ .name = "as", .kind = .As },
    .{ .name = "var", .kind = .VarT },
    .{ .name = "struct", .kind = .Struct },
    .{ .name = "class", .kind = .Class },
    .{ .name = "enum", .kind = .Enum },
    .{ .name = "union", .kind = .Union },
    .{ .name = "new", .kind = .New },
    .{ .name = "move", .kind = .Move },
    .{ .name = "mut", .kind = .Mut },
    .{ .name = "macro", .kind = .Macro },
    .{ .name = "extern", .kind = .Extern },
    // 5 原生标量类型 (硬件基础类型, 保留)
    .{ .name = "int", .kind = .IntT },
    .{ .name = "float", .kind = .FloatT },
    .{ .name = "double", .kind = .DoubleT },
    .{ .name = "bool", .kind = .BoolT },
    .{ .name = "void", .kind = .VoidT },
    // 逻辑运算符关键字 (与符号形式 &&/||/! 并存, 自举代码大量使用)
    .{ .name = "and", .kind = .And },
    .{ .name = "or", .kind = .Or },
    .{ .name = "not", .kind = .Not },
};

/// 编译期关键字查找表，O(log n) 或 O(1) 查找
pub const keyword_map = std.StaticStringMap(TokenType).initComptime(.{
    .{ "if", .If },
    .{ "else", .Else },
    .{ "elif", .Elif },
    .{ "for", .For },
    .{ "while", .While },
    .{ "break", .Break },
    .{ "continue", .Continue },
    .{ "func", .Func },
    .{ "return", .Return },
    .{ "loop", .Loop },
    .{ "match", .Match },
    .{ "true", .True },
    .{ "false", .False },
    .{ "nil", .Nil },
    .{ "in", .In },
    .{ "import", .Import },
    .{ "as", .As },
    .{ "var", .VarT },
    .{ "struct", .Struct },
    .{ "class", .Class },
    .{ "enum", .Enum },
    .{ "union", .Union },
    .{ "new", .New },
    .{ "move", .Move },
    .{ "mut", .Mut },
    .{ "macro", .Macro },
    .{ "extern", .Extern },
    .{ "int", .IntT },
    .{ "float", .FloatT },
    .{ "double", .DoubleT },
    .{ "bool", .BoolT },
    .{ "void", .VoidT },
    .{ "and", .And },
    .{ "or", .Or },
    .{ "not", .Not },
});

// ==================== 词法分析器 ====================

pub const Lexer = struct {
    source: []const u8,
    /// 下一个待消费字符在 `source` 中的字节偏移
    pos: usize,
    line: usize,
    /// 当前字符的列号（1-based）。换行后重置为 1。
    col: usize,
    tokens: std.ArrayList(Token),
    indent_stack: std.ArrayList(usize),
    /// 上一行以可续行运算符结尾（如 or/and/+ 等），下一行的缩进变化不应产生 Indent/Dedent。
    continuation_pending: bool,
    allocator: Allocator,
    /// 最近一次 Lexer 错误（词法错误时设置）
    last_error: ?VXError,
    /// 错误消息的拥有者（由 allocator 分配），last_error.msg 借用此字段
    error_msg: ?[]const u8,

    pub fn init(source: []const u8, allocator: Allocator) Lexer {
        var indent_stack: std.ArrayList(usize) = .empty;
        indent_stack.append(allocator, 0) catch @panic("OOM");
        return Lexer{
            .source = source,
            .pos = 0,
            .line = 1,
            .col = 1,
            .tokens = .empty,
            .indent_stack = indent_stack,
            .continuation_pending = false,
            .allocator = allocator,
            .last_error = null,
            .error_msg = null,
        };
    }

    pub fn deinit(self: *Lexer) void {
        for (self.tokens.items) |*t| self.allocator.free(t.value);
        self.tokens.deinit(self.allocator);
        self.indent_stack.deinit(self.allocator);
        if (self.error_msg) |m| self.allocator.free(m);
    }

    /// 设置词法错误（使用静态/借用消息）
    fn setError(self: *Lexer, msg: []const u8) void {
        if (self.error_msg) |m| self.allocator.free(m);
        self.error_msg = null;
        self.last_error = VXError{
            .msg = msg,
            .line = self.line,
            .col = self.col,
            .end_line = self.line,
            .end_col = self.col,
            .source = self.source,
            .severity = .Error,
            .kind = .Lexical,
            .code = "E0001",
            .related = &.{},
        };
    }

    /// 设置词法错误（使用格式化消息，消息由 allocator 管理）
    fn setErrorFmt(self: *Lexer, comptime fmt: []const u8, args: anytype) void {
        if (self.error_msg) |m| self.allocator.free(m);
        const msg = std.fmt.allocPrint(self.allocator, fmt, args) catch @panic("OOM");
        self.error_msg = msg;
        self.last_error = VXError{
            .msg = msg,
            .line = self.line,
            .col = self.col,
            .end_line = self.line,
            .end_col = self.col,
            .source = self.source,
            .severity = .Error,
            .kind = .Lexical,
            .code = "E0001",
            .related = &.{},
        };
    }

    /// 返回从 `pos` 起第 `offset` 个 Unicode 码点，不会越界。
    /// 对于超出末尾的情况返回 0（与旧的字节语义保持一致）。
    fn peek(self: *const Lexer, offset: usize) u21 {
        var remaining = self.source[self.pos..];
        var i: usize = 0;
        while (i <= offset) {
            if (remaining.len == 0) return 0;
            const seq_len = std.unicode.utf8ByteSequenceLength(remaining[0]) catch 1;
            if (seq_len > remaining.len) return 0;
            if (i == offset) {
                return std.unicode.utf8Decode(remaining[0..seq_len]) catch @as(u21, remaining[0]);
            }
            remaining = remaining[seq_len..];
            i += 1;
        }
        return 0;
    }

    /// 消费一个 Unicode 码点并返回它，同时更新 `pos`（字节偏移）、`line`、`col`。
    fn advance(self: *Lexer) u21 {
        const c = self.peek(0);
        if (c == 0) return 0;
        const seq_len = std.unicode.utf8ByteSequenceLength(self.source[self.pos]) catch 1;
        self.pos += seq_len;
        if (c == '\n') {
            self.line += 1;
            self.col = 1;
        } else {
            self.col += 1;
        }
        return c;
    }

    fn skipWhitespace(self: *Lexer) void {
        while (true) {
            const c = self.peek(0);
            if (!(c == ' ' or c == '\t' or c == '\r' or c == '\u{3000}')) break;
            _ = self.advance();
        }
    }

    fn readString(self: *Lexer, q: u21) ![]const u8 {
        // 注意：此处使用字节级操作，因为字符串内容本质上是字节序列
        _ = self.advance(); // 消费开引号
        var res: std.ArrayList(u8) = .empty;
        while (true) {
            const c = self.peek(0);
            if (c == q or c == 0) break;
            if (c == '\\') {
                _ = self.advance();
                const e = self.advance();
                try res.append(self.allocator, switch (e) {
                    'n' => '\n',
                    't' => '\t',
                    'r' => '\r',
                    '"' => '"',
                    '\'' => '\'',
                    '\\' => '\\',
                    else => @as(u8, @intCast(e)),
                });
            } else {
                // 将 Unicode 码点编码为 UTF-8 字节
                var buf: [4]u8 = undefined;
                const encoded = try std.unicode.utf8Encode(c, &buf);
                try res.appendSlice(self.allocator, buf[0..encoded]);
                _ = self.advance();
            }
        }
        if (self.peek(0) != q) {
            self.setError("未闭合字符串");
            return error.Lexical;
        }
        _ = self.advance();
        return res.toOwnedSlice(self.allocator) catch @panic("OOM");
    }

    fn readNumber(self: *Lexer) Token {
        const sl = self.line;
        const sc = self.col;
        var s: std.ArrayList(u8) = .empty;
        defer s.deinit(self.allocator);
        var is_float = false;

        while (self.peek(0) >= '0' and self.peek(0) <= '9') {
            s.append(self.allocator, @intCast(self.advance())) catch @panic("OOM");
        }
        if (self.peek(0) == '.' and self.peek(1) >= '0' and self.peek(1) <= '9') {
            is_float = true;
            s.append(self.allocator, @intCast(self.advance())) catch @panic("OOM"); // '.'
            while (self.peek(0) >= '0' and self.peek(0) <= '9') {
                s.append(self.allocator, @intCast(self.advance())) catch @panic("OOM");
            }
        }
        if (self.peek(0) == 'e' or self.peek(0) == 'E') {
            is_float = true;
            s.append(self.allocator, @intCast(self.advance())) catch @panic("OOM"); // 'e'/'E'
            if (self.peek(0) == '+' or self.peek(0) == '-') {
                s.append(self.allocator, @intCast(self.advance())) catch @panic("OOM");
            }
            while (self.peek(0) >= '0' and self.peek(0) <= '9') {
                s.append(self.allocator, @intCast(self.advance())) catch @panic("OOM");
            }
        }
        // 标准化数字表示
        const val = if (is_float) blk: {
            const n = std.fmt.parseFloat(f64, s.items) catch 0.0;
            break :blk std.fmt.allocPrint(self.allocator, "{d}", .{n}) catch @panic("OOM");
        } else blk: {
            const n = std.fmt.parseInt(i64, s.items, 10) catch 0;
            break :blk std.fmt.allocPrint(self.allocator, "{}", .{n}) catch @panic("OOM");
        };
        return Token{
            .kind = if (is_float) TokenType.Float else TokenType.Int,
            .value = val,
            .line = sl,
            .col = sc,
        };
    }

    fn readIdentifier(self: *Lexer) Token {
        const sl = self.line;
        const sc = self.col;
        var val: std.ArrayList(u8) = .empty;
        defer val.deinit(self.allocator);
        while (true) {
            const c = self.peek(0);
            if (c == 0) break;
            const is_ascii_alnum = c < 128 and std.ascii.isAlphanumeric(@as(u8, @intCast(c)));
            const is_cjk = c >= 0x4E00 and c <= 0x9FFF;
            if (!is_ascii_alnum and c != '_' and !is_cjk) break;
            // 复制原始字节
            const seq_len = std.unicode.utf8ByteSequenceLength(self.source[self.pos]) catch 1;
            val.appendSlice(self.allocator, self.source[self.pos .. self.pos + seq_len]) catch @panic("OOM");
            _ = self.advance();
        }
        const kind = keyword_map.get(val.items) orelse TokenType.Identifier;
        const value = self.allocator.dupe(u8, val.items) catch @panic("OOM");
        return Token{
            .kind = kind,
            .value = value,
            .line = sl,
            .col = sc,
        };
    }

    fn handleIndent(self: *Lexer) !void {
        while (true) {
            if (self.peek(0) != '\n' and self.tokens.items.len == 0) {
                return;
            }
            while (self.peek(0) == '\n') {
                _ = self.advance();
            }
            if (self.peek(0) == 0) {
                return;
            }
            var indent: usize = 0;
            while (self.peek(0) == ' ' or self.peek(0) == '\u{3000}') {
                indent += 1;
                _ = self.advance();
            }
            while (self.peek(0) == '\t') {
                indent += 4;
                _ = self.advance();
            }
            // 跳过空注释行
            if (self.peek(0) == '#' and switch (self.peek(1)) {
                ' ', '\t', '\n', 0 => true,
                else => false,
            }) {
                while (self.peek(0) != '\n' and self.peek(0) != 0) {
                    _ = self.advance();
                }
                continue;
            }
            const last = if (self.indent_stack.items.len > 0)
                self.indent_stack.items[self.indent_stack.items.len - 1]
            else
                0;
            if (indent > last) {
                if (self.continuation_pending) {
                    self.continuation_pending = false;
                } else {
                    self.indent_stack.append(self.allocator, indent) catch @panic("OOM");
                    const indent_str = std.fmt.allocPrint(self.allocator, "{}", .{indent}) catch @panic("OOM");
                    self.tokens.append(self.allocator, Token{
                        .kind = TokenType.Indent,
                        .value = indent_str,
                        .line = self.line,
                        .col = self.col,
                    }) catch @panic("OOM");
                }
            } else if (indent < last) {
                self.continuation_pending = false;
                while (self.indent_stack.items.len > 0) {
                    const stack_top = self.indent_stack.items[self.indent_stack.items.len - 1];
                    if (indent >= stack_top) break;
                    _ = self.indent_stack.pop();
                    self.tokens.append(self.allocator, Token{
                        .kind = TokenType.Dedent,
                        .value = self.allocator.dupe(u8, "") catch @panic("OOM"),
                        .line = self.line,
                        .col = self.col,
                    }) catch @panic("OOM");
                }
                if (self.indent_stack.items.len > 0) {
                    const last_stack = self.indent_stack.items[self.indent_stack.items.len - 1];
                    if (indent != last_stack) {
                        self.setError("缩进不匹配");
                        return error.Lexical;
                    }
                } else {
                    self.setError("缩进栈为空");
                    return error.Lexical;
                }
            } else {
                self.continuation_pending = false;
            }
            return;
        }
    }

    /// 判断某类 token 是否可作为行尾续行运算符
    fn isContinuationOperator(kind: TokenType) bool {
        return switch (kind) {
            .Or, .And, .Plus, .Minus, .Star, .Slash, .Percent, .Power, .Comma => true,
            else => false,
        };
    }

    pub fn tokenize(self: *Lexer) !std.ArrayList(Token) {
        while (self.pos < self.source.len) {
            self.skipWhitespace();
            const sl = self.line;
            const sc = self.col;
            const c = self.peek(0);

            if (c == '\n' or c == '\r') {
                if (c == '\r' and self.peek(1) == '\n') {
                    _ = self.advance();
                }
                const prev_kind = if (self.tokens.items.len > 0)
                    self.tokens.items[self.tokens.items.len - 1].kind
                else
                    TokenType.EOF;
                self.tokens.append(self.allocator, Token{
                    .kind = TokenType.Newline,
                    .value = self.allocator.dupe(u8, "") catch @panic("OOM"),
                    .line = sl,
                    .col = sc,
                }) catch @panic("OOM");
                self.continuation_pending = Lexer.isContinuationOperator(prev_kind);
                _ = self.advance();
                try self.handleIndent();
                continue;
            }

            if (c == '#') {
                if (switch (self.peek(1)) {
                    ' ', '\t', '\n', 0 => true,
                    else => false,
                }) {
                    while (self.peek(0) != '\n' and self.peek(0) != 0) {
                        _ = self.advance();
                    }
                    continue;
                } else {
                    _ = self.advance();
                    self.tokens.append(self.allocator, Token{
                        .kind = TokenType.Hash,
                        .value = self.allocator.dupe(u8, "#") catch @panic("OOM"),
                        .line = sl,
                        .col = sc,
                    }) catch @panic("OOM");
                    continue;
                }
            }

            if (c == '"' or c == '\'') {
                const val = try self.readString(c);
                self.tokens.append(self.allocator, Token{
                    .kind = TokenType.String,
                    .value = val,
                    .line = sl,
                    .col = sc,
                }) catch @panic("OOM");
                continue;
            }

            if (c >= '0' and c <= '9') {
                const t = self.readNumber();
                self.tokens.append(self.allocator, t) catch @panic("OOM");
                continue;
            }

            if ((c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or c == '_' or (c >= 0x4E00 and c <= 0x9FFF)) {
                const t = self.readIdentifier();
                self.tokens.append(self.allocator, t) catch @panic("OOM");
                continue;
            }

            const n = self.peek(1);
            var handled = true;
            switch (c) {
                '=' => {
                    if (n == '=') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .Eq, .value = self.allocator.dupe(u8, "==") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '!' => {
                    if (n == '=') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .Ne, .value = self.allocator.dupe(u8, "!=") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '<' => {
                    if (n == '=') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .Le, .value = self.allocator.dupe(u8, "<=") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '>' => {
                    if (n == '=') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .Ge, .value = self.allocator.dupe(u8, ">=") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '+' => {
                    if (n == '=') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .PlusAssign, .value = self.allocator.dupe(u8, "+=") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '-' => {
                    if (n == '=') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .MinusAssign, .value = self.allocator.dupe(u8, "-=") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else if (n == '>') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .Arrow, .value = self.allocator.dupe(u8, "->") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '*' => {
                    if (n == '=') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .StarAssign, .value = self.allocator.dupe(u8, "*=") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '/' => {
                    if (n == '=') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .SlashAssign, .value = self.allocator.dupe(u8, "/=") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '%' => {
                    if (n == '=') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .PercentAssign, .value = self.allocator.dupe(u8, "%=") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '^' => {
                    if (n == '=') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .PowerAssign, .value = self.allocator.dupe(u8, "^=") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '&' => {
                    if (n == '&') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .And, .value = self.allocator.dupe(u8, "&&") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                '|' => {
                    if (n == '|') {
                        _ = self.advance();
                        _ = self.advance();
                        self.tokens.append(self.allocator, Token{ .kind = .Or, .value = self.allocator.dupe(u8, "||") catch @panic("OOM"), .line = sl, .col = sc }) catch @panic("OOM");
                    } else {
                        handled = false;
                    }
                },
                else => handled = false,
            }
            if (handled) continue;

            // 单字符运算符/分隔符
            const kind: ?TokenType = switch (c) {
                '+' => .Plus,
                '-' => .Minus,
                '*' => .Star,
                '/' => .Slash,
                '%' => .Percent,
                '^' => .Power,
                '<' => .Lt,
                '>' => .Gt,
                '=' => .Assign,
                '!' => .Not,
                '&' => .Ampersand,
                '#' => .Hash,
                '(' => .LParen,
                ')' => .RParen,
                '[' => .LBracket,
                ']' => .RBracket,
                '{' => .LBrace,
                '}' => .RBrace,
                ':' => .Colon,
                ';' => .Semicolon,
                ',' => .Comma,
                '.' => .Dot,
                else => null,
            };

            if (kind) |k| {
                _ = self.advance();
                // 对单字符 token，value 只用于存储原始字符的 UTF-8 表示
                var char_buf: [4]u8 = undefined;
                const char_slice = try std.unicode.utf8Encode(c, &char_buf);
                const value = self.allocator.dupe(u8, char_buf[0..char_slice]) catch @panic("OOM");
                self.tokens.append(self.allocator, Token{
                    .kind = k,
                    .value = value,
                    .line = sl,
                    .col = sc,
                }) catch @panic("OOM");
            } else {
                // 不在单字符符号表中，作为非法字符报错
                self.setErrorFmt("非法字符: U+{X:0>4}", .{@as(u32, c)});
                return error.Lexical;
            }
        }

        // 闭合所有缩进层级
        while (self.indent_stack.items.len > 1) {
            _ = self.indent_stack.pop();
            self.tokens.append(self.allocator, Token{
                .kind = TokenType.Dedent,
                .value = self.allocator.dupe(u8, "") catch @panic("OOM"),
                .line = self.line,
                .col = self.col,
            }) catch @panic("OOM");
        }

        self.tokens.append(self.allocator, Token{
            .kind = TokenType.EOF,
            .value = self.allocator.dupe(u8, "") catch @panic("OOM"),
            .line = self.line,
            .col = self.col,
        }) catch @panic("OOM");

        // 将 tokens 的所有权转移给调用者
        const result = self.tokens;
        self.tokens = .empty;
        return result;
    }
};
