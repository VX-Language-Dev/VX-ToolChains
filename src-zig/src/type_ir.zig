const std = @import("std");
const Allocator = std.mem.Allocator;

// ==================== Type aliases ====================

pub const VarId = u32;
pub const FuncId = u32;

// ==================== Type System ====================

pub const StructField = struct {
    name: []const u8,
    field_type: Type,
};

pub const StructTypeData = struct {
    name: []const u8,
    fields: []StructField,
};

pub const MapTypeData = struct {
    key: *Type,
    value: *Type,
};

pub const FuncTypeData = struct {
    params: []Type,
    return_type: *Type,
};

pub const GenericTypeData = struct {
    name: []const u8,
    args: []Type,
};

pub const Type = union(enum) {
    Void,
    Int,
    Float,
    Bool,
    String,
    Struct: StructTypeData,
    Array: *Type,
    Map: MapTypeData,
    Func: FuncTypeData,
    Pointer: *Type,
    Generic: GenericTypeData,
    Unknown,

    pub fn isNumeric(self: Type) bool {
        return switch (self) {
            .Int, .Float => true,
            else => false,
        };
    }

    pub fn isIntegral(self: Type) bool {
        return switch (self) {
            .Int, .Bool => true,
            else => false,
        };
    }

    pub fn size(self: Type) usize {
        return switch (self) {
            .Void => 0,
            .Int, .Float, .Bool => 8,
            .String => 16,
            .Pointer => 8,
            else => 8,
        };
    }

    pub fn deinit(self: *Type, allocator: Allocator) void {
        switch (self.*) {
            .Void, .Int, .Float, .Bool, .String, .Unknown => {},
            .Struct => |s| {
                for (s.fields) |*f| {
                    allocator.free(f.name);
                    f.field_type.deinit(allocator);
                }
                allocator.free(s.fields);
                allocator.free(s.name);
            },
            .Array => |ptr| {
                ptr.deinit(allocator);
                allocator.destroy(ptr);
            },
            .Map => |m| {
                m.key.deinit(allocator);
                allocator.destroy(m.key);
                m.value.deinit(allocator);
                allocator.destroy(m.value);
            },
            .Func => |f| {
                for (f.params) |*p| p.deinit(allocator);
                allocator.free(f.params);
                f.return_type.deinit(allocator);
                allocator.destroy(f.return_type);
            },
            .Pointer => |ptr| {
                ptr.deinit(allocator);
                allocator.destroy(ptr);
            },
            .Generic => |g| {
                for (g.args) |*a| a.deinit(allocator);
                allocator.free(g.args);
                allocator.free(g.name);
            },
        }
    }
};

// ==================== Type-annotated Value ====================

pub const TypeValue = union(enum) {
    Int: i64,
    Float: f64,
    Bool: bool,
    String: []const u8,

    pub fn asInt(self: TypeValue) ?i64 {
        return switch (self) {
            .Int => |v| v,
            else => null,
        };
    }

    pub fn asFloat(self: TypeValue) ?f64 {
        return switch (self) {
            .Float => |v| v,
            else => null,
        };
    }

    pub fn asBool(self: TypeValue) ?bool {
        return switch (self) {
            .Bool => |v| v,
            else => null,
        };
    }
};

// ==================== Typed Instructions ====================

pub const StructLayoutId = struct {
    id: u32,
};

pub const MapPair = struct {
    key: VarId,
    value: VarId,
};

pub const TypedInstruction = union(enum) {
    // Constants
    ConstInt: i64,
    ConstFloat: f64,
    ConstBool: bool,
    ConstString: []const u8,
    ConstNil,

    // Variables
    LoadVar: VarId,
    StoreVar: VarId,

    // Arithmetic (typed)
    I32Add: struct { a: VarId, b: VarId },
    I32Sub: struct { a: VarId, b: VarId },
    I32Mul: struct { a: VarId, b: VarId },
    I32Div: struct { a: VarId, b: VarId },
    I32Mod: struct { a: VarId, b: VarId },
    F64Add: struct { a: VarId, b: VarId },
    F64Sub: struct { a: VarId, b: VarId },
    F64Mul: struct { a: VarId, b: VarId },
    F64Div: struct { a: VarId, b: VarId },

    // Comparison (typed)
    I32Eq: struct { a: VarId, b: VarId },
    I32Ne: struct { a: VarId, b: VarId },
    I32Lt: struct { a: VarId, b: VarId },
    I32Gt: struct { a: VarId, b: VarId },
    I32Le: struct { a: VarId, b: VarId },
    I32Ge: struct { a: VarId, b: VarId },
    F64Eq: struct { a: VarId, b: VarId },
    F64Ne: struct { a: VarId, b: VarId },
    F64Lt: struct { a: VarId, b: VarId },
    F64Gt: struct { a: VarId, b: VarId },
    F64Le: struct { a: VarId, b: VarId },
    F64Ge: struct { a: VarId, b: VarId },

    // Unary
    I32Neg: VarId,
    F64Neg: VarId,
    BoolNot: VarId,

    // Bitwise / logical
    I32And: struct { a: VarId, b: VarId },
    I32Or: struct { a: VarId, b: VarId },

    // Control flow
    Jump: u32,
    JumpIfFalse: struct { cond: VarId, target: u32 },
    JumpIfTrue: struct { cond: VarId, target: u32 },

    // Functions
    Call: struct { func_id: FuncId, args: []const VarId, ext_name: ?[]const u8 },
    CallIndirect: struct { ptr: VarId, args: []const VarId },
    Return: ?VarId,

    // Data structures
    MakeStruct: struct { layout: StructLayoutId, args: []const VarId },
    GetField: struct { obj: VarId, idx: u32 },
    SetField: struct { obj: VarId, idx: u32, val: VarId },
    MakeArray: struct { base: VarId, args: []const VarId },
    IndexGet: struct { arr: VarId, idx: VarId },
    IndexSet: struct { arr: VarId, idx: VarId, val: VarId },
    MakeMap: []const MapPair,

    // Memory / Ownership
    Alloc: Type,
    Free: VarId,
    OwnershipMove: VarId,
    Borrow: VarId,
    Deref: VarId,
    AliveCheck: VarId,

    // Stack ops
    Dup,
    Pop,

    pub fn deinit(self: *TypedInstruction, allocator: Allocator) void {
        switch (self.*) {
            .ConstString => |s| allocator.free(s),
            .Call => |c| {
                allocator.free(c.args);
                if (c.ext_name) |n| allocator.free(n);
            },
            .CallIndirect => |c| allocator.free(c.args),
            .MakeStruct => |m| allocator.free(m.args),
            .MakeArray => |m| allocator.free(m.args),
            .MakeMap => |m| allocator.free(m),
            .Alloc => |*t| t.deinit(allocator),
            else => {},
        }
    }
};

// ==================== Type IR Function ====================

pub const Param = struct {
    name: []const u8,
    param_type: Type,
};

pub const TypeFunction = struct {
    name: []const u8,
    id: FuncId,
    params: std.ArrayList(Param),
    return_type: Type,
    body: std.ArrayList(TypedInstruction),
    local_types: std.AutoHashMap(VarId, Type),
    param_count: u32,
    has_return: bool,
    var_count: u32,
    allocator: Allocator,

    pub fn init(name: []const u8, id: FuncId, allocator: Allocator) TypeFunction {
        return TypeFunction{
            .name = allocator.dupe(u8, name) catch @panic("OOM"),
            .id = id,
            .params = .empty,
            .return_type = .Void,
            .body = .empty,
            .local_types = std.AutoHashMap(VarId, Type).init(allocator),
            .param_count = 0,
            .has_return = false,
            .var_count = 0,
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *TypeFunction) void {
        const allocator = self.allocator;
        allocator.free(self.name);
        for (self.params.items) |*p| {
            allocator.free(p.name);
            p.param_type.deinit(allocator);
        }
        self.params.deinit(allocator);
        self.return_type.deinit(allocator);
        for (self.body.items) |*inst| {
            inst.deinit(allocator);
        }
        self.body.deinit(allocator);
        var iter = self.local_types.iterator();
        while (iter.next()) |entry| {
            entry.value_ptr.*.deinit(allocator);
        }
        self.local_types.deinit();
    }

    pub fn addLocal(self: *TypeFunction, ty: Type) VarId {
        const id = self.var_count;
        self.local_types.put(id, ty) catch @panic("OOM");
        self.var_count += 1;
        return id;
    }

    pub fn getType(self: *const TypeFunction, var_id: VarId) ?Type {
        return self.local_types.get(var_id);
    }
};

// ==================== Type IR Module ====================

pub const Linkage = union(enum) {
    Internal,
    External: []const u8,
};

pub const StructLayout = struct {
    name: []const u8,
    fields: []StructField,
};

pub const TypeModule = struct {
    functions: std.ArrayList(TypeFunction),
    struct_layouts: std.ArrayList(StructLayout),
    function_map: std.AutoHashMap(FuncId, []const u8),
    linkage: std.AutoHashMap(FuncId, Linkage),
    entry_point: ?FuncId,
    allocator: Allocator,

    pub fn init(allocator: Allocator) TypeModule {
        return TypeModule{
            .functions = .empty,
            .struct_layouts = .empty,
            .function_map = std.AutoHashMap(FuncId, []const u8).init(allocator),
            .linkage = std.AutoHashMap(FuncId, Linkage).init(allocator),
            .entry_point = null,
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *TypeModule) void {
        const allocator = self.allocator;
        for (self.functions.items) |*f| f.deinit();
        self.functions.deinit(allocator);
        for (self.struct_layouts.items) |*sl| {
            allocator.free(sl.name);
            for (sl.fields) |*f| {
                allocator.free(f.name);
                f.field_type.deinit(allocator);
            }
            allocator.free(sl.fields);
        }
        self.struct_layouts.deinit(allocator);
        var iter = self.function_map.iterator();
        while (iter.next()) |entry| {
            allocator.free(entry.value_ptr.*);
        }
        self.function_map.deinit();
        var link_iter = self.linkage.iterator();
        while (link_iter.next()) |entry| {
            switch (entry.value_ptr.*) {
                .External => |name| allocator.free(name),
                .Internal => {},
            }
        }
        self.linkage.deinit();
    }

    pub fn getFunction(self: *const TypeModule, id: FuncId) ?*const TypeFunction {
        for (self.functions.items) |*f| {
            if (f.id == id) return f;
        }
        return null;
    }

    pub fn getFunctionId(self: *const TypeModule, name: []const u8) ?FuncId {
        var iter = self.function_map.iterator();
        while (iter.next()) |entry| {
            if (std.mem.eql(u8, entry.value_ptr.*, name)) {
                return entry.key_ptr.*;
            }
        }
        return null;
    }

    pub fn addStructLayout(self: *TypeModule, name: []const u8, fields: []StructField) StructLayoutId {
        const id = @as(u32, @intCast(self.struct_layouts.items.len));
        self.struct_layouts.append(self.allocator, StructLayout{
            .name = self.allocator.dupe(u8, name) catch @panic("OOM"),
            .fields = self.allocator.dupe(StructField, fields) catch @panic("OOM"),
        }) catch @panic("OOM");
        return StructLayoutId{ .id = id };
    }
};

// ==================== Helper Functions ====================

fn writeU32Be(buf: *std.ArrayList(u8), allocator: Allocator, value: u32) void {
    var tmp: [4]u8 = undefined;
    std.mem.writeInt(u32, &tmp, value, .big);
    buf.appendSlice(allocator, &tmp) catch @panic("OOM");
}

fn writeStr(buf: *std.ArrayList(u8), allocator: Allocator, s: []const u8) void {
    writeU32Be(buf, allocator, @as(u32, @intCast(s.len)));
    buf.appendSlice(allocator, s) catch @panic("OOM");
}

fn appendFmt(buf: *std.ArrayList(u8), allocator: Allocator, comptime fmt: []const u8, args: anytype) !void {
    const formatted = try std.fmt.allocPrint(allocator, fmt, args);
    defer allocator.free(formatted);
    try buf.appendSlice(allocator, formatted);
}

fn readU32BeAt(data: []const u8, pos: *usize) !u32 {
    if (pos.* + 4 > data.len) return error.InvalidData;
    const v = std.mem.readInt(u32, data[pos.*..][0..4], .big);
    pos.* += 4;
    return v;
}

fn readStrAt(data: []const u8, pos: *usize, allocator: Allocator) ![]const u8 {
    const len = try readU32BeAt(data, pos);
    if (pos.* + len > data.len) return error.InvalidData;
    const s = try allocator.alloc(u8, len);
    @memcpy(s, data[pos.* .. pos.* + len]);
    pos.* += len;
    return s;
}

// ==================== Serialization ====================

pub fn serializeTypeModule(module: *const TypeModule, allocator: Allocator) []u8 {
    var buf: std.ArrayList(u8) = .empty;

    // Counts
    writeU32Be(&buf, allocator, @as(u32, @intCast(module.functions.items.len)));
    writeU32Be(&buf, allocator, @as(u32, @intCast(module.struct_layouts.items.len)));

    // Struct layouts
    for (module.struct_layouts.items) |*layout| {
        writeStr(&buf, allocator, layout.name);
        writeU32Be(&buf, allocator, @as(u32, @intCast(layout.fields.len)));
        for (layout.fields) |*field| {
            writeStr(&buf, allocator, field.name);
            serializeType(&buf, allocator, &field.field_type);
        }
    }

    // Functions
    for (module.functions.items) |*func| {
        writeStr(&buf, allocator, func.name);
        writeU32Be(&buf, allocator, func.id);
        writeU32Be(&buf, allocator, @as(u32, @intCast(func.params.items.len)));
        buf.append(allocator, if (func.has_return) @as(u8, 1) else 0) catch @panic("OOM");
        serializeType(&buf, allocator, &func.return_type);
        for (func.params.items) |*p| {
            writeStr(&buf, allocator, p.name);
            serializeType(&buf, allocator, &p.param_type);
        }
        // var_count + local_types (VXOBJ v4)
        writeU32Be(&buf, allocator, func.var_count);
        writeU32Be(&buf, allocator, @as(u32, @intCast(func.local_types.count())));
        var lt_iter = func.local_types.iterator();
        while (lt_iter.next()) |entry| {
            writeU32Be(&buf, allocator, entry.key_ptr.*);
            serializeType(&buf, allocator, entry.value_ptr);
        }
        writeU32Be(&buf, allocator, @as(u32, @intCast(func.body.items.len)));
        for (func.body.items) |*inst| {
            serializeInstruction(&buf, inst, allocator);
        }
    }

    // Linkage table
    writeU32Be(&buf, allocator, @as(u32, @intCast(module.linkage.count())));
    var link_iter = module.linkage.iterator();
    while (link_iter.next()) |entry| {
        writeU32Be(&buf, allocator, entry.key_ptr.*);
        switch (entry.value_ptr.*) {
            .Internal => buf.append(allocator, 0) catch @panic("OOM"),
            .External => |name| {
                buf.append(allocator, 1) catch @panic("OOM");
                writeStr(&buf, allocator, name);
            },
        }
    }

    return buf.toOwnedSlice(allocator) catch @panic("OOM");
}

pub fn deserializeTypeModule(data: []const u8, allocator: Allocator) !TypeModule {
    var pos: usize = 0;
    if (data.len < 8) return error.InvalidData;
    const num_funcs = try readU32BeAt(data, &pos);
    const num_layouts = try readU32BeAt(data, &pos);
    var module = TypeModule.init(allocator);

    // Struct layouts
    var i: u32 = 0;
    while (i < num_layouts) : (i += 1) {
        const name = try readStrAt(data, &pos, allocator);
        const num_fields = try readU32BeAt(data, &pos);
        var fields = try allocator.alloc(StructField, num_fields);
        var fi: u32 = 0;
        while (fi < num_fields) : (fi += 1) {
            const fname = try readStrAt(data, &pos, allocator);
            const ftype = try deserializeType(data, &pos, allocator);
            fields[fi] = StructField{ .name = fname, .field_type = ftype };
        }
        module.struct_layouts.append(allocator, StructLayout{ .name = name, .fields = fields }) catch @panic("OOM");
    }

    // Functions
    i = 0;
    while (i < num_funcs) : (i += 1) {
        const name = try readStrAt(data, &pos, allocator);
        const id = try readU32BeAt(data, &pos);
        const param_count = try readU32BeAt(data, &pos);
        const has_return = if (pos >= data.len) return error.InvalidData else data[pos] != 0;
        pos += 1;
        const return_type = try deserializeType(data, &pos, allocator);
        var func = TypeFunction.init(name, id, allocator);
        func.param_count = param_count;
        func.has_return = has_return;
        func.return_type = return_type;
        var pi: u32 = 0;
        while (pi < param_count) : (pi += 1) {
            const pname = try readStrAt(data, &pos, allocator);
            const ptype = try deserializeType(data, &pos, allocator);
            func.params.append(allocator, Param{ .name = pname, .param_type = ptype }) catch @panic("OOM");
        }
        // var_count + local_types (VXOBJ v4)
        const var_count = try readU32BeAt(data, &pos);
        func.var_count = var_count;
        const num_local_types = try readU32BeAt(data, &pos);
        var lti: u32 = 0;
        while (lti < num_local_types) : (lti += 1) {
            const vid = try readU32BeAt(data, &pos);
            const vty = try deserializeType(data, &pos, allocator);
            func.local_types.put(vid, vty) catch @panic("OOM");
        }
        const num_insts = try readU32BeAt(data, &pos);
        var ii: u32 = 0;
        while (ii < num_insts) : (ii += 1) {
            const inst = try deserializeInstruction(data, &pos, allocator);
            func.body.append(allocator, inst) catch @panic("OOM");
        }
        module.functions.append(allocator, func) catch @panic("OOM");
        module.function_map.put(id, allocator.dupe(u8, name) catch @panic("OOM")) catch @panic("OOM");
        allocator.free(name);
    }

    // Linkage table (optional for backward compatibility)
    if (pos < data.len) {
        const num_linkages = try readU32BeAt(data, &pos);
        var li: u32 = 0;
        while (li < num_linkages) : (li += 1) {
            const func_id = try readU32BeAt(data, &pos);
            if (pos >= data.len) return error.InvalidData;
            const tag = data[pos];
            pos += 1;
            const linkage: Linkage = switch (tag) {
                0 => Linkage.Internal,
                1 => Linkage{ .External = try readStrAt(data, &pos, allocator) },
                else => return error.InvalidData,
            };
            module.linkage.put(func_id, linkage) catch @panic("OOM");
        }
    }

    return module;
}

fn serializeType(buf: *std.ArrayList(u8), allocator: Allocator, ty: *const Type) void {
    switch (ty.*) {
        .Void => buf.append(allocator, 0) catch @panic("OOM"),
        .Int => buf.append(allocator, 1) catch @panic("OOM"),
        .Float => buf.append(allocator, 2) catch @panic("OOM"),
        .Bool => buf.append(allocator, 3) catch @panic("OOM"),
        .String => buf.append(allocator, 4) catch @panic("OOM"),
        .Struct => |s| {
            buf.append(allocator, 5) catch @panic("OOM");
            writeStr(buf, allocator, s.name);
            writeU32Be(buf, allocator, @as(u32, @intCast(s.fields.len)));
            for (s.fields) |*f| {
                writeStr(buf, allocator, f.name);
                serializeType(buf, allocator, &f.field_type);
            }
        },
        .Array => |inner| {
            buf.append(allocator, 6) catch @panic("OOM");
            serializeType(buf, allocator, inner);
        },
        .Map => |m| {
            buf.append(allocator, 7) catch @panic("OOM");
            serializeType(buf, allocator, m.key);
            serializeType(buf, allocator, m.value);
        },
        .Func => |f| {
            buf.append(allocator, 8) catch @panic("OOM");
            writeU32Be(buf, allocator, @as(u32, @intCast(f.params.len)));
            for (f.params) |*p| serializeType(buf, allocator, p);
            serializeType(buf, allocator, f.return_type);
        },
        .Pointer => |inner| {
            buf.append(allocator, 9) catch @panic("OOM");
            serializeType(buf, allocator, inner);
        },
        .Generic => |g| {
            buf.append(allocator, 10) catch @panic("OOM");
            writeStr(buf, allocator, g.name);
            writeU32Be(buf, allocator, @as(u32, @intCast(g.args.len)));
            for (g.args) |*a| serializeType(buf, allocator, a);
        },
        .Unknown => buf.append(allocator, 255) catch @panic("OOM"),
    }
}

fn deserializeType(data: []const u8, pos: *usize, allocator: Allocator) !Type {
    if (pos.* >= data.len) return error.InvalidData;
    const tag = data[pos.*];
    pos.* += 1;
    switch (tag) {
        0 => return Type.Void,
        1 => return Type.Int,
        2 => return Type.Float,
        3 => return Type.Bool,
        4 => return Type.String,
        5 => {
            const name = try readStrAt(data, pos, allocator);
            const len = try readU32BeAt(data, pos);
            const fields = try allocator.alloc(StructField, len);
            var i: u32 = 0;
            while (i < len) : (i += 1) {
                const fname = try readStrAt(data, pos, allocator);
                const ftype = try deserializeType(data, pos, allocator);
                fields[i] = StructField{ .name = fname, .field_type = ftype };
            }
            return Type{ .Struct = StructTypeData{ .name = name, .fields = fields } };
        },
        6 => {
            const inner = try allocator.create(Type);
            inner.* = try deserializeType(data, pos, allocator);
            return Type{ .Array = inner };
        },
        7 => {
            const key = try allocator.create(Type);
            key.* = try deserializeType(data, pos, allocator);
            const value = try allocator.create(Type);
            value.* = try deserializeType(data, pos, allocator);
            return Type{ .Map = MapTypeData{ .key = key, .value = value } };
        },
        8 => {
            const len = try readU32BeAt(data, pos);
            const params = try allocator.alloc(Type, len);
            var i: u32 = 0;
            while (i < len) : (i += 1) {
                params[i] = try deserializeType(data, pos, allocator);
            }
            const ret = try allocator.create(Type);
            ret.* = try deserializeType(data, pos, allocator);
            return Type{ .Func = FuncTypeData{ .params = params, .return_type = ret } };
        },
        9 => {
            const inner = try allocator.create(Type);
            inner.* = try deserializeType(data, pos, allocator);
            return Type{ .Pointer = inner };
        },
        10 => {
            const name = try readStrAt(data, pos, allocator);
            const len = try readU32BeAt(data, pos);
            const args = try allocator.alloc(Type, len);
            var i: u32 = 0;
            while (i < len) : (i += 1) {
                args[i] = try deserializeType(data, pos, allocator);
            }
            return Type{ .Generic = GenericTypeData{ .name = name, .args = args } };
        },
        255 => return Type.Unknown,
        else => return error.InvalidData,
    }
}

fn serializeInstruction(buf: *std.ArrayList(u8), inst: *const TypedInstruction, allocator: Allocator) void {
    var payload: ?[]const u8 = null;
    defer {
        if (payload) |p| allocator.free(p);
    }

    var tag: u8 = undefined;
    switch (inst.*) {
        .ConstInt => |v| {
            payload = std.fmt.allocPrint(allocator, "i{}", .{v}) catch @panic("OOM");
            tag = 0;
        },
        .ConstFloat => |v| {
            payload = std.fmt.allocPrint(allocator, "f{}", .{v}) catch @panic("OOM");
            tag = 1;
        },
        .ConstBool => |v| {
            payload = std.fmt.allocPrint(allocator, "b{}", .{v}) catch @panic("OOM");
            tag = 2;
        },
        .ConstString => |v| {
            payload = std.fmt.allocPrint(allocator, "s{s}", .{v}) catch @panic("OOM");
            tag = 3;
        },
        .ConstNil => tag = 4,
        .LoadVar => |v| {
            payload = std.fmt.allocPrint(allocator, "{}", .{v}) catch @panic("OOM");
            tag = 5;
        },
        .StoreVar => |v| {
            payload = std.fmt.allocPrint(allocator, "{}", .{v}) catch @panic("OOM");
            tag = 6;
        },
        .I32Add => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 10;
        },
        .I32Sub => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 11;
        },
        .I32Mul => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 12;
        },
        .I32Div => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 13;
        },
        .I32Mod => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 14;
        },
        .F64Add => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 15;
        },
        .F64Sub => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 16;
        },
        .F64Mul => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 17;
        },
        .F64Div => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 18;
        },
        .I32Eq => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 20;
        },
        .I32Ne => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 21;
        },
        .I32Lt => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 22;
        },
        .I32Gt => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 23;
        },
        .I32Le => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 24;
        },
        .I32Ge => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 25;
        },
        .F64Eq => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 26;
        },
        .F64Ne => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 27;
        },
        .F64Lt => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 28;
        },
        .F64Gt => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 29;
        },
        .F64Le => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 30;
        },
        .F64Ge => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 31;
        },
        .I32Neg => |v| {
            payload = std.fmt.allocPrint(allocator, "{}", .{v}) catch @panic("OOM");
            tag = 32;
        },
        .F64Neg => |v| {
            payload = std.fmt.allocPrint(allocator, "{}", .{v}) catch @panic("OOM");
            tag = 33;
        },
        .BoolNot => |v| {
            payload = std.fmt.allocPrint(allocator, "{}", .{v}) catch @panic("OOM");
            tag = 34;
        },
        .I32And => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 35;
        },
        .I32Or => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.a, pair.b }) catch @panic("OOM");
            tag = 36;
        },
        .Jump => |t| {
            payload = std.fmt.allocPrint(allocator, "{}", .{t}) catch @panic("OOM");
            tag = 40;
        },
        .JumpIfFalse => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.cond, pair.target }) catch @panic("OOM");
            tag = 41;
        },
        .JumpIfTrue => |pair| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ pair.cond, pair.target }) catch @panic("OOM");
            tag = 42;
        },
        .Call => |c| {
            var s: std.ArrayList(u8) = .empty;
            appendFmt(&s, allocator, "f{}", .{c.func_id}) catch @panic("OOM");
            for (c.args) |arg| {
                appendFmt(&s, allocator, ",{}", .{arg}) catch @panic("OOM");
            }
            if (c.ext_name) |name| {
                appendFmt(&s, allocator, ";{s}", .{name}) catch @panic("OOM");
            }
            payload = s.toOwnedSlice(allocator) catch @panic("OOM");
            tag = 50;
        },
        .CallIndirect => |c| {
            var s: std.ArrayList(u8) = .empty;
            appendFmt(&s, allocator, "vi{}", .{c.ptr}) catch @panic("OOM");
            for (c.args) |arg| {
                appendFmt(&s, allocator, ",{}", .{arg}) catch @panic("OOM");
            }
            payload = s.toOwnedSlice(allocator) catch @panic("OOM");
            tag = 51;
        },
        .Return => |v| {
            payload = if (v) |val| std.fmt.allocPrint(allocator, "{}", .{val}) catch @panic("OOM") else "";
            tag = 52;
        },
        .MakeStruct => |m| {
            var s: std.ArrayList(u8) = .empty;
            appendFmt(&s, allocator, "s{}", .{m.layout.id}) catch @panic("OOM");
            for (m.args) |arg| {
                appendFmt(&s, allocator, ",{}", .{arg}) catch @panic("OOM");
            }
            payload = s.toOwnedSlice(allocator) catch @panic("OOM");
            tag = 60;
        },
        .GetField => |gf| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ gf.obj, gf.idx }) catch @panic("OOM");
            tag = 61;
        },
        .SetField => |sf| {
            payload = std.fmt.allocPrint(allocator, "{},{},{}", .{ sf.obj, sf.idx, sf.val }) catch @panic("OOM");
            tag = 62;
        },
        .MakeArray => |ma| {
            var s: std.ArrayList(u8) = .empty;
            appendFmt(&s, allocator, "{}", .{ma.base}) catch @panic("OOM");
            for (ma.args) |arg| {
                appendFmt(&s, allocator, ",{}", .{arg}) catch @panic("OOM");
            }
            payload = s.toOwnedSlice(allocator) catch @panic("OOM");
            tag = 63;
        },
        .IndexGet => |ig| {
            payload = std.fmt.allocPrint(allocator, "{},{}", .{ ig.arr, ig.idx }) catch @panic("OOM");
            tag = 64;
        },
        .IndexSet => |is_| {
            payload = std.fmt.allocPrint(allocator, "{},{},{}", .{ is_.arr, is_.idx, is_.val }) catch @panic("OOM");
            tag = 65;
        },
        .MakeMap => |pairs| {
            if (pairs.len > 0) {
                var s: std.ArrayList(u8) = .empty;
                for (pairs, 0..) |pair, j| {
                    if (j > 0) s.append(allocator, ',') catch @panic("OOM");
                    appendFmt(&s, allocator, "{},{}", .{ pair.key, pair.value }) catch @panic("OOM");
                }
                payload = s.toOwnedSlice(allocator) catch @panic("OOM");
            } else {
                payload = "";
            }
            tag = 66;
        },
        .Alloc => tag = 70,
        .Free => |v| {
            payload = std.fmt.allocPrint(allocator, "{}", .{v}) catch @panic("OOM");
            tag = 71;
        },
        .OwnershipMove => |v| {
            payload = std.fmt.allocPrint(allocator, "{}", .{v}) catch @panic("OOM");
            tag = 72;
        },
        .Borrow => |v| {
            payload = std.fmt.allocPrint(allocator, "{}", .{v}) catch @panic("OOM");
            tag = 73;
        },
        .Deref => |v| {
            payload = std.fmt.allocPrint(allocator, "{}", .{v}) catch @panic("OOM");
            tag = 74;
        },
        .AliveCheck => |v| {
            payload = std.fmt.allocPrint(allocator, "{}", .{v}) catch @panic("OOM");
            tag = 75;
        },
        .Dup => tag = 80,
        .Pop => tag = 81,
    }

    buf.append(allocator, tag) catch @panic("OOM");
    if (payload) |p| {
        writeStr(buf, allocator, p);
    }
}

fn deserializeInstruction(data: []const u8, pos: *usize, allocator: Allocator) !TypedInstruction {
    if (pos.* >= data.len) return error.InvalidData;
    const tag = data[pos.*];
    pos.* += 1;

    const readVarsFn = struct {
        fn readVars(s: []const u8) !struct { VarId, VarId } {
            var it = std.mem.splitScalar(u8, s, ',');
            const a = try std.fmt.parseInt(VarId, it.next() orelse return error.InvalidData, 10);
            const b = try std.fmt.parseInt(VarId, it.next() orelse return error.InvalidData, 10);
            return .{ a, b };
        }
    }.readVars;

    const readSingle = struct {
        fn readSingle(s: []const u8) !VarId {
            return std.fmt.parseInt(VarId, s, 10);
        }
    }.readSingle;

    switch (tag) {
        0 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const raw = if (std.mem.startsWith(u8, s, "i")) s[1..] else s;
            return TypedInstruction{ .ConstInt = try std.fmt.parseInt(i64, raw, 10) };
        },
        1 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const raw = if (std.mem.startsWith(u8, s, "f")) s[1..] else s;
            return TypedInstruction{ .ConstFloat = try std.fmt.parseFloat(f64, raw) };
        },
        2 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const raw = if (std.mem.startsWith(u8, s, "b")) s[1..] else s;
            return TypedInstruction{ .ConstBool = try std.fmt.parseInt(u1, raw, 10) != 0 };
        },
        3 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const stripped = if (std.mem.startsWith(u8, s, "s")) s[1..] else s;
            return TypedInstruction{ .ConstString = try allocator.dupe(u8, stripped) };
        },
        4 => return TypedInstruction{ .ConstNil = {} },
        5 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .LoadVar = try std.fmt.parseInt(VarId, s, 10) };
        },
        6 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .StoreVar = try std.fmt.parseInt(VarId, s, 10) };
        },
        10 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Add = .{ .a = pair[0], .b = pair[1] } };
        },
        11 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Sub = .{ .a = pair[0], .b = pair[1] } };
        },
        12 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Mul = .{ .a = pair[0], .b = pair[1] } };
        },
        13 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Div = .{ .a = pair[0], .b = pair[1] } };
        },
        14 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Mod = .{ .a = pair[0], .b = pair[1] } };
        },
        15 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .F64Add = .{ .a = pair[0], .b = pair[1] } };
        },
        16 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .F64Sub = .{ .a = pair[0], .b = pair[1] } };
        },
        17 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .F64Mul = .{ .a = pair[0], .b = pair[1] } };
        },
        18 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .F64Div = .{ .a = pair[0], .b = pair[1] } };
        },
        20 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Eq = .{ .a = pair[0], .b = pair[1] } };
        },
        21 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Ne = .{ .a = pair[0], .b = pair[1] } };
        },
        22 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Lt = .{ .a = pair[0], .b = pair[1] } };
        },
        23 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Gt = .{ .a = pair[0], .b = pair[1] } };
        },
        24 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Le = .{ .a = pair[0], .b = pair[1] } };
        },
        25 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Ge = .{ .a = pair[0], .b = pair[1] } };
        },
        26 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .F64Eq = .{ .a = pair[0], .b = pair[1] } };
        },
        27 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .F64Ne = .{ .a = pair[0], .b = pair[1] } };
        },
        28 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .F64Lt = .{ .a = pair[0], .b = pair[1] } };
        },
        29 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .F64Gt = .{ .a = pair[0], .b = pair[1] } };
        },
        30 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .F64Le = .{ .a = pair[0], .b = pair[1] } };
        },
        31 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .F64Ge = .{ .a = pair[0], .b = pair[1] } };
        },
        32 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .I32Neg = try readSingle(s) };
        },
        33 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .F64Neg = try readSingle(s) };
        },
        34 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .BoolNot = try readSingle(s) };
        },
        35 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32And = .{ .a = pair[0], .b = pair[1] } };
        },
        36 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .I32Or = .{ .a = pair[0], .b = pair[1] } };
        },
        40 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .Jump = try std.fmt.parseInt(u32, s, 10) };
        },
        41 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .JumpIfFalse = .{ .cond = pair[0], .target = pair[1] } };
        },
        42 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .JumpIfTrue = .{ .cond = pair[0], .target = pair[1] } };
        },
        50 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const semicolon_pos = std.mem.indexOfScalar(u8, s, ';');
            const body = if (semicolon_pos) |sp| s[0..sp] else s;
            const ext_name: ?[]const u8 = if (semicolon_pos) |sp|
                if (sp + 1 < s.len) try allocator.dupe(u8, s[sp + 1 ..]) else null
            else
                null;
            const stripped = if (std.mem.startsWith(u8, body, "f")) body[1..] else body;
            var it = std.mem.splitScalar(u8, stripped, ',');
            const func_id = try std.fmt.parseInt(FuncId, it.next() orelse return error.InvalidData, 10);
            var args: std.ArrayList(VarId) = .empty;
            while (it.next()) |part| {
                if (part.len == 0) continue;
                args.append(allocator, try std.fmt.parseInt(VarId, part, 10)) catch @panic("OOM");
            }
            return TypedInstruction{ .Call = .{ .func_id = func_id, .args = args.toOwnedSlice(allocator) catch @panic("OOM"), .ext_name = ext_name } };
        },
        51 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const stripped = if (std.mem.startsWith(u8, s, "vi")) s[2..] else s;
            var it = std.mem.splitScalar(u8, stripped, ',');
            const ptr = try std.fmt.parseInt(VarId, it.next() orelse return error.InvalidData, 10);
            var args: std.ArrayList(VarId) = .empty;
            while (it.next()) |part| {
                if (part.len == 0) continue;
                args.append(allocator, try std.fmt.parseInt(VarId, part, 10)) catch @panic("OOM");
            }
            return TypedInstruction{ .CallIndirect = .{ .ptr = ptr, .args = args.toOwnedSlice(allocator) catch @panic("OOM") } };
        },
        52 => {
            const s = readStrAt(data, pos, allocator) catch {
                // If readStrAt fails (no more data), it's Return(None)
                return TypedInstruction{ .Return = null };
            };
            defer allocator.free(s);
            if (s.len == 0) return TypedInstruction{ .Return = null };
            return TypedInstruction{ .Return = std.fmt.parseInt(VarId, s, 10) catch null };
        },
        60 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const stripped = if (std.mem.startsWith(u8, s, "s")) s[1..] else s;
            var it = std.mem.splitScalar(u8, stripped, ',');
            const id = try std.fmt.parseInt(u32, it.next() orelse return error.InvalidData, 10);
            var args: std.ArrayList(VarId) = .empty;
            while (it.next()) |part| {
                if (part.len == 0) continue;
                args.append(allocator, try std.fmt.parseInt(VarId, part, 10)) catch @panic("OOM");
            }
            return TypedInstruction{ .MakeStruct = .{ .layout = StructLayoutId{ .id = id }, .args = args.toOwnedSlice(allocator) catch @panic("OOM") } };
        },
        61 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .GetField = .{ .obj = pair[0], .idx = pair[1] } };
        },
        62 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            var it = std.mem.splitScalar(u8, s, ',');
            const obj = try std.fmt.parseInt(VarId, it.next() orelse return error.InvalidData, 10);
            const idx = try std.fmt.parseInt(u32, it.next() orelse return error.InvalidData, 10);
            const val = try std.fmt.parseInt(VarId, it.next() orelse return error.InvalidData, 10);
            return TypedInstruction{ .SetField = .{ .obj = obj, .idx = idx, .val = val } };
        },
        63 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            if (s.len == 0) return TypedInstruction{ .MakeArray = .{ .base = 0, .args = &.{} } };
            var it = std.mem.splitScalar(u8, s, ',');
            const base = try std.fmt.parseInt(VarId, it.next() orelse return error.InvalidData, 10);
            var args: std.ArrayList(VarId) = .empty;
            while (it.next()) |part| {
                if (part.len == 0) continue;
                args.append(allocator, try std.fmt.parseInt(VarId, part, 10)) catch @panic("OOM");
            }
            return TypedInstruction{ .MakeArray = .{ .base = base, .args = args.toOwnedSlice(allocator) catch @panic("OOM") } };
        },
        64 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            const pair = try readVarsFn(s);
            return TypedInstruction{ .IndexGet = .{ .arr = pair[0], .idx = pair[1] } };
        },
        65 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            var it = std.mem.splitScalar(u8, s, ',');
            const arr = try std.fmt.parseInt(VarId, it.next() orelse return error.InvalidData, 10);
            const idx = try std.fmt.parseInt(VarId, it.next() orelse return error.InvalidData, 10);
            const val = try std.fmt.parseInt(VarId, it.next() orelse return error.InvalidData, 10);
            return TypedInstruction{ .IndexSet = .{ .arr = arr, .idx = idx, .val = val } };
        },
        66 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            if (s.len == 0) return TypedInstruction{ .MakeMap = &.{} };
            var it = std.mem.splitScalar(u8, s, ',');
            var parts: std.ArrayList([]const u8) = .empty;
            defer parts.deinit(allocator);
            while (it.next()) |part| {
                parts.append(allocator, part) catch @panic("OOM");
            }
            if (parts.items.len % 2 != 0) return error.InvalidData;
            var pairs = try allocator.alloc(MapPair, parts.items.len / 2);
            var j: usize = 0;
            while (j < pairs.len) : (j += 1) {
                pairs[j] = MapPair{
                    .key = try std.fmt.parseInt(VarId, parts.items[j * 2], 10),
                    .value = try std.fmt.parseInt(VarId, parts.items[j * 2 + 1], 10),
                };
            }
            return TypedInstruction{ .MakeMap = pairs };
        },
        70 => {
            // Alloc always deserializes as Type::Unknown
            return TypedInstruction{ .Alloc = Type.Unknown };
        },
        71 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .Free = try readSingle(s) };
        },
        72 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .OwnershipMove = try readSingle(s) };
        },
        73 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .Borrow = try readSingle(s) };
        },
        74 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .Deref = try readSingle(s) };
        },
        75 => {
            const s = try readStrAt(data, pos, allocator);
            defer allocator.free(s);
            return TypedInstruction{ .AliveCheck = try readSingle(s) };
        },
        80 => return TypedInstruction{ .Dup = {} },
        81 => return TypedInstruction{ .Pop = {} },
        else => return error.InvalidData,
    }
}

// ==================== From Bytecode IR Pass ====================

pub const BytecodeInstr = struct {
    op: u8,
    iarg: ?i32,
    sarg: ?[]const u8,
};

pub fn upgradeFromBytecode(
    func_name: []const u8,
    func_id: FuncId,
    instructions: []const BytecodeInstr,
    allocator: Allocator,
) TypeFunction {
    var tf = TypeFunction.init(func_name, func_id, allocator);
    for (instructions) |inst| {
        const ti: ?TypedInstruction = switch (inst.op) {
            0x01 => TypedInstruction{ .ConstInt = 0 },
            0x02 => TypedInstruction{ .ConstNil = {} },
            0x03 => TypedInstruction{ .ConstBool = true },
            0x04 => TypedInstruction{ .ConstBool = false },
            0x05 => TypedInstruction{ .LoadVar = 0 },
            0x06 => TypedInstruction{ .StoreVar = 0 },
            0x07 => TypedInstruction{ .StoreVar = 0 },
            0x08 => if (inst.iarg) |n| TypedInstruction{ .Call = .{ .func_id = 0, .args = tryAllocMany(VarId, @intCast(n), allocator), .ext_name = null } } else null,
            0x09 => TypedInstruction{ .Return = null },
            0x0B => if (inst.iarg) |t| TypedInstruction{ .Jump = @as(u32, @intCast(t)) } else null,
            0x0C => if (inst.iarg) |t| TypedInstruction{ .JumpIfFalse = .{ .cond = 0, .target = @as(u32, @intCast(t)) } } else null,
            0x0D => if (inst.iarg) |t| TypedInstruction{ .JumpIfTrue = .{ .cond = 0, .target = @as(u32, @intCast(t)) } } else null,
            0x10 => TypedInstruction{ .I32Add = .{ .a = 0, .b = 0 } },
            0x11 => TypedInstruction{ .I32Sub = .{ .a = 0, .b = 0 } },
            0x12 => TypedInstruction{ .I32Mul = .{ .a = 0, .b = 0 } },
            0x13 => TypedInstruction{ .I32Div = .{ .a = 0, .b = 0 } },
            0x14 => TypedInstruction{ .I32Mod = .{ .a = 0, .b = 0 } },
            0x26 => TypedInstruction{ .MakeArray = .{ .base = 0, .args = &.{} } },
            0x28 => TypedInstruction{ .IndexGet = .{ .arr = 0, .idx = 0 } },
            0x29 => TypedInstruction{ .IndexSet = .{ .arr = 0, .idx = 0, .val = 0 } },
            0x37 => TypedInstruction{ .Dup = {} },
            0x38 => TypedInstruction{ .Pop = {} },
            0x3B => TypedInstruction{ .OwnershipMove = 0 },
            0x3D => TypedInstruction{ .Borrow = 0 },
            0x3E => TypedInstruction{ .AliveCheck = 0 },
            else => null,
        };
        if (ti) |valid_inst| {
            tf.body.append(allocator, valid_inst) catch @panic("OOM");
        }
    }
    return tf;
}

fn tryAllocMany(comptime T: type, count: usize, allocator: Allocator) []const T {
    if (count == 0) return &.{};
    const result = allocator.alloc(T, count) catch @panic("OOM");
    @memset(result, @as(T, 0));
    return result;
}

// ==================== MIR Optimization Passes ====================

/// 指令是否会产生副作用（调用、内存分配、所有权操作、控制流、副作用的数据结构构造等）。
/// 只有"纯计算"指令才能被安全折叠或消除。
fn hasSideEffects(ins: *const TypedInstruction) bool {
    return switch (ins.*) {
        .Call, .CallIndirect, .Return, .Jump, .JumpIfFalse, .JumpIfTrue, .MakeStruct, .SetField, .MakeArray, .IndexSet, .MakeMap, .Alloc, .Free, .OwnershipMove, .Borrow, .Deref, .AliveCheck, .Dup, .Pop => true,
        else => false,
    };
}

/// 收集一条指令读取的所有 VarId（操作数）。
/// 返回的 slice 由 allocator 分配，调用者负责释放。
fn readVarsAlloc(ins: *const TypedInstruction, allocator: Allocator) []const VarId {
    return switch (ins.*) {
        .LoadVar, .StoreVar, .I32Neg, .F64Neg, .BoolNot, .Free, .OwnershipMove, .Borrow, .Deref, .AliveCheck => |v| {
            const result = allocator.alloc(VarId, 1) catch @panic("OOM");
            result[0] = v;
            return result;
        },
        .JumpIfFalse => |pair| {
            const result = allocator.alloc(VarId, 1) catch @panic("OOM");
            result[0] = pair.cond;
            return result;
        },
        .JumpIfTrue => |pair| {
            const result = allocator.alloc(VarId, 1) catch @panic("OOM");
            result[0] = pair.cond;
            return result;
        },
        .CallIndirect => |c| {
            const result = allocator.alloc(VarId, 1) catch @panic("OOM");
            result[0] = c.ptr;
            return result;
        },
        .Return => |v| {
            if (v) |val| {
                const result = allocator.alloc(VarId, 1) catch @panic("OOM");
                result[0] = val;
                return result;
            } else return &.{};
        },
        .GetField => |gf| {
            const result = allocator.alloc(VarId, 1) catch @panic("OOM");
            result[0] = gf.obj;
            return result;
        },
        .IndexGet => |ig| {
            const result = allocator.alloc(VarId, 1) catch @panic("OOM");
            result[0] = ig.arr;
            return result;
        },
        .I32Add => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Sub => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Mul => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Div => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Mod => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .F64Add => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .F64Sub => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .F64Mul => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .F64Div => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Eq => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Ne => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Lt => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Gt => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Le => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Ge => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .F64Eq => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .F64Ne => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .F64Lt => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .F64Gt => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .F64Le => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .F64Ge => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32And => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .I32Or => |pair| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = pair.a;
            result[1] = pair.b;
            return result;
        },
        .SetField => |sf| {
            const result = allocator.alloc(VarId, 2) catch @panic("OOM");
            result[0] = sf.obj;
            result[1] = sf.val;
            return result;
        },
        .IndexSet => |is_| {
            const result = allocator.alloc(VarId, 3) catch @panic("OOM");
            result[0] = is_.arr;
            result[1] = is_.idx;
            result[2] = is_.val;
            return result;
        },
        .MakeStruct => |ms| {
            return allocator.dupe(VarId, ms.args) catch @panic("OOM");
        },
        .MakeArray => |ma| {
            var list: std.ArrayList(VarId) = .empty;
            list.append(allocator, ma.base) catch @panic("OOM");
            list.appendSlice(allocator, ma.args) catch @panic("OOM");
            return list.toOwnedSlice(allocator) catch @panic("OOM");
        },
        .MakeMap => |pairs| {
            var list: std.ArrayList(VarId) = .empty;
            for (pairs) |pair| {
                list.append(allocator, pair.key) catch @panic("OOM");
                list.append(allocator, pair.value) catch @panic("OOM");
            }
            return list.toOwnedSlice(allocator) catch @panic("OOM");
        },
        .Call => |c| {
            return allocator.dupe(VarId, c.args) catch @panic("OOM");
        },
        // Const*/Jump/Return(None)/Alloc 不读取变量
        .ConstInt, .ConstFloat, .ConstBool, .ConstString, .ConstNil, .Jump, .Alloc, .Dup, .Pop => &.{},
    };
}

/// 常量折叠 Pass。
///
/// 遍历指令流，维护"变量 → 常量值"映射。当一条纯计算指令的所有操作数
/// 都已知为常量时，直接折叠为对应的 Const* 指令。
pub fn constantFold(func: *TypeFunction) void {
    const allocator = func.allocator;
    var const_vals = std.AutoHashMap(VarId, TypeValue).init(allocator);
    defer const_vals.deinit();

    var new_body: std.ArrayList(TypedInstruction) = .empty;

    for (func.body.items) |ins| {
        switch (ins) {
            .ConstInt, .ConstFloat, .ConstBool, .ConstString, .ConstNil => {
                new_body.append(allocator, ins) catch @panic("OOM");
            },
            .StoreVar => |v| {
                if (new_body.items.len > 0) {
                    const last = &new_body.items[new_body.items.len - 1];
                    const cv: ?TypeValue = switch (last.*) {
                        .ConstInt => |i| TypeValue{ .Int = i },
                        .ConstFloat => |f| TypeValue{ .Float = f },
                        .ConstBool => |b| TypeValue{ .Bool = b },
                        else => null,
                    };
                    if (cv) |val| {
                        const_vals.put(v, val) catch @panic("OOM");
                    } else {
                        _ = const_vals.remove(v);
                    }
                }
                new_body.append(allocator, ins) catch @panic("OOM");
            },
            .LoadVar => |v| {
                if (const_vals.get(v)) |val| {
                    switch (val) {
                        .Int => |i| new_body.append(allocator, TypedInstruction{ .ConstInt = i }) catch @panic("OOM"),
                        .Float => |f| new_body.append(allocator, TypedInstruction{ .ConstFloat = f }) catch @panic("OOM"),
                        .Bool => |b| new_body.append(allocator, TypedInstruction{ .ConstBool = b }) catch @panic("OOM"),
                        .String => new_body.append(allocator, ins) catch @panic("OOM"),
                    }
                } else {
                    new_body.append(allocator, ins) catch @panic("OOM");
                }
            },
            else => {
                const folded = foldIfConst(ins, &const_vals, allocator);
                new_body.append(allocator, folded) catch @panic("OOM");
            },
        }
    }

    // Replace body; the new body already contains shallow copies of the
    // instructions, so only the old backing array is freed.
    func.body.deinit(allocator);
    func.body = new_body;
}

/// 若指令所有操作数均为常量，则返回折叠后的 Const*；否则原样返回。
fn foldIfConst(
    ins: TypedInstruction,
    const_vals: *std.AutoHashMap(VarId, TypeValue),
    allocator: Allocator,
) TypedInstruction {
    // Check if this variant is foldable (tag-only switch, no consume)
    const isFoldable = switch (ins) {
        .I32Add, .I32Sub, .I32Mul, .I32Div, .I32Mod, .I32And, .I32Or, .F64Add, .F64Sub, .F64Mul, .F64Div, .I32Eq, .I32Ne, .I32Lt, .I32Gt, .I32Le, .I32Ge, .F64Eq, .F64Ne, .F64Lt, .F64Gt, .F64Le, .F64Ge, .I32Neg, .F64Neg, .BoolNot => true,
        else => false,
    };
    if (!isFoldable) return ins;

    const vars = readVarsAlloc(&ins, allocator);
    defer allocator.free(vars);

    // Check all vars are constant
    for (vars) |v| {
        if (!const_vals.contains(v)) return ins;
    }

    // All constant - try to fold
    const v0 = if (vars.len > 0) const_vals.get(vars[0]).? else return ins;
    const v1: ?TypeValue = if (vars.len > 1) const_vals.get(vars[1]) else null;

    // Handle division by zero before consuming ins
    const isDivByZero = if (v1) |vv1| switch (ins) {
        .I32Div, .I32Mod => vv1.asInt() != null and vv1.asInt().? == 0,
        else => false,
    } else false;
    if (isDivByZero) return ins;

    const isF64DivByZero = if (v1) |vv1| switch (ins) {
        .F64Div => vv1.asFloat() != null and vv1.asFloat().? == 0.0,
        else => false,
    } else false;
    if (isF64DivByZero) return ins;

    // Now consume ins to produce the folded result
    return switch (ins) {
        .I32Add => TypedInstruction{ .ConstInt = v0.asInt().? +% v1.?.asInt().? },
        .I32Sub => TypedInstruction{ .ConstInt = v0.asInt().? -% v1.?.asInt().? },
        .I32Mul => TypedInstruction{ .ConstInt = v0.asInt().? *% v1.?.asInt().? },
        .I32Div => TypedInstruction{ .ConstInt = @divTrunc(v0.asInt().?, v1.?.asInt().?) },
        .I32Mod => TypedInstruction{ .ConstInt = @rem(v0.asInt().?, v1.?.asInt().?) },
        .I32And => TypedInstruction{ .ConstInt = v0.asInt().? & v1.?.asInt().? },
        .I32Or => TypedInstruction{ .ConstInt = v0.asInt().? | v1.?.asInt().? },
        .F64Add => TypedInstruction{ .ConstFloat = v0.asFloat().? + v1.?.asFloat().? },
        .F64Sub => TypedInstruction{ .ConstFloat = v0.asFloat().? - v1.?.asFloat().? },
        .F64Mul => TypedInstruction{ .ConstFloat = v0.asFloat().? * v1.?.asFloat().? },
        .F64Div => TypedInstruction{ .ConstFloat = v0.asFloat().? / v1.?.asFloat().? },
        .I32Neg => TypedInstruction{ .ConstInt = -%v0.asInt().? },
        .F64Neg => TypedInstruction{ .ConstFloat = -v0.asFloat().? },
        .BoolNot => TypedInstruction{ .ConstBool = !v0.asBool().? },
        .I32Eq => TypedInstruction{ .ConstInt = if (v0.asInt().? == v1.?.asInt().?) 1 else 0 },
        .I32Ne => TypedInstruction{ .ConstInt = if (v0.asInt().? != v1.?.asInt().?) 1 else 0 },
        .I32Lt => TypedInstruction{ .ConstInt = if (v0.asInt().? < v1.?.asInt().?) 1 else 0 },
        .I32Gt => TypedInstruction{ .ConstInt = if (v0.asInt().? > v1.?.asInt().?) 1 else 0 },
        .I32Le => TypedInstruction{ .ConstInt = if (v0.asInt().? <= v1.?.asInt().?) 1 else 0 },
        .I32Ge => TypedInstruction{ .ConstInt = if (v0.asInt().? >= v1.?.asInt().?) 1 else 0 },
        .F64Eq => TypedInstruction{ .ConstInt = if (v0.asFloat().? == v1.?.asFloat().?) 1 else 0 },
        .F64Ne => TypedInstruction{ .ConstInt = if (v0.asFloat().? != v1.?.asFloat().?) 1 else 0 },
        .F64Lt => TypedInstruction{ .ConstInt = if (v0.asFloat().? < v1.?.asFloat().?) 1 else 0 },
        .F64Gt => TypedInstruction{ .ConstInt = if (v0.asFloat().? > v1.?.asFloat().?) 1 else 0 },
        .F64Le => TypedInstruction{ .ConstInt = if (v0.asFloat().? <= v1.?.asFloat().?) 1 else 0 },
        .F64Ge => TypedInstruction{ .ConstInt = if (v0.asFloat().? >= v1.?.asFloat().?) 1 else 0 },
        else => ins,
    };
}

/// 死代码消除 Pass。
///
/// TypeIR 的计算结果通过隐式值栈传递（而非 SSA 显式定义），此处采用
/// 基于"指令副作用 + 后续使用"的保守策略：
///
///   - 保留所有有副作用的指令（调用、内存、控制流等）
///   - 保留所有 StoreVar，因为它们可能写入外部可见状态或被后续读取
///   - 删除纯计算指令，若其结果在后续指令中既未被读取、自身也无副作用
///
/// 安全性：反向扫描标记"被使用"的指令；纯计算指令仅当 used[i] 为真时才保留，
/// 且会沿其读取的变量反向保留最近的 StoreVar 来源。
pub fn deadCodeElimination(func: *TypeFunction) void {
    const allocator = func.allocator;
    const n = func.body.items.len;
    if (n == 0) return;

    const used = allocator.alloc(bool, n) catch @panic("OOM");
    defer allocator.free(used);
    @memset(used, false);

    // Reverse scan to mark used instructions
    var i: usize = n;
    while (i > 0) {
        i -= 1;
        const ins = &func.body.items[i];
        if (hasSideEffects(ins) or switch (ins.*) {
            .StoreVar => true,
            else => false,
        }) {
            const vars = readVarsAlloc(ins, allocator);
            for (vars) |v| {
                markProducers(func.body.items, used, v, i);
            }
            allocator.free(vars);
            continue;
        }
        if (used[i]) {
            const vars = readVarsAlloc(ins, allocator);
            for (vars) |v| {
                markProducers(func.body.items, used, v, i);
            }
            allocator.free(vars);
        }
    }

    // Build new body: keep used instructions and side-effecting ones
    var new_body: std.ArrayList(TypedInstruction) = .empty;
    for (func.body.items, 0..) |*ins, j| {
        if (used[j] or hasSideEffects(ins) or switch (ins.*) {
            .StoreVar => true,
            else => false,
        }) {
            new_body.append(allocator, ins.*) catch @panic("OOM");
        } else {
            ins.deinit(allocator);
        }
    }

    func.body.deinit(allocator);
    func.body = new_body;
}

/// 从 `fromIdx` 之前的指令中，反向查找"写入变量 `target`"的最近一条
/// `StoreVar(target)` 并标记为 used。遇到有副作用指令即停止。
fn markProducers(body: []const TypedInstruction, used: []bool, target: VarId, fromIdx: usize) void {
    var j: usize = fromIdx;
    while (j > 0) {
        j -= 1;
        switch (body[j]) {
            .StoreVar => |v| {
                if (v == target) {
                    used[j] = true;
                    return;
                }
            },
            else => {},
        }
        if (hasSideEffects(&body[j])) return;
    }
}

/// MIR 优化入口：依次运行所有 Pass。
pub fn optimizeFunction(func: *TypeFunction) void {
    constantFold(func);
    deadCodeElimination(func);
}

// ==================== Tests ====================

test "test type serialize roundtrip" {
    const allocator = std.testing.allocator;
    var module = TypeModule.init(allocator);
    defer module.deinit();

    var func = TypeFunction.init("test_func", 0, allocator);
    func.return_type = Type.Int;
    func.params.append(allocator, Param{ .name = allocator.dupe(u8, "x") catch @panic("OOM"), .param_type = Type.Int }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .ConstInt = 42 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .Return = 0 }) catch @panic("OOM");
    module.functions.append(allocator, func) catch @panic("OOM");
    module.function_map.put(0, allocator.dupe(u8, "test_func") catch @panic("OOM")) catch @panic("OOM");

    const data = serializeTypeModule(&module, allocator);
    defer allocator.free(data);

    var parsed = try deserializeTypeModule(data, allocator);
    defer parsed.deinit();

    try std.testing.expectEqual(@as(usize, 1), parsed.functions.items.len);
    try std.testing.expectEqualStrings("test_func", parsed.functions.items[0].name);
    try std.testing.expect(parsed.functions.items[0].return_type.isNumeric());
}

test "test make array roundtrip" {
    const allocator = std.testing.allocator;
    var module = TypeModule.init(allocator);
    defer module.deinit();

    var func = TypeFunction.init("test_array", 0, allocator);
    const array_args = try allocator.dupe(VarId, &.{ 1, 2, 3, 4 });
    func.body.append(allocator, TypedInstruction{ .MakeArray = .{ .base = 7, .args = array_args } }) catch @panic("OOM");
    module.functions.append(allocator, func) catch @panic("OOM");
    module.function_map.put(0, allocator.dupe(u8, "test_array") catch @panic("OOM")) catch @panic("OOM");

    const data = serializeTypeModule(&module, allocator);
    defer allocator.free(data);

    var parsed = try deserializeTypeModule(data, allocator);
    defer parsed.deinit();

    switch (parsed.functions.items[0].body.items[0]) {
        .MakeArray => |ma| {
            try std.testing.expectEqual(@as(VarId, 7), ma.base);
            try std.testing.expectEqual(@as(usize, 4), ma.args.len);
            try std.testing.expectEqual(@as(VarId, 1), ma.args[0]);
            try std.testing.expectEqual(@as(VarId, 2), ma.args[1]);
            try std.testing.expectEqual(@as(VarId, 3), ma.args[2]);
            try std.testing.expectEqual(@as(VarId, 4), ma.args[3]);
        },
        else => @panic("类型不匹配"),
    }
}

test "test make array empty roundtrip" {
    const allocator = std.testing.allocator;
    var module = TypeModule.init(allocator);
    defer module.deinit();

    var func = TypeFunction.init("test_empty_array", 0, allocator);
    const empty_array_args = try allocator.alloc(VarId, 0);
    func.body.append(allocator, TypedInstruction{ .MakeArray = .{ .base = 0, .args = empty_array_args } }) catch @panic("OOM");
    module.functions.append(allocator, func) catch @panic("OOM");
    module.function_map.put(0, allocator.dupe(u8, "test_empty_array") catch @panic("OOM")) catch @panic("OOM");

    const data = serializeTypeModule(&module, allocator);
    defer allocator.free(data);

    var parsed = try deserializeTypeModule(data, allocator);
    defer parsed.deinit();

    switch (parsed.functions.items[0].body.items[0]) {
        .MakeArray => |ma| {
            try std.testing.expectEqual(@as(VarId, 0), ma.base);
            try std.testing.expect(ma.args.len == 0);
        },
        else => @panic("类型不匹配"),
    }
}

test "test make map roundtrip" {
    const allocator = std.testing.allocator;
    var module = TypeModule.init(allocator);
    defer module.deinit();

    var func = TypeFunction.init("test_map", 0, allocator);
    const pairs = try allocator.alloc(MapPair, 2);
    pairs[0] = MapPair{ .key = 10, .value = 20 };
    pairs[1] = MapPair{ .key = 30, .value = 40 };
    func.body.append(allocator, TypedInstruction{ .MakeMap = pairs }) catch @panic("OOM");
    module.functions.append(allocator, func) catch @panic("OOM");
    module.function_map.put(0, allocator.dupe(u8, "test_map") catch @panic("OOM")) catch @panic("OOM");

    const data = serializeTypeModule(&module, allocator);
    defer allocator.free(data);

    var parsed = try deserializeTypeModule(data, allocator);
    defer parsed.deinit();

    switch (parsed.functions.items[0].body.items[0]) {
        .MakeMap => |parsed_pairs| {
            try std.testing.expectEqual(@as(usize, 2), parsed_pairs.len);
            try std.testing.expectEqual(@as(VarId, 10), parsed_pairs[0].key);
            try std.testing.expectEqual(@as(VarId, 20), parsed_pairs[0].value);
            try std.testing.expectEqual(@as(VarId, 30), parsed_pairs[1].key);
            try std.testing.expectEqual(@as(VarId, 40), parsed_pairs[1].value);
        },
        else => @panic("类型不匹配"),
    }
}

test "test make map empty roundtrip" {
    const allocator = std.testing.allocator;
    var module = TypeModule.init(allocator);
    defer module.deinit();

    var func = TypeFunction.init("test_empty_map", 0, allocator);
    const empty_pairs = try allocator.alloc(MapPair, 0);
    func.body.append(allocator, TypedInstruction{ .MakeMap = empty_pairs }) catch @panic("OOM");
    module.functions.append(allocator, func) catch @panic("OOM");
    module.function_map.put(0, allocator.dupe(u8, "test_empty_map") catch @panic("OOM")) catch @panic("OOM");

    const data = serializeTypeModule(&module, allocator);
    defer allocator.free(data);

    var parsed = try deserializeTypeModule(data, allocator);
    defer parsed.deinit();

    switch (parsed.functions.items[0].body.items[0]) {
        .MakeMap => |parsed_pairs| {
            try std.testing.expect(parsed_pairs.len == 0);
        },
        else => @panic("类型不匹配"),
    }
}

test "test constant fold i32 add" {
    const allocator = std.testing.allocator;
    var func = TypeFunction.init("test_fold_add", 0, allocator);
    defer func.deinit();

    _ = func.addLocal(Type.Int); // v0
    _ = func.addLocal(Type.Int); // v1
    func.body.append(allocator, TypedInstruction{ .ConstInt = 2 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .ConstInt = 3 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 1 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .LoadVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .LoadVar = 1 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32Add = .{ .a = 0, .b = 1 } }) catch @panic("OOM");

    constantFold(&func);

    var found = false;
    for (func.body.items) |inst| {
        switch (inst) {
            .ConstInt => |v| {
                if (v == 5) found = true;
            },
            else => {},
        }
    }
    try std.testing.expect(found);
}

test "test constant fold binary ops" {
    const allocator = std.testing.allocator;
    var func = TypeFunction.init("test_binary_fold", 0, allocator);
    defer func.deinit();

    _ = func.addLocal(Type.Int); // v0
    _ = func.addLocal(Type.Int); // v1
    _ = func.addLocal(Type.Float); // v2
    _ = func.addLocal(Type.Float); // v3
    func.body.append(allocator, TypedInstruction{ .ConstInt = 10 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .ConstInt = 3 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 1 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .ConstFloat = 2.5 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 2 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .ConstFloat = 1.5 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 3 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32Mul = .{ .a = 0, .b = 1 } }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32And = .{ .a = 0, .b = 1 } }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32Or = .{ .a = 0, .b = 1 } }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32Eq = .{ .a = 0, .b = 1 } }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32Gt = .{ .a = 0, .b = 1 } }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .F64Add = .{ .a = 2, .b = 3 } }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .F64Sub = .{ .a = 2, .b = 3 } }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32Div = .{ .a = 0, .b = 1 } }) catch @panic("OOM");

    constantFold(&func);

    // Check fold results
    var found30 = false;
    var found2 = false;
    var found11 = false;
    var found0 = false;
    var found1 = false;
    var found4 = false;
    var foundF1 = false;
    var found3 = false;
    for (func.body.items) |inst| {
        switch (inst) {
            .ConstInt => |v| {
                if (v == 30) found30 = true;
                if (v == 2) found2 = true;
                if (v == 11) found11 = true;
                if (v == 0) found0 = true;
                if (v == 1) found1 = true;
                if (v == 3) found3 = true;
            },
            .ConstFloat => |v| {
                if (@abs(v - 4.0) < 1e-9) found4 = true;
                if (@abs(v - 1.0) < 1e-9) foundF1 = true;
            },
            else => {},
        }
    }
    try std.testing.expect(found30); // 10*3
    try std.testing.expect(found2); // 10&3
    try std.testing.expect(found11); // 10|3
    try std.testing.expect(found0); // 10==3 -> 0
    try std.testing.expect(found1); // 10>3 -> 1
    try std.testing.expect(found4); // 2.5+1.5
    try std.testing.expect(foundF1); // 2.5-1.5
    try std.testing.expect(found3); // 10/3
}

test "test constant fold neg not" {
    const allocator = std.testing.allocator;
    var func = TypeFunction.init("test_unary_fold", 0, allocator);
    defer func.deinit();

    _ = func.addLocal(Type.Int);
    _ = func.addLocal(Type.Bool);
    func.body.append(allocator, TypedInstruction{ .ConstInt = 42 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .ConstBool = true }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 1 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32Neg = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .BoolNot = 1 }) catch @panic("OOM");

    constantFold(&func);

    var foundNeg = false;
    var foundNot = false;
    for (func.body.items) |inst| {
        switch (inst) {
            .ConstInt => |v| {
                if (v == -42) foundNeg = true;
            },
            .ConstBool => |v| {
                if (!v) foundNot = true;
            },
            else => {},
        }
    }
    try std.testing.expect(foundNeg);
    try std.testing.expect(foundNot);
}

test "test constant fold non const operand" {
    const allocator = std.testing.allocator;
    var func = TypeFunction.init("test_no_fold", 0, allocator);
    defer func.deinit();

    _ = func.addLocal(Type.Int);
    func.body.append(allocator, TypedInstruction{ .I32Add = .{ .a = 0, .b = 0 } }) catch @panic("OOM");

    constantFold(&func);

    var foundI32Add = false;
    for (func.body.items) |inst| {
        switch (inst) {
            .I32Add => foundI32Add = true,
            else => {},
        }
    }
    try std.testing.expect(foundI32Add);
}

test "test constant fold div by zero" {
    const allocator = std.testing.allocator;
    var func = TypeFunction.init("test_div_by_zero", 0, allocator);
    defer func.deinit();

    _ = func.addLocal(Type.Int); // v0
    _ = func.addLocal(Type.Int); // v1
    func.body.append(allocator, TypedInstruction{ .ConstInt = 5 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .ConstInt = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 1 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32Div = .{ .a = 0, .b = 1 } }) catch @panic("OOM");

    constantFold(&func);

    var foundI32Div = false;
    for (func.body.items) |inst| {
        switch (inst) {
            .I32Div => foundI32Div = true,
            else => {},
        }
    }
    try std.testing.expect(foundI32Div);
}

test "test dead code elimination removes unused computation" {
    const allocator = std.testing.allocator;
    var func = TypeFunction.init("test_dce", 0, allocator);
    defer func.deinit();

    _ = func.addLocal(Type.Int); // v0
    _ = func.addLocal(Type.Int); // v1
    func.body.append(allocator, TypedInstruction{ .ConstInt = 1 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .LoadVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .ConstInt = 2 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 1 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .LoadVar = 1 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .LoadVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32Add = .{ .a = 1, .b = 0 } }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .Return = null }) catch @panic("OOM");

    deadCodeElimination(&func);

    // Should have: ConstInt(1), StoreVar(0), ConstInt(2), StoreVar(1), Return(None)
    var storeVarCount: usize = 0;
    var hasReturn = false;
    var hasLoadVar = false;
    var hasI32Add = false;
    for (func.body.items) |inst| {
        switch (inst) {
            .StoreVar => storeVarCount += 1,
            .Return => hasReturn = true,
            .LoadVar => hasLoadVar = true,
            .I32Add => hasI32Add = true,
            else => {},
        }
    }
    try std.testing.expectEqual(@as(usize, 2), storeVarCount);
    try std.testing.expect(!hasLoadVar);
    try std.testing.expect(!hasI32Add);
    try std.testing.expect(hasReturn);
}

test "test dead code elimination keeps used computation" {
    const allocator = std.testing.allocator;
    var func = TypeFunction.init("test_dce_keep", 0, allocator);
    defer func.deinit();

    _ = func.addLocal(Type.Int); // v0
    func.body.append(allocator, TypedInstruction{ .ConstInt = 42 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .LoadVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .Return = 0 }) catch @panic("OOM");

    deadCodeElimination(&func);

    var hasStoreVar = false;
    var hasReturn = false;
    for (func.body.items) |inst| {
        switch (inst) {
            .StoreVar => |v| {
                if (v == 0) hasStoreVar = true;
            },
            .Return => |v| {
                if (v != null and v.? == 0) hasReturn = true;
            },
            else => {},
        }
    }
    try std.testing.expect(hasStoreVar);
    try std.testing.expect(hasReturn);
}

test "test optimize function combines passes" {
    const allocator = std.testing.allocator;
    var func = TypeFunction.init("test_optimize", 0, allocator);
    defer func.deinit();

    _ = func.addLocal(Type.Int); // v0
    _ = func.addLocal(Type.Int); // v1
    func.body.append(allocator, TypedInstruction{ .ConstInt = 7 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .ConstInt = 3 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 1 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .LoadVar = 0 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .LoadVar = 1 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .I32Add = .{ .a = 0, .b = 1 } }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .Return = null }) catch @panic("OOM");

    optimizeFunction(&func);

    var hasI32Add = false;
    var hasLoadVar = false;
    var hasReturn = false;
    for (func.body.items) |inst| {
        switch (inst) {
            .I32Add => hasI32Add = true,
            .LoadVar => hasLoadVar = true,
            .Return => hasReturn = true,
            else => {},
        }
    }
    try std.testing.expect(!hasI32Add);
    try std.testing.expect(!hasLoadVar);
    try std.testing.expect(hasReturn);
}

test "test optimize function preserves side effects" {
    const allocator = std.testing.allocator;
    var func = TypeFunction.init("test_side_effects", 0, allocator);
    defer func.deinit();

    func.body.append(allocator, TypedInstruction{ .ConstInt = 42 }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .StoreVar = 0 }) catch @panic("OOM");
    const call_args = try allocator.alloc(VarId, 1);
    call_args[0] = 0;
    func.body.append(allocator, TypedInstruction{ .Call = .{ .func_id = 0, .args = call_args, .ext_name = allocator.dupe(u8, "external_func") catch @panic("OOM") } }) catch @panic("OOM");
    func.body.append(allocator, TypedInstruction{ .Return = null }) catch @panic("OOM");

    optimizeFunction(&func);

    var hasCall = false;
    var hasStoreVar = false;
    for (func.body.items) |inst| {
        switch (inst) {
            .Call => hasCall = true,
            .StoreVar => hasStoreVar = true,
            else => {},
        }
    }
    try std.testing.expect(hasCall);
    try std.testing.expect(hasStoreVar);
}
