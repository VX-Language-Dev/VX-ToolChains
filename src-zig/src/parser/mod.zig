const std = @import("std");
const Allocator = std.mem.Allocator;
const token = @import("../token.zig");
const ast = @import("ast.zig");

pub const Expr = ast.Expr;
const MapPair = ast.MapPair;
const ElifBranch = ast.ElifBranch;
const MatchArm = ast.MatchArm;
const ParamDef = ast.ParamDef;
const FieldDef = ast.FieldDef;
const ClassFieldDef = ast.ClassFieldDef;
const EnumVariant = ast.EnumVariant;
const UnionField = ast.UnionField;
const expr_to_type_name = ast.expr_to_type_name;

// ==================== EOF 哨兵 ====================

const EOF_SENTINEL: token.Token = .{
    .kind = .EOF,
    .value = "",
    .line = 0,
    .col = 0,
};

// ==================== 解析器错误集 ====================

pub const ParseError = error{
    Syntax,
    OutOfMemory,
};

// ==================== 独立辅助函数 ====================

/// 判断 TokenType 是否属于关键字类别（可降级为标识符）
pub fn isKeywordKind(k: token.TokenType) bool {
    return switch (k) {
        .True, .False, .Nil, .In, .Import, .As, .VarT, .Struct, .Class, .Enum, .Union, .New, .Move, .Macro, .Extern, .IntT, .FloatT, .DoubleT, .BoolT, .VoidT, .And, .Or, .Not => true,
        else => false,
    };
}

/// 关键字字面值是否可作为标识符使用（字母或下划线开头）
pub fn isIdentifierLikeValue(s: []const u8) bool {
    if (s.len == 0) return false;
    const c = s[0];
    // 非 ASCII 字节（CJK 等）也算合法标识符起始
    return std.ascii.isAlphabetic(c) or c == '_' or c >= 0x80;
}

// ==================== 语法分析器 ====================

pub const Parser = struct {
    tokens: std.ArrayList(token.Token),
    source: []const u8,
    pos: usize,
    allocator: Allocator,
    last_error: ?token.VXError,
    /// 用于存储动态格式化的错误消息（需要手动释放）
    error_msg: ?[]const u8,

    pub fn init(
        tokens: std.ArrayList(token.Token),
        source: []const u8,
        allocator: Allocator,
    ) Parser {
        return Parser{
            .tokens = tokens,
            .source = source,
            .pos = 0,
            .allocator = allocator,
            .last_error = null,
            .error_msg = null,
        };
    }

    pub fn deinit(self: *Parser) void {
        // tokens 的所有权属于调用者，不由 Parser 释放
        if (self.error_msg) |m| self.allocator.free(m);
    }

    // ---------- 错误辅助 ----------

    fn syntaxError(self: *Parser, msg: []const u8, line: usize, col: usize) ParseError {
        self.last_error = token.VXError.new(msg, line, col)
            .withSource(self.source)
            .withKind(.Syntax);
        return error.Syntax;
    }

    // ==================== 核心辅助方法（来自 mod.rs） ====================

    pub fn current(self: *Parser) *const token.Token {
        if (self.pos < self.tokens.items.len) {
            return &self.tokens.items[self.pos];
        }
        return &EOF_SENTINEL;
    }

    pub fn peek(self: *Parser, offset: usize) *const token.Token {
        if (self.pos + offset < self.tokens.items.len) {
            return &self.tokens.items[self.pos + offset];
        }
        return &EOF_SENTINEL;
    }

    pub fn advance(self: *Parser) token.Token {
        if (self.pos < self.tokens.items.len - 1) {
            self.pos += 1;
        }
        const src = self.tokens.items[self.pos - 1];
        return token.Token{
            .kind = src.kind,
            .value = self.allocator.dupe(u8, src.value) catch @panic("OOM"),
            .line = src.line,
            .col = src.col,
        };
    }

    pub fn expect(
        self: *Parser,
        kind: token.TokenType,
        msg: ?[]const u8,
    ) ParseError!token.Token {
        if (self.current().kind != kind) {
            const message = if (msg) |m| m else "unexpected token";
            return self.syntaxError(message, self.current().line, self.current().col);
        }
        return self.advance();
    }

    /// 接受关键字 token 作为标识符（自举兼容）
    pub fn expectIdentifierOrKeyword(self: *Parser) ParseError!token.Token {
        const kind = self.current().kind;
        if (kind == .Identifier or
            (isKeywordKind(kind) and isIdentifierLikeValue(self.current().value)))
        {
            return self.advance();
        }
        const t = self.current();
        return self.syntaxError("期望标识符", t.line, t.col);
    }

    pub fn matchKind(self: *Parser, kinds: []const token.TokenType) bool {
        return std.mem.indexOfScalar(token.TokenType, kinds, self.current().kind) != null;
    }

    pub fn skipNewlines(self: *Parser) void {
        while (self.current().kind == token.TokenType.Newline) {
            _ = self.advance();
        }
    }

    /// 二元运算符后是否还有合法表达式延续（多行条件兼容）
    pub fn peekContinuationAfterBinaryOp(self: *Parser) bool {
        var look = self.pos + 1;
        // 跳过换行
        while (look < self.tokens.items.len and
            self.tokens.items[look].kind == token.TokenType.Newline) look += 1;
        // 跳过 Dedent
        while (look < self.tokens.items.len and
            self.tokens.items[look].kind == token.TokenType.Dedent)
        {
            look += 1;
            while (look < self.tokens.items.len and
                self.tokens.items[look].kind == token.TokenType.Newline) look += 1;
        }
        // 跳过 Indent
        while (look < self.tokens.items.len and
            self.tokens.items[look].kind == token.TokenType.Indent)
        {
            look += 1;
            while (look < self.tokens.items.len and
                self.tokens.items[look].kind == token.TokenType.Newline) look += 1;
        }
        if (look >= self.tokens.items.len) return false;
        const nk = self.tokens.items[look].kind;
        if (nk == token.TokenType.Colon) return false;
        if (nk == token.TokenType.EOF) return false;
        if (nk == .Else or nk == .Elif or nk == .RParen or
            nk == .RBracket or nk == .RBrace or nk == .Comma) return false;
        return true;
    }

    /// 解析可选的泛型参数列表 `<T, U>`
    pub fn parseGenericParams(self: *Parser) ParseError!std.ArrayList([]const u8) {
        var params: std.ArrayList([]const u8) = .empty;
        if (self.current().kind == token.TokenType.Lt) {
            _ = self.advance();
            const t0 = try self.expect(token.TokenType.Identifier, "期望类型参数名");
            try params.append(self.allocator, t0.value);
            while (self.current().kind == token.TokenType.Comma) {
                _ = self.advance();
                const tn = try self.expect(token.TokenType.Identifier, "期望类型参数名");
                try params.append(self.allocator, tn.value);
            }
            _ = try self.expect(token.TokenType.Gt, "期望 '>'");
        }
        return params;
    }

    // ==================== 表达式解析（来自 expr.rs） ====================

    pub fn parseExpression(self: *Parser) ParseError!Expr {
        return self.parseAssignment();
    }

    fn parseAssignment(self: *Parser) ParseError!Expr {
        var lhs = try self.parseOr();
        const assign_kinds = [_]token.TokenType{
            .Assign,      .PlusAssign,    .MinusAssign, .StarAssign,
            .SlashAssign, .PercentAssign, .PowerAssign,
        };
        if (self.matchKind(&assign_kinds)) {
            const line = lhs.e_line();
            const col = lhs.e_col();
            // 赋值目标必须是变量/索引/属性/解引用
            switch (lhs) {
                .Identifier, .IndexAccess, .PropertyAccess, .Deref => {},
                else => return self.syntaxError("赋值目标必须是变量/索引/属性", line, col),
            }
            const op_tok = self.advance();
            const op = op_tok.value; // owned
            const rhs = try self.parseAssignment();
            const lhs_ptr = try self.allocator.create(Expr);
            lhs_ptr.* = lhs;
            const rhs_ptr = try self.allocator.create(Expr);
            rhs_ptr.* = rhs;
            return Expr{ .Assign = .{
                .target = lhs_ptr,
                .op = op,
                .value = rhs_ptr,
                .line = line,
                .col = col,
            } };
        }
        return lhs;
    }

    fn parseOr(self: *Parser) ParseError!Expr {
        var left = try self.parseAnd();
        while (self.current().kind == token.TokenType.Or) {
            if (!self.peekContinuationAfterBinaryOp()) break;
            const op_tok = self.advance();
            const op = op_tok.value;
            self.skipNewlines();
            while (self.current().kind == token.TokenType.Dedent) {
                _ = self.advance();
                self.skipNewlines();
            }
            while (self.current().kind == token.TokenType.Indent) {
                _ = self.advance();
                self.skipNewlines();
            }
            const right = try self.parseAnd();
            const l_ptr = try self.allocator.create(Expr);
            l_ptr.* = left;
            const r_ptr = try self.allocator.create(Expr);
            r_ptr.* = right;
            const line = l_ptr.e_line();
            const col = l_ptr.e_col();
            left = Expr{ .BinaryOp = .{ .op = op, .left = l_ptr, .right = r_ptr, .line = line, .col = col } };
        }
        return left;
    }

    fn parseAnd(self: *Parser) ParseError!Expr {
        var left = try self.parseEquality();
        while (self.current().kind == token.TokenType.And) {
            if (!self.peekContinuationAfterBinaryOp()) break;
            const op_tok = self.advance();
            const op = op_tok.value;
            self.skipNewlines();
            while (self.current().kind == token.TokenType.Dedent) {
                _ = self.advance();
                self.skipNewlines();
            }
            while (self.current().kind == token.TokenType.Indent) {
                _ = self.advance();
                self.skipNewlines();
            }
            const right = try self.parseEquality();
            const l_ptr = try self.allocator.create(Expr);
            l_ptr.* = left;
            const r_ptr = try self.allocator.create(Expr);
            r_ptr.* = right;
            const line = l_ptr.e_line();
            const col = l_ptr.e_col();
            left = Expr{ .BinaryOp = .{ .op = op, .left = l_ptr, .right = r_ptr, .line = line, .col = col } };
        }
        return left;
    }

    fn parseEquality(self: *Parser) ParseError!Expr {
        var left = try self.parseComparison();
        const eq_kinds = [_]token.TokenType{ .Eq, .Ne };
        while (self.matchKind(&eq_kinds)) {
            const op_tok = self.advance();
            const op = op_tok.value;
            const right = try self.parseComparison();
            const l_ptr = try self.allocator.create(Expr);
            l_ptr.* = left;
            const r_ptr = try self.allocator.create(Expr);
            r_ptr.* = right;
            const line = l_ptr.e_line();
            const col = l_ptr.e_col();
            left = Expr{ .BinaryOp = .{ .op = op, .left = l_ptr, .right = r_ptr, .line = line, .col = col } };
        }
        return left;
    }

    fn parseComparison(self: *Parser) ParseError!Expr {
        var left = try self.parseAdditive();
        const cmp_kinds = [_]token.TokenType{ .Lt, .Gt, .Le, .Ge };
        while (self.matchKind(&cmp_kinds)) {
            const op_tok = self.advance();
            const op = op_tok.value;
            const right = try self.parseAdditive();
            const l_ptr = try self.allocator.create(Expr);
            l_ptr.* = left;
            const r_ptr = try self.allocator.create(Expr);
            r_ptr.* = right;
            const line = l_ptr.e_line();
            const col = l_ptr.e_col();
            left = Expr{ .BinaryOp = .{ .op = op, .left = l_ptr, .right = r_ptr, .line = line, .col = col } };
        }
        return left;
    }

    fn parseAdditive(self: *Parser) ParseError!Expr {
        var left = try self.parseMultiplicative();
        const add_kinds = [_]token.TokenType{ .Plus, .Minus };
        while (self.matchKind(&add_kinds)) {
            const op_tok = self.advance();
            const op = op_tok.value;
            const right = try self.parseMultiplicative();
            const l_ptr = try self.allocator.create(Expr);
            l_ptr.* = left;
            const r_ptr = try self.allocator.create(Expr);
            r_ptr.* = right;
            const line = l_ptr.e_line();
            const col = l_ptr.e_col();
            left = Expr{ .BinaryOp = .{ .op = op, .left = l_ptr, .right = r_ptr, .line = line, .col = col } };
        }
        return left;
    }

    fn parseMultiplicative(self: *Parser) ParseError!Expr {
        var left = try self.parsePower();
        const mul_kinds = [_]token.TokenType{ .Star, .Slash, .Percent };
        while (self.matchKind(&mul_kinds)) {
            const op_tok = self.advance();
            const op = op_tok.value;
            const right = try self.parsePower();
            const l_ptr = try self.allocator.create(Expr);
            l_ptr.* = left;
            const r_ptr = try self.allocator.create(Expr);
            r_ptr.* = right;
            const line = l_ptr.e_line();
            const col = l_ptr.e_col();
            left = Expr{ .BinaryOp = .{ .op = op, .left = l_ptr, .right = r_ptr, .line = line, .col = col } };
        }
        return left;
    }

    fn parsePower(self: *Parser) ParseError!Expr {
        const left = try self.parseUnary();
        if (self.current().kind == token.TokenType.Power) {
            const op_tok = self.advance();
            const op = op_tok.value;
            const right = try self.parsePower();
            const l_ptr = try self.allocator.create(Expr);
            l_ptr.* = left;
            const r_ptr = try self.allocator.create(Expr);
            r_ptr.* = right;
            const line = l_ptr.e_line();
            const col = l_ptr.e_col();
            return Expr{ .BinaryOp = .{ .op = op, .left = l_ptr, .right = r_ptr, .line = line, .col = col } };
        }
        return left;
    }

    fn parseUnary(self: *Parser) ParseError!Expr {
        const l = self.current().line;
        const c = self.current().col;
        if (self.current().kind == token.TokenType.Ampersand) {
            _ = self.advance();
            const is_mut = self.current().kind == token.TokenType.Mut;
            if (is_mut) _ = self.advance();
            const expr = try self.parseUnary();
            const e_ptr = try self.allocator.create(Expr);
            e_ptr.* = expr;
            return Expr{ .AddressOf = .{ .expr = e_ptr, .is_mut = is_mut, .line = l, .col = c } };
        }
        if (self.current().kind == token.TokenType.Star) {
            _ = self.advance();
            const expr = try self.parseUnary();
            const e_ptr = try self.allocator.create(Expr);
            e_ptr.* = expr;
            return Expr{ .Deref = .{ .expr = e_ptr, .line = l, .col = c } };
        }
        const unary_kinds = [_]token.TokenType{ .Minus, .Not };
        if (self.matchKind(&unary_kinds)) {
            const op_tok = self.advance();
            const op = op_tok.value;
            const expr = try self.parseUnary();
            const e_ptr = try self.allocator.create(Expr);
            e_ptr.* = expr;
            return Expr{ .UnaryOp = .{ .op = op, .expr = e_ptr, .line = l, .col = c } };
        }
        return self.parsePostfix();
    }

    fn parsePostfix(self: *Parser) ParseError!Expr {
        var e = try self.parsePrimary();
        while (true) {
            if (self.current().kind == token.TokenType.LParen) {
                e = try self.parseCall(e);
            } else if (self.current().kind == token.TokenType.LBracket) {
                e = try self.parseIndex(e);
            } else if (self.current().kind == token.TokenType.Dot) {
                _ = self.advance();
                const p = try self.expectIdentifierOrKeyword();
                const e_ptr = try self.allocator.create(Expr);
                e_ptr.* = e;
                e = Expr{
                    .PropertyAccess = .{
                        .target = e_ptr,
                        .prop = p.value, // owned
                        .line = p.line,
                        .col = p.col,
                    },
                };
            } else if (self.current().kind == token.TokenType.Arrow) {
                _ = self.advance();
                const m = try self.expectIdentifierOrKeyword();
                const e_ptr = try self.allocator.create(Expr);
                e_ptr.* = e;
                e = Expr{
                    .PointerMember = .{
                        .expr = e_ptr,
                        .member = m.value, // owned
                        .line = m.line,
                        .col = m.col,
                    },
                };
            } else {
                break;
            }
        }
        return e;
    }

    fn parseCall(self: *Parser, callee: Expr) ParseError!Expr {
        const l = self.current().line;
        const c = self.current().col;
        _ = self.advance(); // consume '('
        var args: std.ArrayList(*Expr) = .empty;
        self.skipNewlines();
        while (self.current().kind == token.TokenType.Indent or
            self.current().kind == token.TokenType.Dedent)
        {
            _ = self.advance();
            self.skipNewlines();
        }
        if (!self.matchKind(&.{token.TokenType.RParen})) {
            {
                const arg = try self.parseExpression();
                const a_ptr = try self.allocator.create(Expr);
                a_ptr.* = arg;
                try args.append(self.allocator, a_ptr);
            }
            while (self.current().kind == token.TokenType.Comma or
                self.current().kind == token.TokenType.Newline)
            {
                if (self.current().kind == token.TokenType.Comma) {
                    _ = self.advance();
                } else {
                    self.skipNewlines();
                }
                self.skipNewlines();
                while (self.current().kind == token.TokenType.Indent or
                    self.current().kind == token.TokenType.Dedent)
                {
                    _ = self.advance();
                    self.skipNewlines();
                }
                if (self.current().kind == token.TokenType.RParen) break;
                const arg = try self.parseExpression();
                const a_ptr = try self.allocator.create(Expr);
                a_ptr.* = arg;
                try args.append(self.allocator, a_ptr);
            }
        }
        self.skipNewlines();
        while (self.current().kind == token.TokenType.Dedent) {
            _ = self.advance();
            self.skipNewlines();
        }
        _ = try self.expect(token.TokenType.RParen, null);
        const callee_ptr = try self.allocator.create(Expr);
        callee_ptr.* = callee;
        return Expr{ .CallExpr = .{ .callee = callee_ptr, .args = args, .line = l, .col = c } };
    }

    fn parseIndex(self: *Parser, target: Expr) ParseError!Expr {
        const l = self.current().line;
        const c = self.current().col;
        _ = self.advance(); // consume '['
        const index = try self.parseExpression();
        _ = try self.expect(token.TokenType.RBracket, null);
        const t_ptr = try self.allocator.create(Expr);
        t_ptr.* = target;
        const i_ptr = try self.allocator.create(Expr);
        i_ptr.* = index;
        return Expr{ .IndexAccess = .{ .target = t_ptr, .index = i_ptr, .line = l, .col = c } };
    }

    fn parsePrimary(self: *Parser) ParseError!Expr {
        // 浅拷贝当前 token 用于读取其字段
        const t = self.current().*;
        switch (t.kind) {
            token.TokenType.Int => {
                _ = self.advance();
                const val = std.fmt.parseInt(i64, t.value, 10) catch {
                    return self.syntaxError("无效整数字面量", t.line, t.col);
                };
                return Expr{ .IntLiteral = .{ .val = val, .line = t.line, .col = t.col } };
            },
            token.TokenType.Float => {
                _ = self.advance();
                const val = std.fmt.parseFloat(f64, t.value) catch {
                    return self.syntaxError("无效浮点数字面量", t.line, t.col);
                };
                return Expr{ .FloatLiteral = .{ .val = val, .line = t.line, .col = t.col } };
            },
            token.TokenType.String => {
                _ = self.advance();
                return Expr{ .StringLiteral = .{
                    .val = try self.allocator.dupe(u8, t.value),
                    .line = t.line,
                    .col = t.col,
                } };
            },
            token.TokenType.True, token.TokenType.False => {
                _ = self.advance();
                return Expr{ .BoolLiteral = .{ .val = t.kind == .True, .line = t.line, .col = t.col } };
            },
            token.TokenType.Nil => {
                _ = self.advance();
                return Expr{ .NilLiteral = .{ .line = t.line, .col = t.col } };
            },
            token.TokenType.New => return self.parseNewExpr(),
            token.TokenType.Move => return self.parseMoveExpr(),
            token.TokenType.In => {
                _ = self.advance();
                return Expr{ .Identifier = .{
                    .name = try self.allocator.dupe(u8, t.value),
                    .line = t.line,
                    .col = t.col,
                } };
            },
            token.TokenType.Identifier => {
                _ = self.advance();
                return Expr{ .Identifier = .{
                    .name = try self.allocator.dupe(u8, t.value),
                    .line = t.line,
                    .col = t.col,
                } };
            },
            token.TokenType.LBracket => return self.parseArray(),
            token.TokenType.LBrace => return self.parseMap(),
            token.TokenType.LParen => {
                _ = self.advance();
                const e = try self.parseExpression();
                _ = try self.expect(token.TokenType.RParen, null);
                return e;
            },
            token.TokenType.Match => {
                // match 关键字在表达式位置作为标识符
                _ = self.advance();
                return Expr{ .Identifier = .{
                    .name = try self.allocator.dupe(u8, "match"),
                    .line = t.line,
                    .col = t.col,
                } };
            },
            token.TokenType.Hash => {
                // 宏调用表达式
                _ = self.advance();
                const name = try self.expectIdentifierOrKeyword();
                _ = try self.expect(token.TokenType.LParen, "期望 '('");
                var args: std.ArrayList(*Expr) = .empty;
                if (self.current().kind != token.TokenType.RParen) {
                    while (true) {
                        const arg = try self.parseExpression();
                        const a_ptr = try self.allocator.create(Expr);
                        a_ptr.* = arg;
                        try args.append(self.allocator, a_ptr);
                        if (self.current().kind == token.TokenType.RParen) break;
                        _ = try self.expect(token.TokenType.Comma, "期望 ',' 或 ')'");
                    }
                }
                _ = try self.expect(token.TokenType.RParen, "期望 ')'");
                return Expr{
                    .MacroCall = .{
                        .name = name.value, // owned
                        .args = args,
                        .line = t.line,
                        .col = t.col,
                    },
                };
            },
            else => return self.syntaxError("意外token", t.line, t.col),
        }
    }

    fn parseNewExpr(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'new'
        const l = t.line;
        const c = t.col;
        const tn_tok = try self.expectIdentifierOrKeyword();
        const type_name = tn_tok.value; // owned

        var positional: std.ArrayList(*Expr) = .empty;
        if (self.current().kind == token.TokenType.Lt) {
            _ = self.advance();
            {
                const ta = try self.parseType();
                const ta_ptr = try self.allocator.create(Expr);
                ta_ptr.* = ta;
                try positional.append(self.allocator, ta_ptr);
            }
            while (self.current().kind == token.TokenType.Comma) {
                _ = self.advance();
                const ta = try self.parseType();
                const ta_ptr = try self.allocator.create(Expr);
                ta_ptr.* = ta;
                try positional.append(self.allocator, ta_ptr);
            }
            _ = try self.expect(token.TokenType.Gt, null);
        }

        var named: std.ArrayList(*Expr) = .empty;
        if (self.current().kind == token.TokenType.LParen) {
            _ = self.advance();
            if (!self.matchKind(&.{token.TokenType.RParen})) {
                {
                    const arg = try self.parseExpression();
                    const a_ptr = try self.allocator.create(Expr);
                    a_ptr.* = arg;
                    try named.append(self.allocator, a_ptr);
                }
                while (self.current().kind == token.TokenType.Comma) {
                    _ = self.advance();
                    const arg = try self.parseExpression();
                    const a_ptr = try self.allocator.create(Expr);
                    a_ptr.* = arg;
                    try named.append(self.allocator, a_ptr);
                }
            }
            _ = try self.expect(token.TokenType.RParen, null);
        }
        return Expr{ .NewExpr = .{
            .type_name = type_name,
            .positional = positional,
            .named = named,
            .line = l,
            .col = c,
        } };
    }

    fn parseMoveExpr(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'move'
        const l = t.line;
        const c = t.col;
        const expr = try self.parseUnary();
        const e_ptr = try self.allocator.create(Expr);
        e_ptr.* = expr;
        return Expr{ .MoveExpr = .{ .expr = e_ptr, .line = l, .col = c } };
    }

    fn parseArray(self: *Parser) ParseError!Expr {
        const l = self.current().line;
        const c = self.current().col;
        _ = self.advance(); // consume '['
        var elements: std.ArrayList(*Expr) = .empty;
        if (!self.matchKind(&.{token.TokenType.RBracket})) {
            {
                const e = try self.parseExpression();
                const e_ptr = try self.allocator.create(Expr);
                e_ptr.* = e;
                try elements.append(self.allocator, e_ptr);
            }
            while (self.current().kind == token.TokenType.Comma) {
                _ = self.advance();
                const e = try self.parseExpression();
                const e_ptr = try self.allocator.create(Expr);
                e_ptr.* = e;
                try elements.append(self.allocator, e_ptr);
            }
        }
        _ = try self.expect(token.TokenType.RBracket, null);
        return Expr{ .ArrayLiteral = .{ .elements = elements, .line = l, .col = c } };
    }

    fn parseMap(self: *Parser) ParseError!Expr {
        const l = self.current().line;
        const c = self.current().col;
        _ = self.advance(); // consume '{'

        if (self.matchKind(&.{token.TokenType.RBrace})) {
            return Expr{ .MapLiteral = .{
                .pairs = .empty,
                .line = l,
                .col = c,
            } };
        }

        const first = try self.parseExpression();
        if (self.current().kind == token.TokenType.Colon) {
            // Map 字面量 { k: v, ... }
            _ = self.advance();
            const v = try self.parseExpression();
            var pairs: std.ArrayList(MapPair) = .empty;
            {
                const kp = try self.allocator.create(Expr);
                kp.* = first;
                const vp = try self.allocator.create(Expr);
                vp.* = v;
                try pairs.append(self.allocator, MapPair{ .key = kp, .value = vp });
            }
            while (self.current().kind == token.TokenType.Comma) {
                _ = self.advance();
                const kk = try self.parseExpression();
                _ = try self.expect(token.TokenType.Colon, null);
                const vv = try self.parseExpression();
                const kp = try self.allocator.create(Expr);
                kp.* = kk;
                const vp = try self.allocator.create(Expr);
                vp.* = vv;
                try pairs.append(self.allocator, MapPair{ .key = kp, .value = vp });
            }
            _ = try self.expect(token.TokenType.RBrace, null);
            return Expr{ .MapLiteral = .{ .pairs = pairs, .line = l, .col = c } };
        } else {
            // 无冒号 → 数组字面量（兼容 { expr, ... } 语法）
            var elements: std.ArrayList(*Expr) = .empty;
            {
                const ep = try self.allocator.create(Expr);
                ep.* = first;
                try elements.append(self.allocator, ep);
            }
            while (self.current().kind == token.TokenType.Comma) {
                _ = self.advance();
                const elem = try self.parseExpression();
                const ep = try self.allocator.create(Expr);
                ep.* = elem;
                try elements.append(self.allocator, ep);
            }
            _ = try self.expect(token.TokenType.RBrace, null);
            return Expr{ .ArrayLiteral = .{ .elements = elements, .line = l, .col = c } };
        }
    }

    pub fn parseType(self: *Parser) ParseError!Expr {
        const l = self.current().line;
        const c = self.current().col;

        // var 动态类型已移除
        if (self.current().kind == token.TokenType.VarT) {
            _ = self.advance();
            return self.syntaxError(
                "var 动态类型已移除，VX 为纯静态类型语言，请使用具体类型（如 int、bool、pointer）",
                l,
                c,
            );
        }

        const type_builtin = [_]token.TokenType{ .IntT, .FloatT, .DoubleT, .BoolT, .VoidT };
        const nm = if (self.matchKind(&type_builtin)) blk1: {
            const t = self.advance();
            break :blk1 t.value; // owned
        } else if (self.current().kind == token.TokenType.Identifier) blk2: {
            const t = self.advance();
            break :blk2 t.value; // owned
        } else {
            return self.syntaxError("期望类型", l, c);
        };

        var type_args: std.ArrayList(*Expr) = .empty;
        if (self.current().kind == token.TokenType.Lt) {
            _ = self.advance();
            {
                const ta = try self.parseType();
                const ta_ptr = try self.allocator.create(Expr);
                ta_ptr.* = ta;
                try type_args.append(self.allocator, ta_ptr);
            }
            while (self.current().kind == token.TokenType.Comma) {
                _ = self.advance();
                const ta = try self.parseType();
                const ta_ptr = try self.allocator.create(Expr);
                ta_ptr.* = ta;
                try type_args.append(self.allocator, ta_ptr);
            }
            _ = try self.expect(token.TokenType.Gt, null);
        }

        if (type_args.items.len > 0) {
            // 构建泛型类型名 "Type<A, B>"
            var buf: std.ArrayList(u8) = .empty;
            try buf.appendSlice(self.allocator, nm);
            try buf.append(self.allocator, '<');
            for (type_args.items, 0..) |ta, i| {
                if (i > 0) try buf.appendSlice(self.allocator, ", ");
                try buf.appendSlice(self.allocator, expr_to_type_name(ta));
            }
            try buf.append(self.allocator, '>');
            const full_name = try buf.toOwnedSlice(self.allocator);
            return Expr{ .TypeExpr = .{ .name = full_name, .line = l, .col = c } };
        }
        return Expr{ .TypeExpr = .{ .name = nm, .line = l, .col = c } };
    }

    // ==================== 语句解析（来自 stmt.rs） ====================

    pub fn parseStatement(self: *Parser) ParseError!Expr {
        self.skipNewlines();
        // 跳过残留的 Indent/Dedent
        while (self.current().kind == token.TokenType.Indent or
            self.current().kind == token.TokenType.Dedent)
        {
            _ = self.advance();
            self.skipNewlines();
        }

        switch (self.current().kind) {
            token.TokenType.Struct => return self.parseStructDecl(),
            token.TokenType.Class => return self.parseClassDecl(),
            token.TokenType.Enum => return self.parseEnumDecl(),
            token.TokenType.Union => return self.parseUnionDecl(),
            token.TokenType.Macro => return self.parseMacroDef(),
            token.TokenType.Hash => return self.parseMacroCallStmt(),
            token.TokenType.Identifier => {
                if (self.peek(1).kind == token.TokenType.Colon) {
                    return self.parseVarDecl();
                }
                // 否则作为表达式语句
            },
            token.TokenType.Mut => return self.parseVarDecl(),
            token.TokenType.Import => return self.parseImportStmt(),
            token.TokenType.Func => return self.parseFuncDecl(),
            token.TokenType.Extern => return self.parseExternDecl(),
            token.TokenType.If => return self.parseIfStmt(),
            token.TokenType.Else, token.TokenType.Elif => {
                return self.syntaxError(
                    "意外的 else/elif (else/elif 必须紧跟 if/elif 块)",
                    self.current().line,
                    self.current().col,
                );
            },
            token.TokenType.Match => return self.parseMatchStmt(),
            token.TokenType.While => return self.parseWhileStmt(),
            token.TokenType.For => return self.parseForStmt(),
            token.TokenType.Return => return self.parseReturnStmt(),
            token.TokenType.Loop => return self.parseLoopStmt(),
            token.TokenType.Break => {
                const t = self.advance();
                const label = if (self.current().kind == token.TokenType.Identifier or
                    self.current().kind == token.TokenType.Loop)
                blk: {
                    const lt = self.advance();
                    break :blk lt.value; // owned
                } else null;
                return Expr{ .BreakStmt = .{ .label = label, .line = t.line, .col = t.col } };
            },
            token.TokenType.Continue => {
                const t = self.advance();
                const label = if (self.current().kind == token.TokenType.Identifier or
                    self.current().kind == token.TokenType.Loop)
                blk: {
                    const lt = self.advance();
                    break :blk lt.value;
                } else null;
                return Expr{ .ContinueStmt = .{ .label = label, .line = t.line, .col = t.col } };
            },
            token.TokenType.VarT => {
                const t = self.advance();
                return self.syntaxError(
                    "var 类型推断已移除，VX 为纯静态类型语言，请使用 `name: Type = value` 语法",
                    t.line,
                    t.col,
                );
            },
            else => {
                const e = try self.parseExpression();
                const line = e.e_line();
                const col = e.e_col();
                const e_ptr = try self.allocator.create(Expr);
                e_ptr.* = e;
                return Expr{ .ExprStmt = .{ .expr = e_ptr, .line = line, .col = col } };
            },
        }

        // Identifier 非 var-decl 时作为表达式语句
        const e = try self.parseExpression();
        const line = e.e_line();
        const col = e.e_col();
        const e_ptr = try self.allocator.create(Expr);
        e_ptr.* = e;
        return Expr{ .ExprStmt = .{ .expr = e_ptr, .line = line, .col = col } };
    }

    fn parseStructDecl(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'struct'
        const l = t.line;
        const c = t.col;
        const n_tok = try self.expectIdentifierOrKeyword();
        const name = n_tok.value; // owned
        const gp = try self.parseGenericParams();
        _ = try self.expect(token.TokenType.Colon, null);
        self.skipNewlines();

        var fields: std.ArrayList(FieldDef) = .empty;
        var methods: std.ArrayList(*Expr) = .empty;
        _ = try self.expect(token.TokenType.Indent, null);

        while (!self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) {
            self.skipNewlines();
            if (self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) break;
            if (self.current().kind == token.TokenType.Func) {
                const func = try self.parseFuncDecl();
                const f_ptr = try self.allocator.create(Expr);
                f_ptr.* = func;
                try methods.append(self.allocator, f_ptr);
            } else {
                const fn_tok = try self.expectIdentifierOrKeyword();
                const fn_name = fn_tok.value; // owned
                _ = try self.expect(token.TokenType.Colon, null);
                const ft = try self.parseType();
                try fields.append(self.allocator, FieldDef{
                    .name = fn_name,
                    .field_type = try self.allocator.dupe(u8, expr_to_type_name(&ft)),
                });
            }
        }

        while (self.current().kind == token.TokenType.Dedent) {
            _ = self.advance();
        }
        return Expr{ .StructDecl = .{
            .name = name,
            .generic_params = gp,
            .fields = fields,
            .methods = methods,
            .line = l,
            .col = c,
        } };
    }

    fn parseClassDecl(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'class'
        const l = t.line;
        const c = t.col;
        const n_tok = try self.expectIdentifierOrKeyword();
        const name = n_tok.value; // owned
        const gp = try self.parseGenericParams();

        var parent: ?[]const u8 = null;
        var interfaces: std.ArrayList([]const u8) = .empty;

        if (self.current().kind == token.TokenType.Colon) {
            _ = self.advance();
            self.skipNewlines();
            if (self.current().kind == token.TokenType.Identifier) {
                const p_tok = try self.expectIdentifierOrKeyword();
                parent = p_tok.value; // owned
                while (self.current().kind == token.TokenType.Comma) {
                    _ = self.advance();
                    const i_tok = try self.expectIdentifierOrKeyword();
                    try interfaces.append(self.allocator, i_tok.value); // owned
                }
                if (self.current().kind == token.TokenType.Colon) {
                    _ = self.advance();
                }
            }
        }

        self.skipNewlines();
        _ = try self.expect(token.TokenType.Indent, null);

        var fields: std.ArrayList(ClassFieldDef) = .empty;
        var methods: std.ArrayList(*Expr) = .empty;

        while (!self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) {
            self.skipNewlines();
            if (self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) break;

            if (self.current().kind == token.TokenType.Func) {
                const func = try self.parseFuncDecl();
                const f_ptr = try self.allocator.create(Expr);
                f_ptr.* = func;
                try methods.append(self.allocator, f_ptr);
            } else {
                const fn_tok = try self.expectIdentifierOrKeyword();
                const fn_name = fn_tok.value; // owned
                if (self.current().kind == token.TokenType.Colon) {
                    _ = self.advance();
                    const ft = try self.parseType();
                    const ftype = try self.allocator.dupe(u8, expr_to_type_name(&ft));
                    if (self.current().kind == token.TokenType.Assign) {
                        _ = self.advance();
                        _ = try self.parseExpression();
                    }
                    try fields.append(self.allocator, ClassFieldDef{
                        .name = fn_name,
                        .field_type = ftype,
                        .visibility = try self.allocator.dupe(u8, "public"),
                    });
                } else if (self.current().kind == token.TokenType.Assign) {
                    return self.syntaxError(
                        "类字段必须提供类型注解（VX 已移除 var 动态类型）",
                        self.current().line,
                        self.current().col,
                    );
                } else {
                    return self.syntaxError(
                        "类字段声明需要类型注解或默认值",
                        self.current().line,
                        self.current().col,
                    );
                }
            }
        }

        while (self.current().kind == token.TokenType.Dedent) {
            _ = self.advance();
        }
        return Expr{ .ClassDecl = .{
            .name = name,
            .generic_params = gp,
            .fields = fields,
            .methods = methods,
            .parent = parent,
            .interfaces = interfaces,
            .line = l,
            .col = c,
        } };
    }

    fn parseEnumDecl(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'enum'
        const l = t.line;
        const c = t.col;
        const n_tok = try self.expectIdentifierOrKeyword();
        const name = n_tok.value; // owned
        _ = try self.expect(token.TokenType.Colon, null);
        self.skipNewlines();
        _ = try self.expect(token.TokenType.Indent, null);

        var variants: std.ArrayList(EnumVariant) = .empty;
        var auto: i64 = 0;

        while (!self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) {
            self.skipNewlines();
            if (self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) break;

            const vn_tok = try self.expectIdentifierOrKeyword();
            const vn = vn_tok.value; // owned
            var vv = auto;
            if (self.current().kind == token.TokenType.Assign) {
                _ = self.advance();
                const tv = try self.expect(token.TokenType.Int, null);
                vv = std.fmt.parseInt(i64, tv.value, 10) catch {
                    return self.syntaxError("枚举值必须是整数", tv.line, tv.col);
                };
            }
            try variants.append(self.allocator, EnumVariant{ .name = vn, .value = vv });
            auto = vv + 1;
        }

        while (self.current().kind == token.TokenType.Dedent) {
            _ = self.advance();
        }
        return Expr{ .EnumDecl = .{ .name = name, .variants = variants, .line = l, .col = c } };
    }

    fn parseUnionDecl(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'union'
        const l = t.line;
        const c = t.col;
        const n_tok = try self.expectIdentifierOrKeyword();
        const name = n_tok.value; // owned
        _ = try self.expect(token.TokenType.Colon, null);
        self.skipNewlines();
        _ = try self.expect(token.TokenType.Indent, null);

        var fields: std.ArrayList(UnionField) = .empty;
        while (!self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) {
            self.skipNewlines();
            if (self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) break;

            const fn_tok = try self.expectIdentifierOrKeyword();
            const fn_name = fn_tok.value; // owned
            _ = try self.expect(token.TokenType.Colon, null);
            const ft = try self.parseType();
            const ftype = try self.allocator.dupe(u8, expr_to_type_name(&ft));
            try fields.append(self.allocator, UnionField{ .name = fn_name, .field_type = ftype });
        }

        while (self.current().kind == token.TokenType.Dedent) {
            _ = self.advance();
        }
        return Expr{ .UnionDecl = .{ .name = name, .fields = fields, .line = l, .col = c } };
    }

    fn parseVarDecl(self: *Parser) ParseError!Expr {
        const is_mut = self.current().kind == token.TokenType.Mut;
        if (is_mut) _ = self.advance();

        const nm_tok = try self.expectIdentifierOrKeyword();
        const nm = nm_tok.value; // owned
        _ = try self.expect(token.TokenType.Colon, null);

        var th = try self.parseType();
        const line = th.e_line();
        const col = th.e_col();
        while (self.current().kind == token.TokenType.Star) {
            _ = self.advance();
            th = Expr{ .TypeExpr = .{
                .name = try self.allocator.dupe(u8, "pointer"),
                .line = line,
                .col = col,
            } };
        }

        var init_val = Expr{ .NilLiteral = .{ .line = line, .col = col } };
        if (self.current().kind == token.TokenType.Assign) {
            _ = self.advance();
            init_val = try self.parseExpression();
        }

        const type_ptr = try self.allocator.create(Expr);
        type_ptr.* = th;
        const init_ptr = try self.allocator.create(Expr);
        init_ptr.* = init_val;
        return Expr{ .VarDecl = .{
            .name = nm,
            .type_expr = type_ptr,
            .init = init_ptr,
            .is_const = !is_mut,
            .line = line,
            .col = col,
        } };
    }

    fn parseFuncDecl(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'func'
        const l = t.line;
        const c = t.col;
        const n_tok = try self.expectIdentifierOrKeyword();
        const name = n_tok.value; // owned
        const gp = try self.parseGenericParams();
        _ = try self.expect(token.TokenType.LParen, null);

        var params: std.ArrayList(ParamDef) = .empty;
        if (!self.matchKind(&.{token.TokenType.RParen})) {
            {
                const pn_tok = try self.expectIdentifierOrKeyword();
                const pn = pn_tok.value; // owned
                _ = try self.expect(token.TokenType.Colon, null);
                const pt = try self.parseType();
                try params.append(self.allocator, ParamDef{
                    .name = pn,
                    .param_type = try self.allocator.dupe(u8, expr_to_type_name(&pt)),
                });
            }
            while (self.current().kind == token.TokenType.Comma) {
                _ = self.advance();
                const pn_tok = try self.expectIdentifierOrKeyword();
                const pn = pn_tok.value;
                _ = try self.expect(token.TokenType.Colon, null);
                const pt = try self.parseType();
                try params.append(self.allocator, ParamDef{
                    .name = pn,
                    .param_type = try self.allocator.dupe(u8, expr_to_type_name(&pt)),
                });
            }
        }
        _ = try self.expect(token.TokenType.RParen, null);

        var return_type: ?[]const u8 = null;
        if (self.current().kind == token.TokenType.Arrow) {
            _ = self.advance();
            const rt = try self.parseType();
            return_type = try self.allocator.dupe(u8, expr_to_type_name(&rt));
        }

        const body = try self.parseBlock();
        return Expr{ .FuncDecl = .{
            .name = name,
            .generic_params = gp,
            .params = params,
            .return_type = return_type,
            .body = body,
            .line = l,
            .col = c,
        } };
    }

    fn parseExternDecl(self: *Parser) ParseError!Expr {
        _ = self.advance(); // consume 'extern'

        // 可选 "C" 字符串字面量
        if (self.current().kind == token.TokenType.String and
            std.mem.eql(u8, self.current().value, "C"))
        {
            _ = self.advance();
        }

        if (self.current().kind != token.TokenType.Func) {
            return self.syntaxError(
                "Expected 'func' after 'extern'",
                self.current().line,
                self.current().col,
            );
        }

        const t = self.advance(); // consume 'func'
        const l = t.line;
        const c = t.col;
        const n_tok = try self.expectIdentifierOrKeyword();
        const name = n_tok.value; // owned
        const gp = try self.parseGenericParams();
        _ = try self.expect(token.TokenType.LParen, null);

        var params: std.ArrayList(ParamDef) = .empty;
        if (!self.matchKind(&.{token.TokenType.RParen})) {
            {
                const pn_tok = try self.expectIdentifierOrKeyword();
                const pn = pn_tok.value;
                _ = try self.expect(token.TokenType.Colon, null);
                const pt = try self.parseType();
                try params.append(self.allocator, ParamDef{
                    .name = pn,
                    .param_type = try self.allocator.dupe(u8, expr_to_type_name(&pt)),
                });
            }
            while (self.current().kind == token.TokenType.Comma) {
                _ = self.advance();
                const pn_tok = try self.expectIdentifierOrKeyword();
                const pn = pn_tok.value;
                _ = try self.expect(token.TokenType.Colon, null);
                const pt = try self.parseType();
                try params.append(self.allocator, ParamDef{
                    .name = pn,
                    .param_type = try self.allocator.dupe(u8, expr_to_type_name(&pt)),
                });
            }
        }
        _ = try self.expect(token.TokenType.RParen, null);

        var return_type: ?[]const u8 = null;
        if (self.current().kind == token.TokenType.Arrow) {
            _ = self.advance();
            const rt = try self.parseType();
            return_type = try self.allocator.dupe(u8, expr_to_type_name(&rt));
        }

        // extern 声明没有函数体
        return Expr{ .ExternDecl = .{
            .name = name,
            .generic_params = gp,
            .params = params,
            .return_type = return_type,
            .line = l,
            .col = c,
        } };
    }

    fn parseIfStmt(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'if'
        const l = t.line;
        const c = t.col;
        const cond = try self.parseExpression();
        const body = try self.parseBlock();

        var elif_branches: std.ArrayList(ElifBranch) = .empty;
        self.skipNewlines();
        while (self.current().kind == token.TokenType.Elif) {
            _ = self.advance();
            const ec = try self.parseExpression();
            const eb = try self.parseBlock();
            const ec_ptr = try self.allocator.create(Expr);
            ec_ptr.* = ec;
            try elif_branches.append(self.allocator, ElifBranch{ .condition = ec_ptr, .body = eb });
            self.skipNewlines();
        }

        var else_branch: ?std.ArrayList(*Expr) = null;
        if (self.current().kind == token.TokenType.Else) {
            _ = self.advance();
            self.skipNewlines();
            else_branch = try self.parseBlock();
        }

        const cond_ptr = try self.allocator.create(Expr);
        cond_ptr.* = cond;
        return Expr{ .IfStmt = .{
            .condition = cond_ptr,
            .then_branch = body,
            .elif_branches = elif_branches,
            .else_branch = else_branch,
            .line = l,
            .col = c,
        } };
    }

    fn parseMatchStmt(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'match'
        const l = t.line;
        const c = t.col;
        const subject = try self.parseExpression();
        const arms = try self.parseMatchArms();
        const subj_ptr = try self.allocator.create(Expr);
        subj_ptr.* = subject;
        return Expr{ .MatchStmt = .{ .target = subj_ptr, .arms = arms, .line = l, .col = c } };
    }

    fn parseMatchArms(self: *Parser) ParseError!std.ArrayList(MatchArm) {
        _ = try self.expect(token.TokenType.Colon, "期望 match 后的 ':'");
        self.skipNewlines();

        if (!self.matchKind(&.{token.TokenType.Indent})) {
            const arm = try self.parseMatchArm();
            var arms: std.ArrayList(MatchArm) = .empty;
            try arms.append(self.allocator, arm);
            return arms;
        }

        _ = self.advance(); // consume Indent
        var arms: std.ArrayList(MatchArm) = .empty;
        while (!self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) {
            self.skipNewlines();
            if (self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) break;
            try arms.append(self.allocator, try self.parseMatchArm());
        }
        _ = try self.expect(token.TokenType.Dedent, null);
        return arms;
    }

    fn parseMatchArm(self: *Parser) ParseError!MatchArm {
        const pattern = try self.parseExpression();
        _ = try self.expect(token.TokenType.Colon, "期望分支模式后的 ':'");
        self.skipNewlines();

        var body: std.ArrayList(*Expr) = .empty;
        if (self.current().kind == token.TokenType.Indent) {
            _ = self.advance();
            while (!self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) {
                self.skipNewlines();
                if (self.matchKind(&.{ token.TokenType.Dedent, token.TokenType.EOF })) break;
                const stmt = try self.parseStatement();
                const s_ptr = try self.allocator.create(Expr);
                s_ptr.* = stmt;
                try body.append(self.allocator, s_ptr);
            }
            if (self.current().kind == token.TokenType.Dedent) {
                _ = self.advance();
            }
        } else {
            const stmt = try self.parseStatement();
            const s_ptr = try self.allocator.create(Expr);
            s_ptr.* = stmt;
            try body.append(self.allocator, s_ptr);
        }

        const pat_ptr = try self.allocator.create(Expr);
        pat_ptr.* = pattern;
        return MatchArm{ .pattern = pat_ptr, .body = body };
    }

    fn parseForStmt(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'for'
        const l = t.line;
        const c = t.col;
        const var_tok = try self.expectIdentifierOrKeyword();
        const var_name = var_tok.value; // owned
        _ = try self.expect(token.TokenType.In, null);
        const iterable = try self.parseExpression();
        const body = try self.parseBlock();
        const iter_ptr = try self.allocator.create(Expr);
        iter_ptr.* = iterable;
        return Expr{ .ForStmt = .{
            .var_name = var_name,
            .iterable = iter_ptr,
            .body = body,
            .line = l,
            .col = c,
        } };
    }

    fn parseWhileStmt(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'while'
        const l = t.line;
        const c = t.col;
        const cond = try self.parseExpression();
        const body = try self.parseBlock();
        const cond_ptr = try self.allocator.create(Expr);
        cond_ptr.* = cond;
        return Expr{ .WhileStmt = .{ .condition = cond_ptr, .body = body, .line = l, .col = c } };
    }

    fn parseLoopStmt(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'loop'
        const l = t.line;
        const c = t.col;
        const label = if (self.current().kind == token.TokenType.Identifier) blk: {
            const lt = self.advance();
            break :blk lt.value; // owned
        } else null;
        if (self.current().kind == token.TokenType.Colon) {
            _ = self.advance();
        }
        const body = try self.parseBlock();
        return Expr{ .LoopStmt = .{ .label = label, .body = body, .line = l, .col = c } };
    }

    fn parseReturnStmt(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'return'
        const l = t.line;
        const c = t.col;

        const value = if (!self.matchKind(&.{
            token.TokenType.Newline, token.TokenType.Dedent, token.TokenType.EOF,
        })) blk: {
            const v = try self.parseExpression();
            const v_ptr = try self.allocator.create(Expr);
            v_ptr.* = v;
            break :blk v_ptr;
        } else null;

        return Expr{ .ReturnStmt = .{ .value = value, .line = l, .col = c } };
    }

    fn parseImportStmt(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'import'
        const l = t.line;
        const c = t.col;

        // 支持点分路径: import std.collections.vec
        const nm_tok = try self.expectIdentifierOrKeyword();
        var nm_buf: std.ArrayList(u8) = .empty;
        try nm_buf.appendSlice(self.allocator, nm_tok.value);
        while (self.current().kind == token.TokenType.Dot) {
            _ = self.advance();
            const next = try self.expectIdentifierOrKeyword();
            try nm_buf.append(self.allocator, '.');
            try nm_buf.appendSlice(self.allocator, next.value);
        }
        const path = try nm_buf.toOwnedSlice(self.allocator);

        var alias: ?[]const u8 = null;
        var dirs: std.ArrayList([]const u8) = .empty;

        if (self.current().kind == token.TokenType.As) {
            _ = self.advance();
            const a_tok = try self.expectIdentifierOrKeyword();
            alias = a_tok.value; // owned
        }

        while (self.current().kind == token.TokenType.String) {
            const s = self.advance();
            try dirs.append(self.allocator, try self.allocator.dupe(u8, s.value));
        }

        return Expr{ .ImportStmt = .{ .path = path, .alias = alias, .dirs = dirs, .line = l, .col = c } };
    }

    /// 解析代码块（冒号后的缩进块或单行语句）
    pub fn parseBlock(self: *Parser) ParseError!std.ArrayList(*Expr) {
        var stmts: std.ArrayList(*Expr) = .empty;
        self.skipNewlines();

        if (self.current().kind == token.TokenType.Colon) {
            _ = self.advance();
            self.skipNewlines();
            if (!self.matchKind(&.{token.TokenType.Indent})) {
                // 单行块
                const stmt = try self.parseStatement();
                const s_ptr = try self.allocator.create(Expr);
                s_ptr.* = stmt;
                try stmts.append(self.allocator, s_ptr);
                return stmts;
            }
            _ = self.advance(); // consume Indent
        }

        const block_enders = [_]token.TokenType{ .Dedent, .EOF, .Else, .Elif };
        while (!self.matchKind(&block_enders)) {
            self.skipNewlines();
            if (self.matchKind(&block_enders)) break;
            const stmt = try self.parseStatement();
            const s_ptr = try self.allocator.create(Expr);
            s_ptr.* = stmt;
            try stmts.append(self.allocator, s_ptr);
        }

        if (self.current().kind == token.TokenType.Dedent) {
            _ = self.advance();
        }
        return stmts;
    }

    /// 顶层解析入口
    pub fn parse(self: *Parser) ParseError!std.ArrayList(*Expr) {
        var stmts: std.ArrayList(*Expr) = .empty;
        while (!self.matchKind(&.{token.TokenType.EOF})) {
            self.skipNewlines();
            while (self.current().kind == token.TokenType.Dedent) {
                _ = self.advance();
            }
            if (self.matchKind(&.{token.TokenType.EOF})) break;
            const stmt = try self.parseStatement();
            const s_ptr = try self.allocator.create(Expr);
            s_ptr.* = stmt;
            try stmts.append(self.allocator, s_ptr);
        }
        return stmts;
    }

    // ==================== 宏系统解析 ====================

    /// 解析宏定义: macro name(params) { body }
    fn parseMacroDef(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume 'macro'
        const l = t.line;
        const c = t.col;
        const name_tok = try self.expectIdentifierOrKeyword();
        const name = name_tok.value; // owned

        _ = try self.expect(token.TokenType.LParen, "期望 '('");
        var params: std.ArrayList([]const u8) = .empty;
        if (self.current().kind != token.TokenType.RParen) {
            while (true) {
                const param_tok = try self.expectIdentifierOrKeyword();
                try params.append(self.allocator, param_tok.value); // owned
                if (self.current().kind == token.TokenType.RParen) break;
                _ = try self.expect(token.TokenType.Comma, "期望 ',' 或 ')'");
            }
        }
        _ = try self.expect(token.TokenType.RParen, "期望 ')'");

        _ = try self.expect(token.TokenType.LBrace, "期望 '{'");
        var body: std.ArrayList(*Expr) = .empty;
        while (self.current().kind != token.TokenType.RBrace and
            self.current().kind != token.TokenType.EOF)
        {
            const stmt = try self.parseStatement();
            const s_ptr = try self.allocator.create(Expr);
            s_ptr.* = stmt;
            try body.append(self.allocator, s_ptr);
        }
        _ = try self.expect(token.TokenType.RBrace, "期望 '}'");

        return Expr{ .MacroDef = .{ .name = name, .params = params, .body = body, .line = l, .col = c } };
    }

    /// 解析宏调用语句: #macro_name(args)
    fn parseMacroCallStmt(self: *Parser) ParseError!Expr {
        const t = self.advance(); // consume '#'
        const l = t.line;
        const c = t.col;
        const name_tok = try self.expectIdentifierOrKeyword();
        const name = name_tok.value; // owned

        _ = try self.expect(token.TokenType.LParen, "期望 '('");
        var args: std.ArrayList(*Expr) = .empty;
        if (self.current().kind != token.TokenType.RParen) {
            while (true) {
                const arg = try self.parseExpression();
                const a_ptr = try self.allocator.create(Expr);
                a_ptr.* = arg;
                try args.append(self.allocator, a_ptr);
                if (self.current().kind == token.TokenType.RParen) break;
                _ = try self.expect(token.TokenType.Comma, "期望 ',' 或 ')'");
            }
        }
        _ = try self.expect(token.TokenType.RParen, "期望 ')'");

        return Expr{ .MacroCall = .{
            .name = name,
            .args = args,
            .line = l,
            .col = c,
        } };
    }
};
