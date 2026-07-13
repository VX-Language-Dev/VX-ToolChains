const std = @import("std");
const Allocator = std.mem.Allocator;
const bytecode = @import("bytecode.zig");
const VxObjV4Container = bytecode.VxObjV4Container;

/// VXOBJ 数据尾部标记
const TRAILER_MAGIC: [4]u8 = .{ 'V', 'X', 'O', 'B' };
const TRAILER_SIZE: u64 = 12; // 8 bytes offset + 4 bytes magic

/// VXOBJ 容器信息
pub const ContainerInfo = struct {
    version: u32,
    flags: u32,
    target_triple: []const u8,
    section_count: usize,
    total_size: usize,
};

/// 从可执行文件中提取 VXOBJ v4 容器
pub fn extractVxobjFromExecutable(io: std.Io, allocator: Allocator, path: []const u8) !VxObjV4Container {
    const data = try std.Io.Dir.cwd().readFileAlloc(io, path, allocator, .unlimited);
    defer allocator.free(data);

    const file_len = data.len;
    if (file_len < TRAILER_SIZE) {
        return error.FileTooSmall;
    }

    // 读取尾部标记 (8 bytes offset + 4 bytes magic)
    const trailer = data[file_len - TRAILER_SIZE ..];

    // 验证尾部 magic (最后 4 字节)
    const magic = trailer[8..12];
    if (!std.mem.eql(u8, magic, &TRAILER_MAGIC)) {
        return error.VxobjTrailerNotFound;
    }

    // 读取偏移量 (前 8 字节，le u64)
    const vxobj_offset = std.mem.readInt(u64, trailer[0..8], .little);

    if (vxobj_offset >= file_len - TRAILER_SIZE) {
        return error.InvalidOffset;
    }

    const vxobj_size = file_len - TRAILER_SIZE - vxobj_offset;
    if (vxobj_size == 0) {
        return error.EmptyVxobjData;
    }

    // 解析 VXOBJ v4 容器
    return try VxObjV4Container.parse(allocator, data[vxobj_offset..][0..vxobj_size]);
}

/// 打印 VXOBJ 容器信息（仅显示基本信息）
pub fn printContainerInfo(container: *const VxObjV4Container, writer: anytype) !void {
    try writer.print("VXOBJ v{} Container\n", .{container.header.version});
    try writer.print("  Target: {s}\n", .{container.header.target_triple});
    try writer.print("  Sections: {}\n", .{container.header.sections.items.len});
    for (container.header.sections.items) |sec| {
        const data = container.getSection(sec.name) orelse "";
        try writer.print("    - {s}: {} bytes\n", .{ sec.name, data.len });
    }
    if (container.hasExternalDeps()) {
        try writer.print("  Has external dependencies\n", .{});
    }
}

/// 将 VXOBJ 数据追加到可执行文件尾部（供后续反链接使用）
pub fn appendVxobjToExecutable(_: Allocator, exec_path: []const u8, vxobj_data: []const u8) !void {
    const file = try std.fs.cwd().openFile(exec_path, .{ .mode = .write_only });
    defer file.close();

    try file.seekTo(try file.getEndPos());

    // 写入 VXOBJ 数据
    try file.writeAll(vxobj_data);

    // 写入偏移量标记
    const file_len = try file.getEndPos();
    const vxobj_start = file_len - vxobj_data.len;

    var offset_buf: [8]u8 = undefined;
    std.mem.writeInt(u64, &offset_buf, @as(u64, @intCast(vxobj_start)), .little);
    try file.writeAll(&offset_buf);

    // 写入尾部 magic
    try file.writeAll(&TRAILER_MAGIC);
}

test "trailer magic size constant" {
    try std.testing.expectEqual(@as(u64, 12), TRAILER_SIZE);
}
