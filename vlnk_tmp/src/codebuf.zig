// ==================== 通用机器码缓冲区（所有架构共享） ====================

const std = @import("std");

pub const CodegenError = error{
    OutOfMemory,
    UnsupportedInstruction,
    UnknownFunction,
    InvalidJumpTarget,
    TooManyLocals,
};

pub const VarSlot = struct { offset: i32 };
pub const JumpPatch = struct { source_offset: usize, target_pc: u32 };

// ==================== 机器码缓冲区 ====================

pub const CodeBuffer = struct {
    bytes: std.ArrayList(u8),
    call_patches: std.ArrayList(CallPatch),
    allocator: std.mem.Allocator,

    const CallPatch = struct {
        source_offset: usize,
        target_name: []const u8,
    };

    pub fn init(allocator: std.mem.Allocator) CodeBuffer {
        return .{
            .bytes = .empty,
            .call_patches = .empty,
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *CodeBuffer) void {
        self.bytes.deinit(self.allocator);
        for (self.call_patches.items) |p| {
            self.allocator.free(p.target_name);
        }
        self.call_patches.deinit(self.allocator);
    }

    pub fn len(self: *const CodeBuffer) usize {
        return self.bytes.items.len;
    }

    pub fn append(self: *CodeBuffer, b: u8) !void {
        try self.bytes.append(self.allocator, b);
    }

    pub fn appendSlice(self: *CodeBuffer, slice: []const u8) !void {
        try self.bytes.appendSlice(self.allocator, slice);
    }

    pub fn reserve(self: *CodeBuffer, count: usize) !void {
        try self.bytes.ensureUnusedCapacity(self.allocator, count);
    }

    pub fn writeU32LE(self: *CodeBuffer, offset: usize, value: u32) void {
        std.mem.writeInt(u32, self.bytes.items[offset..][0..4], value, .little);
    }

    pub fn emitU32LE(self: *CodeBuffer, value: u32) !void {
        var buf: [4]u8 = undefined;
        std.mem.writeInt(u32, &buf, value, .little);
        try self.appendSlice(&buf);
    }

    pub fn emitU64LE(self: *CodeBuffer, value: u64) !void {
        var buf: [8]u8 = undefined;
        std.mem.writeInt(u64, &buf, value, .little);
        try self.appendSlice(&buf);
    }

    pub fn emitI32LE(self: *CodeBuffer, value: i32) !void {
        try self.emitU32LE(@bitCast(value));
    }

    pub fn addCallPatch(self: *CodeBuffer, source_offset: usize, target_name: []const u8) !void {
        const name_copy = try self.allocator.dupe(u8, target_name);
        try self.call_patches.append(self.allocator, .{
            .source_offset = source_offset,
            .target_name = name_copy,
        });
    }

    pub fn toOwnedSlice(self: *CodeBuffer) ![]u8 {
        return try self.bytes.toOwnedSlice(self.allocator);
    }
};
