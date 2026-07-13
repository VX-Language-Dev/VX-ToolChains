const std = @import("std");
const Allocator = std.mem.Allocator;
const Expr = @import("parser/ast.zig").Expr;
const VXError = @import("token.zig").VXError;
const VXErrorKind = @import("token.zig").VXErrorKind;
const ElifBranch = @import("parser/ast.zig").ElifBranch;
const MatchArm = @import("parser/ast.zig").MatchArm;
const ParamDef = @import("parser/ast.zig").ParamDef;
const MapPair = @import("parser/ast.zig").MapPair;
const FieldDef = @import("parser/ast.zig").FieldDef;
const ClassFieldDef = @import("parser/ast.zig").ClassFieldDef;
const EnumVariant = @import("parser/ast.zig").EnumVariant;
const UnionField = @import("parser/ast.zig").UnionField;

/// 宏定义结构体
pub const Macro = struct {
    name: []const u8,
    params: std.ArrayList([]const u8),
    body: std.ArrayList(*Expr),
    line: usize,
    col: usize,

    const Self = @This();

    pub fn deinit(self: *Self, allocator: Allocator) void {
        allocator.free(self.name);
        for (self.params.items) |p| allocator.free(p);
        self.params.deinit(allocator);
        for (self.body.items) |e| {
            e.deinit(allocator);
            allocator.destroy(e);
        }
        self.body.deinit(allocator);
    }
};

/// 带缓存的宏对象（简化缓存实现）
const CachedMacro = struct {
    macro_obj: Macro,
    expansion_cache: std.StringHashMap(std.ArrayList(*Expr)),

    const Self = @This();

    fn deinit(self: *Self, allocator: Allocator) void {
        self.macro_obj.deinit(allocator);
        // 释放所有缓存条目
        var it = self.expansion_cache.iterator();
        while (it.next()) |entry| {
            allocator.free(entry.key_ptr.*);
            const list = entry.value_ptr.*;
            for (list.items) |e| {
                e.deinit(allocator);
                allocator.destroy(e);
            }
            var list_mut = list;
            list_mut.deinit(allocator);
        }
        self.expansion_cache.deinit();
    }
};

/// 宏注册表，管理所有宏的定义和展开
pub const MacroRegistry = struct {
    macros: std.StringHashMap(CachedMacro),
    /// 统计信息：展开次数
    expand_count: u64,
    /// 统计信息：缓存命中次数
    cache_hit_count: u64,
    allocator: Allocator,

    const Self = @This();

    /// 创建新的宏注册表
    pub fn init(allocator: Allocator) Self {
        return Self{
            .macros = std.StringHashMap(CachedMacro).init(allocator),
            .expand_count = 0,
            .cache_hit_count = 0,
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *Self) void {
        var it = self.macros.iterator();
        while (it.next()) |entry| {
            self.allocator.free(entry.key_ptr.*);
            entry.value_ptr.*.deinit(self.allocator);
        }
        self.macros.deinit();
    }

    /// 注册宏定义
    pub fn registerMacro(self: *Self, mac: Macro) !void {
        if (self.macros.contains(mac.name)) {
            // 手动构造 VXError（避免 allocPrint 因格式问题出错）
            const msg = std.fmt.allocPrint(self.allocator, "宏 '{s}' 已经存在", .{mac.name}) catch @panic("OOM");
            return VXError.new(msg, mac.line, mac.col).withKind(VXErrorKind.Compile);
        }

        const name_key = try self.allocator.dupe(u8, mac.name);
        const cached = CachedMacro{
            .macro_obj = mac,
            .expansion_cache = std.StringHashMap(std.ArrayList(*Expr)).init(self.allocator),
        };
        self.macros.put(name_key, cached) catch @panic("OOM");
    }

    /// 检查宏是否存在
    pub fn hasMacro(self: *const Self, name: []const u8) bool {
        return self.macros.contains(name);
    }

    /// 展开宏调用
    pub fn expandMacro(self: *Self, name: []const u8, args: []*Expr) !std.ArrayList(*Expr) {
        self.expand_count += 1;

        const cached = self.macros.getPtr(name) orelse {
            const msg = std.fmt.allocPrint(self.allocator, "未找到宏: '{s}'", .{name}) catch @panic("OOM");
            return VXError.new(msg, 0, 0).withKind(VXErrorKind.Compile);
        };

        // 验证参数数量
        if (args.len != cached.macro_obj.params.items.len) {
            const mac = &cached.macro_obj;
            const msg = std.fmt.allocPrint(
                self.allocator,
                "宏 '{s}' 期望 {} 个参数，但提供了 {} 个",
                .{ name, mac.params.items.len, args.len },
            ) catch @panic("OOM");
            return VXError.new(msg, mac.line, mac.col).withKind(VXErrorKind.Compile);
        }

        // 简化缓存：跳过复杂缓存逻辑，始终展开
        // （保留 expansion_cache 字段用于 API 兼容性）

        // 创建参数绑定映射：param_name -> *Expr（借用 args）
        var bindings = std.StringHashMap(*Expr).init(self.allocator);
        defer bindings.deinit();

        for (cached.macro_obj.params.items, 0..) |param, i| {
            bindings.put(param, args[i]) catch @panic("OOM");
        }

        // 展开宏体，替换参数
        var result: std.ArrayList(*Expr) = .empty;
        for (cached.macro_obj.body.items) |expr| {
            const substituted = try substituteParams(self.allocator, expr, &bindings);
            result.append(self.allocator, substituted) catch @panic("OOM");
        }

        // 简化：不缓存结果
        return result;
    }

    /// 获取统计信息
    pub fn getStats(self: *const Self) struct { u64, u64, f64 } {
        const hit_rate = if (self.expand_count > 0)
            (@as(f64, @floatFromInt(self.cache_hit_count)) / @as(f64, @floatFromInt(self.expand_count))) * 100.0
        else
            0.0;
        return .{ self.expand_count, self.cache_hit_count, hit_rate };
    }
};

// ==================== 辅助函数 ====================

/// 递归克隆整个 Expr 树
fn cloneExpr(allocator: Allocator, expr: *const Expr) !*Expr {
    switch (expr.*) {
        .IntLiteral => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .IntLiteral = .{ .val = v.val, .line = v.line, .col = v.col } };
            return node;
        },
        .FloatLiteral => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .FloatLiteral = .{ .val = v.val, .line = v.line, .col = v.col } };
            return node;
        },
        .StringLiteral => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .StringLiteral = .{ .val = try allocator.dupe(u8, v.val), .line = v.line, .col = v.col } };
            return node;
        },
        .BoolLiteral => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .BoolLiteral = .{ .val = v.val, .line = v.line, .col = v.col } };
            return node;
        },
        .NilLiteral => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .NilLiteral = .{ .line = v.line, .col = v.col } };
            return node;
        },
        .Identifier => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .Identifier = .{ .name = try allocator.dupe(u8, v.name), .line = v.line, .col = v.col } };
            return node;
        },
        .TypeExpr => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .TypeExpr = .{ .name = try allocator.dupe(u8, v.name), .line = v.line, .col = v.col } };
            return node;
        },
        .ArrayLiteral => |v| {
            const node = try allocator.create(Expr);
            var elements: std.ArrayList(*Expr) = .empty;
            for (v.elements.items) |elem| {
                elements.append(allocator, try cloneExpr(allocator, elem)) catch @panic("OOM");
            }
            node.* = Expr{ .ArrayLiteral = .{ .elements = elements, .line = v.line, .col = v.col } };
            return node;
        },
        .MapLiteral => |v| {
            const node = try allocator.create(Expr);
            var pairs: std.ArrayList(MapPair) = .empty;
            for (v.pairs.items) |*pair| {
                pairs.append(allocator, MapPair{
                    .key = try cloneExpr(allocator, pair.key),
                    .value = try cloneExpr(allocator, pair.value),
                }) catch @panic("OOM");
            }
            node.* = Expr{ .MapLiteral = .{ .pairs = pairs, .line = v.line, .col = v.col } };
            return node;
        },
        .AddressOf => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .AddressOf = .{
                .expr = try cloneExpr(allocator, v.expr),
                .is_mut = v.is_mut,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .Deref => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .Deref = .{ .expr = try cloneExpr(allocator, v.expr), .line = v.line, .col = v.col } };
            return node;
        },
        .PointerMember => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .PointerMember = .{
                .expr = try cloneExpr(allocator, v.expr),
                .member = try allocator.dupe(u8, v.member),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .BinaryOp => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .BinaryOp = .{
                .op = try allocator.dupe(u8, v.op),
                .left = try cloneExpr(allocator, v.left),
                .right = try cloneExpr(allocator, v.right),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .UnaryOp => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .UnaryOp = .{
                .op = try allocator.dupe(u8, v.op),
                .expr = try cloneExpr(allocator, v.expr),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .VarDecl => |v| {
            const node = try allocator.create(Expr);
            const new_type = if (v.type_expr) |te|
                try cloneExpr(allocator, te)
            else
                null;
            node.* = Expr{ .VarDecl = .{
                .name = try allocator.dupe(u8, v.name),
                .type_expr = new_type,
                .init = try cloneExpr(allocator, v.init),
                .is_const = v.is_const,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .Assign => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .Assign = .{
                .target = try cloneExpr(allocator, v.target),
                .op = try allocator.dupe(u8, v.op),
                .value = try cloneExpr(allocator, v.value),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .IndexAccess => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .IndexAccess = .{
                .target = try cloneExpr(allocator, v.target),
                .index = try cloneExpr(allocator, v.index),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .PropertyAccess => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .PropertyAccess = .{
                .target = try cloneExpr(allocator, v.target),
                .prop = try allocator.dupe(u8, v.prop),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .IfStmt => |v| {
            const node = try allocator.create(Expr);
            var then_branch: std.ArrayList(*Expr) = .empty;
            for (v.then_branch.items) |stmt| {
                then_branch.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
            }
            var elif_branches: std.ArrayList(ElifBranch) = .empty;
            for (v.elif_branches.items) |*elif| {
                var elif_body: std.ArrayList(*Expr) = .empty;
                for (elif.body.items) |stmt| {
                    elif_body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
                }
                elif_branches.append(allocator, ElifBranch{
                    .condition = try cloneExpr(allocator, elif.condition),
                    .body = elif_body,
                }) catch @panic("OOM");
            }
            const else_branch = if (v.else_branch) |else_body| blk: {
                var new_else: std.ArrayList(*Expr) = .empty;
                for (else_body.items) |stmt| {
                    new_else.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
                }
                break :blk new_else;
            } else null;
            node.* = Expr{ .IfStmt = .{
                .condition = try cloneExpr(allocator, v.condition),
                .then_branch = then_branch,
                .elif_branches = elif_branches,
                .else_branch = else_branch,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .MatchStmt => |v| {
            const node = try allocator.create(Expr);
            var arms: std.ArrayList(MatchArm) = .empty;
            for (v.arms.items) |*arm| {
                var body: std.ArrayList(*Expr) = .empty;
                for (arm.body.items) |stmt| {
                    body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
                }
                arms.append(allocator, MatchArm{
                    .pattern = try cloneExpr(allocator, arm.pattern),
                    .body = body,
                }) catch @panic("OOM");
            }
            node.* = Expr{ .MatchStmt = .{
                .target = try cloneExpr(allocator, v.target),
                .arms = arms,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .WhileStmt => |v| {
            const node = try allocator.create(Expr);
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
            }
            node.* = Expr{ .WhileStmt = .{
                .condition = try cloneExpr(allocator, v.condition),
                .body = body,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .ForStmt => |v| {
            const node = try allocator.create(Expr);
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
            }
            node.* = Expr{ .ForStmt = .{
                .var_name = try allocator.dupe(u8, v.var_name),
                .iterable = try cloneExpr(allocator, v.iterable),
                .body = body,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .LoopStmt => |v| {
            const node = try allocator.create(Expr);
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
            }
            const label = if (v.label) |l| try allocator.dupe(u8, l) else null;
            node.* = Expr{ .LoopStmt = .{ .label = label, .body = body, .line = v.line, .col = v.col } };
            return node;
        },
        .BreakStmt => |v| {
            const node = try allocator.create(Expr);
            const label = if (v.label) |l| try allocator.dupe(u8, l) else null;
            node.* = Expr{ .BreakStmt = .{ .label = label, .line = v.line, .col = v.col } };
            return node;
        },
        .ContinueStmt => |v| {
            const node = try allocator.create(Expr);
            const label = if (v.label) |l| try allocator.dupe(u8, l) else null;
            node.* = Expr{ .ContinueStmt = .{ .label = label, .line = v.line, .col = v.col } };
            return node;
        },
        .FuncDecl => |v| {
            const node = try allocator.create(Expr);
            var generic_params: std.ArrayList([]const u8) = .empty;
            for (v.generic_params.items) |gp| {
                generic_params.append(allocator, try allocator.dupe(u8, gp)) catch @panic("OOM");
            }
            var params: std.ArrayList(ParamDef) = .empty;
            for (v.params.items) |*p| {
                params.append(allocator, ParamDef{
                    .name = try allocator.dupe(u8, p.name),
                    .param_type = try allocator.dupe(u8, p.param_type),
                }) catch @panic("OOM");
            }
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
            }
            const ret_type = if (v.return_type) |rt| try allocator.dupe(u8, rt) else null;
            node.* = Expr{ .FuncDecl = .{
                .name = try allocator.dupe(u8, v.name),
                .generic_params = generic_params,
                .params = params,
                .return_type = ret_type,
                .body = body,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .ReturnStmt => |v| {
            const node = try allocator.create(Expr);
            const value = if (v.value) |val| try cloneExpr(allocator, val) else null;
            node.* = Expr{ .ReturnStmt = .{ .value = value, .line = v.line, .col = v.col } };
            return node;
        },
        .CallExpr => |v| {
            const node = try allocator.create(Expr);
            var args: std.ArrayList(*Expr) = .empty;
            for (v.args.items) |arg| {
                args.append(allocator, try cloneExpr(allocator, arg)) catch @panic("OOM");
            }
            node.* = Expr{ .CallExpr = .{
                .callee = try cloneExpr(allocator, v.callee),
                .args = args,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .StructDecl => |v| {
            const node = try allocator.create(Expr);
            var generic_params: std.ArrayList([]const u8) = .empty;
            for (v.generic_params.items) |gp| {
                generic_params.append(allocator, try allocator.dupe(u8, gp)) catch @panic("OOM");
            }
            var fields: std.ArrayList(FieldDef) = .empty;
            for (v.fields.items) |*f| {
                fields.append(allocator, FieldDef{
                    .name = try allocator.dupe(u8, f.name),
                    .field_type = try allocator.dupe(u8, f.field_type),
                }) catch @panic("OOM");
            }
            var methods: std.ArrayList(*Expr) = .empty;
            for (v.methods.items) |m| {
                methods.append(allocator, try cloneExpr(allocator, m)) catch @panic("OOM");
            }
            node.* = Expr{ .StructDecl = .{
                .name = try allocator.dupe(u8, v.name),
                .generic_params = generic_params,
                .fields = fields,
                .methods = methods,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .ClassDecl => |v| {
            const node = try allocator.create(Expr);
            var generic_params: std.ArrayList([]const u8) = .empty;
            for (v.generic_params.items) |gp| {
                generic_params.append(allocator, try allocator.dupe(u8, gp)) catch @panic("OOM");
            }
            var fields: std.ArrayList(ClassFieldDef) = .empty;
            for (v.fields.items) |*f| {
                fields.append(allocator, ClassFieldDef{
                    .name = try allocator.dupe(u8, f.name),
                    .field_type = try allocator.dupe(u8, f.field_type),
                    .visibility = try allocator.dupe(u8, f.visibility),
                }) catch @panic("OOM");
            }
            var methods: std.ArrayList(*Expr) = .empty;
            for (v.methods.items) |m| {
                methods.append(allocator, try cloneExpr(allocator, m)) catch @panic("OOM");
            }
            const parent = if (v.parent) |p| try allocator.dupe(u8, p) else null;
            var interfaces: std.ArrayList([]const u8) = .empty;
            for (v.interfaces.items) |iface| {
                interfaces.append(allocator, try allocator.dupe(u8, iface)) catch @panic("OOM");
            }
            node.* = Expr{ .ClassDecl = .{
                .name = try allocator.dupe(u8, v.name),
                .generic_params = generic_params,
                .fields = fields,
                .methods = methods,
                .parent = parent,
                .interfaces = interfaces,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .EnumDecl => |v| {
            const node = try allocator.create(Expr);
            var variants: std.ArrayList(EnumVariant) = .empty;
            for (v.variants.items) |*var_| {
                variants.append(allocator, EnumVariant{
                    .name = try allocator.dupe(u8, var_.name),
                    .value = var_.value,
                }) catch @panic("OOM");
            }
            node.* = Expr{ .EnumDecl = .{
                .name = try allocator.dupe(u8, v.name),
                .variants = variants,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .UnionDecl => |v| {
            const node = try allocator.create(Expr);
            var fields: std.ArrayList(UnionField) = .empty;
            for (v.fields.items) |*f| {
                fields.append(allocator, UnionField{
                    .name = try allocator.dupe(u8, f.name),
                    .field_type = try allocator.dupe(u8, f.field_type),
                }) catch @panic("OOM");
            }
            node.* = Expr{ .UnionDecl = .{
                .name = try allocator.dupe(u8, v.name),
                .fields = fields,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .NewExpr => |v| {
            const node = try allocator.create(Expr);
            var positional: std.ArrayList(*Expr) = .empty;
            for (v.positional.items) |arg| {
                positional.append(allocator, try cloneExpr(allocator, arg)) catch @panic("OOM");
            }
            var named: std.ArrayList(*Expr) = .empty;
            for (v.named.items) |arg| {
                named.append(allocator, try cloneExpr(allocator, arg)) catch @panic("OOM");
            }
            node.* = Expr{ .NewExpr = .{
                .type_name = try allocator.dupe(u8, v.type_name),
                .positional = positional,
                .named = named,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .MoveExpr => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .MoveExpr = .{ .expr = try cloneExpr(allocator, v.expr), .line = v.line, .col = v.col } };
            return node;
        },
        .ExprStmt => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .ExprStmt = .{ .expr = try cloneExpr(allocator, v.expr), .line = v.line, .col = v.col } };
            return node;
        },
        .ImportStmt => |v| {
            const node = try allocator.create(Expr);
            var dirs: std.ArrayList([]const u8) = .empty;
            for (v.dirs.items) |d| {
                dirs.append(allocator, try allocator.dupe(u8, d)) catch @panic("OOM");
            }
            const alias = if (v.alias) |a| try allocator.dupe(u8, a) else null;
            node.* = Expr{ .ImportStmt = .{
                .path = try allocator.dupe(u8, v.path),
                .alias = alias,
                .dirs = dirs,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .ExternDecl => |v| {
            const node = try allocator.create(Expr);
            var generic_params: std.ArrayList([]const u8) = .empty;
            for (v.generic_params.items) |gp| {
                generic_params.append(allocator, try allocator.dupe(u8, gp)) catch @panic("OOM");
            }
            var params: std.ArrayList(ParamDef) = .empty;
            for (v.params.items) |*p| {
                params.append(allocator, ParamDef{
                    .name = try allocator.dupe(u8, p.name),
                    .param_type = try allocator.dupe(u8, p.param_type),
                }) catch @panic("OOM");
            }
            const ret_type = if (v.return_type) |rt| try allocator.dupe(u8, rt) else null;
            node.* = Expr{ .ExternDecl = .{
                .name = try allocator.dupe(u8, v.name),
                .generic_params = generic_params,
                .params = params,
                .return_type = ret_type,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .MacroDef => |v| {
            const node = try allocator.create(Expr);
            var params: std.ArrayList([]const u8) = .empty;
            for (v.params.items) |p| {
                params.append(allocator, try allocator.dupe(u8, p)) catch @panic("OOM");
            }
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
            }
            node.* = Expr{ .MacroDef = .{
                .name = try allocator.dupe(u8, v.name),
                .params = params,
                .body = body,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .MacroCall => |v| {
            const node = try allocator.create(Expr);
            var args: std.ArrayList(*Expr) = .empty;
            for (v.args.items) |arg| {
                args.append(allocator, try cloneExpr(allocator, arg)) catch @panic("OOM");
            }
            node.* = Expr{ .MacroCall = .{
                .name = try allocator.dupe(u8, v.name),
                .args = args,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
    }
}

/// 递归替换表达式中的参数引用
fn substituteParams(allocator: Allocator, expr: *const Expr, bindings: *const std.StringHashMap(*Expr)) !*Expr {
    switch (expr.*) {
        // 替换标识符（参数引用）
        .Identifier => |v| {
            if (bindings.get(v.name)) |bound| {
                return try cloneExpr(allocator, bound);
            }
            // 非参数标识符，原样克隆
            return try cloneExpr(allocator, expr);
        },

        // 递归处理二元操作
        .BinaryOp => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .BinaryOp = .{
                .op = try allocator.dupe(u8, v.op),
                .left = try substituteParams(allocator, v.left, bindings),
                .right = try substituteParams(allocator, v.right, bindings),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理一元操作
        .UnaryOp => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .UnaryOp = .{
                .op = try allocator.dupe(u8, v.op),
                .expr = try substituteParams(allocator, v.expr, bindings),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理函数调用
        .CallExpr => |v| {
            const node = try allocator.create(Expr);
            var args: std.ArrayList(*Expr) = .empty;
            for (v.args.items) |arg| {
                args.append(allocator, try substituteParams(allocator, arg, bindings)) catch @panic("OOM");
            }
            node.* = Expr{ .CallExpr = .{
                .callee = try substituteParams(allocator, v.callee, bindings),
                .args = args,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理变量声明（不替换类型标注）
        .VarDecl => |v| {
            const node = try allocator.create(Expr);
            const new_type = if (v.type_expr) |te|
                try cloneExpr(allocator, te)
            else
                null;
            node.* = Expr{ .VarDecl = .{
                .name = try allocator.dupe(u8, v.name),
                .type_expr = new_type,
                .init = try substituteParams(allocator, v.init, bindings),
                .is_const = v.is_const,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理赋值语句
        .Assign => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .Assign = .{
                .target = try substituteParams(allocator, v.target, bindings),
                .op = try allocator.dupe(u8, v.op),
                .value = try substituteParams(allocator, v.value, bindings),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理条件语句
        .IfStmt => |v| {
            const node = try allocator.create(Expr);
            var then_branch: std.ArrayList(*Expr) = .empty;
            for (v.then_branch.items) |stmt| {
                then_branch.append(allocator, try substituteParams(allocator, stmt, bindings)) catch @panic("OOM");
            }
            var elif_branches: std.ArrayList(ElifBranch) = .empty;
            for (v.elif_branches.items) |*elif| {
                var elif_body: std.ArrayList(*Expr) = .empty;
                for (elif.body.items) |stmt| {
                    elif_body.append(allocator, try substituteParams(allocator, stmt, bindings)) catch @panic("OOM");
                }
                elif_branches.append(allocator, ElifBranch{
                    .condition = try substituteParams(allocator, elif.condition, bindings),
                    .body = elif_body,
                }) catch @panic("OOM");
            }
            const else_branch = if (v.else_branch) |else_body| blk: {
                var new_else: std.ArrayList(*Expr) = .empty;
                for (else_body.items) |stmt| {
                    new_else.append(allocator, try substituteParams(allocator, stmt, bindings)) catch @panic("OOM");
                }
                break :blk new_else;
            } else null;
            node.* = Expr{ .IfStmt = .{
                .condition = try substituteParams(allocator, v.condition, bindings),
                .then_branch = then_branch,
                .elif_branches = elif_branches,
                .else_branch = else_branch,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理 while 循环
        .WhileStmt => |v| {
            const node = try allocator.create(Expr);
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try substituteParams(allocator, stmt, bindings)) catch @panic("OOM");
            }
            node.* = Expr{ .WhileStmt = .{
                .condition = try substituteParams(allocator, v.condition, bindings),
                .body = body,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理 for 循环（不替换循环变量名）
        .ForStmt => |v| {
            const node = try allocator.create(Expr);
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try substituteParams(allocator, stmt, bindings)) catch @panic("OOM");
            }
            node.* = Expr{ .ForStmt = .{
                .var_name = try allocator.dupe(u8, v.var_name),
                .iterable = try substituteParams(allocator, v.iterable, bindings),
                .body = body,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理 match 语句
        .MatchStmt => |v| {
            const node = try allocator.create(Expr);
            var arms: std.ArrayList(MatchArm) = .empty;
            for (v.arms.items) |*arm| {
                var body: std.ArrayList(*Expr) = .empty;
                for (arm.body.items) |stmt| {
                    body.append(allocator, try substituteParams(allocator, stmt, bindings)) catch @panic("OOM");
                }
                arms.append(allocator, MatchArm{
                    .pattern = try substituteParams(allocator, arm.pattern, bindings),
                    .body = body,
                }) catch @panic("OOM");
            }
            node.* = Expr{ .MatchStmt = .{
                .target = try substituteParams(allocator, v.target, bindings),
                .arms = arms,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理函数定义（不替换泛型参数、函数参数和返回类型）
        .FuncDecl => |v| {
            const node = try allocator.create(Expr);
            var generic_params: std.ArrayList([]const u8) = .empty;
            for (v.generic_params.items) |gp| {
                generic_params.append(allocator, try allocator.dupe(u8, gp)) catch @panic("OOM");
            }
            var params: std.ArrayList(ParamDef) = .empty;
            for (v.params.items) |*p| {
                params.append(allocator, ParamDef{
                    .name = try allocator.dupe(u8, p.name),
                    .param_type = try allocator.dupe(u8, p.param_type),
                }) catch @panic("OOM");
            }
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try substituteParams(allocator, stmt, bindings)) catch @panic("OOM");
            }
            const ret_type = if (v.return_type) |rt| try allocator.dupe(u8, rt) else null;
            node.* = Expr{ .FuncDecl = .{
                .name = try allocator.dupe(u8, v.name),
                .generic_params = generic_params,
                .params = params,
                .return_type = ret_type,
                .body = body,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理返回语句
        .ReturnStmt => |v| {
            const node = try allocator.create(Expr);
            const value = if (v.value) |val|
                try substituteParams(allocator, val, bindings)
            else
                null;
            node.* = Expr{ .ReturnStmt = .{ .value = value, .line = v.line, .col = v.col } };
            return node;
        },

        // 递归处理数组字面量
        .ArrayLiteral => |v| {
            const node = try allocator.create(Expr);
            var elements: std.ArrayList(*Expr) = .empty;
            for (v.elements.items) |elem| {
                elements.append(allocator, try substituteParams(allocator, elem, bindings)) catch @panic("OOM");
            }
            node.* = Expr{ .ArrayLiteral = .{ .elements = elements, .line = v.line, .col = v.col } };
            return node;
        },

        // 递归处理索引访问
        .IndexAccess => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .IndexAccess = .{
                .target = try substituteParams(allocator, v.target, bindings),
                .index = try substituteParams(allocator, v.index, bindings),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 递归处理属性访问（不替换属性名）
        .PropertyAccess => |v| {
            const node = try allocator.create(Expr);
            node.* = Expr{ .PropertyAccess = .{
                .target = try substituteParams(allocator, v.target, bindings),
                .prop = try allocator.dupe(u8, v.prop),
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },

        // 其他表达式类型原样克隆（字面量、声明等）
        else => return try cloneExpr(allocator, expr),
    }
}
