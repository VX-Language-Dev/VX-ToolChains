const std = @import("std");
const Allocator = std.mem.Allocator;
const OpCode = @import("opcode.zig").OpCode;
const Type = @import("type_ir.zig").Type;

// ==================== 编译器字节码格式 ====================
// 注意：此处的字节码仅为编译器内部用于推导 TypeIR 的临时表示，
// 不再作为可持久化或执行的输出格式。

pub const BytecodeArg = union(enum) {
    None,
    Int: i32,
    String: []const u8,
    ImportTuple: struct { a: []const u8, b: ?[]const u8, c: ?[]const u8 },

    /// Convert to VM argument representation.
    /// Returns a tuple of (optional int, optional owned string).
    /// The caller is responsible for freeing the returned string if present.
    pub fn toVmArgs(self: BytecodeArg, allocator: Allocator) struct { ?i32, ?[]u8 } {
        return switch (self) {
            .None => .{ null, null },
            .Int => |v| .{ v, null },
            .String => |s| .{ null, allocator.dupe(u8, s) catch @panic("OOM") },
            .ImportTuple => |t| {
                const result = std.fmt.allocPrint(allocator, "{s},{s},{s}", .{
                    t.b orelse "",
                    t.c orelse "",
                    t.a,
                }) catch @panic("OOM");
                return .{ null, result };
            },
        };
    }
};

pub const Instruction = struct {
    op: OpCode,
    arg: BytecodeArg,

    pub fn init(op: OpCode) Instruction {
        return Instruction{ .op = op, .arg = .None };
    }

    pub fn withArg(op: OpCode, arg: BytecodeArg) Instruction {
        return Instruction{ .op = op, .arg = arg };
    }
};

pub const BytecodeFunction = struct {
    name: []const u8,
    instructions: std.ArrayList(Instruction),
    num_params: usize,
    has_return: bool,
    param_names: std.ArrayList([]const u8),
    /// 参数类型（VX 静态类型，以 TypeIR Type 表示）
    param_types: std.ArrayList(Type),

    pub fn deinit(self: *BytecodeFunction, allocator: Allocator) void {
        allocator.free(self.name);
        self.instructions.deinit(allocator);
        for (self.param_names.items) |pn| allocator.free(pn);
        self.param_names.deinit(allocator);
        self.param_types.deinit(allocator);
    }
};

pub const ConstantValue = union(enum) {
    Nil,
    Bool: bool,
    Int: i64,
    Float: f64,
    String: []const u8,
};

pub const StructEntry = struct { name: []const u8, fields: std.ArrayList([]const u8) };
pub const ClassEntry = struct { name: []const u8, fields: std.ArrayList([]const u8) };

pub const CompiledModule = struct {
    functions: std.ArrayList(BytecodeFunction),
    constants: std.ArrayList(ConstantValue),
    structs: std.ArrayList(StructEntry),
    classes: std.ArrayList(ClassEntry),
    type_ir_data: []const u8,
    target_triple: []const u8,
    /// 外部依赖信息，用于静态链接时的动态库链接
    external_deps: std.ArrayList([]const u8),

    pub fn deinit(self: *CompiledModule, allocator: Allocator) void {
        for (self.functions.items) |*f| f.deinit(allocator);
        self.functions.deinit(allocator);
        self.constants.deinit(allocator);

        for (self.structs.items) |*s| {
            allocator.free(s.name);
            for (s.fields.items) |f| allocator.free(f);
            s.fields.deinit(allocator);
        }
        self.structs.deinit(allocator);

        for (self.classes.items) |*c| {
            allocator.free(c.name);
            for (c.fields.items) |f| allocator.free(f);
            c.fields.deinit(allocator);
        }
        self.classes.deinit(allocator);

        allocator.free(self.type_ir_data);
        allocator.free(self.target_triple);
        for (self.external_deps.items) |dep| allocator.free(dep);
        self.external_deps.deinit(allocator);
    }
};
