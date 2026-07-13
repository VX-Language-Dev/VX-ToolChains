// ==================== VX 字节码优化器 ====================
//
// opt_level 1-10:
//   Debug (1-4):   L1=无, L2=常量折叠, L3=+死代码消除, L4=+常量传播
//   Release (5-7): L5=+分支折叠, L6=+死块消除, L7=+多趟
//   Super  (8-10): L8=+强化传播, L9=+强化DCE, L10=极致(16趟)
//
const std = @import("std");
const Allocator = std.mem.Allocator;
const OpCode = @import("../opcode.zig").OpCode;
const BytecodeArg = @import("../compiler_bytecode.zig").BytecodeArg;
const ConstantValue = @import("../compiler_bytecode.zig").ConstantValue;
const Instruction = @import("../compiler_bytecode.zig").Instruction;

// ==================== 统计 ====================

pub const OptStats = struct {
    folds: u32 = 0,
    dead: u32 = 0,
    prop: u32 = 0,
    branch: u32 = 0,
    unreach: u32 = 0,
    passes: u32 = 0,

    pub fn total(self: *const OptStats) u32 {
        return self.folds + self.dead + self.prop + self.branch + self.unreach;
    }
};

// ==================== 工具函数 ====================

inline fn argInt(a: BytecodeArg) ?i32 {
    return switch (a) {
        .Int => |v| v,
        else => null,
    };
}

inline fn removeAt(ins: *std.ArrayList(Instruction), i: usize) void {
    _ = ins.orderedRemove(i);
}

fn addConst(pool: *std.ArrayList(ConstantValue), v: ConstantValue, allocator: Allocator) usize {
    const idx = pool.items.len;
    pool.append(allocator, v) catch @panic("OOM");
    return idx;
}

fn getConst(pool: std.ArrayList(ConstantValue).Slice, a: BytecodeArg) ?ConstantValue {
    const raw = argInt(a) orelse return null;
    if (raw < 0) return null;
    const i: usize = @intCast(raw);
    return if (i < pool.len) pool[i] else null;
}

fn isBinary(op: OpCode) bool {
    return switch (op) {
        .BinaryAdd,
        .BinarySub,
        .BinaryMul,
        .BinaryDiv,
        .BinaryMod,
        .BinaryPow,
        .BinaryEq,
        .BinaryNe,
        .BinaryLt,
        .BinaryGt,
        .BinaryLe,
        .BinaryGe,
        .BinaryAnd,
        .BinaryOr,
        .AddInt,
        .AddFloat,
        .SubInt,
        .SubFloat,
        .MulInt,
        .MulFloat,
        .DivInt,
        .DivFloat,
        .ModInt,
        .EqInt,
        .EqFloat,
        .LtInt,
        .LtFloat,
        .GtInt,
        .GtFloat,
        .LeInt,
        .LeFloat,
        .GeInt,
        .GeFloat,
        .And,
        .Or,
        => true,
        else => false,
    };
}

fn isEffectFree(op: OpCode) bool {
    return switch (op) {
        .LoadConst,
        .LoadNil,
        .LoadTrue,
        .LoadFalse,
        .LoadVar,
        .UnaryNeg,
        .UnaryNot,
        .NegInt,
        .NegFloat,
        .Not,
        .BinaryAdd,
        .BinarySub,
        .BinaryMul,
        .BinaryDiv,
        .BinaryMod,
        .BinaryPow,
        .BinaryEq,
        .BinaryNe,
        .BinaryLt,
        .BinaryGt,
        .BinaryLe,
        .BinaryGe,
        .BinaryAnd,
        .BinaryOr,
        .AddInt,
        .AddFloat,
        .SubInt,
        .SubFloat,
        .MulInt,
        .MulFloat,
        .DivInt,
        .DivFloat,
        .ModInt,
        .EqInt,
        .EqFloat,
        .LtInt,
        .LtFloat,
        .GtInt,
        .GtFloat,
        .LeInt,
        .LeFloat,
        .GeInt,
        .GeFloat,
        .And,
        .Or,
        .Dup,
        => true,
        else => false,
    };
}

fn isTerm(op: OpCode) bool {
    return switch (op) {
        .Return, .Halt, .Jump, .JumpIfFalse, .JumpIfTrue, .Break, .Continue => true,
        else => false,
    };
}

// ==================== 常量运算 ====================

fn evalIntBin(l: i64, r: i64, op: OpCode) ?ConstantValue {
    return switch (op) {
        .BinaryAdd, .AddInt => ConstantValue{ .Int = l +% r },
        .BinarySub, .SubInt => ConstantValue{ .Int = l -% r },
        .BinaryMul, .MulInt => ConstantValue{ .Int = l *% r },
        .BinaryDiv, .DivInt => if (r == 0) null else ConstantValue{ .Int = @divTrunc(l, r) },
        .BinaryMod, .ModInt => if (r == 0) null else ConstantValue{ .Int = @rem(l, r) },
        .BinaryPow => {
            if (r < 0) return null;
            var b: i64 = l;
            var e: u64 = @intCast(r);
            var res: i64 = 1;
            while (e > 0) : (e >>= 1) {
                if (e & 1 == 1) res = res *| b;
                b = b *| b;
            }
            return ConstantValue{ .Int = res };
        },
        .BinaryEq, .EqInt => ConstantValue{ .Bool = l == r },
        .BinaryNe => ConstantValue{ .Bool = l != r },
        .BinaryLt, .LtInt => ConstantValue{ .Bool = l < r },
        .BinaryGt, .GtInt => ConstantValue{ .Bool = l > r },
        .BinaryLe, .LeInt => ConstantValue{ .Bool = l <= r },
        .BinaryGe, .GeInt => ConstantValue{ .Bool = l >= r },
        else => null,
    };
}

fn evalFloatBin(l: f64, r: f64, op: OpCode) ?ConstantValue {
    return switch (op) {
        .BinaryAdd, .AddFloat => ConstantValue{ .Float = l + r },
        .BinarySub, .SubFloat => ConstantValue{ .Float = l - r },
        .BinaryMul, .MulFloat => ConstantValue{ .Float = l * r },
        .BinaryDiv, .DivFloat => if (r == 0.0) null else ConstantValue{ .Float = l / r },
        .BinaryEq, .EqFloat => ConstantValue{ .Bool = l == r },
        .BinaryNe => ConstantValue{ .Bool = l != r },
        .BinaryLt, .LtFloat => ConstantValue{ .Bool = l < r },
        .BinaryGt, .GtFloat => ConstantValue{ .Bool = l > r },
        .BinaryLe, .LeFloat => ConstantValue{ .Bool = l <= r },
        .BinaryGe, .GeFloat => ConstantValue{ .Bool = l >= r },
        else => null,
    };
}

fn evalBoolBin(l: bool, r: bool, op: OpCode) ?ConstantValue {
    return switch (op) {
        .BinaryAnd, .And => ConstantValue{ .Bool = l and r },
        .BinaryOr, .Or => ConstantValue{ .Bool = l or r },
        else => null,
    };
}

// ==================== 主入口 ====================

pub fn optimize(
    opt_level: u8,
    ins: *std.ArrayList(Instruction),
    constants: *std.ArrayList(ConstantValue),
    allocator: Allocator,
) OptStats {
    if (opt_level <= 1) return .{};

    const max_pass: u32 = if (opt_level >= 10) 16 else if (opt_level >= 7) 8 else 1;
    var total = OptStats{};
    var changed = true;

    while (changed and total.passes < max_pass) : (total.passes += 1) {
        changed = false;

        var s = constFoldPass(ins, constants, allocator);
        if (s.folds > 0) changed = true;
        total.folds += s.folds;

        if (opt_level >= 3) {
            s = deadCodePass(ins);
            if (s.dead > 0) changed = true;
            total.dead += s.dead;
        }

        if (opt_level >= 4) {
            s = constPropPass(ins, constants, allocator);
            if (s.prop > 0) changed = true;
            total.prop += s.prop;
        }

        if (opt_level >= 5) {
            s = branchFoldPass(ins, constants);
            if (s.branch + s.unreach > 0) changed = true;
            total.branch += s.branch;
            total.unreach += s.unreach;
        }

        if (opt_level >= 6) {
            s = deadBlockPass(ins);
            if (s.unreach > 0) changed = true;
            total.unreach += s.unreach;
        }

        if (opt_level < 7) break;
    }

    return total;
}

// ==================== Pass 1: 常量折叠 ====================

fn constFoldPass(
    ins: *std.ArrayList(Instruction),
    constants: *std.ArrayList(ConstantValue),
    allocator: Allocator,
) OptStats {
    var stats = OptStats{};
    var i: usize = 0;
    while (i < ins.items.len) {
        const a = ins.items[i].op;

        // LoadConst + LoadConst + BinaryOp → folded (需要 3 条指令)
        if (i + 2 < ins.items.len and a == .LoadConst and ins.items[i + 1].op == .LoadConst and isBinary(ins.items[i + 2].op)) {
            const pool = constants.items;
            const c0 = getConst(pool, ins.items[i].arg) orelse {
                i += 1;
                continue;
            };
            const c1 = getConst(pool, ins.items[i + 1].arg) orelse {
                i += 1;
                continue;
            };
            if (foldBin(c0, c1, ins.items[i + 2].op)) |result| {
                const idx = addConst(constants, result, allocator);
                ins.items[i] = Instruction.withArg(.LoadConst, BytecodeArg{ .Int = @intCast(idx) });
                removeAt(ins, i + 1);
                removeAt(ins, i + 1);
                stats.folds += 1;
                continue;
            }
        }

        // LoadConst + UnaryNeg → folded
        if (i + 1 < ins.items.len and a == .LoadConst and ins.items[i + 1].op == .UnaryNeg) {
            const pool = constants.items;
            const c0 = getConst(pool, ins.items[i].arg) orelse {
                i += 1;
                continue;
            };
            const result: ?ConstantValue = switch (c0) {
                .Int => |v| ConstantValue{ .Int = -v },
                .Float => |v| ConstantValue{ .Float = -v },
                else => null,
            };
            if (result) |r| {
                const idx = addConst(constants, r, allocator);
                ins.items[i] = Instruction.withArg(.LoadConst, BytecodeArg{ .Int = @intCast(idx) });
                removeAt(ins, i + 1);
                stats.folds += 1;
                continue;
            }
        }

        // LoadConst + UnaryNot → folded
        if (i + 1 < ins.items.len and a == .LoadConst and ins.items[i + 1].op == .UnaryNot) {
            const pool = constants.items;
            const c0 = getConst(pool, ins.items[i].arg) orelse {
                i += 1;
                continue;
            };
            if (c0 == .Bool) {
                const idx = addConst(constants, ConstantValue{ .Bool = !c0.Bool }, allocator);
                ins.items[i] = Instruction.withArg(.LoadConst, BytecodeArg{ .Int = @intCast(idx) });
                removeAt(ins, i + 1);
                stats.folds += 1;
                continue;
            }
        }

        i += 1;
    }
    return stats;
}

fn foldBin(c0: ConstantValue, c1: ConstantValue, op: OpCode) ?ConstantValue {
    const t0: enum { int, flt, b, str, nil } = switch (c0) {
        .Int => .int,
        .Float => .flt,
        .Bool => .b,
        .String => .str,
        .Nil => .nil,
    };
    const t1: @TypeOf(t0) = switch (c1) {
        .Int => .int,
        .Float => .flt,
        .Bool => .b,
        .String => .str,
        .Nil => .nil,
    };

    // Int-Int
    if (t0 == .int and t1 == .int) return evalIntBin(c0.Int, c1.Int, op);
    // Float-Float
    if (t0 == .flt and t1 == .flt) return evalFloatBin(c0.Float, c1.Float, op);
    // Int-Float → promote
    if (t0 == .int and t1 == .flt) return evalFloatBin(@floatFromInt(c0.Int), c1.Float, op);
    if (t0 == .flt and t1 == .int) return evalFloatBin(c0.Float, @floatFromInt(c1.Int), op);
    // Bool-Bool
    if (t0 == .b and t1 == .b) return evalBoolBin(c0.Bool, c1.Bool, op);

    return null;
}

// ==================== Pass 2: 死代码消除 ====================

fn deadCodePass(ins: *std.ArrayList(Instruction)) OptStats {
    var stats = OptStats{};
    var i: usize = 0;
    while (i < ins.items.len) {
        // 效率自由指令 + Pop → 移除 Pair
        if (ins.items[i].op == .Pop and i > 0) {
            const prev = ins.items[i - 1];
            if (isEffectFree(prev.op)) {
                removeAt(ins, i - 1);
                removeAt(ins, i - 1);
                stats.dead += 1;
                if (i >= 2) i -= 2 else i = 0;
                continue;
            }
        }
        // Pop + Pop → Pop
        if (ins.items[i].op == .Pop and i + 1 < ins.items.len and ins.items[i + 1].op == .Pop) {
            removeAt(ins, i + 1);
            stats.dead += 1;
            continue;
        }
        i += 1;
    }
    return stats;
}

// ==================== Pass 3: 常量传播 ====================

fn constPropPass(
    ins: *std.ArrayList(Instruction),
    constants: *std.ArrayList(ConstantValue),
    allocator: Allocator,
) OptStats {
    var stats = OptStats{};
    // 使用 HashMap 追踪 slot 的常量值，无 slot 数量限制
    var slot_val = std.AutoHashMap(u32, u32).init(allocator);
    defer slot_val.deinit();
    var slot_type = std.AutoHashMap(u32, u8).init(allocator);
    defer slot_type.deinit();
    _ = constants;

    var i: usize = 0;
    while (i < ins.items.len) {
        const op = ins.items[i].op;
        switch (op) {
            .DefineVar, .StoreVar => {
                const sid = argInt(ins.items[i].arg) orelse {
                    i += 1;
                    continue;
                };
                const s: u32 = if (sid >= 0) @intCast(sid) else {
                    i += 1;
                    continue;
                };
                // 前一条指令是否是 LoadConst?
                if (i > 0 and ins.items[i - 1].op == .LoadConst) {
                    const ci = argInt(ins.items[i - 1].arg) orelse 0;
                    if (ci >= 0) {
                        const cur_type = slot_type.get(s) orelse 0;
                        const cur_val = slot_val.get(s) orelse 0;
                        if (cur_type == 1 and cur_val != @as(u32, @intCast(ci))) {
                            _ = slot_type.put(s, 2) catch {}; // 多次不同赋值 → unknown
                        } else {
                            _ = slot_type.put(s, 1) catch {};
                            _ = slot_val.put(s, @intCast(ci)) catch {};
                        }
                    } else {
                        _ = slot_type.put(s, 2) catch {};
                    }
                } else {
                    _ = slot_type.put(s, 2) catch {};
                }
            },
            .LoadVar => {
                const sid = argInt(ins.items[i].arg) orelse {
                    i += 1;
                    continue;
                };
                const s: u32 = if (sid >= 0) @intCast(sid) else {
                    i += 1;
                    continue;
                };
                if (slot_type.get(s) == 1) {
                    if (slot_val.get(s)) |v| {
                        ins.items[i] = Instruction.withArg(.LoadConst, BytecodeArg{ .Int = @intCast(v) });
                        stats.prop += 1;
                    }
                }
            },
            // 循环/跳转重置所有 slot 追踪
            .Jump, .JumpIfFalse, .JumpIfTrue, .Continue, .Break, .Iterate, .Next => {
                var it = slot_type.iterator();
                while (it.next()) |entry| {
                    entry.value_ptr.* = 2;
                }
            },
            else => {},
        }
        i += 1;
    }
    return stats;
}

// ==================== Pass 4: 分支折叠 ====================

fn branchFoldPass(
    ins: *std.ArrayList(Instruction),
    constants: *std.ArrayList(ConstantValue),
) OptStats {
    var stats = OptStats{};
    var i: usize = 0;
    while (i + 1 < ins.items.len) {
        const a = ins.items[i].op;
        const b = ins.items[i + 1].op;

        // LoadTrue + JumpIfFalse → 跳过测试 (条件始终 true)
        if (a == .LoadTrue and b == .JumpIfFalse) {
            removeAt(ins, i);
            removeAt(ins, i);
            stats.branch += 1;
            continue;
        }
        // LoadFalse + JumpIfTrue → 跳过测试 (条件始终 false)
        if (a == .LoadFalse and b == .JumpIfTrue) {
            removeAt(ins, i);
            removeAt(ins, i);
            stats.branch += 1;
            continue;
        }
        // LoadTrue + JumpIfTrue → 无条件 Jump
        if (a == .LoadTrue and b == .JumpIfTrue) {
            ins.items[i] = Instruction.withArg(.Jump, ins.items[i + 1].arg);
            removeAt(ins, i + 1);
            stats.branch += 1;
            continue;
        }
        // LoadFalse + JumpIfFalse → 无条件 Jump
        if (a == .LoadFalse and b == .JumpIfFalse) {
            ins.items[i] = Instruction.withArg(.Jump, ins.items[i + 1].arg);
            removeAt(ins, i + 1);
            stats.branch += 1;
            continue;
        }
        // LoadConst(bool) + JumpIfFalse/JumpIfTrue → 折叠常量条件
        if (a == .LoadConst) {
            const pool = constants.items;
            const cv = getConst(pool, ins.items[i].arg);
            if (cv) |v| {
                if (v == .Bool) {
                    if (b == .JumpIfFalse) {
                        if (!v.Bool) {
                            // 条件 false → 直接跳转
                            ins.items[i] = Instruction.withArg(.Jump, ins.items[i + 1].arg);
                            removeAt(ins, i + 1);
                        } else {
                            // 条件 true → 移除测试
                            removeAt(ins, i);
                            removeAt(ins, i);
                        }
                        stats.branch += 1;
                        continue;
                    }
                    if (b == .JumpIfTrue) {
                        if (v.Bool) {
                            ins.items[i] = Instruction.withArg(.Jump, ins.items[i + 1].arg);
                            removeAt(ins, i + 1);
                        } else {
                            removeAt(ins, i);
                            removeAt(ins, i);
                        }
                        stats.branch += 1;
                        continue;
                    }
                }
            }
        }
        i += 1;
    }
    return stats;
}

// ==================== Pass 5: 死代码块消除 ====================

fn deadBlockPass(ins: *std.ArrayList(Instruction)) OptStats {
    var stats = OptStats{};
    var i: usize = 0;
    while (i < ins.items.len) {
        const op = ins.items[i].op;

        // Jump(target) 且 target == i+1 → 移除冗余跳转
        if (op == .Jump) {
            const t = argInt(ins.items[i].arg) orelse {
                i += 1;
                continue;
            };
            if (t >= 0 and @as(usize, @intCast(t)) == i + 1) {
                removeAt(ins, i);
                stats.unreach += 1;
                continue;
            }
        }

        // JumpIfFalse(target) 且 target == i+1
        if (op == .JumpIfFalse) {
            const t = argInt(ins.items[i].arg) orelse {
                i += 1;
                continue;
            };
            if (t >= 0 and @as(usize, @intCast(t)) == i + 1) {
                removeAt(ins, i);
                stats.unreach += 1;
                continue;
            }
        }

        // JumpIfTrue(target) 且 target == i+1
        if (op == .JumpIfTrue) {
            const t = argInt(ins.items[i].arg) orelse {
                i += 1;
                continue;
            };
            if (t >= 0 and @as(usize, @intCast(t)) == i + 1) {
                removeAt(ins, i);
                stats.unreach += 1;
                continue;
            }
        }

        // 终结指令后紧跟的不可达代码 → 移除
        if (isTerm(op) and i + 1 < ins.items.len) {
            var j = i + 1;
            while (j < ins.items.len and !isLabelLike(ins.items[j].op)) : (j += 1) {}
            const dead = j - i - 1;
            if (dead > 0) {
                for (0..dead) |_| removeAt(ins, i + 1);
                stats.unreach += @intCast(dead);
                continue;
            }
        }

        i += 1;
    }
    return stats;
}

/// 指令是否可能是跳转目标 (label-like): 变量定义/赋值/内置/调用/内存操作等
fn isLabelLike(op: OpCode) bool {
    return switch (op) {
        .DefineVar,
        .StoreVar,
        .StoreField,
        .IndexSet,
        .PropertySet,
        .SysArgv,
        .System,
        .FileRead,
        .FileWrite,
        .FileExists,
        .Call,
        .Import,
        .MakeFunction,
        .MakeStruct,
        .MakeClass,
        .MakeEnum,
        .MakeUnion,
        .MakeArray,
        .MakeMap,
        .New,
        .Newz,
        .Free,
        .OwnershipMove,
        .ScopeDrop,
        .BorrowCheck,
        .AliveCheck,
        .Iterate,
        .Next,
        => true,
        else => false,
    };
}
