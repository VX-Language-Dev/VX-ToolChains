const std = @import("std");
const OpCode = @import("../opcode.zig").OpCode;
const BytecodeArg = @import("../compiler_bytecode.zig").BytecodeArg;
const ConstantValue = @import("../compiler_bytecode.zig").ConstantValue;
const Expr = @import("../parser/ast.zig").Expr;
const Stmt = Expr;
const Compiler = @import("core.zig").Compiler;
const LoopInfo = @import("core.zig").LoopInfo;
const KnownType = @import("core.zig").KnownType;
const compileExpr = @import("expr.zig").compileExpr;
const compileAssign = @import("expr.zig").compileAssign;

/// 编译语句节点。
pub fn compileStmt(self: *Compiler, s: *const Stmt) !void {
    switch (s.*) {
        .ExprStmt => |es| {
            if (es.expr.* == .Assign) {
                const assign = &es.expr.Assign;
                try compileAssign(self, assign.target, assign.op, assign.value);
            } else {
                try compileExpr(self, es.expr);
            }
        },
        .VarDecl => |vd| {
            const declared = blk: {
                if (vd.type_expr) |te| {
                    const type_name = exprToTypeName(te);
                    break :blk Compiler.typeNameToKnownType(type_name);
                } else {
                    return error.MissingTypeAnnotation;
                }
            };
            try compileExpr(self, vd.init);
            // 丢弃从初始值推导出的临时类型，变量类型以显式声明为准
            _ = self.popStackType();
            self.setVarType(vd.name, declared);
            const slot = self.allocateSlot(vd.name);
            _ = self.emit(OpCode.DefineVar, BytecodeArg{ .Int = @as(i32, @intCast(slot)) });
        },
        .IfStmt => |if_s| {
            try compileExpr(self, if_s.condition);
            const jump_to_elif = self.emit(OpCode.JumpIfFalse, .None);
            for (if_s.then_branch.items) |x| try compileStmt(self, x);
            var exit_jumps: std.ArrayList(usize) = .empty;
            try exit_jumps.append(self.allocator, self.emit(OpCode.Jump, .None));
            self.patch(jump_to_elif, self.instructions.items.len);
            for (if_s.elif_branches.items) |*elif| {
                try compileExpr(self, elif.condition);
                const jump_to_next = self.emit(OpCode.JumpIfFalse, .None);
                for (elif.body.items) |x| try compileStmt(self, x);
                try exit_jumps.append(self.allocator, self.emit(OpCode.Jump, .None));
                self.patch(jump_to_next, self.instructions.items.len);
            }
            if (if_s.else_branch) |else_body| {
                for (else_body.items) |x| try compileStmt(self, x);
            }
            const end_pc = self.instructions.items.len;
            for (exit_jumps.items) |j| self.patch(j, end_pc);
            exit_jumps.deinit(self.allocator);
        },
        .WhileStmt => |while_s| {
            const start = self.instructions.items.len;
            const loop_info = LoopInfo{
                .start = start,
                .break_jumps = .empty,
                .continue_jumps = .empty,
                .label = null,
            };
            self.loop_stack.append(self.allocator, loop_info) catch @panic("OOM");
            try compileExpr(self, while_s.condition);
            const exit_j = self.emit(OpCode.JumpIfFalse, .None);
            for (while_s.body.items) |x| try compileStmt(self, x);
            _ = self.emit(OpCode.Jump, BytecodeArg{ .Int = @as(i32, @intCast(start)) });
            const exit_pc = self.instructions.items.len;
            self.patch(exit_j, exit_pc);
            // 处理 break/continue
            const last_info = &self.loop_stack.items[self.loop_stack.items.len - 1];
            for (last_info.break_jumps.items) |bj| self.patch(bj, exit_pc);
            for (last_info.continue_jumps.items) |cj| self.patch(cj, start);
            // 清理
            last_info.break_jumps.deinit(self.allocator);
            last_info.continue_jumps.deinit(self.allocator);
            _ = self.loop_stack.pop();
        },
        .ForStmt => |for_s| {
            const for_id = self.for_counter;
            self.for_counter += 1;
            const src_var = std.fmt.allocPrint(self.allocator, "__for_{}_src", .{for_id}) catch @panic("OOM");
            const idx_var = std.fmt.allocPrint(self.allocator, "__for_{}_idx", .{for_id}) catch @panic("OOM");
            try compileExpr(self, for_s.iterable);
            const src_slot = self.allocateSlot(src_var);
            _ = self.emit(OpCode.DefineVar, BytecodeArg{ .Int = @as(i32, @intCast(src_slot)) });
            const const_0 = self.addConst(ConstantValue{ .Int = 0 });
            _ = self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(const_0)) });
            const idx_slot = self.allocateSlot(idx_var);
            _ = self.emit(OpCode.DefineVar, BytecodeArg{ .Int = @as(i32, @intCast(idx_slot)) });
            const start = self.instructions.items.len;
            const loop_info = LoopInfo{
                .start = start,
                .break_jumps = .empty,
                .continue_jumps = .empty,
                .label = null,
            };
            self.loop_stack.append(self.allocator, loop_info) catch @panic("OOM");
            const idx_slot2 = self.allocateSlot(idx_var);
            _ = self.emit(OpCode.LoadVar, BytecodeArg{ .Int = @as(i32, @intCast(idx_slot2)) });
            const src_slot2 = self.allocateSlot(src_var);
            _ = self.emit(OpCode.LoadVar, BytecodeArg{ .Int = @as(i32, @intCast(src_slot2)) });
            const const_len = self.addConst(ConstantValue{ .String = "len" });
            _ = self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(const_len)) });
            _ = self.emit(OpCode.Call, BytecodeArg{ .Int = 1 });
            _ = self.emit(OpCode.BinaryLt, .None);
            const exit_j = self.emit(OpCode.JumpIfFalse, .None);
            const src_slot3 = self.allocateSlot(src_var);
            _ = self.emit(OpCode.LoadVar, BytecodeArg{ .Int = @as(i32, @intCast(src_slot3)) });
            const idx_slot3 = self.allocateSlot(idx_var);
            _ = self.emit(OpCode.LoadVar, BytecodeArg{ .Int = @as(i32, @intCast(idx_slot3)) });
            _ = self.emit(OpCode.IndexGet, .None);
            const var_slot = self.allocateSlot(for_s.var_name);
            _ = self.emit(OpCode.DefineVar, BytecodeArg{ .Int = @as(i32, @intCast(var_slot)) });
            for (for_s.body.items) |x| try compileStmt(self, x);
            const cont_pc = self.instructions.items.len;
            self.loop_stack.items[self.loop_stack.items.len - 1].start = cont_pc;
            const idx_slot4 = self.allocateSlot(idx_var);
            _ = self.emit(OpCode.LoadVar, BytecodeArg{ .Int = @as(i32, @intCast(idx_slot4)) });
            const const_1 = self.addConst(ConstantValue{ .Int = 1 });
            _ = self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(const_1)) });
            _ = self.emit(OpCode.BinaryAdd, .None);
            const idx_slot5 = self.allocateSlot(idx_var);
            _ = self.emit(OpCode.StoreVar, BytecodeArg{ .Int = @as(i32, @intCast(idx_slot5)) });
            _ = self.emit(OpCode.Jump, BytecodeArg{ .Int = @as(i32, @intCast(start)) });
            const exit_pc = self.instructions.items.len;
            self.patch(exit_j, exit_pc);
            const last_info = &self.loop_stack.items[self.loop_stack.items.len - 1];
            for (last_info.break_jumps.items) |bj| self.patch(bj, exit_pc);
            for (last_info.continue_jumps.items) |cj| self.patch(cj, cont_pc);
            last_info.break_jumps.deinit(self.allocator);
            last_info.continue_jumps.deinit(self.allocator);
            _ = self.loop_stack.pop();
        },
        .LoopStmt => |loop_s| {
            const start = self.instructions.items.len;
            const loop_info = LoopInfo{
                .start = start,
                .break_jumps = .empty,
                .continue_jumps = .empty,
                .label = if (loop_s.label) |l| self.allocator.dupe(u8, l) catch @panic("OOM") else null,
            };
            self.loop_stack.append(self.allocator, loop_info) catch @panic("OOM");
            for (loop_s.body.items) |x| try compileStmt(self, x);
            _ = self.emit(OpCode.Jump, BytecodeArg{ .Int = @as(i32, @intCast(start)) });
            const exit_pc = self.instructions.items.len;
            const last_info = &self.loop_stack.items[self.loop_stack.items.len - 1];
            for (last_info.break_jumps.items) |bj| self.patch(bj, exit_pc);
            for (last_info.continue_jumps.items) |cj| self.patch(cj, start);
            last_info.break_jumps.deinit(self.allocator);
            last_info.continue_jumps.deinit(self.allocator);
            _ = self.loop_stack.pop();
        },
        .BreakStmt => |brk| {
            const idx: ?usize = if (brk.label) |l| blk: {
                var found: ?usize = null;
                for (0..self.loop_stack.items.len) |i| {
                    const info = &self.loop_stack.items[self.loop_stack.items.len - 1 - i];
                    if (info.label) |info_label| {
                        if (std.mem.eql(u8, info_label, l)) {
                            found = self.loop_stack.items.len - 1 - i;
                            break;
                        }
                    }
                }
                break :blk found;
            } else if (self.loop_stack.items.len > 0) self.loop_stack.items.len - 1 else null;

            const actual_idx = idx orelse return error.BreakOutsideLoop;
            const bj = self.emit(OpCode.Jump, .None);
            self.loop_stack.items[actual_idx].break_jumps.append(self.allocator, bj) catch @panic("OOM");
        },
        .ContinueStmt => |cont| {
            const idx: ?usize = if (cont.label) |l| blk: {
                var found: ?usize = null;
                for (0..self.loop_stack.items.len) |i| {
                    const info = &self.loop_stack.items[self.loop_stack.items.len - 1 - i];
                    if (info.label) |info_label| {
                        if (std.mem.eql(u8, info_label, l)) {
                            found = self.loop_stack.items.len - 1 - i;
                            break;
                        }
                    }
                }
                break :blk found;
            } else if (self.loop_stack.items.len > 0) self.loop_stack.items.len - 1 else null;

            const actual_idx = idx orelse return error.ContinueOutsideLoop;
            const cj = self.emit(OpCode.Jump, .None);
            self.loop_stack.items[actual_idx].continue_jumps.append(self.allocator, cj) catch @panic("OOM");
        },
        .ReturnStmt => |ret| {
            if (ret.value) |v| {
                try compileExpr(self, v);
            } else {
                _ = self.emit(OpCode.LoadNil, .None);
            }
            _ = self.emit(OpCode.Return, .None);
        },
        .MatchStmt => |match_s| {
            try compileExpr(self, match_s.target);
            _ = self.popStackType();
            const subject_slot_name = std.fmt.allocPrint(self.allocator, "__match_subj_{}", .{self.instructions.items.len}) catch @panic("OOM");
            const subject_slot = self.allocateSlot(subject_slot_name);
            _ = self.emit(OpCode.StoreVar, BytecodeArg{ .Int = @as(i32, @intCast(subject_slot)) });

            var end_jumps: std.ArrayList(usize) = .empty;
            var default_body: ?usize = null;

            for (match_s.arms.items, 0..) |*arm, arm_idx| {
                if (isDefaultPattern(arm.pattern)) {
                    default_body = arm_idx;
                    continue;
                }
                try compileMatchPattern(self, arm.pattern);
                _ = self.emit(OpCode.LoadVar, BytecodeArg{ .Int = @as(i32, @intCast(subject_slot)) });
                _ = self.emit(OpCode.BinaryEq, .None);
                const next_arm = self.emit(OpCode.JumpIfFalse, .None);
                for (arm.body.items) |st| try compileStmt(self, st);
                try end_jumps.append(self.allocator, self.emit(OpCode.Jump, .None));
                self.patch(next_arm, self.instructions.items.len);
            }

            if (default_body) |_| {
                for (match_s.arms.items[default_body.?].body.items) |st| try compileStmt(self, st);
            }

            const end_pc = self.instructions.items.len;
            for (end_jumps.items) |j| self.patch(j, end_pc);
            end_jumps.deinit(self.allocator);
        },
        else => {
            // 对于不可达的其他节点（应该在解析阶段被处理）返回错误
            return error.UnsupportedStatement;
        },
    }
}

fn isDefaultPattern(pat: *const Expr) bool {
    if (pat.* == .Identifier) {
        return std.mem.eql(u8, pat.Identifier.name, "_");
    }
    return false;
}

fn compileMatchPattern(self: *Compiler, pat: *const Expr) !void {
    switch (pat.*) {
        .IntLiteral, .FloatLiteral, .StringLiteral, .BoolLiteral, .NilLiteral, .Identifier => {
            try compileExpr(self, pat);
        },
        .PropertyAccess => |prop| {
            if (prop.target.* == .Identifier) {
                const enum_name = prop.target.Identifier.name;
                if (self.enum_defs.get(enum_name)) |variants| {
                    for (variants.items) |*v| {
                        if (std.mem.eql(u8, v.name, prop.prop)) {
                            const idx = self.addConst(ConstantValue{ .Int = v.value });
                            _ = self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(idx)) });
                            self.pushStackType(.Int);
                            return;
                        }
                    }
                }
            }
            return error.InvalidEnumVariant;
        },
        else => return error.UnsupportedMatchPattern,
    }
}

fn exprToTypeName(e: *const Expr) []const u8 {
    switch (e.*) {
        .TypeExpr => |te| return te.name,
        else => return "",
    }
}
