const std = @import("std");
const vx_vm = @import("lib.zig");

pub fn main(init: std.process.Init) !void {
    const allocator = init.gpa;
    const io = init.io;

    // 收集参数
    var arg_iter = std.process.Args.Iterator.init(init.minimal.args);
    var args: std.ArrayList([]const u8) = .empty;
    while (arg_iter.next()) |arg| {
        try args.append(allocator, arg);
    }

    const prog = if (args.items.len > 0) args.items[0] else "vdlnk";

    if (args.items.len < 2) {
        printUsage(prog);
        std.process.exit(1);
    }

    const input = args.items[1];
    var output: []const u8 = "";
    var info_only = false;

    var i: usize = 2;
    while (i < args.items.len) : (i += 1) {
        if (std.mem.eql(u8, args.items[i], "-o")) {
            i += 1;
            if (i >= args.items.len) {
                std.debug.print("[Error] Missing argument for -o\n", .{});
                std.process.exit(1);
            }
            output = args.items[i];
        } else if (std.mem.eql(u8, args.items[i], "--info")) {
            info_only = true;
        } else if (std.mem.eql(u8, args.items[i], "--decompile")) {
            std.debug.print("[Warn] --decompile not yet implemented in Zig\n", .{});
        } else {
            std.debug.print("[Error] Unknown argument: {s}\n", .{args.items[i]});
            printUsage(prog);
            std.process.exit(1);
        }
    }

    var container = vx_vm.delinker.extractVxobjFromExecutable(io, allocator, input) catch |err| {
        std.debug.print("[Error] De-linking failed: {}\n", .{err});
        std.debug.print("\nMake sure the executable was linked with '--embed-vxobj' flag.\n", .{});
        std.process.exit(1);
    };
    defer container.deinit();

    if (info_only) {
        var buf: [64]u8 = undefined;
        const stderr = std.debug.lockStderr(&buf);
        defer std.debug.unlockStderr();
        vx_vm.delinker.printContainerInfo(&container, &stderr.file_writer.interface) catch {};
        return;
    }

    const output_path = if (output.len > 0) output else blk: {
        if (std.mem.lastIndexOfScalar(u8, input, '.')) |dot| {
            const ext = input[dot..];
            if (std.mem.eql(u8, ext, ".out") or std.mem.eql(u8, ext, ".exe")) {
                break :blk try std.mem.concat(allocator, u8, &[_][]const u8{ input[0..dot], ".vxobj" });
            }
        }
        break :blk try std.mem.concat(allocator, u8, &[_][]const u8{ input, ".vxobj" });
    };

    var buf: std.Io.Writer.Allocating = .init(allocator);
    try container.write(&buf.writer);
    const vxobj_data = try buf.toOwnedSlice();
    defer allocator.free(vxobj_data);

    const cwd = std.Io.Dir.cwd();
    cwd.writeFile(io, .{ .sub_path = output_path, .data = vxobj_data }) catch |err| {
        std.debug.print("[Error] Write failed: {}\n", .{err});
        std.process.exit(1);
    };
}

fn printUsage(prog: []const u8) void {
    std.debug.print("VX De-Linker v4 - Extract VXOBJ from native executables\n", .{});
    std.debug.print("Usage: {s} <executable> [options]\n", .{prog});
    std.debug.print("Options:\n", .{});
    std.debug.print("  -o <path>      Output .vxobj path (default: input with .vxobj)\n", .{});
    std.debug.print("  --info         Show VXOBJ container info only\n", .{});
    std.debug.print("  --decompile    Extract and decompile in one step (not yet implemented)\n", .{});
    std.debug.print("\nNote: Only works with executables linked with '--embed-vxobj' flag.\n", .{});
}
