const std = @import("std");
const OpCode = @import("../opcode.zig").OpCode;
const BytecodeArg = @import("../compiler_bytecode.zig").BytecodeArg;
const ConstantValue = @import("../compiler_bytecode.zig").ConstantValue;
const Expr = @import("../parser/ast.zig").Expr;
const Compiler = @import("core.zig").Compiler;
const KnownType = @import("core.zig").KnownType;

/// 编译表达式节点。
pub fn compileExpr(self: *Compiler, e: *const Expr) !void {
    switch (e.*) {
        .IntLiteral => |v| {
            const idx = self.addConst(ConstantValue{ .Int = v.val });
            self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(idx)) });
            self.pushStackType(.Int);
        },
        .FloatLiteral => |v| {
            const idx = self.addConst(ConstantValue{ .Float = v.val });
            self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(idx)) });
            self.pushStackType(.Float);
        },
        .StringLiteral => |v| {
            const idx = self.addConst(ConstantValue{ .String = v.val });
            self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(idx)) });
            self.pushStackType(.String);
        },
        .BoolLiteral => |v| {
            if (v.val) {
                self.emit(OpCode.LoadTrue, .None);
            } else {
                self.emit(OpCode.LoadFalse, .None);
            }
            self.pushStackType(.Bool);
        },
        .NilLiteral => {
            self.emit(OpCode.LoadNil, .None);
            self.pushStackType(.Nil);
        },
        .Identifier => |id| {
            if (std.mem.eql(u8, id.name, "sys_argv")) {
                self.emit(OpCode.SysArgv, .None);
                self.pushStackType(.Unknown);
            } else if (std.mem.eql(u8, id.name, "os_system")) {
                self.emit(OpCode.System, .None);
                self.pushStackType(.Int);
            } else if (std.mem.eql(u8, id.name, "file_read")) {
                self.emit(OpCode.FileRead, .None);
                self.pushStackType(.String);
            } else if (std.mem.eql(u8, id.name, "file_write")) {
                self.emit(OpCode.FileWrite, .None);
                self.pushStackType(.Unknown);
            } else if (std.mem.eql(u8, id.name, "file_exists")) {
                self.emit(OpCode.FileExists, .None);
                self.pushStackType(.Bool);
            } else {
                const var_type = self.getVarType(id.name);
                const slot = self.allocateSlot(id.name);
                self.emit(OpCode.LoadVar, BytecodeArg{ .Int = @as(i32, @intCast(slot)) });
                self.pushStackType(var_type);
            }
        },
        .BinaryOp => |bin| {
            try compileExpr(self, bin.left);
            try compileExpr(self, bin.right);
            const right_type = self.popStackType();
            const left_type = self.popStackType();
            const oc: OpCode = if (self.binaryOpSpecialized(bin.op, left_type, right_type)) |oc_spec|
                oc_spec
            else if (std.mem.eql(u8, bin.op, "+")) .BinaryAdd
            else if (std.mem.eql(u8, bin.op, "-")) .BinarySub
            else if (std.mem.eql(u8, bin.op, "*")) .BinaryMul
            else if (std.mem.eql(u8, bin.op, "/")) .BinaryDiv
            else if (std.mem.eql(u8, bin.op, "%")) .BinaryMod
            else if (std.mem.eql(u8, bin.op, "^")) .BinaryPow
            else if (std.mem.eql(u8, bin.op, "==")) .BinaryEq
            else if (std.mem.eql(u8, bin.op, "!=")) .BinaryNe
            else if (std.mem.eql(u8, bin.op, "<")) .BinaryLt
            else if (std.mem.eql(u8, bin.op, ">")) .BinaryGt
            else if (std.mem.eql(u8, bin.op, "<=")) .BinaryLe
            else if (std.mem.eql(u8, bin.op, ">=")) .BinaryGe
            else if (std.mem.eql(u8, bin.op, "&&") or std.mem.eql(u8, bin.op, "and")) .BinaryAnd
            else if (std.mem.eql(u8, bin.op, "||") or std.mem.eql(u8, bin.op, "or")) .BinaryOr
            else return error.UnknownBinaryOp;
            self.emit(oc, .None);
            const result_type: KnownType = blk: {
                if (std.mem.eql(u8, bin.op, "+") and left_type == .Int and right_type == .Int) break :blk .Int;
                if (std.mem.eql(u8, bin.op, "+") and left_type == .Float and right_type == .Float) break :blk .Float;
                if (std.mem.eql(u8, bin.op, "-") and left_type == .Int and right_type == .Int) break :blk .Int;
                if (std.mem.eql(u8, bin.op, "-") and left_type == .Float and right_type == .Float) break :blk .Float;
                if (std.mem.eql(u8, bin.op, "*") and left_type == .Int and right_type == .Int) break :blk .Int;
                if (std.mem.eql(u8, bin.op, "*") and left_type == .Float and right_type == .Float) break :blk .Float;
                if (std.mem.eql(u8, bin.op, "/") and left_type == .Int and right_type == .Int) break :blk .Int;
                if (std.mem.eql(u8, bin.op, "/") and left_type == .Float and right_type == .Float) break :blk .Float;
                if (std.mem.eql(u8, bin.op, "%") and left_type == .Int and right_type == .Int) break :blk .Int;
                if ((std.mem.eql(u8, bin.op, "==") or std.mem.eql(u8, bin.op, "!=") or
                    std.mem.eql(u8, bin.op, "<") or std.mem.eql(u8, bin.op, ">") or
                    std.mem.eql(u8, bin.op, "<=") or std.mem.eql(u8, bin.op, ">=")) and
                    left_type == .Int and right_type == .Int) break :blk .Bool;
                if ((std.mem.eql(u8, bin.op, "==") or std.mem.eql(u8, bin.op, "!=") or
                    std.mem.eql(u8, bin.op, "<") or std.mem.eql(u8, bin.op, ">") or
                    std.mem.eql(u8, bin.op, "<=") or std.mem.eql(u8, bin.op, ">=")) and
                    left_type == .Float and right_type == .Float) break :blk .Bool;
                if ((std.mem.eql(u8, bin.op, "&&") or std.mem.eql(u8, bin.op, "||") or
                    std.mem.eql(u8, bin.op, "and") or std.mem.eql(u8, bin.op, "or")) and
                    left_type == .Bool and right_type == .Bool) break :blk .Bool;
                break :blk .Unknown;
            };
            self.pushStackType(result_type);
        },
        .UnaryOp => |un| {
            try compileExpr(self, un.expr);
            const operand_type = self.popStackType();
            const oc = self.unaryOpSpecialized(un.op, operand_type) orelse blk: {
                if (std.mem.eql(u8, un.op, "-")) break :blk OpCode.UnaryNeg;
                break :blk OpCode.UnaryNot;
            };
            self.emit(oc, .None);
            const result_type: KnownType = blk: {
                if (std.mem.eql(u8, un.op, "-") and operand_type == .Int) break :blk .Int;
                if (std.mem.eql(u8, un.op, "-") and operand_type == .Float) break :blk .Float;
                if ((std.mem.eql(u8, un.op, "!") or std.mem.eql(u8, un.op, "not")) and operand_type == .Bool) break :blk .Bool;
                break :blk .Unknown;
            };
            self.pushStackType(result_type);
        },
        .CallExpr => |call| {
            // 内置函数特殊处理
            if (call.callee.* == .Identifier) {
                const name = call.callee.Identifier.name;
                if (std.mem.eql(u8, name, "os_system") or
                    std.mem.eql(u8, name, "file_read") or
                    std.mem.eql(u8, name, "file_write") or
                    std.mem.eql(u8, name, "file_exists"))
                {
                    for (call.args.items) |a| try compileExpr(self, a);
                    const builtin_op: OpCode = if (std.mem.eql(u8, name, "os_system")) .System
                    else if (std.mem.eql(u8, name, "file_read")) .FileRead
                    else if (std.mem.eql(u8, name, "file_write")) .FileWrite
                    else .FileExists;
                    self.emit(builtin_op, .None);
                    return;
                }
            }

            if (call.callee.* == .PropertyAccess) {
                const prop = &call.callee.PropertyAccess;
                try compileExpr(self, prop.target);
                const idx = self.addConst(ConstantValue{ .String = prop.prop });
                self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(idx)) });
                for (call.args.items) |a| try compileExpr(self, a);
                self.emit(OpCode.Call, BytecodeArg{ .Int = @as(i32, @intCast(1 + call.args.items.len)) });
            } else if (call.callee.* == .Identifier) {
                const name = call.callee.Identifier.name;
                const idx = self.addConst(ConstantValue{ .String = name });
                self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(idx)) });
                for (call.args.items) |a| try compileExpr(self, a);
                self.emit(OpCode.Call, BytecodeArg{ .Int = @as(i32, @intCast(call.args.items.len)) });
                self.pushStackType(.Unknown);
            } else {
                try compileExpr(self, call.callee);
                for (call.args.items) |a| try compileExpr(self, a);
                self.emit(OpCode.Call, BytecodeArg{ .Int = @as(i32, @intCast(call.args.items.len)) });
                self.pushStackType(.Unknown);
            }
        },
        .IndexAccess => |idx_acc| {
            try compileExpr(self, idx_acc.target);
            try compileExpr(self, idx_acc.index);
            self.emit(OpCode.IndexGet, .None);
        },
        .PropertyAccess => |prop| {
            try compileExpr(self, prop.target);
            self.emit(OpCode.PropertyGet, BytecodeArg{ .String = prop.prop });
        },
        .ArrayLiteral => |arr| {
            for (arr.elements.items) |x| try compileExpr(self, x);
            self.emit(OpCode.MakeArray, BytecodeArg{ .Int = @as(i32, @intCast(arr.elements.items.len)) });
            self.pushStackType(.Array);
        },
        .MapLiteral => |map| {
            for (map.pairs.items) |*pair| {
                try compileExpr(self, pair.key);
                try compileExpr(self, pair.value);
            }
            self.emit(OpCode.MakeMap, BytecodeArg{ .Int = @as(i32, @intCast(map.pairs.items.len)) });
            self.pushStackType(.Map);
        },
        .NewExpr => |new| {
            const idx = self.addConst(ConstantValue{ .String = new.type_name });
            self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(idx)) });
            for (new.positional.items) |a| try compileExpr(self, a);
            for (new.named.items) |a| try compileExpr(self, a);
            const total_args = new.positional.items.len + new.named.items.len;
            self.emit(OpCode.Call, BytecodeArg{ .Int = @as(i32, @intCast(total_args)) });
            self.pushStackType(.Instance);
        },
        .MoveExpr => |me| {
            try compileExpr(self, me.expr);
            self.emit(OpCode.OwnershipMove, .None);
        },
        .AddressOf => |addr| {
            try compileExpr(self, addr.expr);
            self.emit(OpCode.BorrowCheck, .None);
            self.emit(OpCode.AddressOf, .None);
            self.pushStackType(.Pointer);
        },
        .Deref => |d| {
            try compileExpr(self, d.expr);
            self.emit(OpCode.AliveCheck, .None);
            self.emit(OpCode.Deref, .None);
            self.pushStackType(.Instance);
        },
        .PointerMember => |pm| {
            try compileExpr(self, pm.expr);
            self.emit(OpCode.AliveCheck, .None);
            self.emit(OpCode.PropertyGet, BytecodeArg{ .String = pm.member });
        },
        // 语句级节点在表达式位置不应出现（解析器保证不可达）
        .StructDecl, .ClassDecl, .EnumDecl, .UnionDecl,
        .FuncDecl, .ImportStmt, .ExternDecl, .TypeExpr,
        .ExprStmt, .VarDecl, .Assign, .IfStmt,
        .WhileStmt, .ForStmt, .LoopStmt, .BreakStmt,
        .ContinueStmt, .ReturnStmt => {},
        .MatchStmt => return error.MatchAsExpression,
        .MacroDef, .MacroCall => return error.UnexpandedMacro,
    }
}

/// 编译赋值语句。
pub fn compileAssign(self: *Compiler, target: *const Expr, op: []const u8, value: *const Expr) !void {
    if (std.mem.eql(u8, op, "=")) {
        switch (target.*) {
            .Identifier => |id| {
                try compileExpr(self, value);
                const value_type = self.popStackType();
                self.setVarType(id.name, value_type);
                const slot = self.allocateSlot(id.name);
                self.emit(OpCode.StoreVar, BytecodeArg{ .Int = @as(i32, @intCast(slot)) });
            },
            .IndexAccess => |idx_acc| {
                try compileExpr(self, value);
                try compileExpr(self, idx_acc.target);
                try compileExpr(self, idx_acc.index);
                self.emit(OpCode.IndexSet, .None);
            },
            .PropertyAccess => |prop| {
                try compileExpr(self, value);
                try compileExpr(self, prop.target);
                self.emit(OpCode.PropertySet, BytecodeArg{ .String = prop.prop });
                self.emit(OpCode.Pop, .None);
            },
            else => {},
        }
    } else {
        const bin_op: []const u8 = if (std.mem.eql(u8, op, "+=")) "+"
        else if (std.mem.eql(u8, op, "-=")) "-"
        else if (std.mem.eql(u8, op, "*=")) "*"
        else if (std.mem.eql(u8, op, "/=")) "/"
        else if (std.mem.eql(u8, op, "%=")) "%"
        else if (std.mem.eql(u8, op, "^=")) "^"
        else op;

        switch (target.*) {
            .Identifier => |id| {
                const slot = self.allocateSlot(id.name);
                self.emit(OpCode.LoadVar, BytecodeArg{ .Int = @as(i32, @intCast(slot)) });
                const var_type = self.getVarType(id.name);
                self.pushStackType(var_type);
                try compileExpr(self, value);
                const value_type = self.popStackType();
                const oc: OpCode = if (self.binaryOpSpecialized(bin_op, var_type, value_type)) |oc_spec|
                    oc_spec
                else if (std.mem.eql(u8, bin_op, "+")) .BinaryAdd
                else if (std.mem.eql(u8, bin_op, "-")) .BinarySub
                else if (std.mem.eql(u8, bin_op, "*")) .BinaryMul
                else if (std.mem.eql(u8, bin_op, "/")) .BinaryDiv
                else if (std.mem.eql(u8, bin_op, "%")) .BinaryMod
                else if (std.mem.eql(u8, bin_op, "^")) .BinaryPow
                else return error.UnknownBinaryOp;
                self.emit(oc, .None);
                const result_type: KnownType = blk: {
                    if (std.mem.eql(u8, bin_op, "+") and var_type == .Int and value_type == .Int) break :blk .Int;
                    if (std.mem.eql(u8, bin_op, "+") and var_type == .Float and value_type == .Float) break :blk .Float;
                    if (std.mem.eql(u8, bin_op, "-") and var_type == .Int and value_type == .Int) break :blk .Int;
                    if (std.mem.eql(u8, bin_op, "-") and var_type == .Float and value_type == .Float) break :blk .Float;
                    if (std.mem.eql(u8, bin_op, "*") and var_type == .Int and value_type == .Int) break :blk .Int;
                    if (std.mem.eql(u8, bin_op, "*") and var_type == .Float and value_type == .Float) break :blk .Float;
                    if (std.mem.eql(u8, bin_op, "/") and var_type == .Int and value_type == .Int) break :blk .Int;
                    if (std.mem.eql(u8, bin_op, "/") and var_type == .Float and value_type == .Float) break :blk .Float;
                    if (std.mem.eql(u8, bin_op, "%") and var_type == .Int and value_type == .Int) break :blk .Int;
                    break :blk .Unknown;
                };
                self.setVarType(id.name, result_type);
                const slot2 = self.allocateSlot(id.name);
                self.emit(OpCode.StoreVar, BytecodeArg{ .Int = @as(i32, @intCast(slot2)) });
            },
            .IndexAccess => |idx_acc| {
                try compileExpr(self, idx_acc.target);
                try compileExpr(self, idx_acc.index);
                self.emit(OpCode.IndexGet, .None);
                try compileExpr(self, value);
                const oc: OpCode = if (std.mem.eql(u8, bin_op, "+")) .BinaryAdd
                else if (std.mem.eql(u8, bin_op, "-")) .BinarySub
                else if (std.mem.eql(u8, bin_op, "*")) .BinaryMul
                else if (std.mem.eql(u8, bin_op, "/")) .BinaryDiv
                else if (std.mem.eql(u8, bin_op, "%")) .BinaryMod
                else if (std.mem.eql(u8, bin_op, "^")) .BinaryPow
                else return error.UnknownBinaryOp;
                self.emit(oc, .None);
                const tmp = std.fmt.allocPrint(self.allocator, "__asg_v_{}", .{self.instructions.items.len}) catch @panic("OOM");
                const tmp_slot = self.allocateSlot(tmp);
                self.emit(OpCode.StoreVar, BytecodeArg{ .Int = @as(i32, @intCast(tmp_slot)) });
                try compileExpr(self, idx_acc.target);
                try compileExpr(self, idx_acc.index);
                const tmp_slot2 = self.allocateSlot(tmp);
                self.emit(OpCode.LoadVar, BytecodeArg{ .Int = @as(i32, @intCast(tmp_slot2)) });
                self.emit(OpCode.IndexSet, .None);
            },
            .PropertyAccess => |prop| {
                try compileExpr(self, prop.target);
                self.emit(OpCode.PropertyGet, BytecodeArg{ .String = prop.prop });
                try compileExpr(self, value);
                const oc: OpCode = if (std.mem.eql(u8, bin_op, "+")) .BinaryAdd
                else if (std.mem.eql(u8, bin_op, "-")) .BinarySub
                else if (std.mem.eql(u8, bin_op, "*")) .BinaryMul
                else if (std.mem.eql(u8, bin_op, "/")) .BinaryDiv
                else if (std.mem.eql(u8, bin_op, "%")) .BinaryMod
                else if (std.mem.eql(u8, bin_op, "^")) .BinaryPow
                else return error.UnknownBinaryOp;
                self.emit(oc, .None);
                const tmp = std.fmt.allocPrint(self.allocator, "__asg_v_{}", .{self.instructions.items.len}) catch @panic("OOM");
                const tmp_slot = self.allocateSlot(tmp);
                self.emit(OpCode.StoreVar, BytecodeArg{ .Int = @as(i32, @intCast(tmp_slot)) });
                try compileExpr(self, prop.target);
                self.emit(OpCode.PropertySet, BytecodeArg{ .String = prop.prop });
                self.emit(OpCode.Pop, .None);
            },
            else => {},
        }
    }
}
