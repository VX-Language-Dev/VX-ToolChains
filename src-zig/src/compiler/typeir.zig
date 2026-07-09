const std = @import("std");
const OpCode = @import("../opcode.zig").OpCode;
const Instruction = @import("../compiler_bytecode.zig").Instruction;
const BytecodeArg = @import("../compiler_bytecode.zig").BytecodeArg;
const ConstantValue = @import("../compiler_bytecode.zig").ConstantValue;
const TypedInstruction = @import("../type_ir.zig").TypedInstruction;
const Type = @import("../type_ir.zig").Type;
const VarId = @import("../type_ir.zig").VarId;
const FuncId = @import("../type_ir.zig").FuncId;
const StructLayoutId = @import("../type_ir.zig").StructLayoutId;

/// TypeIR 栈模拟器：在生成 TypeIR 时追踪字节码栈，
/// 将栈位置映射为正确的 VarId（TypeIR 中的指令索引）。
pub const TypeIRSimulator = struct {
    allocator: std.mem.Allocator,
    body: std.ArrayList(TypedInstruction),
    /// 记录每个 slot 的类型信息，用于填充 TypeFunction::local_types
    slot_types: std.AutoHashMap(u32, Type),
    /// 追踪栈上每个值对应的 VarId
    stack: std.ArrayList(VarId),
    /// 跟踪字符串常量 VarId → 函数名，用于解析 Call 的 callee
    const_strings: std.AutoHashMap(VarId, []const u8),
    /// 函数名 → TypeIR FuncId
    func_name_to_id: std.StringHashMap(FuncId),
    /// 限定名 → 原始 C 符号名映射（如 "io.write" → "write"）
    extern_qualified_names: std.StringHashMap([]const u8),
    /// 最大 VarId + 1
    max_slot: u32,

    pub fn init(
        allocator: std.mem.Allocator,
        func_name_to_id: std.StringHashMap(FuncId),
        extern_qualified_names: std.StringHashMap([]const u8),
    ) TypeIRSimulator {
        return TypeIRSimulator{
            .allocator = allocator,
            .body = std.ArrayList(TypedInstruction).init(allocator),
            .slot_types = std.AutoHashMap(u32, Type).init(allocator),
            .stack = std.ArrayList(VarId).init(allocator),
            .const_strings = std.AutoHashMap(VarId, []const u8).init(allocator),
            .func_name_to_id = func_name_to_id,
            .extern_qualified_names = extern_qualified_names,
            .max_slot = 0,
        };
    }

    pub fn deinit(self: *TypeIRSimulator) void {
        for (self.body.items) |*inst| inst.deinit(self.allocator);
        self.body.deinit();
        self.slot_types.deinit();
        self.stack.deinit();
        var cs_iter = self.const_strings.iterator();
        while (cs_iter.next()) |entry| {
            self.allocator.free(entry.value_ptr.*);
        }
        self.const_strings.deinit();
        self.func_name_to_id.deinit();
        var eqn_iter = self.extern_qualified_names.iterator();
        while (eqn_iter.next()) |entry| {
            self.allocator.free(entry.key_ptr.*);
            self.allocator.free(entry.value_ptr.*);
        }
        self.extern_qualified_names.deinit();
    }

    fn emit(self: *TypeIRSimulator, inst: TypedInstruction) void {
        self.body.append(inst) catch @panic("OOM");
    }

    fn pushVal(self: *TypeIRSimulator, var_id: VarId) void {
        self.stack.append(var_id) catch @panic("OOM");
        if (var_id >= self.max_slot) {
            self.max_slot = var_id + 1;
        }
    }

    pub fn popVal(self: *TypeIRSimulator) VarId {
        return self.stack.popOrNull() orelse 0;
    }

    fn peekVal(self: *const TypeIRSimulator) ?VarId {
        if (self.stack.items.len == 0) return null;
        return self.stack.items[self.stack.items.len - 1];
    }

    /// 分配一个新的 VarId
    fn allocVid(self: *TypeIRSimulator) VarId {
        const vid = self.max_slot;
        self.max_slot += 1;
        return vid;
    }

    /// 记录 slot 的类型（用于后续填充 local_types）
    pub fn setSlotType(self: *TypeIRSimulator, slot: u32, ty: Type) void {
        self.slot_types.put(slot, ty) catch @panic("OOM");
        if (slot >= self.max_slot) {
            self.max_slot = slot + 1;
        }
    }

    /// 获取当前最大 VarId + 1（即变量总数）
    pub fn varCount(self: *const TypeIRSimulator) u32 {
        return self.max_slot;
    }

    /// 获取收集到的 slot 类型映射
    pub fn slotTypes(self: *const TypeIRSimulator) *const std.AutoHashMap(u32, Type) {
        return &self.slot_types;
    }

    pub fn translateInst(self: *TypeIRSimulator, inst: *const Instruction, constants: []const ConstantValue) void {
        switch (inst.op) {
            .LoadConst => {
                const cv: ?ConstantValue = switch (inst.arg) {
                    .Int => |idx| if (idx >= 0 and @as(usize, @intCast(idx)) < constants.len)
                        constants[@as(usize, @intCast(idx))]
                    else
                        null,
                    else => null,
                };
                const vid = self.allocVid();
                const typed: TypedInstruction = if (cv) |c| blk: {
                    switch (c) {
                        .Int => |v| break :blk TypedInstruction{ .ConstInt = v },
                        .Float => |v| break :blk TypedInstruction{ .ConstFloat = v },
                        .Bool => |v| break :blk TypedInstruction{ .ConstBool = v },
                        .String => |s| {
                            self.const_strings.put(vid, self.allocator.dupe(u8, s) catch @panic("OOM")) catch @panic("OOM");
                            break :blk TypedInstruction{ .ConstString = self.allocator.dupe(u8, s) catch @panic("OOM") };
                        },
                        else => break :blk TypedInstruction{ .ConstNil = {} },
                    }
                } else TypedInstruction{ .ConstNil = {} };
                self.emit(typed);
                self.pushVal(vid);
            },
            .LoadNil => {
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .ConstNil = {} });
                self.pushVal(vid);
            },
            .LoadTrue => {
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .ConstBool = true });
                self.pushVal(vid);
            },
            .LoadFalse => {
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .ConstBool = false });
                self.pushVal(vid);
            },
            .LoadVar => {
                const slot: u32 = switch (inst.arg) {
                    .Int => |s| @as(u32, @intCast(s)),
                    else => 0,
                };
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .LoadVar = slot });
                self.pushVal(vid);
            },
            .StoreVar, .DefineVar => {
                const slot: u32 = switch (inst.arg) {
                    .Int => |s| @as(u32, @intCast(s)),
                    else => 0,
                };
                _ = self.popVal();
                self.emit(TypedInstruction{ .StoreVar = slot });
                self.setSlotType(slot, .Unknown);
            },
            .Dup => {
                if (self.peekVal()) |v| {
                    self.emit(TypedInstruction{ .Dup = {} });
                    self.pushVal(v);
                } else {
                    self.emit(TypedInstruction{ .Dup = {} });
                }
            },
            .Pop => {
                _ = self.popVal();
                self.emit(TypedInstruction{ .Pop = {} });
            },
            .Jump => {
                const t: u32 = switch (inst.arg) {
                    .Int => |v| @as(u32, @intCast(v)),
                    else => 0,
                };
                self.emit(TypedInstruction{ .Jump = t });
            },
            .JumpIfFalse => {
                const vid = self.popVal();
                const t: u32 = switch (inst.arg) {
                    .Int => |v| @as(u32, @intCast(v)),
                    else => 0,
                };
                self.emit(TypedInstruction{ .JumpIfFalse = .{ .cond = vid, .target = t } });
            },
            .JumpIfTrue => {
                const vid = self.popVal();
                const t: u32 = switch (inst.arg) {
                    .Int => |v| @as(u32, @intCast(v)),
                    else => 0,
                };
                self.emit(TypedInstruction{ .JumpIfTrue = .{ .cond = vid, .target = t } });
            },
            .AddInt, .BinaryAdd => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Add = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .SubInt, .BinarySub => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Sub = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .MulInt, .BinaryMul => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Mul = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .DivInt, .BinaryDiv => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Div = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .ModInt, .BinaryMod => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Mod = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .AddFloat => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .F64Add = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .SubFloat => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .F64Sub = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .MulFloat => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .F64Mul = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .DivFloat => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .F64Div = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .EqInt, .BinaryEq => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Eq = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .BinaryNe => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Ne = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .LtInt, .BinaryLt => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Lt = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .GtInt, .BinaryGt => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Gt = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .LeInt, .BinaryLe => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Le = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .GeInt, .BinaryGe => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Ge = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .EqFloat => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .F64Eq = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .LtFloat => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .F64Lt = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .GtFloat => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .F64Gt = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .LeFloat => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .F64Le = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .GeFloat => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .F64Ge = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .NegInt => {
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Neg = a });
                self.pushVal(vid);
            },
            .NegFloat => {
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .F64Neg = a });
                self.pushVal(vid);
            },
            .Not, .UnaryNot => {
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .BoolNot = a });
                self.pushVal(vid);
            },
            .And => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32And = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .Or => {
                const b = self.popVal();
                const a = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .I32Or = .{ .a = a, .b = b } });
                self.pushVal(vid);
            },
            .MakeArray => {
                const count: usize = switch (inst.arg) {
                    .Int => |n| @as(usize, @intCast(n)),
                    else => 0,
                };
                var elems = std.ArrayList(VarId).init(self.allocator);
                var i: usize = 0;
                while (i < count) : (i += 1) {
                    elems.append(self.popVal()) catch @panic("OOM");
                }
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .MakeArray = .{ .base = 0, .args = elems.toOwnedSlice() catch @panic("OOM") } });
                self.pushVal(vid);
            },
            .IndexGet => {
                const idx = self.popVal();
                const obj = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .IndexGet = .{ .arr = obj, .idx = idx } });
                self.pushVal(vid);
            },
            .IndexSet => {
                const val = self.popVal();
                const idx = self.popVal();
                const obj = self.popVal();
                self.emit(TypedInstruction{ .IndexSet = .{ .arr = obj, .idx = idx, .val = val } });
                self.pushVal(obj);
            },
            .MakeMap => {
                const count: usize = switch (inst.arg) {
                    .Int => |n| @as(usize, @intCast(n)),
                    else => 0,
                };
                var i: usize = 0;
                while (i < count * 2) : (i += 1) _ = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .MakeMap = &.{} });
                self.pushVal(vid);
            },
            .PropertyGet, .PointerMember => {
                const obj = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .GetField = .{ .obj = obj, .idx = 0 } });
                self.pushVal(vid);
            },
            .PropertySet => {
                const val = self.popVal();
                const obj = self.popVal();
                self.emit(TypedInstruction{ .SetField = .{ .obj = obj, .idx = 0, .val = val } });
                self.pushVal(obj);
            },
            .OwnershipMove => {
                if (self.peekVal()) |v| {
                    self.emit(TypedInstruction{ .OwnershipMove = v });
                } else {
                    self.emit(TypedInstruction{ .OwnershipMove = 0 });
                }
            },
            .BorrowCheck => {
                if (self.peekVal()) |v| {
                    self.emit(TypedInstruction{ .Borrow = v });
                } else {
                    self.emit(TypedInstruction{ .Borrow = 0 });
                }
            },
            .AliveCheck => {
                if (self.peekVal()) |v| {
                    self.emit(TypedInstruction{ .AliveCheck = v });
                }
            },
            .Free => {
                const ptr = self.popVal();
                self.emit(TypedInstruction{ .Free = ptr });
            },
            .Call => {
                const num_args: usize = switch (inst.arg) {
                    .Int => |n| @as(usize, @intCast(n)),
                    else => 0,
                };
                var args = std.ArrayList(VarId).init(self.allocator);
                var i: usize = 0;
                while (i < num_args) : (i += 1) {
                    args.append(self.popVal()) catch @panic("OOM");
                }
                // 反转：栈顶是最后一个参数，恢复原始顺序
                std.mem.reverse(VarId, args.items);
                const callee_vid = self.popVal();
                // 根据 callee 字符串常量解析函数 ID
                const callee_name = self.const_strings.get(callee_vid);
                const callee_id = if (callee_name) |name| self.func_name_to_id.get(name) else null;

                if (callee_id) |fid| {
                    const vid = self.allocVid();
                    self.emit(TypedInstruction{ .Call = .{
                        .func_id = fid,
                        .args = args.toOwnedSlice() catch @panic("OOM"),
                        .ext_name = null,
                    } });
                    self.pushVal(vid);
                } else {
                    // 未解析的函数名（内建函数或外部函数）
                    const name_for_ext = callee_name;
                    // 属性访问调用修复
                    var ext_name: ?[]const u8 = null;
                    var real_args: []VarId = undefined;
                    if (args.items.len > 0) {
                        const first_vid = args.items[0];
                        if (self.const_strings.get(first_vid)) |method_name| {
                            // 属性访问模式：args[0] = 方法名，callee = 对象
                            var new_args = std.ArrayList(VarId).init(self.allocator);
                            new_args.append(callee_vid) catch @panic("OOM");
                            var j: usize = 1;
                            while (j < args.items.len) : (j += 1) {
                                new_args.append(args.items[j]) catch @panic("OOM");
                            }
                            ext_name = self.allocator.dupe(u8, method_name) catch @panic("OOM");
                            real_args = new_args.toOwnedSlice() catch @panic("OOM");
                            args.deinit();
                        } else {
                            // 普通外部调用
                            if (name_for_ext) |n| {
                                ext_name = if (self.extern_qualified_names.get(n)) |resolved|
                                    self.allocator.dupe(u8, resolved) catch @panic("OOM")
                                else
                                    self.allocator.dupe(u8, n) catch @panic("OOM");
                            }
                            real_args = args.toOwnedSlice() catch @panic("OOM");
                        }
                    } else {
                        if (name_for_ext) |n| {
                            ext_name = if (self.extern_qualified_names.get(n)) |resolved|
                                self.allocator.dupe(u8, resolved) catch @panic("OOM")
                            else
                                self.allocator.dupe(u8, n) catch @panic("OOM");
                        }
                        real_args = args.toOwnedSlice() catch @panic("OOM");
                    }
                    const vid = self.allocVid();
                    self.emit(TypedInstruction{ .Call = .{
                        .func_id = std.math.maxInt(FuncId),
                        .args = real_args,
                        .ext_name = ext_name,
                    } });
                    self.pushVal(vid);
                }
            },
            .Return => {
                const ret = self.popVal();
                self.emit(TypedInstruction{ .Return = ret });
            },
            .AddressOf => {
                if (self.peekVal()) |v| {
                    self.pushVal(v);
                }
            },
            .New, .Newz, .MakeStruct, .MakeClass => {
                const count: usize = switch (inst.arg) {
                    .Int => |n| @as(usize, @intCast(n)),
                    else => 0,
                };
                var i: usize = 0;
                while (i < count) : (i += 1) _ = self.popVal();
                // New/Newz 也会弹出类名字符串
                if (inst.op == .New or inst.op == .Newz) {
                    _ = self.popVal();
                }
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .MakeStruct = .{
                    .layout = StructLayoutId{ .id = 0 },
                    .args = &.{},
                } });
                self.pushVal(vid);
            },
            // 系统调用：忽略 TypeIR 映射
            .SysArgv => {
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .MakeArray = .{ .base = 0, .args = &.{} } });
                self.pushVal(vid);
            },
            .System => {
                _ = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .ConstInt = 0 });
                self.pushVal(vid);
            },
            .FileRead => {
                _ = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .ConstString = &.{} });
                self.pushVal(vid);
            },
            .FileWrite => {
                _ = self.popVal();
                _ = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .ConstBool = false });
                self.pushVal(vid);
            },
            .FileExists => {
                _ = self.popVal();
                const vid = self.allocVid();
                self.emit(TypedInstruction{ .ConstBool = false });
                self.pushVal(vid);
            },
            // 忽略 OpCode（无栈效果或仅控制流）
            .ScopeDrop, .Halt, .Import, .BinaryPow, .Break, .Continue, .Iterate, .Next => {},
            else => {},
        }
    }

    pub fn intoBody(self: *TypeIRSimulator) std.ArrayList(TypedInstruction) {
        return self.body;
    }
};
