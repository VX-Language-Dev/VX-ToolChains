const std = @import("std");

/// VX Linker (Zig wrapper)
///
/// 通过 VLKNPATH 环境变量查找 Rust vxlinker 可执行文件，然后 spawn 调用。
/// - 找不到 VLKNPATH 环境变量 → 报错
/// - VLKNPATH 指向的文件不存在 → 报错
/// - 文件是有效 vxlinker → 透传参数调用
pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;
    const io = init.io;

    // 收集参数
    var arg_iter = std.process.Args.Iterator.init(init.minimal.args);
    var args: std.ArrayList([]const u8) = .empty;
    defer args.deinit(gpa);
    while (arg_iter.next()) |arg| {
        try args.append(gpa, arg);
    }

    // 1. 读取 VLKNPATH 环境变量
    var environ_map = try std.process.Environ.createMap(init.minimal.environ, gpa);
    defer environ_map.deinit();

    const vlnk_path_raw = environ_map.get("VLKNPATH") orelse {
        std.debug.print("[Error] VLKNPATH 环境变量未设置\n", .{});
        std.debug.print("  请设置 VLKNPATH 指向 vxlinker 可执行文件路径:\n", .{});
        std.debug.print("    export VLKNPATH=/path/to/vxlinker\n", .{});
        std.process.exit(1);
    };

    // 去除尾部空白
    const trimmed = std.mem.trimEnd(u8, vlnk_path_raw, &[_]u8{ '\n', '\r', ' ', '\t' });

    if (trimmed.len == 0) {
        std.debug.print("[Error] VLKNPATH 环境变量为空\n", .{});
        std.process.exit(1);
    }

    // 2. 验证文件存在
    const cwd = std.Io.Dir.cwd();
    _ = cwd.openFile(io, trimmed, .{}) catch |err| {
        std.debug.print("[Error] VLKNPATH 指向的文件不可访问: {s}\n", .{trimmed});
        std.debug.print("  原因: {}\n", .{err});
        std.process.exit(1);
    };

    // 3. 构建 argv: [trimmed_path, args[1..]]
    var child_args: std.ArrayList([]const u8) = .empty;
    defer child_args.deinit(gpa);
    try child_args.append(gpa, trimmed);
    if (args.items.len > 1) {
        try child_args.appendSlice(gpa, args.items[1..]);
    }

    // 4. spawn 子进程调用 vxlinker (继承 stdin/stdout/stderr)
    var child = std.process.spawn(io, .{
        .argv = child_args.items,
    }) catch |err| {
        std.debug.print("[Error] 无法执行 vxlinker: {s}\n", .{trimmed});
        std.debug.print("  原因: {}\n", .{err});
        std.debug.print("  该文件可能不是有效的可执行文件或已损坏\n", .{});
        std.process.exit(1);
    };

    // 5. 等待子进程结束
    const term = child.wait(io) catch |err| {
        std.debug.print("[Error] 等待 vxlinker 结束失败: {}\n", .{err});
        std.process.exit(1);
    };

    // 6. 传递退出码
    switch (term) {
        .exited => |code| {
            std.process.exit(code);
        },
        .signal => {
            std.debug.print("[Error] vxlinker 被信号终止\n", .{});
            std.process.exit(1);
        },
        .stopped => {
            std.debug.print("[Error] vxlinker 被停止\n", .{});
            std.process.exit(1);
        },
        .unknown => {
            std.debug.print("[Error] vxlinker 以未知状态退出\n", .{});
            std.process.exit(1);
        },
    }
}
