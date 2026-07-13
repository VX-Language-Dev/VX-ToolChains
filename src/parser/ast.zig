const std = @import("std");
const Allocator = std.mem.Allocator;

// ==================== 辅助类型（用于 AST 节点中的复合字段） ====================

/// MapLiteral 的键值对
pub const MapPair = struct {
    key: *Expr,
    value: *Expr,
};

/// IfStmt 的 elif 分支
pub const ElifBranch = struct {
    condition: *Expr,
    body: std.ArrayList(*Expr),
};

/// MatchStmt 的模式匹配分支
pub const MatchArm = struct {
    pattern: *Expr,
    body: std.ArrayList(*Expr),
};

/// 函数/外部函数的参数定义
pub const ParamDef = struct {
    name: []const u8,
    param_type: []const u8,
};

/// StructDecl 的字段定义
pub const FieldDef = struct {
    name: []const u8,
    field_type: []const u8,
};

/// ClassDecl 的字段定义（含可见性）
pub const ClassFieldDef = struct {
    name: []const u8,
    field_type: []const u8,
    visibility: []const u8,
};

/// EnumDecl 的变体定义
pub const EnumVariant = struct {
    name: []const u8,
    value: i64,
};

/// UnionDecl 的字段定义
pub const UnionField = struct {
    name: []const u8,
    field_type: []const u8,
};

// ==================== AST ====================

pub const Expr = union(enum) {
    IntLiteral: struct { val: i64, line: usize, col: usize },
    FloatLiteral: struct { val: f64, line: usize, col: usize },
    StringLiteral: struct { val: []const u8, line: usize, col: usize },
    BoolLiteral: struct { val: bool, line: usize, col: usize },
    NilLiteral: struct { line: usize, col: usize },
    Identifier: struct { name: []const u8, line: usize, col: usize },
    ArrayLiteral: struct { elements: std.ArrayList(*Expr), line: usize, col: usize },
    MapLiteral: struct { pairs: std.ArrayList(MapPair), line: usize, col: usize },
    AddressOf: struct { expr: *Expr, is_mut: bool, line: usize, col: usize },
    Deref: struct { expr: *Expr, line: usize, col: usize },
    PointerMember: struct { expr: *Expr, member: []const u8, line: usize, col: usize },
    TypeExpr: struct { name: []const u8, line: usize, col: usize },
    BinaryOp: struct { op: []const u8, left: *Expr, right: *Expr, line: usize, col: usize },
    UnaryOp: struct { op: []const u8, expr: *Expr, line: usize, col: usize },
    VarDecl: struct { name: []const u8, type_expr: ?*Expr, init: *Expr, is_const: bool, line: usize, col: usize },
    Assign: struct { target: *Expr, op: []const u8, value: *Expr, line: usize, col: usize },
    IndexAccess: struct { target: *Expr, index: *Expr, line: usize, col: usize },
    PropertyAccess: struct { target: *Expr, prop: []const u8, line: usize, col: usize },
    IfStmt: struct {
        condition: *Expr,
        then_branch: std.ArrayList(*Expr),
        elif_branches: std.ArrayList(ElifBranch),
        else_branch: ?std.ArrayList(*Expr),
        line: usize,
        col: usize,
    },
    MatchStmt: struct {
        target: *Expr,
        arms: std.ArrayList(MatchArm),
        line: usize,
        col: usize,
    },
    WhileStmt: struct { condition: *Expr, body: std.ArrayList(*Expr), line: usize, col: usize },
    ForStmt: struct { var_name: []const u8, iterable: *Expr, body: std.ArrayList(*Expr), line: usize, col: usize },
    LoopStmt: struct { label: ?[]const u8, body: std.ArrayList(*Expr), line: usize, col: usize },
    BreakStmt: struct { label: ?[]const u8, line: usize, col: usize },
    ContinueStmt: struct { label: ?[]const u8, line: usize, col: usize },
    FuncDecl: struct {
        name: []const u8,
        generic_params: std.ArrayList([]const u8),
        params: std.ArrayList(ParamDef),
        return_type: ?[]const u8,
        body: std.ArrayList(*Expr),
        line: usize,
        col: usize,
    },
    ReturnStmt: struct { value: ?*Expr, line: usize, col: usize },
    CallExpr: struct { callee: *Expr, args: std.ArrayList(*Expr), line: usize, col: usize },
    StructDecl: struct {
        name: []const u8,
        generic_params: std.ArrayList([]const u8),
        fields: std.ArrayList(FieldDef),
        methods: std.ArrayList(*Expr),
        line: usize,
        col: usize,
    },
    ClassDecl: struct {
        name: []const u8,
        generic_params: std.ArrayList([]const u8),
        fields: std.ArrayList(ClassFieldDef),
        methods: std.ArrayList(*Expr),
        parent: ?[]const u8,
        interfaces: std.ArrayList([]const u8),
        line: usize,
        col: usize,
    },
    EnumDecl: struct {
        name: []const u8,
        variants: std.ArrayList(EnumVariant),
        line: usize,
        col: usize,
    },
    UnionDecl: struct {
        name: []const u8,
        fields: std.ArrayList(UnionField),
        line: usize,
        col: usize,
    },
    NewExpr: struct {
        type_name: []const u8,
        positional: std.ArrayList(*Expr),
        named: std.ArrayList(*Expr),
        line: usize,
        col: usize,
    },
    MoveExpr: struct { expr: *Expr, line: usize, col: usize },
    ExprStmt: struct { expr: *Expr, line: usize, col: usize },
    ImportStmt: struct {
        path: []const u8,
        alias: ?[]const u8,
        dirs: std.ArrayList([]const u8),
        line: usize,
        col: usize,
    },
    ExternDecl: struct {
        name: []const u8,
        generic_params: std.ArrayList([]const u8),
        params: std.ArrayList(ParamDef),
        return_type: ?[]const u8,
        line: usize,
        col: usize,
    },
    MacroDef: struct {
        name: []const u8,
        params: std.ArrayList([]const u8),
        body: std.ArrayList(*Expr),
        line: usize,
        col: usize,
    },
    MacroCall: struct {
        name: []const u8,
        args: std.ArrayList(*Expr),
        line: usize,
        col: usize,
    },

    // ==================== 析构 ====================

    /// 递归释放 Expr 节点及其所有子节点。
    /// 调用者负责在 allocator 上 free 此 Expr 指针本身。
    pub fn deinit(self: *Expr, allocator: Allocator) void {
        switch (self.*) {
            .IntLiteral, .FloatLiteral, .BoolLiteral, .NilLiteral => {},
            .StringLiteral => |s| allocator.free(s.val),
            .Identifier => |s| allocator.free(s.name),
            .TypeExpr => |s| allocator.free(s.name),

            .ArrayLiteral => |*arr| {
                for (arr.elements.items) |elem| {
                    elem.deinit(allocator);
                    allocator.destroy(elem);
                }
                arr.elements.deinit(allocator);
            },
            .MapLiteral => |*map| {
                for (map.pairs.items) |*pair| {
                    pair.key.deinit(allocator);
                    allocator.destroy(pair.key);
                    pair.value.deinit(allocator);
                    allocator.destroy(pair.value);
                }
                map.pairs.deinit(allocator);
            },
            .AddressOf => |a| {
                a.expr.deinit(allocator);
                allocator.destroy(a.expr);
            },
            .Deref => |d| {
                d.expr.deinit(allocator);
                allocator.destroy(d.expr);
            },
            .PointerMember => |pm| {
                pm.expr.deinit(allocator);
                allocator.destroy(pm.expr);
                allocator.free(pm.member);
            },
            .BinaryOp => |b| {
                allocator.free(b.op);
                b.left.deinit(allocator);
                allocator.destroy(b.left);
                b.right.deinit(allocator);
                allocator.destroy(b.right);
            },
            .UnaryOp => |u| {
                allocator.free(u.op);
                u.expr.deinit(allocator);
                allocator.destroy(u.expr);
            },
            .VarDecl => |vd| {
                allocator.free(vd.name);
                if (vd.type_expr) |te| {
                    te.deinit(allocator);
                    allocator.destroy(te);
                }
                vd.init.deinit(allocator);
                allocator.destroy(vd.init);
            },
            .Assign => |a| {
                a.target.deinit(allocator);
                allocator.destroy(a.target);
                allocator.free(a.op);
                a.value.deinit(allocator);
                allocator.destroy(a.value);
            },
            .IndexAccess => |ia| {
                ia.target.deinit(allocator);
                allocator.destroy(ia.target);
                ia.index.deinit(allocator);
                allocator.destroy(ia.index);
            },
            .PropertyAccess => |pa| {
                pa.target.deinit(allocator);
                allocator.destroy(pa.target);
                allocator.free(pa.prop);
            },
            .IfStmt => |*is_| {
                is_.condition.deinit(allocator);
                allocator.destroy(is_.condition);
                for (is_.then_branch.items) |stmt| {
                    stmt.deinit(allocator);
                    allocator.destroy(stmt);
                }
                is_.then_branch.deinit(allocator);
                for (is_.elif_branches.items) |*elif| {
                    elif.condition.deinit(allocator);
                    allocator.destroy(elif.condition);
                    for (elif.body.items) |stmt| {
                        stmt.deinit(allocator);
                        allocator.destroy(stmt);
                    }
                    elif.body.deinit(allocator);
                }
                is_.elif_branches.deinit(allocator);
                if (is_.else_branch) |*else_body| {
                    for (else_body.items) |stmt| {
                        stmt.deinit(allocator);
                        allocator.destroy(stmt);
                    }
                    else_body.deinit(allocator);
                }
            },
            .MatchStmt => |*ms| {
                ms.target.deinit(allocator);
                allocator.destroy(ms.target);
                for (ms.arms.items) |*arm| {
                    arm.pattern.deinit(allocator);
                    allocator.destroy(arm.pattern);
                    for (arm.body.items) |stmt| {
                        stmt.deinit(allocator);
                        allocator.destroy(stmt);
                    }
                    arm.body.deinit(allocator);
                }
                ms.arms.deinit(allocator);
            },
            .WhileStmt => |*ws| {
                ws.condition.deinit(allocator);
                allocator.destroy(ws.condition);
                for (ws.body.items) |stmt| {
                    stmt.deinit(allocator);
                    allocator.destroy(stmt);
                }
                ws.body.deinit(allocator);
            },
            .ForStmt => |*fs| {
                allocator.free(fs.var_name);
                fs.iterable.deinit(allocator);
                allocator.destroy(fs.iterable);
                for (fs.body.items) |stmt| {
                    stmt.deinit(allocator);
                    allocator.destroy(stmt);
                }
                fs.body.deinit(allocator);
            },
            .LoopStmt => |*ls| {
                if (ls.label) |l| allocator.free(l);
                for (ls.body.items) |stmt| {
                    stmt.deinit(allocator);
                    allocator.destroy(stmt);
                }
                ls.body.deinit(allocator);
            },
            .BreakStmt => |bs| {
                if (bs.label) |l| allocator.free(l);
            },
            .ContinueStmt => |cs| {
                if (cs.label) |l| allocator.free(l);
            },
            .FuncDecl => |*fd| {
                allocator.free(fd.name);
                for (fd.generic_params.items) |gp| allocator.free(gp);
                fd.generic_params.deinit(allocator);
                for (fd.params.items) |*p| {
                    allocator.free(p.name);
                    allocator.free(p.param_type);
                }
                fd.params.deinit(allocator);
                if (fd.return_type) |rt| allocator.free(rt);
                for (fd.body.items) |stmt| {
                    stmt.deinit(allocator);
                    allocator.destroy(stmt);
                }
                fd.body.deinit(allocator);
            },
            .ReturnStmt => |rs| {
                if (rs.value) |v| {
                    v.deinit(allocator);
                    allocator.destroy(v);
                }
            },
            .CallExpr => |*ce| {
                ce.callee.deinit(allocator);
                allocator.destroy(ce.callee);
                for (ce.args.items) |arg| {
                    arg.deinit(allocator);
                    allocator.destroy(arg);
                }
                ce.args.deinit(allocator);
            },
            .StructDecl => |*sd| {
                allocator.free(sd.name);
                for (sd.generic_params.items) |gp| allocator.free(gp);
                sd.generic_params.deinit(allocator);
                for (sd.fields.items) |*f| {
                    allocator.free(f.name);
                    allocator.free(f.field_type);
                }
                sd.fields.deinit(allocator);
                for (sd.methods.items) |m| {
                    m.deinit(allocator);
                    allocator.destroy(m);
                }
                sd.methods.deinit(allocator);
            },
            .ClassDecl => |*cd| {
                allocator.free(cd.name);
                for (cd.generic_params.items) |gp| allocator.free(gp);
                cd.generic_params.deinit(allocator);
                for (cd.fields.items) |*f| {
                    allocator.free(f.name);
                    allocator.free(f.field_type);
                    allocator.free(f.visibility);
                }
                cd.fields.deinit(allocator);
                for (cd.methods.items) |m| {
                    m.deinit(allocator);
                    allocator.destroy(m);
                }
                if (cd.parent) |p| allocator.free(p);
                for (cd.interfaces.items) |i| allocator.free(i);
                cd.interfaces.deinit(allocator);
            },
            .EnumDecl => |*ed| {
                allocator.free(ed.name);
                for (ed.variants.items) |*v| allocator.free(v.name);
                ed.variants.deinit(allocator);
            },
            .UnionDecl => |*ud| {
                allocator.free(ud.name);
                for (ud.fields.items) |*f| {
                    allocator.free(f.name);
                    allocator.free(f.field_type);
                }
                ud.fields.deinit(allocator);
            },
            .NewExpr => |*ne| {
                allocator.free(ne.type_name);
                for (ne.positional.items) |arg| {
                    arg.deinit(allocator);
                    allocator.destroy(arg);
                }
                ne.positional.deinit(allocator);
                for (ne.named.items) |arg| {
                    arg.deinit(allocator);
                    allocator.destroy(arg);
                }
                ne.named.deinit(allocator);
            },
            .MoveExpr => |me| {
                me.expr.deinit(allocator);
                allocator.destroy(me.expr);
            },
            .ExprStmt => |es| {
                es.expr.deinit(allocator);
                allocator.destroy(es.expr);
            },
            .ImportStmt => |*is_| {
                allocator.free(is_.path);
                if (is_.alias) |a| allocator.free(a);
                for (is_.dirs.items) |d| allocator.free(d);
                is_.dirs.deinit(allocator);
            },
            .ExternDecl => |*ed| {
                allocator.free(ed.name);
                for (ed.generic_params.items) |gp| allocator.free(gp);
                ed.generic_params.deinit(allocator);
                for (ed.params.items) |*p| {
                    allocator.free(p.name);
                    allocator.free(p.param_type);
                }
                ed.params.deinit(allocator);
                if (ed.return_type) |rt| allocator.free(rt);
            },
            .MacroDef => |*md| {
                allocator.free(md.name);
                for (md.params.items) |p| allocator.free(p);
                md.params.deinit(allocator);
                for (md.body.items) |stmt| {
                    stmt.deinit(allocator);
                    allocator.destroy(stmt);
                }
                md.body.deinit(allocator);
            },
            .MacroCall => |*mc| {
                allocator.free(mc.name);
                for (mc.args.items) |arg| {
                    arg.deinit(allocator);
                    allocator.destroy(arg);
                }
                mc.args.deinit(allocator);
            },
        }
    }

    // ==================== 位置辅助函数 ====================

    /// 提取任意 Expr 节点的 (行, 列) 位置。
    pub fn pos(self: *const Expr) struct { usize, usize } {
        return switch (self.*) {
            .IntLiteral => |v| .{ v.line, v.col },
            .FloatLiteral => |v| .{ v.line, v.col },
            .StringLiteral => |v| .{ v.line, v.col },
            .BoolLiteral => |v| .{ v.line, v.col },
            .NilLiteral => |v| .{ v.line, v.col },
            .Identifier => |v| .{ v.line, v.col },
            .ArrayLiteral => |v| .{ v.line, v.col },
            .MapLiteral => |v| .{ v.line, v.col },
            .AddressOf => |v| .{ v.line, v.col },
            .Deref => |v| .{ v.line, v.col },
            .PointerMember => |v| .{ v.line, v.col },
            .TypeExpr => |v| .{ v.line, v.col },
            .BinaryOp => |v| .{ v.line, v.col },
            .UnaryOp => |v| .{ v.line, v.col },
            .VarDecl => |v| .{ v.line, v.col },
            .Assign => |v| .{ v.line, v.col },
            .IndexAccess => |v| .{ v.line, v.col },
            .PropertyAccess => |v| .{ v.line, v.col },
            .IfStmt => |v| .{ v.line, v.col },
            .MatchStmt => |v| .{ v.line, v.col },
            .WhileStmt => |v| .{ v.line, v.col },
            .ForStmt => |v| .{ v.line, v.col },
            .LoopStmt => |v| .{ v.line, v.col },
            .BreakStmt => |v| .{ v.line, v.col },
            .ContinueStmt => |v| .{ v.line, v.col },
            .FuncDecl => |v| .{ v.line, v.col },
            .ReturnStmt => |v| .{ v.line, v.col },
            .CallExpr => |v| .{ v.line, v.col },
            .StructDecl => |v| .{ v.line, v.col },
            .ClassDecl => |v| .{ v.line, v.col },
            .EnumDecl => |v| .{ v.line, v.col },
            .UnionDecl => |v| .{ v.line, v.col },
            .NewExpr => |v| .{ v.line, v.col },
            .MoveExpr => |v| .{ v.line, v.col },
            .ExprStmt => |v| .{ v.line, v.col },
            .ImportStmt => |v| .{ v.line, v.col },
            .ExternDecl => |v| .{ v.line, v.col },
            .MacroDef => |v| .{ v.line, v.col },
            .MacroCall => |v| .{ v.line, v.col },
        };
    }

    pub fn e_line(self: *const Expr) usize {
        return self.pos()[0];
    }

    pub fn e_col(self: *const Expr) usize {
        return self.pos()[1];
    }
};

/// Stmt 是 Expr 的类型别名（AST 中语句即表达式）。
pub const Stmt = Expr;

// ==================== 独立辅助函数 ====================

/// 如果 Expr 是 TypeExpr 节点，返回其类型名称；否则返回空字符串。
pub fn expr_to_type_name(e: *const Expr) []const u8 {
    switch (e.*) {
        .TypeExpr => |te| return te.name,
        else => return "",
    }
}

/// 从源码中按 1-indexed 行号提取源代码行；行号越界返回空串。
pub fn get_src_line(source: []const u8, line: usize) []const u8 {
    if (line == 0) return "";
    var iter = std.mem.splitScalar(u8, source, '\n');
    var current: usize = 1;
    while (iter.next()) |l| {
        if (current == line) return l;
        current += 1;
    }
    return "";
}
