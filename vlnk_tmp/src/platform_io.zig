const std = @import("std");

pub fn write(fd: i32, buf: []const u8) !void {
    var written: usize = 0;
    while (written < buf.len) {
        const n = std.os.linux.write(fd, buf[written..].ptr, buf.len - written);
        const errno = std.posix.errno(n);
        if (errno != .SUCCESS) return error.WriteError;
        written += n;
    }
}

pub fn createFile(path: []const u8) !i32 {
    const path_z = try std.mem.concatWithSentinel(std.heap.page_allocator, u8, &.{path}, 0);
    defer std.heap.page_allocator.free(path_z);

    const fd = std.os.linux.open(path_z, .{
        .ACCMODE = .WRONLY,
        .CREAT = true,
        .TRUNC = true,
    }, 0o644);
    const errno = std.posix.errno(fd);
    if (errno != .SUCCESS) return error.OpenError;
    return @intCast(fd);
}

pub fn close(fd: i32) void {
    _ = std.os.linux.close(fd);
}

pub fn readFileAlloc(allocator: std.mem.Allocator, path: []const u8, max_size: usize) ![]u8 {
    const path_z = try std.mem.concatWithSentinel(allocator, u8, &.{path}, 0);
    defer allocator.free(path_z);

    const fd = std.os.linux.open(path_z, .{ .ACCMODE = .RDONLY }, 0);
    const errno = std.posix.errno(fd);
    if (errno != .SUCCESS) return error.OpenError;
    const fd_i: i32 = @intCast(fd);
    defer _ = std.os.linux.close(fd_i);

    var result: std.ArrayList(u8) = .empty;
    errdefer result.deinit(allocator);

    var buf: [4096]u8 = undefined;
    while (true) {
        const n = std.os.linux.read(fd_i, &buf, buf.len);
        const read_errno = std.posix.errno(n);
        if (read_errno != .SUCCESS) return error.ReadError;
        if (n == 0) break;
        if (result.items.len + n > max_size) return error.FileTooLarge;
        try result.appendSlice(allocator, buf[0..n]);
    }

    return result.toOwnedSlice(allocator);
}

pub fn chmod(path: []const u8, mode: u32) !void {
    const path_z = try std.mem.concatWithSentinel(std.heap.page_allocator, u8, &.{path}, 0);
    defer std.heap.page_allocator.free(path_z);

    const rc = std.os.linux.chmod(path_z, mode);
    const errno = std.posix.errno(rc);
    if (errno != .SUCCESS) return error.ChmodError;
}

pub const FileWriter = struct {
    fd: i32,
    pos: u64,

    pub const Error = error{WriteError};

    pub fn init(fd: i32) FileWriter {
        return .{
            .fd = fd,
            .pos = 0,
        };
    }

    pub fn writeAll(self: *FileWriter, bytes: []const u8) Error!void {
        var written: usize = 0;
        while (written < bytes.len) {
            const n = std.os.linux.write(self.fd, bytes[written..].ptr, bytes.len - written);
            const errno = std.posix.errno(n);
            if (errno != .SUCCESS) return error.WriteError;
            written += n;
        }
        self.pos += bytes.len;
    }

    pub fn writeByte(self: *FileWriter, byte: u8) Error!void {
        try self.writeAll(&.{byte});
    }

    pub fn getPos(self: FileWriter) u64 {
        return self.pos;
    }
};

pub const STDERR_FILENO = std.posix.STDERR_FILENO;
pub const STDOUT_FILENO = std.posix.STDOUT_FILENO;
