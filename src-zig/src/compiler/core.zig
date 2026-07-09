const std = @import("std");
const OpCode = @import("../opcode.zig").OpCode;
const Instruction = @import("../compiler_bytecode.zig").Instruction;
const BytecodeArg = @import("../compiler_bytecode.zig").BytecodeArg;
const BytecodeFunction = @import("../compiler_bytecode.zig").BytecodeFunction;
const ConstantValue = @import("../compiler_bytecode.zig").ConstantValue;
const Expr = @import("../parser/ast.zig").Expr;
const EnumVariant = @import("../parser/ast.zig").EnumVariant;
const VxSettings = @import("../vxsetting.zig").VxSettings;

const EnumVariantList = std.ArrayList(EnumVariant);

// ==================== LoopInfo ====================

pub const LoopInfo = struct {
    start: usize,
    break_jumps: std.ArrayList(usize),
    continue_jumps: std.ArrayList(usize),
    label: ?[]const u8,
};

// ==================== KnownType ====================

pub const KnownType = enum(u8) {
    Int,
    Float,
    Bool,
    String,
    Array,
    Map,
    Instance,
    Pointer,
    Nil,
    Unknown,

    /// 返回类型的显示名称
    pub fn as_str(self: KnownType) []const u8 {
        return switch (self) {
            .Int => "int",
            .Float => "float",
            .Bool => "bool",
            .String => "string",
            .Array => "array",
            .Map => "map",
            .Instance => "instance",
            .Pointer => "pointer",
            .Nil => "nil",
            .Unknown => "unknown",
        };
    }

    /// 判断是否为数值类型（可用于算术运算）
    pub fn is_numeric(self: KnownType) bool {
        return switch (self) {
            .Int, .Float => true,
            else => false,
        };
    }

    /// 判断是否为复合类型
    pub fn is_compound(self: KnownType) bool {
        return switch (self) {
            .Array, .Map, .Instance => true,
            else => false,
        };
    }
};

// ==================== Compiler ====================

pub const Compiler = struct {
    allocator: std.mem.Allocator,

    settings: VxSettings,
    constants: std.ArrayList(ConstantValue),
    instructions: std.ArrayList(Instruction),
    functions: std.ArrayList(BytecodeFunction),
    loop_stack: std.ArrayList(LoopInfo),
    for_counter: usize,
    var_types: std.StringHashMap(KnownType),
    var_slots: std.StringHashMap(u32),
    next_slot: u32,
    stack_types: std.ArrayList(KnownType),
    opt_level: u8,
    warn_dead_code: bool,
    error_dead_code: bool,
    /// 宏注册表（placeholder 简化实现）
    macros: MacroRegistry,
    /// 外部依赖（import 语句），用于静态链接时的动态库链接
    external_deps: std.ArrayList([]const u8),
    /// 限定名 → 原始 C 符号名映射（如 "io.write" → "write"）
    extern_qualified_names: std.StringHashMap([]const u8),
    /// slot 号 -> KnownType 映射，用于 TypeIR 生成
    slot_types: std.AutoHashMap(u32, KnownType),
    /// 枚举名 -> 变体列表（变体名, 值），供 match 模式解析枚举变体
    enum_defs: std.StringHashMap(EnumVariantList),

    pub fn init(allocator: std.mem.Allocator, settings: VxSettings) Compiler {
        return Compiler{
            .allocator = allocator,
            .settings = settings,
            .constants = .empty,
            .instructions = .empty,
            .functions = .empty,
            .loop_stack = .empty,
            .for_counter = 0,
            .var_types = std.StringHashMap(KnownType).init(allocator),
            .var_slots = std.StringHashMap(u32).init(allocator),
            .next_slot = 0,
            .stack_types = .empty,
            .opt_level = 0,
            .warn_dead_code = false,
            .error_dead_code = false,
            .macros = MacroRegistry.init(allocator),
            .external_deps = .empty,
            .extern_qualified_names = std.StringHashMap([]const u8).init(allocator),
            .slot_types = std.AutoHashMap(u32, KnownType).init(allocator),
            .enum_defs = std.StringHashMap(EnumVariantList).init(allocator),
        };
    }

    pub fn deinit(self: *Compiler) void {
        self.constants.deinit(self.allocator);
        self.instructions.deinit(self.allocator);
        for (self.functions.items) |*f| f.deinit(self.allocator);
        self.functions.deinit(self.allocator);
        for (self.loop_stack.items) |*li| {
            li.break_jumps.deinit(self.allocator);
            li.continue_jumps.deinit(self.allocator);
            if (li.label) |l| self.allocator.free(l);
        }
        self.loop_stack.deinit(self.allocator);
        self.var_types.deinit();
        self.var_slots.deinit();
        self.stack_types.deinit(self.allocator);
        self.macros.deinit();
        for (self.external_deps.items) |dep| self.allocator.free(dep);
        self.external_deps.deinit(self.allocator);
        var eql_iter = self.extern_qualified_names.iterator();
        while (eql_iter.next()) |entry| {
            self.allocator.free(entry.key_ptr.*);
            self.allocator.free(entry.value_ptr.*);
        }
        self.extern_qualified_names.deinit();
        self.slot_types.deinit();
        var ed_iter = self.enum_defs.iterator();
        while (ed_iter.next()) |entry| {
            for (entry.value_ptr.*.items) |*v| self.allocator.free(v.name);
            entry.value_ptr.*.deinit(self.allocator);
            self.allocator.free(entry.key_ptr.*);
        }
        self.enum_defs.deinit();
    }

    pub fn withOptions(self: *Compiler, opt_level: u8, warn_dead_code: bool, error_dead_code: bool) *Compiler {
        self.opt_level = opt_level;
        self.warn_dead_code = warn_dead_code;
        self.error_dead_code = error_dead_code;
        return self;
    }

    /// 在编译之前展开宏
    pub fn expandMacros(self: *Compiler, ast: std.ArrayList(*Expr)) !std.ArrayList(*Expr) {
        _ = self;
        return ast; // placeholder: 直接返回 AST
    }

    /// 获取宏系统的统计信息（placeholder）
    pub fn getMacroStats(self: *Compiler) struct { u64, u64, f64 } {
        _ = self;
        return .{ 0, 0, 0.0 };
    }

    pub fn allocateSlot(self: *Compiler, name: []const u8) u32 {
        if (self.var_slots.get(name)) |slot| return slot;
        const slot = self.next_slot;
        self.next_slot += 1;
        self.var_slots.put(name, slot) catch @panic("OOM");
        // 同时记录该变量的类型，用于后续 TypeIR 生成
        if (self.var_types.get(name)) |ty| {
            self.slot_types.put(slot, ty) catch @panic("OOM");
        }
        return slot;
    }

    pub fn pushStackType(self: *Compiler, t: KnownType) void {
        self.stack_types.append(self.allocator, t) catch @panic("OOM");
    }

    pub fn popStackType(self: *Compiler) KnownType {
        return self.stack_types.popOrNull() orelse .Unknown;
    }

    pub fn setVarType(self: *Compiler, name: []const u8, t: KnownType) void {
        self.var_types.put(name, t) catch @panic("OOM");
        // 如果 slot 已经分配，同步更新 slot_types
        if (self.var_slots.get(name)) |slot| {
            self.slot_types.put(slot, t) catch @panic("OOM");
        }
    }

    pub fn getVarType(self: *const Compiler, name: []const u8) KnownType {
        return self.var_types.get(name) orelse .Unknown;
    }

    /// 将类型名字符串解析为 KnownType
    pub fn typeNameToKnownType(type_str: []const u8) KnownType {
        if (std.mem.eql(u8, type_str, "int")) return .Int;
        if (std.mem.eql(u8, type_str, "float") or std.mem.eql(u8, type_str, "double")) return .Float;
        if (std.mem.eql(u8, type_str, "bool")) return .Bool;
        if (std.mem.eql(u8, type_str, "string") or std.mem.eql(u8, type_str, "String")) return .String;
        if (std.mem.eql(u8, type_str, "pointer")) return .Pointer;
        if (std.mem.eql(u8, type_str, "void") or std.mem.eql(u8, type_str, "nil")) return .Nil;
        if (std.mem.eql(u8, type_str, "array")) return .Array;
        if (std.mem.eql(u8, type_str, "map")) return .Map;
        return .Unknown;
    }

    pub fn binaryOpSpecialized(self: *const Compiler, op: []const u8, left: KnownType, right: KnownType) ?OpCode {
        _ = self;
        if (std.mem.eql(u8, op, "+")) {
            if (left == .Int and right == .Int) return .AddInt;
            if (left == .Float and right == .Float) return .AddFloat;
        }
        if (std.mem.eql(u8, op, "-")) {
            if (left == .Int and right == .Int) return .SubInt;
            if (left == .Float and right == .Float) return .SubFloat;
        }
        if (std.mem.eql(u8, op, "*")) {
            if (left == .Int and right == .Int) return .MulInt;
            if (left == .Float and right == .Float) return .MulFloat;
        }
        if (std.mem.eql(u8, op, "/")) {
            if (left == .Int and right == .Int) return .DivInt;
            if (left == .Float and right == .Float) return .DivFloat;
        }
        if (std.mem.eql(u8, op, "%")) {
            if (left == .Int and right == .Int) return .ModInt;
        }
        if (std.mem.eql(u8, op, "==")) {
            if (left == .Int and right == .Int) return .EqInt;
            if (left == .Float and right == .Float) return .EqFloat;
        }
        if (std.mem.eql(u8, op, "<")) {
            if (left == .Int and right == .Int) return .LtInt;
            if (left == .Float and right == .Float) return .LtFloat;
        }
        if (std.mem.eql(u8, op, ">")) {
            if (left == .Int and right == .Int) return .GtInt;
            if (left == .Float and right == .Float) return .GtFloat;
        }
        if (std.mem.eql(u8, op, "<=")) {
            if (left == .Int and right == .Int) return .LeInt;
            if (left == .Float and right == .Float) return .LeFloat;
        }
        if (std.mem.eql(u8, op, ">=")) {
            if (left == .Int and right == .Int) return .GeInt;
            if (left == .Float and right == .Float) return .GeFloat;
        }
        if (std.mem.eql(u8, op, "&&")) {
            if (left == .Bool and right == .Bool) return .And;
        }
        if (std.mem.eql(u8, op, "||")) {
            if (left == .Bool and right == .Bool) return .Or;
        }
        return null;
    }

    pub fn unaryOpSpecialized(self: *const Compiler, op: []const u8, operand: KnownType) ?OpCode {
        _ = self;
        if (std.mem.eql(u8, op, "-")) {
            if (operand == .Int) return .NegInt;
            if (operand == .Float) return .NegFloat;
        }
        if (std.mem.eql(u8, op, "!") or std.mem.eql(u8, op, "not")) {
            if (operand == .Bool) return .Not;
        }
        return null;
    }

    pub fn addConst(self: *Compiler, v: ConstantValue) usize {
        self.constants.append(self.allocator, v) catch @panic("OOM");
        return self.constants.items.len - 1;
    }

    pub fn emit(self: *Compiler, op: OpCode, arg: BytecodeArg) usize {
        self.instructions.append(self.allocator, Instruction{ .op = op, .arg = arg }) catch @panic("OOM");
        return self.instructions.items.len - 1;
    }

    pub fn patch(self: *Compiler, pos: usize, tgt: usize) void {
        if (pos < self.instructions.items.len) {
            const inst = &self.instructions.items[pos];
            inst.arg = .{ .Int = @as(i32, @intCast(tgt)) };
        }
    }
};

// ==================== MacroRegistry placeholder ====================

pub const MacroRegistry = struct {
    allocator: std.mem.Allocator,

    pub fn init(allocator: std.mem.Allocator) MacroRegistry {
        return MacroRegistry{ .allocator = allocator };
    }

    pub fn deinit(self: *MacroRegistry) void {
        _ = self;
    }

    pub fn getStats(self: *const MacroRegistry) struct { u64, u64, f64 } {
        _ = self;
        return .{ 0, 0, 0.0 };
    }
};
