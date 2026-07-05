// ==================== TypeIR 反序列化模块 ====================
// 解析 Rust 端 serialize_type_module 产生的二进制格式。
// 格式参考: VX-ToolChains/src/type_ir.rs

const std = @import("std");

pub const ParseError = error{
    TruncatedData,
    InvalidTypeTag,
    InvalidInstructionTag,
    InvalidPayload,
    OutOfMemory,
};

// ==================== 类型系统 ====================

pub const TypeTag = enum(u8) {
    Void = 0,
    Int = 1,
    Float = 2,
    Bool = 3,
    String = 4,
    Struct = 5,
    Array = 6,
    Map = 7,
    Func = 8,
    Pointer = 9,
    Generic = 10,
    Unknown = 255,
};

// ==================== 指令 ====================

pub const Instruction = union(enum) {
    const_int: i64,
    const_float: f64,
    const_bool: bool,
    const_string: []const u8,
    const_nil,

    load_var: u32,
    store_var: u32,

    i32_add: [2]u32,
    i32_sub: [2]u32,
    i32_mul: [2]u32,
    i32_div: [2]u32,
    i32_mod: [2]u32,
    f64_add: [2]u32,
    f64_sub: [2]u32,
    f64_mul: [2]u32,
    f64_div: [2]u32,

    i32_eq: [2]u32,
    i32_ne: [2]u32,
    i32_lt: [2]u32,
    i32_gt: [2]u32,
    i32_le: [2]u32,
    i32_ge: [2]u32,
    f64_eq: [2]u32,
    f64_ne: [2]u32,
    f64_lt: [2]u32,
    f64_gt: [2]u32,
    f64_le: [2]u32,
    f64_ge: [2]u32,

    i32_neg: u32,
    f64_neg: u32,
    bool_not: u32,

    i32_and: [2]u32,
    i32_or: [2]u32,

    jump: u32,
    jump_if_false: [2]u32,
    jump_if_true: [2]u32,

    call: CallInfo,
    ret: ?u32,

    // 未支持的指令（代码生成时会产生 trap）
    unsupported: u8,
    dup,
    pop,
};

pub const CallInfo = struct {
    func: u32,
    args: []u32,
    ext_name: ?[]const u8,
};

// ==================== 函数与模块 ====================

pub const TypeFunction = struct {
    name: []const u8,
    id: u32,
    param_count: u32,
    has_return: bool,
    var_count: u32,
    body: []Instruction,
};

pub const LinkageKind = enum {
    internal,
    external,
};

pub const LinkageEntry = struct {
    func_id: u32,
    kind: LinkageKind,
    name: ?[]const u8,
};

pub const TypeModule = struct {
    functions: []TypeFunction,
    linkage: []LinkageEntry,

    pub fn deinit(self: TypeModule, allocator: std.mem.Allocator) void {
        for (self.functions) |func| {
            allocator.free(func.name);
            for (func.body) |inst| {
                switch (inst) {
                    .const_string => |s| allocator.free(s),
                    .call => |c| {
                        allocator.free(c.args);
                        if (c.ext_name) |n| allocator.free(n);
                    },
                    else => {},
                }
            }
            allocator.free(func.body);
        }
        allocator.free(self.functions);
        for (self.linkage) |l| {
            if (l.name) |n| allocator.free(n);
        }
        allocator.free(self.linkage);
    }

    pub fn getFunction(self: TypeModule, id: u32) ?*const TypeFunction {
        for (self.functions) |*func| {
            if (func.id == id) return func;
        }
        return null;
    }

    pub fn getFunctionByName(self: TypeModule, name: []const u8) ?*const TypeFunction {
        for (self.functions) |*func| {
            if (std.mem.eql(u8, func.name, name)) return func;
        }
        return null;
    }

    pub fn isExternal(self: TypeModule, func_id: u32) bool {
        for (self.linkage) |l| {
            if (l.func_id == func_id) {
                return l.kind == .external;
            }
        }
        return false;
    }
};

// ==================== 解析器 ====================

const Cursor = struct {
    data: []const u8,
    pos: usize = 0,

    fn remaining(self: Cursor) usize {
        return self.data.len - self.pos;
    }

    fn readU8(self: *Cursor) ParseError!u8 {
        if (self.pos >= self.data.len) return ParseError.TruncatedData;
        const val = self.data[self.pos];
        self.pos += 1;
        return val;
    }

    fn readU32BE(self: *Cursor) ParseError!u32 {
        if (self.pos + 4 > self.data.len) return ParseError.TruncatedData;
        const val = std.mem.readInt(u32, self.data[self.pos .. self.pos + 4][0..4], .big);
        self.pos += 4;
        return val;
    }

    fn skipBytes(self: *Cursor, n: usize) ParseError!void {
        if (self.pos + n > self.data.len) return ParseError.TruncatedData;
        self.pos += n;
    }

    fn readStringAlloc(self: *Cursor, allocator: std.mem.Allocator) ParseError![]u8 {
        const len = try self.readU32BE();
        if (self.pos + len > self.data.len) return ParseError.TruncatedData;
        const str = try allocator.dupe(u8, self.data[self.pos .. self.pos + len]);
        self.pos += len;
        return str;
    }

    fn skipString(self: *Cursor) ParseError!void {
        const len = try self.readU32BE();
        try self.skipBytes(len);
    }

    fn skipType(self: *Cursor) ParseError!void {
        const tag = try self.readU8();
        switch (tag) {
            0, 1, 2, 3, 4, 255 => {},
            5 => {
                try self.skipString();
                const num_fields = try self.readU32BE();
                for (0..num_fields) |_| {
                    try self.skipString();
                    try self.skipType();
                }
            },
            6 => try self.skipType(),
            7 => {
                try self.skipType();
                try self.skipType();
            },
            8 => {
                const num_params = try self.readU32BE();
                for (0..num_params) |_| try self.skipType();
                try self.skipType();
            },
            9 => try self.skipType(),
            10 => {
                try self.skipString();
                const num_args = try self.readU32BE();
                for (0..num_args) |_| try self.skipType();
            },
            else => return ParseError.InvalidTypeTag,
        }
    }
};

// ==================== 指令解析 ====================

fn parseVarPair(s: []const u8) ParseError![2]u32 {
    var parts = std.mem.splitScalar(u8, s, ',');
    const a_str = parts.next() orelse return ParseError.InvalidPayload;
    const b_str = parts.next() orelse return ParseError.InvalidPayload;
    const a = std.fmt.parseInt(u32, a_str, 10) catch return ParseError.InvalidPayload;
    const b = std.fmt.parseInt(u32, b_str, 10) catch return ParseError.InvalidPayload;
    return .{ a, b };
}

fn parseSingleVar(s: []const u8) ParseError!u32 {
    return std.fmt.parseInt(u32, s, 10) catch ParseError.InvalidPayload;
}

fn parseCallPayload(allocator: std.mem.Allocator, payload: []const u8) ParseError!CallInfo {
    var body = payload;
    var ext_name: ?[]const u8 = null;

    if (std.mem.indexOfScalar(u8, payload, ';')) |idx| {
        body = payload[0..idx];
        const name_bytes = payload[idx + 1 ..];
        ext_name = try allocator.dupe(u8, name_bytes);
    }

    if (body.len > 0 and body[0] == 'f') {
        body = body[1..];
    }

    var parts = std.mem.splitScalar(u8, body, ',');
    const func_str = parts.next() orelse return ParseError.InvalidPayload;
    const func = std.fmt.parseInt(u32, func_str, 10) catch return ParseError.InvalidPayload;

    var args_list: std.ArrayList(u32) = .empty;
    defer args_list.deinit(allocator);
    while (parts.next()) |part| {
        try args_list.append(allocator, std.fmt.parseInt(u32, part, 10) catch return ParseError.InvalidPayload);
    }

    const args = try args_list.toOwnedSlice(allocator);
    return .{ .func = func, .args = args, .ext_name = ext_name };
}

fn parseInstruction(allocator: std.mem.Allocator, cursor: *Cursor) ParseError!Instruction {
    const tag = try cursor.readU8();

    // 无 payload 的指令
    switch (tag) {
        4 => return .const_nil,
        70 => return .{ .unsupported = 70 },
        80 => return .dup,
        81 => return .pop,
        else => {},
    }

    // 有 payload 的指令：读取字符串
    const payload = try cursor.readStringAlloc(allocator);
    defer allocator.free(payload);

    switch (tag) {
        0 => {
            // ConstInt: "i<val>"
            const raw = if (payload.len > 0 and payload[0] == 'i') payload[1..] else payload;
            const val = std.fmt.parseInt(i64, raw, 10) catch return ParseError.InvalidPayload;
            return .{ .const_int = val };
        },
        1 => {
            // ConstFloat: "f<val>"
            const raw = if (payload.len > 0 and payload[0] == 'f') payload[1..] else payload;
            const val = std.fmt.parseFloat(f64, raw) catch return ParseError.InvalidPayload;
            return .{ .const_float = val };
        },
        2 => {
            // ConstBool: "b<val>"
            const raw = if (payload.len > 0 and payload[0] == 'b') payload[1..] else payload;
            const val = std.mem.eql(u8, raw, "true") or std.mem.eql(u8, raw, "1");
            return .{ .const_bool = val };
        },
        3 => {
            // ConstString: "s<val>"
            const stripped = if (payload.len > 0 and payload[0] == 's') payload[1..] else payload;
            const str = try allocator.dupe(u8, stripped);
            return .{ .const_string = str };
        },
        5 => return .{ .load_var = try parseSingleVar(payload) },
        6 => return .{ .store_var = try parseSingleVar(payload) },
        10 => return .{ .i32_add = try parseVarPair(payload) },
        11 => return .{ .i32_sub = try parseVarPair(payload) },
        12 => return .{ .i32_mul = try parseVarPair(payload) },
        13 => return .{ .i32_div = try parseVarPair(payload) },
        14 => return .{ .i32_mod = try parseVarPair(payload) },
        15 => return .{ .f64_add = try parseVarPair(payload) },
        16 => return .{ .f64_sub = try parseVarPair(payload) },
        17 => return .{ .f64_mul = try parseVarPair(payload) },
        18 => return .{ .f64_div = try parseVarPair(payload) },
        20 => return .{ .i32_eq = try parseVarPair(payload) },
        21 => return .{ .i32_ne = try parseVarPair(payload) },
        22 => return .{ .i32_lt = try parseVarPair(payload) },
        23 => return .{ .i32_gt = try parseVarPair(payload) },
        24 => return .{ .i32_le = try parseVarPair(payload) },
        25 => return .{ .i32_ge = try parseVarPair(payload) },
        26 => return .{ .f64_eq = try parseVarPair(payload) },
        27 => return .{ .f64_ne = try parseVarPair(payload) },
        28 => return .{ .f64_lt = try parseVarPair(payload) },
        29 => return .{ .f64_gt = try parseVarPair(payload) },
        30 => return .{ .f64_le = try parseVarPair(payload) },
        31 => return .{ .f64_ge = try parseVarPair(payload) },
        32 => return .{ .i32_neg = try parseSingleVar(payload) },
        33 => return .{ .f64_neg = try parseSingleVar(payload) },
        34 => return .{ .bool_not = try parseSingleVar(payload) },
        35 => return .{ .i32_and = try parseVarPair(payload) },
        36 => return .{ .i32_or = try parseVarPair(payload) },
        40 => return .{ .jump = try parseSingleVar(payload) },
        41 => return .{ .jump_if_false = try parseVarPair(payload) },
        42 => return .{ .jump_if_true = try parseVarPair(payload) },
        50 => {
            const call_info = try parseCallPayload(allocator, payload);
            return .{ .call = call_info };
        },
        52 => {
            // Return: "" or "<vid>"
            if (payload.len == 0) return .{ .ret = null };
            const val = std.fmt.parseInt(u32, payload, 10) catch return ParseError.InvalidPayload;
            return .{ .ret = val };
        },
        else => return .{ .unsupported = tag },
    }
}

// ==================== 模块解析 ====================

pub fn parse(allocator: std.mem.Allocator, data: []const u8) ParseError!TypeModule {
    var cursor = Cursor{ .data = data };

    if (cursor.remaining() < 8) return ParseError.TruncatedData;

    const num_funcs = try cursor.readU32BE();
    const num_layouts = try cursor.readU32BE();

    // 跳过 struct_layouts
    for (0..num_layouts) |_| {
        try cursor.skipString();
        const num_fields = try cursor.readU32BE();
        for (0..num_fields) |_| {
            try cursor.skipString();
            try cursor.skipType();
        }
    }

    // 解析函数
    var functions: std.ArrayList(TypeFunction) = .empty;
    defer functions.deinit(allocator);
    errdefer {
        for (functions.items) |func| {
            allocator.free(func.name);
            for (func.body) |inst| {
                switch (inst) {
                    .const_string => |s| allocator.free(s),
                    .call => |c| {
                        allocator.free(c.args);
                        if (c.ext_name) |n| allocator.free(n);
                    },
                    else => {},
                }
            }
            allocator.free(func.body);
        }
    }

    for (0..num_funcs) |_| {
        const name = try cursor.readStringAlloc(allocator);
        errdefer allocator.free(name);

        const id = try cursor.readU32BE();
        const param_count = try cursor.readU32BE();
        const has_return_byte = try cursor.readU8();
        const has_return = has_return_byte != 0;

        // 跳过 return_type
        try cursor.skipType();

        // 跳过 params
        for (0..param_count) |_| {
            try cursor.skipString();
            try cursor.skipType();
        }

        // var_count + local_types
        const var_count = try cursor.readU32BE();
        const num_local_types = try cursor.readU32BE();
        for (0..num_local_types) |_| {
            _ = try cursor.readU32BE(); // vid
            try cursor.skipType();
        }

        // body
        const num_insts = try cursor.readU32BE();
        const body = try allocator.alloc(Instruction, num_insts);
        errdefer allocator.free(body);

        var body_filled: usize = 0;
        errdefer {
            for (body[0..body_filled]) |inst| {
                switch (inst) {
                    .const_string => |s| allocator.free(s),
                    .call => |c| {
                        allocator.free(c.args);
                        if (c.ext_name) |n| allocator.free(n);
                    },
                    else => {},
                }
            }
        }

        for (0..num_insts) |i| {
            body[i] = try parseInstruction(allocator, &cursor);
            body_filled = i + 1;
        }

        try functions.append(allocator, .{
            .name = name,
            .id = id,
            .param_count = param_count,
            .has_return = has_return,
            .var_count = var_count,
            .body = body,
        });
    }

    // 解析 linkage 表（可选）
    var linkage: std.ArrayList(LinkageEntry) = .empty;
    defer linkage.deinit(allocator);
    errdefer {
        for (linkage.items) |l| {
            if (l.name) |n| allocator.free(n);
        }
    }

    if (cursor.remaining() >= 4) {
        const num_linkages = try cursor.readU32BE();
        for (0..num_linkages) |_| {
            const func_id = try cursor.readU32BE();
            const link_tag = try cursor.readU8();
            switch (link_tag) {
                0 => try linkage.append(allocator, .{
                    .func_id = func_id,
                    .kind = .internal,
                    .name = null,
                }),
                1 => {
                    const name = try cursor.readStringAlloc(allocator);
                    try linkage.append(allocator, .{
                        .func_id = func_id,
                        .kind = .external,
                        .name = name,
                    });
                },
                else => return ParseError.InvalidPayload,
            }
        }
    }

    return .{
        .functions = try functions.toOwnedSlice(allocator),
        .linkage = try linkage.toOwnedSlice(allocator),
    };
}

// ==================== 测试 ====================

test "parse empty module" {
    const allocator = std.testing.allocator;
    var buf: [16]u8 = undefined;
    var pos: usize = 0;
    std.mem.writeInt(u32, buf[pos..][0..4], 0, .big);
    pos += 4; // num_funcs = 0
    std.mem.writeInt(u32, buf[pos..][0..4], 0, .big);
    pos += 4; // num_layouts = 0

    var module = try parse(allocator, buf[0..pos]);
    defer module.deinit(allocator);
    try std.testing.expectEqual(@as(usize, 0), module.functions.len);
}
