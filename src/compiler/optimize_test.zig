// VX 字节码优化器单元测试
const std = @import("std");
const testing = std.testing;
const OpCode = @import("../opcode.zig").OpCode;
const BytecodeArg = @import("../compiler_bytecode.zig").BytecodeArg;
const ConstantValue = @import("../compiler_bytecode.zig").ConstantValue;
const Instruction = @import("../compiler_bytecode.zig").Instruction;
const optimize = @import("optimize.zig");

const OptStats = optimize.OptStats;

fn makeIns(op: OpCode) Instruction {
    return Instruction.init(op);
}

fn addConstToPool(pool: *std.ArrayList(ConstantValue), v: ConstantValue) i32 {
    const idx = pool.items.len;
    pool.append(testing.allocator, v) catch @panic("OOM");
    return @intCast(idx);
}

// ==================== 常量折叠测试 ====================

test "const fold int addition" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    const c0 = addConstToPool(&pool, ConstantValue{ .Int = 10 });
    const c1 = addConstToPool(&pool, ConstantValue{ .Int = 20 });
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c0 }));
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c1 }));
    try ins.append(testing.allocator, makeIns(.BinaryAdd));

    const stats = optimize.optimize(2, &ins, &pool, testing.allocator);

    // 应该折叠为 1 条 LoadConst(30)
    try testing.expectEqual(@as(usize, 1), ins.items.len);
    try testing.expectEqual(OpCode.LoadConst, ins.items[0].op);
    try testing.expectEqual(@as(u32, 1), stats.folds);

    // 检查常量池中有值为 30 的常量
    const folded_idx: usize = @intCast(ins.items[0].arg.Int);
    try testing.expectEqual(ConstantValue{ .Int = 30 }, pool.items[folded_idx]);
}

test "const fold int subtraction" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    const c0 = addConstToPool(&pool, ConstantValue{ .Int = 100 });
    const c1 = addConstToPool(&pool, ConstantValue{ .Int = 37 });
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c0 }));
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c1 }));
    try ins.append(testing.allocator, makeIns(.BinarySub));

    const stats = optimize.optimize(2, &ins, &pool, testing.allocator);

    try testing.expectEqual(@as(usize, 1), ins.items.len);
    try testing.expectEqual(@as(u32, 1), stats.folds);

    const folded_idx: usize = @intCast(ins.items[0].arg.Int);
    try testing.expectEqual(ConstantValue{ .Int = 63 }, pool.items[folded_idx]);
}

test "const fold float multiplication" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    const c0 = addConstToPool(&pool, ConstantValue{ .Float = 3.0 });
    const c1 = addConstToPool(&pool, ConstantValue{ .Float = 4.0 });
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c0 }));
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c1 }));
    try ins.append(testing.allocator, makeIns(.MulFloat));

    const stats = optimize.optimize(2, &ins, &pool, testing.allocator);

    try testing.expectEqual(@as(usize, 1), ins.items.len);
    try testing.expectEqual(@as(u32, 1), stats.folds);

    const folded_idx: usize = @intCast(ins.items[0].arg.Int);
    try testing.expectEqual(ConstantValue{ .Float = 12.0 }, pool.items[folded_idx]);
}

test "const fold division by zero produces nothing" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    const c0 = addConstToPool(&pool, ConstantValue{ .Int = 10 });
    const c1 = addConstToPool(&pool, ConstantValue{ .Int = 0 });
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c0 }));
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c1 }));
    try ins.append(testing.allocator, makeIns(.DivInt));

    const stats = optimize.optimize(2, &ins, &pool, testing.allocator);

    // 除零不折叠, 保持原样
    try testing.expectEqual(@as(usize, 3), ins.items.len);
    try testing.expectEqual(@as(u32, 0), stats.folds);
}

test "const fold unary negation" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    const c0 = addConstToPool(&pool, ConstantValue{ .Int = 42 });
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c0 }));
    try ins.append(testing.allocator, makeIns(.UnaryNeg));

    const stats = optimize.optimize(2, &ins, &pool, testing.allocator);

    try testing.expectEqual(@as(usize, 1), ins.items.len);
    try testing.expectEqual(@as(u32, 1), stats.folds);

    const folded_idx: usize = @intCast(ins.items[0].arg.Int);
    try testing.expectEqual(ConstantValue{ .Int = -42 }, pool.items[folded_idx]);
}

test "const fold unary not" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    const c0 = addConstToPool(&pool, ConstantValue{ .Bool = true });
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c0 }));
    try ins.append(testing.allocator, makeIns(.UnaryNot));

    const stats = optimize.optimize(2, &ins, &pool, testing.allocator);

    try testing.expectEqual(@as(usize, 1), ins.items.len);
    try testing.expectEqual(@as(u32, 1), stats.folds);

    const folded_idx: usize = @intCast(ins.items[0].arg.Int);
    try testing.expectEqual(ConstantValue{ .Bool = false }, pool.items[folded_idx]);
}

test "const fold boolean and" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    const c0 = addConstToPool(&pool, ConstantValue{ .Bool = true });
    const c1 = addConstToPool(&pool, ConstantValue{ .Bool = false });
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c0 }));
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c1 }));
    try ins.append(testing.allocator, makeIns(.BinaryAnd));

    const stats = optimize.optimize(2, &ins, &pool, testing.allocator);

    try testing.expectEqual(@as(usize, 1), ins.items.len);
    try testing.expectEqual(@as(u32, 1), stats.folds);

    const folded_idx: usize = @intCast(ins.items[0].arg.Int);
    try testing.expectEqual(ConstantValue{ .Bool = false }, pool.items[folded_idx]);
}

test "const fold equality" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    const c0 = addConstToPool(&pool, ConstantValue{ .Int = 5 });
    const c1 = addConstToPool(&pool, ConstantValue{ .Int = 5 });
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c0 }));
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c1 }));
    try ins.append(testing.allocator, makeIns(.EqInt));

    _ = optimize.optimize(2, &ins, &pool, testing.allocator);

    try testing.expectEqual(@as(usize, 1), ins.items.len);

    const folded_idx: usize = @intCast(ins.items[0].arg.Int);
    try testing.expectEqual(ConstantValue{ .Bool = true }, pool.items[folded_idx]);
}

// ==================== 死代码消除测试 ====================

test "dead code: effect-free + Pop" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    const c0 = addConstToPool(&pool, ConstantValue{ .Int = 42 });
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c0 }));
    try ins.append(testing.allocator, makeIns(.Pop));

    _ = optimize.optimize(3, &ins, &pool, testing.allocator);

    // LoadConst + Pop → 移除 pair
    try testing.expectEqual(@as(usize, 0), ins.items.len);
}

test "dead code: Pop + Pop → Pop" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    try ins.append(testing.allocator, makeIns(.Pop));
    try ins.append(testing.allocator, makeIns(.Pop));

    _ = optimize.optimize(3, &ins, &pool, testing.allocator);

    try testing.expectEqual(@as(usize, 1), ins.items.len);
    try testing.expectEqual(OpCode.Pop, ins.items[0].op);
}

// ==================== 分支折叠测试 ====================

test "branch fold: LoadTrue + JumpIfFalse → removed" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    try ins.append(testing.allocator, makeIns(.LoadTrue));
    try ins.append(testing.allocator, Instruction.withArg(.JumpIfFalse, BytecodeArg{ .Int = 5 }));

    _ = optimize.optimize(5, &ins, &pool, testing.allocator);

    // LoadTrue + JumpIfFalse → 条件始终 true, 测试移除
    try testing.expectEqual(@as(usize, 0), ins.items.len);
}

test "branch fold: LoadFalse + JumpIfTrue → removed" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    try ins.append(testing.allocator, makeIns(.LoadFalse));
    try ins.append(testing.allocator, Instruction.withArg(.JumpIfTrue, BytecodeArg{ .Int = 5 }));

    _ = optimize.optimize(5, &ins, &pool, testing.allocator);

    try testing.expectEqual(@as(usize, 0), ins.items.len);
}

test "branch fold: LoadTrue + JumpIfTrue → unconditional Jump" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    try ins.append(testing.allocator, makeIns(.LoadTrue));
    try ins.append(testing.allocator, Instruction.withArg(.JumpIfTrue, BytecodeArg{ .Int = 42 }));

    _ = optimize.optimize(5, &ins, &pool, testing.allocator);

    try testing.expectEqual(@as(usize, 1), ins.items.len);
    try testing.expectEqual(OpCode.Jump, ins.items[0].op);
    try testing.expectEqual(@as(i32, 42), ins.items[0].arg.Int);
}

// ==================== 死代码块消除测试 ====================

test "dead block: Jump to next instruction → removed" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    // Jump(i+1) → 跳到下一条, 冗余
    try ins.append(testing.allocator, Instruction.withArg(.Jump, BytecodeArg{ .Int = 1 }));
    try ins.append(testing.allocator, makeIns(.LoadNil));

    _ = optimize.optimize(6, &ins, &pool, testing.allocator);

    try testing.expectEqual(@as(usize, 1), ins.items.len);
}

// ==================== opt_level 1 测试 ====================

test "opt level 1: no optimization" {
    var ins: std.ArrayList(Instruction) = .empty;
    defer ins.deinit(testing.allocator);
    var pool: std.ArrayList(ConstantValue) = .empty;
    defer pool.deinit(testing.allocator);

    const c0 = addConstToPool(&pool, ConstantValue{ .Int = 10 });
    const c1 = addConstToPool(&pool, ConstantValue{ .Int = 20 });
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c0 }));
    try ins.append(testing.allocator, Instruction.withArg(.LoadConst, BytecodeArg{ .Int = c1 }));
    try ins.append(testing.allocator, makeIns(.BinaryAdd));

    const stats = optimize.optimize(1, &ins, &pool, testing.allocator);

    // L1 不做任何优化
    try testing.expectEqual(@as(usize, 3), ins.items.len);
    try testing.expectEqual(@as(u32, 0), stats.total());
}
