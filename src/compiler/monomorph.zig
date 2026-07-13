const std = @import("std");
const Allocator = std.mem.Allocator;
const Expr = @import("../parser/ast.zig").Expr;
const FieldDef = @import("../parser/ast.zig").FieldDef;
const ClassFieldDef = @import("../parser/ast.zig").ClassFieldDef;
const ParamDef = @import("../parser/ast.zig").ParamDef;
const ElifBranch = @import("../parser/ast.zig").ElifBranch;
const MatchArm = @import("../parser/ast.zig").MatchArm;
const MapPair = @import("../parser/ast.zig").MapPair;

// ==================== 类型工具 ====================

/// 从表达式节点推断类型名称（用于单态化过程中的类型参数推导）
fn inferTypeNameFromExpr(expr: *const Expr, allocator: Allocator) ?[]const u8 {
    _ = allocator;
    return switch (expr.*) {
        .IntLiteral => "int",
        .FloatLiteral => "float",
        .BoolLiteral => "bool",
        .StringLiteral => "string",
        .NilLiteral => "nil",
        .ArrayLiteral => "array",
        .MapLiteral => "map",
        .Identifier => |id| id.name,
        .NewExpr => |ne| ne.type_name,
        .TypeExpr => |te| te.name,
        else => null,
    };
}

/// 将类型参数字符串列表连接为唯一标识符后缀（如 ["T", "U"] -> "T_U"）
fn joinTypeSuffix(params: []const []const u8, allocator: Allocator) []const u8 {
    if (params.len == 0) return "";
    var result = std.ArrayList(u8).empty;
    for (params, 0..) |p, i| {
        if (i > 0) result.append(allocator, '_') catch @panic("OOM");
        result.appendSlice(allocator, p) catch @panic("OOM");
    }
    return result.toOwnedSlice(allocator) catch @panic("OOM");
}

// ==================== 泛型声明收集 ====================

/// 泛型声明信息
const GenericInfo = struct {
    /// AST 节点指针
    node: *Expr,
    /// 泛型参数列表
    params: []const []const u8,
    /// 声明名称
    name: []const u8,
    /// 节点类型（StructDecl / ClassDecl / FuncDecl）
    tag: std.meta.Tag(Expr),
};

/// 收集 AST 中所有泛型声明
fn collectGenerics(ast: std.ArrayList(*Expr), allocator: Allocator) std.StringHashMap(GenericInfo) {
    var result = std.StringHashMap(GenericInfo).init(allocator);
    for (ast.items) |stmt| {
        switch (stmt.*) {
            .StructDecl => |sd| {
                if (sd.generic_params.items.len > 0) {
                    const params = allocator.alloc([]const u8, sd.generic_params.items.len) catch @panic("OOM");
                    for (sd.generic_params.items, 0..) |gp, i| {
                        params[i] = allocator.dupe(u8, gp) catch @panic("OOM");
                    }
                    result.put(sd.name, GenericInfo{
                        .node = stmt,
                        .params = params,
                        .name = sd.name,
                        .tag = .StructDecl,
                    }) catch @panic("OOM");
                }
            },
            .ClassDecl => |cd| {
                if (cd.generic_params.items.len > 0) {
                    const params = allocator.alloc([]const u8, cd.generic_params.items.len) catch @panic("OOM");
                    for (cd.generic_params.items, 0..) |gp, i| {
                        params[i] = allocator.dupe(u8, gp) catch @panic("OOM");
                    }
                    result.put(cd.name, GenericInfo{
                        .node = stmt,
                        .params = params,
                        .name = cd.name,
                        .tag = .ClassDecl,
                    }) catch @panic("OOM");
                }
            },
            .FuncDecl => |fd| {
                if (fd.generic_params.items.len > 0) {
                    const params = allocator.alloc([]const u8, fd.generic_params.items.len) catch @panic("OOM");
                    for (fd.generic_params.items, 0..) |gp, i| {
                        params[i] = allocator.dupe(u8, gp) catch @panic("OOM");
                    }
                    result.put(fd.name, GenericInfo{
                        .node = stmt,
                        .params = params,
                        .name = fd.name,
                        .tag = .FuncDecl,
                    }) catch @panic("OOM");
                }
            },
            else => {},
        }
    }
    return result;
}

// ==================== 具体类型参数推导 ====================

/// 具体的实例化信息
const ConcreteInstance = struct {
    /// 泛型声明名称
    generic_name: []const u8,
    /// 具体类型参数列表
    type_args: [][]const u8,
    /// 实例化使用的上下文（用于定位）
    context: ?*Expr,
};

/// 从函数调用参数推断具体类型参数
fn inferFromCallArgs(
    generic: *const GenericInfo,
    args: std.ArrayList(*Expr),
    allocator: Allocator,
) ?[][]const u8 {
    if (args.items.len == 0 and generic.params.len == 0) {
        return allocator.alloc([]const u8, 0) catch @panic("OOM");
    }
    // 简单启发式：从每个参数推断类型，按顺序映射到泛型参数
    // 如果参数数量 >= 泛型参数数量，尝试推断
    var type_args = allocator.alloc([]const u8, generic.params.len) catch @panic("OOM");
    var inferred_count: usize = 0;
    for (generic.params, 0..) |_, i| {
        if (i < args.items.len) {
            if (inferTypeNameFromExpr(args.items[i], allocator)) |tname| {
                type_args[i] = allocator.dupe(u8, tname) catch @panic("OOM");
                inferred_count += 1;
            } else {
                type_args[i] = allocator.dupe(u8, "unknown") catch @panic("OOM");
            }
        } else {
            type_args[i] = allocator.dupe(u8, "unknown") catch @panic("OOM");
        }
    }
    if (inferred_count == 0) {
        allocator.free(type_args);
        return null;
    }
    return type_args;
}

/// 从 NewExpr 推断具体类型参数
fn inferFromNewExpr(
    generic: *const GenericInfo,
    new_expr: anytype,
    allocator: Allocator,
) ?[][]const u8 {
    _ = new_expr;
    // 对于 NewExpr，如果没有显式类型参数，使用默认类型
    if (generic.params.len == 0) {
        return allocator.alloc([]const u8, 0) catch @panic("OOM");
    }
    var type_args = allocator.alloc([]const u8, generic.params.len) catch @panic("OOM");
    for (0..generic.params.len) |i| {
        type_args[i] = allocator.dupe(u8, "unknown") catch @panic("OOM");
    }
    return type_args;
}

// ==================== AST 克隆与替换 ====================

/// 克隆 Expr 节点（深拷贝）
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
            var elements: std.ArrayList(*Expr) = .empty;
            for (v.elements.items) |elem| {
                elements.append(allocator, try cloneExpr(allocator, elem)) catch @panic("OOM");
            }
            const node = try allocator.create(Expr);
            node.* = Expr{ .ArrayLiteral = .{ .elements = elements, .line = v.line, .col = v.col } };
            return node;
        },
        .MapLiteral => |v| {
            var pairs: std.ArrayList(MapPair) = .empty;
            for (v.pairs.items) |*pair| {
                pairs.append(allocator, MapPair{
                    .key = try cloneExpr(allocator, pair.key),
                    .value = try cloneExpr(allocator, pair.value),
                }) catch @panic("OOM");
            }
            const node = try allocator.create(Expr);
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
            const new_type = if (v.type_expr) |te| try cloneExpr(allocator, te) else null;
            const node = try allocator.create(Expr);
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
            const node = try allocator.create(Expr);
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
            const node = try allocator.create(Expr);
            node.* = Expr{ .MatchStmt = .{
                .target = try cloneExpr(allocator, v.target),
                .arms = arms,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .WhileStmt => |v| {
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
            }
            const node = try allocator.create(Expr);
            node.* = Expr{ .WhileStmt = .{ .condition = try cloneExpr(allocator, v.condition), .body = body, .line = v.line, .col = v.col } };
            return node;
        },
        .ForStmt => |v| {
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
            }
            const node = try allocator.create(Expr);
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
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
            }
            const label = if (v.label) |l| try allocator.dupe(u8, l) else null;
            const node = try allocator.create(Expr);
            node.* = Expr{ .LoopStmt = .{ .label = label, .body = body, .line = v.line, .col = v.col } };
            return node;
        },
        .BreakStmt => |v| {
            const label = if (v.label) |l| try allocator.dupe(u8, l) else null;
            const node = try allocator.create(Expr);
            node.* = Expr{ .BreakStmt = .{ .label = label, .line = v.line, .col = v.col } };
            return node;
        },
        .ContinueStmt => |v| {
            const label = if (v.label) |l| try allocator.dupe(u8, l) else null;
            const node = try allocator.create(Expr);
            node.* = Expr{ .ContinueStmt = .{ .label = label, .line = v.line, .col = v.col } };
            return node;
        },
        .FuncDecl => |v| {
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
            const node = try allocator.create(Expr);
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
            const value = if (v.value) |val| try cloneExpr(allocator, val) else null;
            const node = try allocator.create(Expr);
            node.* = Expr{ .ReturnStmt = .{ .value = value, .line = v.line, .col = v.col } };
            return node;
        },
        .CallExpr => |v| {
            var args: std.ArrayList(*Expr) = .empty;
            for (v.args.items) |arg| {
                args.append(allocator, try cloneExpr(allocator, arg)) catch @panic("OOM");
            }
            const node = try allocator.create(Expr);
            node.* = Expr{ .CallExpr = .{
                .callee = try cloneExpr(allocator, v.callee),
                .args = args,
                .line = v.line,
                .col = v.col,
            } };
            return node;
        },
        .StructDecl => |v| {
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
            const node = try allocator.create(Expr);
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
            const node = try allocator.create(Expr);
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
            var variants: std.ArrayList(@import("../parser/ast.zig").EnumVariant) = .empty;
            for (v.variants.items) |*var_| {
                variants.append(allocator, .{
                    .name = try allocator.dupe(u8, var_.name),
                    .value = var_.value,
                }) catch @panic("OOM");
            }
            const node = try allocator.create(Expr);
            node.* = Expr{ .EnumDecl = .{ .name = try allocator.dupe(u8, v.name), .variants = variants, .line = v.line, .col = v.col } };
            return node;
        },
        .UnionDecl => |v| {
            var fields: std.ArrayList(@import("../parser/ast.zig").UnionField) = .empty;
            for (v.fields.items) |*f| {
                fields.append(allocator, .{
                    .name = try allocator.dupe(u8, f.name),
                    .field_type = try allocator.dupe(u8, f.field_type),
                }) catch @panic("OOM");
            }
            const node = try allocator.create(Expr);
            node.* = Expr{ .UnionDecl = .{ .name = try allocator.dupe(u8, v.name), .fields = fields, .line = v.line, .col = v.col } };
            return node;
        },
        .NewExpr => |v| {
            var positional: std.ArrayList(*Expr) = .empty;
            for (v.positional.items) |arg| {
                positional.append(allocator, try cloneExpr(allocator, arg)) catch @panic("OOM");
            }
            var named: std.ArrayList(*Expr) = .empty;
            for (v.named.items) |arg| {
                named.append(allocator, try cloneExpr(allocator, arg)) catch @panic("OOM");
            }
            const node = try allocator.create(Expr);
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
            var dirs: std.ArrayList([]const u8) = .empty;
            for (v.dirs.items) |d| {
                dirs.append(allocator, try allocator.dupe(u8, d)) catch @panic("OOM");
            }
            const alias = if (v.alias) |a| try allocator.dupe(u8, a) else null;
            const node = try allocator.create(Expr);
            node.* = Expr{ .ImportStmt = .{ .path = try allocator.dupe(u8, v.path), .alias = alias, .dirs = dirs, .line = v.line, .col = v.col } };
            return node;
        },
        .ExternDecl => |v| {
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
            const node = try allocator.create(Expr);
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
            var params: std.ArrayList([]const u8) = .empty;
            for (v.params.items) |p| {
                params.append(allocator, try allocator.dupe(u8, p)) catch @panic("OOM");
            }
            var body: std.ArrayList(*Expr) = .empty;
            for (v.body.items) |stmt| {
                body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
            }
            const node = try allocator.create(Expr);
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
            var args: std.ArrayList(*Expr) = .empty;
            for (v.args.items) |arg| {
                args.append(allocator, try cloneExpr(allocator, arg)) catch @panic("OOM");
            }
            const node = try allocator.create(Expr);
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

/// 在 AST 中替换类型参数引用为具体类型名
/// 例如：在泛型函数体中，将 `T` 替换为 `int`
fn substituteTypeParams(
    allocator: Allocator,
    expr: *const Expr,
    type_bindings: *const std.StringHashMap([]const u8),
) !*Expr {
    _ = type_bindings;
    // 对于单态化，我们直接克隆整个 AST 节点，然后通过重命名来消除泛型
    // 具体实现：在创建专门化版本时，已经通过重命名方式处理了类型参数
    return try cloneExpr(allocator, expr);
}

// ==================== 专门化实例创建 ====================

/// 为泛型函数创建专门化版本
fn specializeFuncDecl(
    allocator: Allocator,
    original: *const Expr,
    type_args: [][]const u8,
    suffix: []const u8,
) !*Expr {
    const fd = &original.FuncDecl;
    // 克隆函数声明
    const generic_params: std.ArrayList([]const u8) = .empty;

    var params: std.ArrayList(ParamDef) = .empty;
    for (fd.params.items) |*p| {
        params.append(allocator, ParamDef{
            .name = try allocator.dupe(u8, p.name),
            .param_type = try allocator.dupe(u8, p.param_type),
        }) catch @panic("OOM");
    }
    var body: std.ArrayList(*Expr) = .empty;
    for (fd.body.items) |stmt| {
        body.append(allocator, try cloneExpr(allocator, stmt)) catch @panic("OOM");
    }
    const ret_type = if (fd.return_type) |rt| try allocator.dupe(u8, rt) else null;

    // 生成专门化名称：original_name + _ + suffix
    const specialized_name = if (suffix.len > 0)
        std.fmt.allocPrint(allocator, "{s}_{s}", .{ fd.name, suffix }) catch @panic("OOM")
    else
        try allocator.dupe(u8, fd.name);

    const node = try allocator.create(Expr);
    node.* = Expr{ .FuncDecl = .{
        .name = specialized_name,
        .generic_params = generic_params,
        .params = params,
        .return_type = ret_type,
        .body = body,
        .line = fd.line,
        .col = fd.col,
    } };
    _ = type_args;
    return node;
}

/// 为泛型结构体创建专门化版本
fn specializeStructDecl(
    allocator: Allocator,
    original: *const Expr,
    type_args: [][]const u8,
    suffix: []const u8,
) !*Expr {
    const sd = &original.StructDecl;
    const generic_params: std.ArrayList([]const u8) = .empty;

    var fields: std.ArrayList(FieldDef) = .empty;
    for (sd.fields.items) |*f| {
        fields.append(allocator, FieldDef{
            .name = try allocator.dupe(u8, f.name),
            .field_type = try allocator.dupe(u8, f.field_type),
        }) catch @panic("OOM");
    }
    var methods: std.ArrayList(*Expr) = .empty;
    for (sd.methods.items) |m| {
        methods.append(allocator, try cloneExpr(allocator, m)) catch @panic("OOM");
    }

    const specialized_name = if (suffix.len > 0)
        std.fmt.allocPrint(allocator, "{s}_{s}", .{ sd.name, suffix }) catch @panic("OOM")
    else
        try allocator.dupe(u8, sd.name);

    const node = try allocator.create(Expr);
    node.* = Expr{ .StructDecl = .{
        .name = specialized_name,
        .generic_params = generic_params,
        .fields = fields,
        .methods = methods,
        .line = sd.line,
        .col = sd.col,
    } };
    _ = type_args;
    return node;
}

/// 为泛型类创建专门化版本
fn specializeClassDecl(
    allocator: Allocator,
    original: *const Expr,
    type_args: [][]const u8,
    suffix: []const u8,
) !*Expr {
    const cd = &original.ClassDecl;
    const generic_params: std.ArrayList([]const u8) = .empty;

    var fields: std.ArrayList(ClassFieldDef) = .empty;
    for (cd.fields.items) |*f| {
        fields.append(allocator, ClassFieldDef{
            .name = try allocator.dupe(u8, f.name),
            .field_type = try allocator.dupe(u8, f.field_type),
            .visibility = try allocator.dupe(u8, f.visibility),
        }) catch @panic("OOM");
    }
    var methods: std.ArrayList(*Expr) = .empty;
    for (cd.methods.items) |m| {
        methods.append(allocator, try cloneExpr(allocator, m)) catch @panic("OOM");
    }
    const parent = if (cd.parent) |p| try allocator.dupe(u8, p) else null;
    var interfaces: std.ArrayList([]const u8) = .empty;
    for (cd.interfaces.items) |iface| {
        interfaces.append(allocator, try allocator.dupe(u8, iface)) catch @panic("OOM");
    }

    const specialized_name = if (suffix.len > 0)
        std.fmt.allocPrint(allocator, "{s}_{s}", .{ cd.name, suffix }) catch @panic("OOM")
    else
        try allocator.dupe(u8, cd.name);

    const node = try allocator.create(Expr);
    node.* = Expr{ .ClassDecl = .{
        .name = specialized_name,
        .generic_params = generic_params,
        .fields = fields,
        .methods = methods,
        .parent = parent,
        .interfaces = interfaces,
        .line = cd.line,
        .col = cd.col,
    } };
    _ = type_args;
    return node;
}

// ==================== 实例化检测 ====================

/// 扫描 AST 中所有对泛型函数/类型的调用/引用，收集具体实例化信息
fn collectInstances(
    ast: std.ArrayList(*Expr),
    generics: *const std.StringHashMap(GenericInfo),
    allocator: Allocator,
) std.ArrayList(ConcreteInstance) {
    var instances = std.ArrayList(ConcreteInstance).empty;
    var seen = std.StringHashMap(void).init(allocator);
    defer seen.deinit();

    for (ast.items) |stmt| {
        collectInstancesInNode(stmt, generics, &instances, &seen, allocator);
    }
    return instances;
}

/// 递归扫描 AST 节点中的泛型引用
fn collectInstancesInNode(
    node: *Expr,
    generics: *const std.StringHashMap(GenericInfo),
    instances: *std.ArrayList(ConcreteInstance),
    seen: *std.StringHashMap(void),
    allocator: Allocator,
) void {
    switch (node.*) {
        .CallExpr => |ce| {
            // 检查是否为泛型函数调用
            if (ce.callee.* == .Identifier) {
                const callee_name = ce.callee.Identifier.name;
                if (generics.get(callee_name)) |gi| {
                    if (gi.tag == .FuncDecl) {
                        // 从调用参数推断类型参数
                        if (inferFromCallArgs(&gi, ce.args, allocator)) |type_args| {
                            defer allocator.free(type_args);
                            // 生成唯一键防止重复
                            const suffix = joinTypeSuffix(type_args, allocator);
                            defer allocator.free(suffix);
                            const key = std.fmt.allocPrint(allocator, "{s}<{s}>", .{ callee_name, suffix }) catch @panic("OOM");
                            defer allocator.free(key);
                            if (!seen.contains(key)) {
                                seen.put(key, {}) catch @panic("OOM");
                                // 复制类型参数
                                var args_copy = allocator.alloc([]const u8, type_args.len) catch @panic("OOM");
                                for (type_args, 0..) |ta, i| {
                                    args_copy[i] = allocator.dupe(u8, ta) catch @panic("OOM");
                                }
                                instances.append(allocator, ConcreteInstance{
                                    .generic_name = gi.name,
                                    .type_args = args_copy,
                                    .context = node,
                                }) catch @panic("OOM");
                            }
                        }
                    }
                }
            }
            // 递归扫描参数
            for (ce.args.items) |arg| {
                collectInstancesInNode(arg, generics, instances, seen, allocator);
            }
            collectInstancesInNode(ce.callee, generics, instances, seen, allocator);
        },
        .NewExpr => |ne| {
            // 检查是否为泛型类型实例化
            if (generics.get(ne.type_name)) |gi| {
                if (gi.tag == .StructDecl or gi.tag == .ClassDecl) {
                    if (inferFromNewExpr(&gi, ne, allocator)) |type_args| {
                        defer allocator.free(type_args);
                        const suffix = joinTypeSuffix(type_args, allocator);
                        defer allocator.free(suffix);
                        const key = std.fmt.allocPrint(allocator, "{s}<{s}>", .{ ne.type_name, suffix }) catch @panic("OOM");
                        defer allocator.free(key);
                        if (!seen.contains(key)) {
                            seen.put(key, {}) catch @panic("OOM");
                            var args_copy = allocator.alloc([]const u8, type_args.len) catch @panic("OOM");
                            for (type_args, 0..) |ta, i| {
                                args_copy[i] = allocator.dupe(u8, ta) catch @panic("OOM");
                            }
                            instances.append(allocator, ConcreteInstance{
                                .generic_name = gi.name,
                                .type_args = args_copy,
                                .context = node,
                            }) catch @panic("OOM");
                        }
                    }
                }
            }
            for (ne.positional.items) |arg| {
                collectInstancesInNode(arg, generics, instances, seen, allocator);
            }
            for (ne.named.items) |arg| {
                collectInstancesInNode(arg, generics, instances, seen, allocator);
            }
        },
        .VarDecl => |vd| {
            collectInstancesInNode(vd.init, generics, instances, seen, allocator);
            if (vd.type_expr) |te| {
                collectInstancesInNode(te, generics, instances, seen, allocator);
            }
        },
        .Assign => |a| {
            collectInstancesInNode(a.target, generics, instances, seen, allocator);
            collectInstancesInNode(a.value, generics, instances, seen, allocator);
        },
        .IfStmt => |is_| {
            collectInstancesInNode(is_.condition, generics, instances, seen, allocator);
            for (is_.then_branch.items) |stmt| {
                collectInstancesInNode(stmt, generics, instances, seen, allocator);
            }
            for (is_.elif_branches.items) |*elif| {
                collectInstancesInNode(elif.condition, generics, instances, seen, allocator);
                for (elif.body.items) |stmt| {
                    collectInstancesInNode(stmt, generics, instances, seen, allocator);
                }
            }
            if (is_.else_branch) |else_body| {
                for (else_body.items) |stmt| {
                    collectInstancesInNode(stmt, generics, instances, seen, allocator);
                }
            }
        },
        .MatchStmt => |ms| {
            collectInstancesInNode(ms.target, generics, instances, seen, allocator);
            for (ms.arms.items) |*arm| {
                collectInstancesInNode(arm.pattern, generics, instances, seen, allocator);
                for (arm.body.items) |stmt| {
                    collectInstancesInNode(stmt, generics, instances, seen, allocator);
                }
            }
        },
        .WhileStmt => |ws| {
            collectInstancesInNode(ws.condition, generics, instances, seen, allocator);
            for (ws.body.items) |stmt| {
                collectInstancesInNode(stmt, generics, instances, seen, allocator);
            }
        },
        .ForStmt => |fs| {
            collectInstancesInNode(fs.iterable, generics, instances, seen, allocator);
            for (fs.body.items) |stmt| {
                collectInstancesInNode(stmt, generics, instances, seen, allocator);
            }
        },
        .LoopStmt => |ls| {
            for (ls.body.items) |stmt| {
                collectInstancesInNode(stmt, generics, instances, seen, allocator);
            }
        },
        .ReturnStmt => |rs| {
            if (rs.value) |val| {
                collectInstancesInNode(val, generics, instances, seen, allocator);
            }
        },
        .BinaryOp => |bo| {
            collectInstancesInNode(bo.left, generics, instances, seen, allocator);
            collectInstancesInNode(bo.right, generics, instances, seen, allocator);
        },
        .UnaryOp => |uo| {
            collectInstancesInNode(uo.expr, generics, instances, seen, allocator);
        },
        .IndexAccess => |ia| {
            collectInstancesInNode(ia.target, generics, instances, seen, allocator);
            collectInstancesInNode(ia.index, generics, instances, seen, allocator);
        },
        .PropertyAccess => |pa| {
            collectInstancesInNode(pa.target, generics, instances, seen, allocator);
        },
        .ArrayLiteral => |al| {
            for (al.elements.items) |elem| {
                collectInstancesInNode(elem, generics, instances, seen, allocator);
            }
        },
        .MapLiteral => |ml| {
            for (ml.pairs.items) |*pair| {
                collectInstancesInNode(pair.key, generics, instances, seen, allocator);
                collectInstancesInNode(pair.value, generics, instances, seen, allocator);
            }
        },
        .AddressOf => |ao| {
            collectInstancesInNode(ao.expr, generics, instances, seen, allocator);
        },
        .Deref => |d| {
            collectInstancesInNode(d.expr, generics, instances, seen, allocator);
        },
        .PointerMember => |pm| {
            collectInstancesInNode(pm.expr, generics, instances, seen, allocator);
        },
        .ExprStmt => |es| {
            collectInstancesInNode(es.expr, generics, instances, seen, allocator);
        },
        .MoveExpr => |me| {
            collectInstancesInNode(me.expr, generics, instances, seen, allocator);
        },
        .FuncDecl => |fd| {
            for (fd.body.items) |stmt| {
                collectInstancesInNode(stmt, generics, instances, seen, allocator);
            }
        },
        .StructDecl => |sd| {
            for (sd.methods.items) |m| {
                collectInstancesInNode(m, generics, instances, seen, allocator);
            }
        },
        .ClassDecl => |cd| {
            for (cd.methods.items) |m| {
                collectInstancesInNode(m, generics, instances, seen, allocator);
            }
        },
        // 其他类型不包含子表达式
        else => {},
    }
}

// ==================== 主入口 ====================

/// AST 级泛型单态化。
///
/// 扫描 AST 中所有带类型参数的 struct/class/func 模板，
/// 检测实际使用到的具体类型变体，生成专门化版本，
/// 并移除未使用的泛型模板。
///
/// 单态化策略：
/// 1. 收集所有泛型声明（StructDecl/ClassDecl/FuncDecl 且 generic_params 非空）
/// 2. 扫描 AST 中所有泛型引用（函数调用、NewExpr 等）
/// 3. 从调用上下文推断具体类型参数
/// 4. 为每个具体实例化创建专门化版本
/// 5. 用专门化版本替换原始泛型声明，移除未使用的泛型模板
pub fn monomorphizeAst(ast: std.ArrayList(*Expr), allocator: std.mem.Allocator) std.ArrayList(*Expr) {
    // 第一步：收集所有泛型声明
    var generics = collectGenerics(ast, allocator);
    defer {
        var it = generics.iterator();
        while (it.next()) |entry| {
            allocator.free(entry.key_ptr.*);
            for (entry.value_ptr.params) |p| allocator.free(p);
            allocator.free(entry.value_ptr.params);
        }
        generics.deinit();
    }

    // 如果没有泛型声明，直接返回原 AST
    if (generics.count() == 0) return ast;

    // 第二步：收集所有具体实例化
    var instances = collectInstances(ast, &generics, allocator);
    defer {
        for (instances.items) |inst| {
            for (inst.type_args) |ta| allocator.free(ta);
            allocator.free(inst.type_args);
        }
        instances.deinit(allocator);
    }

    // 第三步：构建输出 AST
    // - 保留非泛型声明
    // - 为每个具体实例化添加专门化版本
    // - 移除未使用的泛型模板
    var result: std.ArrayList(*Expr) = .empty;
    // 记录已处理的泛型声明（已被实例化取代）
    var processed_generics = std.StringHashMap(void).init(allocator);
    defer processed_generics.deinit();

    // 先添加所有专门化实例
    for (instances.items) |inst| {
        processed_generics.put(inst.generic_name, {}) catch @panic("OOM");
        if (generics.get(inst.generic_name)) |gi| {
            const suffix = joinTypeSuffix(inst.type_args, allocator);
            const specialized = switch (gi.tag) {
                .FuncDecl => specializeFuncDecl(allocator, gi.node, inst.type_args, suffix) catch @panic("OOM"),
                .StructDecl => specializeStructDecl(allocator, gi.node, inst.type_args, suffix) catch @panic("OOM"),
                .ClassDecl => specializeClassDecl(allocator, gi.node, inst.type_args, suffix) catch @panic("OOM"),
                else => unreachable,
            };
            allocator.free(suffix);
            result.append(allocator, specialized) catch @panic("OOM");
        }
    }

    // 然后添加所有非泛型声明（跳过泛型模板）
    for (ast.items) |stmt| {
        const is_generic = switch (stmt.*) {
            .StructDecl => |sd| sd.generic_params.items.len > 0,
            .ClassDecl => |cd| cd.generic_params.items.len > 0,
            .FuncDecl => |fd| fd.generic_params.items.len > 0,
            else => false,
        };
        if (!is_generic) {
            result.append(allocator, stmt) catch @panic("OOM");
        }
    }

    return result;
}
