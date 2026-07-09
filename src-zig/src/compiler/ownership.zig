const std = @import("std");
const Expr = @import("../parser/ast.zig").Expr;
const get_src_line = @import("../parser/ast.zig").get_src_line;
const expr_to_type_name = @import("../parser/ast.zig").expr_to_type_name;

// ==================== OwnershipState ====================

pub const OwnershipState = enum {
    Owned,
    Moved,
    Borrowed,
    Freed,
};

// ==================== BorrowKind ====================

pub const BorrowKind = enum {
    Immutable,
    Mutable,
};

const BorrowInfo = struct { owner: []const u8, kind: BorrowKind };
const ActiveBorrow = struct { borrower: []const u8, kind: BorrowKind };

// ==================== OwnershipChecker ====================

pub const OwnershipChecker = struct {
    allocator: std.mem.Allocator,
    source: []const u8,
    scopes: std.ArrayList(std.StringHashMap(OwnershipState)),
    heap_vars: std.StringHashMap(void),
    mut_vars: std.StringHashMap(void),
    borrows: std.StringHashMap(BorrowInfo),
    var_types: std.StringHashMap([]const u8),
    errors: std.ArrayList([]const u8),
    warnings: std.ArrayList([]const u8),

    pub fn init(allocator: std.mem.Allocator, source: []const u8) OwnershipChecker {
        var scopes: std.ArrayList(std.StringHashMap(OwnershipState)) = .empty;
        scopes.append(allocator, std.StringHashMap(OwnershipState).init(allocator)) catch @panic("OOM");
        return OwnershipChecker{
            .allocator = allocator,
            .source = source,
            .scopes = scopes,
            .heap_vars = std.StringHashMap(void).init(allocator),
            .mut_vars = std.StringHashMap(void).init(allocator),
            .borrows = std.StringHashMap(BorrowInfo).init(allocator),
            .var_types = std.StringHashMap([]const u8).init(allocator),
            .errors = .empty,
            .warnings = .empty,
        };
    }

    pub fn deinit(self: *OwnershipChecker) void {
        for (self.scopes.items) |*scope| scope.deinit();
        self.scopes.deinit(self.allocator);
        self.heap_vars.deinit();
        self.mut_vars.deinit();
        var b_iter = self.borrows.iterator();
        while (b_iter.next()) |entry| {
            self.allocator.free(entry.value_ptr.owner);
        }
        self.borrows.deinit();
        var vt_iter = self.var_types.iterator();
        while (vt_iter.next()) |entry| {
            self.allocator.free(entry.key_ptr.*);
            self.allocator.free(entry.value_ptr.*);
        }
        self.var_types.deinit();
        for (self.errors.items) |e| self.allocator.free(e);
        self.errors.deinit(self.allocator);
        for (self.warnings.items) |w| self.allocator.free(w);
        self.warnings.deinit(self.allocator);
    }

    /// 标量类型默认实现 Copy 语义，赋值/传参时复制而非移动所有权。
    fn isCopyType(t: []const u8) bool {
        return std.mem.eql(u8, t, "int") or
            std.mem.eql(u8, t, "float") or
            std.mem.eql(u8, t, "double") or
            std.mem.eql(u8, t, "bool");
    }

    pub fn pushScope(self: *OwnershipChecker) void {
        self.scopes.append(self.allocator, std.StringHashMap(OwnershipState).init(self.allocator)) catch @panic("OOM");
    }

    pub fn popScope(self: *OwnershipChecker) void {
        if (self.scopes.items.len <= 1) return;
        var scope = self.scopes.pop().?;
        var scope_iter = scope.iterator();
        while (scope_iter.next()) |entry| {
            if (self.heap_vars.contains(entry.key_ptr.*) and entry.value_ptr.* == .Owned) {
                const msg = std.fmt.allocPrint(
                    self.allocator,
                    "[error] 堆变量 '{s}' 在作用域结束时未被显式释放（内存泄漏），请调用 free({s})",
                    .{ entry.key_ptr.*, entry.key_ptr.* },
                ) catch @panic("OOM");
                self.errors.append(self.allocator, msg) catch @panic("OOM");
            }
        }
        scope.deinit();

        // 清理在当前作用域中创建的借用
        var to_remove: std.ArrayList([]const u8) = .empty;
        defer {
            for (to_remove.items) |key| self.allocator.free(key);
            to_remove.deinit(self.allocator);
        }
        var b_iter = self.borrows.iterator();
        while (b_iter.next()) |entry| {
            if (scope.contains(entry.value_ptr.owner)) {
                const key_dup = self.allocator.dupe(u8, entry.key_ptr.*) catch @panic("OOM");
                to_remove.append(self.allocator, key_dup) catch @panic("OOM");
            }
        }
        for (to_remove.items) |b| {
            self.endBorrow(b);
        }
    }

    pub fn declareVar(self: *OwnershipChecker, name: []const u8, is_heap: bool, is_mut: bool, ty: []const u8) void {
        if (self.scopes.items.len > 0) {
            self.scopes.items[self.scopes.items.len - 1].put(name, .Owned) catch @panic("OOM");
        }
        if (is_heap) {
            self.heap_vars.put(name, {}) catch @panic("OOM");
        }
        if (is_mut) {
            self.mut_vars.put(name, {}) catch @panic("OOM");
        }
        self.var_types.put(self.allocator.dupe(u8, name) catch @panic("OOM"), self.allocator.dupe(u8, ty) catch @panic("OOM")) catch @panic("OOM");
    }

    fn getState(self: *const OwnershipChecker, name: []const u8) ?OwnershipState {
        var i: usize = self.scopes.items.len;
        while (i > 0) {
            i -= 1;
            if (self.scopes.items[i].get(name)) |s| return s;
        }
        return null;
    }

    fn setState(self: *OwnershipChecker, name: []const u8, state: OwnershipState) void {
        var i: usize = self.scopes.items.len;
        while (i > 0) {
            i -= 1;
            if (self.scopes.items[i].contains(name)) {
                self.scopes.items[i].put(name, state) catch @panic("OOM");
                return;
            }
        }
    }

    fn isVarMut(self: *const OwnershipChecker, name: []const u8) bool {
        return self.mut_vars.contains(name);
    }

    fn varType(self: *const OwnershipChecker, name: []const u8) ?[]const u8 {
        return self.var_types.get(name);
    }

    /// 返回某所有者当前所有活跃借用。
    fn activeBorrowsOf(self: *const OwnershipChecker, owner: []const u8) std.ArrayList(ActiveBorrow) {
        var result: std.ArrayList(ActiveBorrow) = .empty;
        var b_iter = self.borrows.iterator();
        while (b_iter.next()) |entry| {
            if (std.mem.eql(u8, entry.value_ptr.owner, owner)) {
                result.append(self.allocator, .{
                    .borrower = entry.key_ptr.*,
                    .kind = entry.value_ptr.kind,
                }) catch @panic("OOM");
            }
        }
        return result;
    }

    pub fn checkUse(self: *OwnershipChecker, name: []const u8, line: usize, col: usize) bool {
        _ = col;
        if (self.getState(name)) |s| {
            if (s == .Moved) {
                const msg = std.fmt.allocPrint(
                    self.allocator,
                    "变量 '{s}' 的所有权已被转移（use-after-move）\n {} | {s}",
                    .{ name, line, self.getSrcLine(line) },
                ) catch @panic("OOM");
                self.errors.append(self.allocator, msg) catch @panic("OOM");
                return false;
            }
            if (s == .Freed) {
                const msg = std.fmt.allocPrint(
                    self.allocator,
                    "变量 '{s}' 已被释放（use-after-free/悬垂指针）\n {} | {s}",
                    .{ name, line, self.getSrcLine(line) },
                ) catch @panic("OOM");
                self.errors.append(self.allocator, msg) catch @panic("OOM");
                return false;
            }
            if (s == .Borrowed) {
                // 若存在活跃可变借用，则所有者不可使用
                var active = self.activeBorrowsOf(name);
                defer active.deinit(self.allocator);
                var has_mut = false;
                for (active.items) |b| {
                    if (b.kind == .Mutable) has_mut = true;
                }
                if (has_mut) {
                    const msg = std.fmt.allocPrint(
                        self.allocator,
                        "变量 '{s}' 存在活跃可变借用，无法使用\n {} | {s}",
                        .{ name, line, self.getSrcLine(line) },
                    ) catch @panic("OOM");
                    self.errors.append(self.allocator, msg) catch @panic("OOM");
                    return false;
                }
            }
        }
        return true;
    }

    pub fn checkFree(self: *OwnershipChecker, name: []const u8, line: usize, col: usize) bool {
        _ = line;
        _ = col;
        if (self.getState(name)) |s| {
            if (s == .Moved) {
                self.errors.append(
                    self.allocator,
                    std.fmt.allocPrint(self.allocator, "变量 '{s}' 所有权已转移，无法释放", .{name}) catch @panic("OOM"),
                ) catch @panic("OOM");
                return false;
            }
            if (s == .Freed) {
                self.errors.append(
                    self.allocator,
                    std.fmt.allocPrint(self.allocator, "变量 '{s}' 已被释放（双重释放/double-free）", .{name}) catch @panic("OOM"),
                ) catch @panic("OOM");
                return false;
            }
            var active = self.activeBorrowsOf(name);
            defer active.deinit(self.allocator);
            if (active.items.len > 0) {
                self.errors.append(
                    self.allocator,
                    std.fmt.allocPrint(self.allocator, "变量 '{s}' 存在活跃借用，无法释放（违反借用规则）", .{name}) catch @panic("OOM"),
                ) catch @panic("OOM");
                return false;
            }
            return true;
        }
        self.errors.append(
            self.allocator,
            std.fmt.allocPrint(self.allocator, "未定义的变量 '{s}'", .{name}) catch @panic("OOM"),
        ) catch @panic("OOM");
        return false;
    }

    pub fn doFree(self: *OwnershipChecker, name: []const u8) void {
        self.setState(name, .Freed);
        _ = self.heap_vars.remove(name);
    }

    pub fn checkMove(self: *OwnershipChecker, src: []const u8, line: usize, col: usize) bool {
        _ = line;
        _ = col;
        if (self.getState(src)) |s| {
            if (s == .Moved) {
                self.errors.append(
                    self.allocator,
                    std.fmt.allocPrint(self.allocator, "变量 '{s}' 所有权已转移，无法再次移动", .{src}) catch @panic("OOM"),
                ) catch @panic("OOM");
                return false;
            }
            if (s == .Freed) {
                self.errors.append(
                    self.allocator,
                    std.fmt.allocPrint(self.allocator, "变量 '{s}' 已被释放，无法移动", .{src}) catch @panic("OOM"),
                ) catch @panic("OOM");
                return false;
            }
            if (s == .Borrowed) {
                self.errors.append(
                    self.allocator,
                    std.fmt.allocPrint(self.allocator, "变量 '{s}' 存在活跃借用，无法移动", .{src}) catch @panic("OOM"),
                ) catch @panic("OOM");
                return false;
            }
            return true;
        }
        self.errors.append(
            self.allocator,
            std.fmt.allocPrint(self.allocator, "未定义的变量 '{s}'", .{src}) catch @panic("OOM"),
        ) catch @panic("OOM");
        return false;
    }

    pub fn doMove(self: *OwnershipChecker, src: []const u8, dst: []const u8) void {
        self.setState(src, .Moved);
        const is_heap = self.heap_vars.contains(src);
        _ = self.heap_vars.remove(src);
        const ty = self.var_types.get(src) orelse "unknown";
        self.declareVar(dst, is_heap, false, ty);
    }

    pub fn doBorrow(self: *OwnershipChecker, owner: []const u8, borrower: []const u8, kind: BorrowKind) void {
        // Rust 借用规则：aliasing XOR mutation
        var active = self.activeBorrowsOf(owner);
        defer active.deinit(self.allocator);

        switch (kind) {
            .Immutable => {
                // 不可变借用：不允许与任何可变借用共存
                for (active.items) |b| {
                    if (b.kind == .Mutable) {
                        self.errors.append(
                            self.allocator,
                            std.fmt.allocPrint(
                                self.allocator,
                                "变量 '{s}' 已存在可变借用，无法创建不可变借用（aliasing XOR mutation）",
                                .{owner},
                            ) catch @panic("OOM"),
                        ) catch @panic("OOM");
                        return;
                    }
                }
            },
            .Mutable => {
                // 可变借用：不允许任何其它借用共存
                if (active.items.len > 0) {
                    self.errors.append(
                        self.allocator,
                        std.fmt.allocPrint(
                            self.allocator,
                            "变量 '{s}' 已存在借用，无法创建可变借用（aliasing XOR mutation）",
                            .{owner},
                        ) catch @panic("OOM"),
                    ) catch @panic("OOM");
                    return;
                }
            },
        }

        self.borrows.put(
            self.allocator.dupe(u8, borrower) catch @panic("OOM"),
            .{
                .owner = self.allocator.dupe(u8, owner) catch @panic("OOM"),
                .kind = kind,
            },
        ) catch @panic("OOM");
        self.setState(owner, .Borrowed);
        self.declareVar(borrower, false, false, "pointer");
    }

    fn endBorrow(self: *OwnershipChecker, borrower: []const u8) void {
        if (self.borrows.get(borrower)) |entry| {
            _ = self.borrows.remove(borrower);
            _ = self.mut_vars.remove(borrower);
            _ = self.var_types.remove(borrower);
            var active = self.activeBorrowsOf(entry.owner);
            defer active.deinit(self.allocator);
            if (active.items.len == 0) {
                self.setState(entry.owner, .Owned);
            }
        }
    }

    pub fn checkAssign(self: *OwnershipChecker, target: *const Expr, value: *const Expr, line: usize, col: usize) void {
        // 检查赋值目标是否可变
        const target_name: ?[]const u8 = switch (target.*) {
            .Identifier => |id| id.name,
            else => null,
        };
        if (target_name) |name| {
            if (!self.isVarMut(name)) {
                const msg = std.fmt.allocPrint(
                    self.allocator,
                    "不能对不可变变量 '{s}' 赋值\n {} | {s}",
                    .{ name, line, self.getSrcLine(line) },
                ) catch @panic("OOM");
                self.errors.append(self.allocator, msg) catch @panic("OOM");
                return;
            }
        }

        // 若目标正被借用，则无法赋值
        if (target.* == .Identifier) {
            const id_name = target.Identifier.name;
            if (self.getState(id_name) == .Borrowed) {
                self.errors.append(
                    self.allocator,
                    std.fmt.allocPrint(self.allocator, "变量 '{s}' 存在活跃借用，无法赋值", .{id_name}) catch @panic("OOM"),
                ) catch @panic("OOM");
                return;
            }
        }

        switch (value.*) {
            .NewExpr => {
                if (target.* == .Identifier) {
                    const id_name = target.Identifier.name;
                    if (self.getState(id_name) == .Owned and self.heap_vars.contains(id_name)) {
                        self.warnings.append(
                            self.allocator,
                            std.fmt.allocPrint(
                                self.allocator,
                                "变量 '{s}' 持有堆所有权，赋值前请先释放（内存泄漏风险）",
                                .{id_name},
                            ) catch @panic("OOM"),
                        ) catch @panic("OOM");
                    }
                    self.declareVar(id_name, true, true, "pointer");
                }
            },
            .Identifier => |src| {
                if (target.* == .Identifier) {
                    const id_name = target.Identifier.name;
                    const src_is_copy = if (self.varType(src.name)) |t| OwnershipChecker.isCopyType(t) else false;
                    if (self.getState(src.name) == .Owned and self.heap_vars.contains(src.name) and !src_is_copy) {
                        self.doMove(src.name, id_name);
                    } else {
                        _ = self.checkUse(src.name, line, col);
                    }
                }
            },
            .MoveExpr => |me| {
                if (me.expr.* == .Identifier) {
                    const src_name = me.expr.Identifier.name;
                    if (target.* == .Identifier) {
                        const id_name = target.Identifier.name;
                        if (self.checkMove(src_name, line, col)) {
                            self.doMove(src_name, id_name);
                        }
                    }
                } else {
                    self.errors.append(
                        self.allocator,
                        std.fmt.allocPrint(self.allocator, "move 只能应用于标识符", .{}) catch @panic("OOM"),
                    ) catch @panic("OOM");
                }
            },
            .AddressOf => |addr| {
                if (addr.expr.* == .Identifier) {
                    const src_name = addr.expr.Identifier.name;
                    if (target.* == .Identifier) {
                        const id_name = target.Identifier.name;
                        const kind: BorrowKind = if (addr.is_mut) .Mutable else .Immutable;
                        self.doBorrow(src_name, id_name, kind);
                    }
                }
            },
            else => {
                if (target.* == .Identifier) {
                    const id_name = target.Identifier.name;
                    if (self.getState(id_name) == .Owned and self.heap_vars.contains(id_name)) {
                        self.warnings.append(
                            self.allocator,
                            std.fmt.allocPrint(
                                self.allocator,
                                "变量 '{s}' 持有堆所有权，覆盖赋值将导致内存泄漏",
                                .{id_name},
                            ) catch @panic("OOM"),
                        ) catch @panic("OOM");
                    }
                }
            },
        }
    }

    fn getSrcLine(self: *const OwnershipChecker, line: usize) []const u8 {
        return get_src_line(self.source, line);
    }

    pub fn checkAst(self: *OwnershipChecker, ast: []*const Expr) void {
        for (ast) |stmt| {
            self.checkStmt(stmt);
        }
    }

    fn checkStmt(self: *OwnershipChecker, s: *const Expr) void {
        switch (s.*) {
            .VarDecl => |vd| {
                const type_name = if (vd.type_expr) |te| expr_to_type_name(te) else blk: {
                    self.errors.append(
                        self.allocator,
                        std.fmt.allocPrint(
                            self.allocator,
                            "[line {}, col {}] 变量 '{s}' 缺少类型注解，VX 为纯静态类型语言",
                            .{ vd.line, vd.col, vd.name },
                        ) catch @panic("OOM"),
                    ) catch @panic("OOM");
                    break :blk "unknown";
                };
                switch (vd.init.*) {
                    .NewExpr => {
                        self.declareVar(vd.name, true, !vd.is_const, type_name);
                    },
                    .Identifier => |src| {
                        const src_is_copy = if (self.varType(src.name)) |t| OwnershipChecker.isCopyType(t) else false;
                        if (self.getState(src.name) == .Owned and self.heap_vars.contains(src.name) and !src_is_copy) {
                            self.doMove(src.name, vd.name);
                            if (!vd.is_const) {
                                self.mut_vars.put(vd.name, {}) catch @panic("OOM");
                            }
                            _ = self.var_types.remove(vd.name);
                            self.var_types.put(
                                self.allocator.dupe(u8, vd.name) catch @panic("OOM"),
                                self.allocator.dupe(u8, type_name) catch @panic("OOM"),
                            ) catch @panic("OOM");
                        } else {
                            _ = self.checkUse(src.name, vd.line, vd.col);
                            self.declareVar(vd.name, false, !vd.is_const, type_name);
                        }
                    },
                    .MoveExpr => |me| {
                        if (me.expr.* == .Identifier) {
                            const src_name = me.expr.Identifier.name;
                            if (self.checkMove(src_name, vd.line, vd.col)) {
                                self.doMove(src_name, vd.name);
                                if (!vd.is_const) {
                                    self.mut_vars.put(vd.name, {}) catch @panic("OOM");
                                }
                                _ = self.var_types.remove(vd.name);
                                self.var_types.put(
                                    self.allocator.dupe(u8, vd.name) catch @panic("OOM"),
                                    self.allocator.dupe(u8, type_name) catch @panic("OOM"),
                                ) catch @panic("OOM");
                            }
                        }
                    },
                    .AddressOf => |addr| {
                        if (addr.expr.* == .Identifier) {
                            const src_name = addr.expr.Identifier.name;
                            if (addr.is_mut and !self.isVarMut(src_name)) {
                                const msg = std.fmt.allocPrint(
                                    self.allocator,
                                    "不能从不可变变量 '{s}' 创建可变借用\n {} | {s}",
                                    .{ src_name, addr.line, self.getSrcLine(addr.line) },
                                ) catch @panic("OOM");
                                self.errors.append(self.allocator, msg) catch @panic("OOM");
                            } else {
                                const kind: BorrowKind = if (addr.is_mut) .Mutable else .Immutable;
                                self.doBorrow(src_name, vd.name, kind);
                            }
                        }
                    },
                    else => {
                        self.checkExpr(vd.init, vd.line, vd.col);
                        self.declareVar(vd.name, false, !vd.is_const, type_name);
                    },
                }
            },
            .Assign => |asgn| {
                self.checkAssign(asgn.target, asgn.value, asgn.line, asgn.col);
            },
            .ExprStmt => |es| {
                self.checkExpr(es.expr, es.line, es.col);
            },
            .IfStmt => |if_s| {
                self.pushScope();
                self.checkExpr(if_s.condition, 0, 0);
                for (if_s.then_branch.items) |stmt| self.checkStmt(stmt);
                self.popScope();
                for (if_s.elif_branches.items) |*elif| {
                    self.pushScope();
                    self.checkExpr(elif.condition, 0, 0);
                    for (elif.body.items) |stmt| self.checkStmt(stmt);
                    self.popScope();
                }
                if (if_s.else_branch) |else_body| {
                    self.pushScope();
                    for (else_body.items) |stmt| self.checkStmt(stmt);
                    self.popScope();
                }
            },
            .WhileStmt => |while_s| {
                self.pushScope();
                self.checkExpr(while_s.condition, 0, 0);
                for (while_s.body.items) |stmt| self.checkStmt(stmt);
                self.popScope();
            },
            .ForStmt => |for_s| {
                self.pushScope();
                self.declareVar(for_s.var_name, false, true, "int");
                self.checkExpr(for_s.iterable, 0, 0);
                for (for_s.body.items) |stmt| self.checkStmt(stmt);
                self.popScope();
            },
            .FuncDecl => |fd| {
                self.pushScope();
                for (fd.params.items) |*p| {
                    self.declareVar(p.name, false, false, p.param_type);
                }
                for (fd.body.items) |stmt| self.checkStmt(stmt);
                self.popScope();
            },
            .ReturnStmt => |ret| {
                if (ret.value) |val| {
                    if (val.* == .Identifier) {
                        const src_name = val.Identifier.name;
                        if (self.getState(src_name) == .Owned and self.heap_vars.contains(src_name)) {
                            self.warnings.append(
                                self.allocator,
                                std.fmt.allocPrint(
                                    self.allocator,
                                    "返回堆变量 '{s}' 会转移所有权，调用者需负责释放",
                                    .{src_name},
                                ) catch @panic("OOM"),
                            ) catch @panic("OOM");
                        }
                    }
                }
            },
            else => {},
        }
    }

    fn checkExpr(self: *OwnershipChecker, e: *const Expr, default_line: usize, default_col: usize) void {
        switch (e.*) {
            .Identifier => |id| {
                _ = self.checkUse(id.name, id.line, id.col);
            },
            .BinaryOp => |bin| {
                self.checkExpr(bin.left, default_line, default_col);
                self.checkExpr(bin.right, default_line, default_col);
            },
            .UnaryOp => |un| {
                self.checkExpr(un.expr, default_line, default_col);
            },
            .CallExpr => |call| {
                if (call.callee.* == .Identifier) {
                    const name = call.callee.Identifier.name;
                    if (std.mem.eql(u8, name, "free") and call.args.items.len == 1) {
                        const arg = call.args.items[0];
                        if (arg.* == .Identifier) {
                            const arg_name = arg.Identifier.name;
                            const arg_line = arg.Identifier.line;
                            const arg_col = arg.Identifier.col;
                            if (self.checkFree(arg_name, arg_line, arg_col)) {
                                self.doFree(arg_name);
                            }
                        }
                        return;
                    }
                }
                self.checkExpr(call.callee, default_line, default_col);
                for (call.args.items) |a| self.checkExpr(a, default_line, default_col);
            },
            .PropertyAccess => |prop| {
                self.checkExpr(prop.target, default_line, default_col);
            },
            .IndexAccess => |idx_acc| {
                self.checkExpr(idx_acc.target, default_line, default_col);
                self.checkExpr(idx_acc.index, default_line, default_col);
            },
            .Deref => |d| {
                self.checkExpr(d.expr, default_line, default_col);
            },
            .Assign => |asgn| {
                self.checkAssign(asgn.target, asgn.value, asgn.line, asgn.col);
            },
            .AddressOf => |addr| {
                self.checkExpr(addr.expr, addr.line, addr.col);
                if (addr.expr.* == .Identifier) {
                    const name = addr.expr.Identifier.name;
                    const state = self.getState(name);
                    if (state == .Moved or state == .Freed) {
                        const msg = std.fmt.allocPrint(
                            self.allocator,
                            "变量 '{s}' 已被移动/释放，无法借用\n {} | {s}",
                            .{ name, addr.line, self.getSrcLine(addr.line) },
                        ) catch @panic("OOM");
                        self.errors.append(self.allocator, msg) catch @panic("OOM");
                    }
                    if (addr.is_mut and !self.isVarMut(name)) {
                        const msg = std.fmt.allocPrint(
                            self.allocator,
                            "不能从不可变变量 '{s}' 创建可变借用\n {} | {s}",
                            .{ name, addr.line, self.getSrcLine(addr.line) },
                        ) catch @panic("OOM");
                        self.errors.append(self.allocator, msg) catch @panic("OOM");
                    }
                }
            },
            else => {},
        }
    }
};
