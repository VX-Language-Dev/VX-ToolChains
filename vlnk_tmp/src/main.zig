const std = @import("std");
const linker = @import("linker");
const platform_io = @import("platform_io");

const LinkMode = enum {
    Native,

    pub fn fromString(s: []const u8) ?LinkMode {
        var buf: [64]u8 = undefined;
        const lower = std.ascii.lowerString(&buf, s);
        if (std.mem.eql(u8, lower, "native") or std.mem.eql(u8, lower, "n") or std.mem.eql(u8, lower, "static") or std.mem.eql(u8, lower, "s")) {
            return .Native;
        }
        return null;
    }
};

const RawLinkOptions = struct {
    text: ?[]const u8 = null,
    rodata: ?[]const u8 = null,
    data: ?[]const u8 = null,
    bss_size: u64 = 0,
    entry_offset: u64 = 0,
    target: ?linker.Target = null,
    output: ?[]const u8 = null,
};

pub fn main(init: std.process.Init) !void {
    const allocator = init.arena.allocator();

    var args_iter = std.process.Args.Iterator.init(init.minimal.args);
    defer args_iter.deinit();

    const prog_name = args_iter.next() orelse "vxlinker";

    var raw: RawLinkOptions = .{};
    var input_file: ?[]const u8 = null;
    var output_file: ?[]const u8 = null;
    var mode = LinkMode.Native;
    var dump = false;
    var explicit_target: ?linker.Target = null;
    var raw_mode = false;

    while (args_iter.next()) |arg| {
        if (std.mem.eql(u8, arg, "-o")) {
            output_file = args_iter.next() orelse {
                logErr("Missing argument for -o\n");
                std.process.exit(1);
            };
        } else if (std.mem.eql(u8, arg, "--mode")) {
            const m = args_iter.next() orelse {
                logErr("Missing argument for --mode\n");
                std.process.exit(1);
            };
            mode = LinkMode.fromString(m) orelse {
                logErr("Unknown mode. Use: native\n");
                std.process.exit(1);
            };
        } else if (std.mem.eql(u8, arg, "--target")) {
            const t = args_iter.next() orelse {
                logErr("Missing argument for --target\n");
                std.process.exit(1);
            };
            explicit_target = try linker.Target.parse(t);
        } else if (std.mem.eql(u8, arg, "--dump")) {
            dump = true;
        } else if (std.mem.eql(u8, arg, "--text")) {
            const path = args_iter.next() orelse {
                logErr("Missing argument for --text\n");
                std.process.exit(1);
            };
            raw.text = path;
            raw_mode = true;
        } else if (std.mem.eql(u8, arg, "--rodata")) {
            const path = args_iter.next() orelse {
                logErr("Missing argument for --rodata\n");
                std.process.exit(1);
            };
            raw.rodata = path;
            raw_mode = true;
        } else if (std.mem.eql(u8, arg, "--data")) {
            const path = args_iter.next() orelse {
                logErr("Missing argument for --data\n");
                std.process.exit(1);
            };
            raw.data = path;
            raw_mode = true;
        } else if (std.mem.eql(u8, arg, "--bss-size")) {
            const s = args_iter.next() orelse {
                logErr("Missing argument for --bss-size\n");
                std.process.exit(1);
            };
            raw.bss_size = std.fmt.parseInt(u64, s, 10) catch {
                logErr("Invalid --bss-size value\n");
                std.process.exit(1);
            };
            raw_mode = true;
        } else if (std.mem.eql(u8, arg, "--entry-offset")) {
            const s = args_iter.next() orelse {
                logErr("Missing argument for --entry-offset\n");
                std.process.exit(1);
            };
            raw.entry_offset = std.fmt.parseInt(u64, s, 10) catch {
                logErr("Invalid --entry-offset value\n");
                std.process.exit(1);
            };
            raw_mode = true;
        } else if (std.mem.eql(u8, arg, "--help") or std.mem.eql(u8, arg, "-h")) {
            printUsage(prog_name);
            std.process.exit(0);
        } else if (std.mem.startsWith(u8, arg, "-")) {
            logErr("Unknown argument\n");
            printUsage(prog_name);
            std.process.exit(1);
        } else {
            input_file = arg;
        }
    }

    if (raw_mode) {
        // 裸段链接模式：由 Rust AOT 后端解析对象文件后传入
        if (raw.text == null) {
            logErr("Raw link mode requires --text\n");
            std.process.exit(1);
        }
        if (output_file == null) {
            logErr("Raw link mode requires -o\n");
            std.process.exit(1);
        }

        const text_data = try platform_io.readFileAlloc(allocator, raw.text.?, 1024 * 1024 * 1024);
        defer allocator.free(text_data);

        const rodata_data = if (raw.rodata) |path| blk: {
            const d = try platform_io.readFileAlloc(allocator, path, 1024 * 1024 * 1024);
            break :blk d;
        } else &[_]u8{};
        defer if (raw.rodata != null) allocator.free(rodata_data);

        const data_data = if (raw.data) |path| blk: {
            const d = try platform_io.readFileAlloc(allocator, path, 1024 * 1024 * 1024);
            break :blk d;
        } else &[_]u8{};
        defer if (raw.data != null) allocator.free(data_data);

        const target = explicit_target orelse linker.hostTarget();
        const arch_str = linker.architectureToString(target.architecture);

        const input = linker.LinkInput{
            .text = text_data,
            .rodata = rodata_data,
            .data = data_data,
            .bss_size = raw.bss_size,
            .entry_offset = raw.entry_offset,
        };

        try linker.linkDirect(allocator, input, output_file.?, target, arch_str);

        if (@import("builtin").os.tag == .linux or @import("builtin").os.tag == .macos or @import("builtin").os.tag.isBSD()) {
            try platform_io.chmod(output_file.?, 0o755);
        }
        return;
    }

    // VXOBJ 链接模式（兼容旧行为）
    const vxobj_input = input_file orelse {
        printUsage(prog_name);
        std.process.exit(1);
    };

    if (dump) {
        try dumpSectionStats(allocator, vxobj_input);
        return;
    }

    const final_output = if (output_file) |o| o else blk: {
        const host = linker.hostTarget();
        const ext = host.defaultOutputExtension();
        break :blk try std.mem.concat(allocator, u8, &.{ vxobj_input, ".", ext });
    };

    if (output_file == null) {
        defer allocator.free(final_output);
    }

    switch (mode) {
        .Native => {
            try linker.linkVxObj(allocator, vxobj_input, final_output, explicit_target);
        },
    }
}

fn printUsage(prog_name: [:0]const u8) void {
    _ = platform_io.write(platform_io.STDERR_FILENO, "VX Linker v4 - Native static linker (Zig implementation)\nUsage: ") catch {};
    _ = platform_io.write(platform_io.STDERR_FILENO, prog_name) catch {};
    _ = platform_io.write(platform_io.STDERR_FILENO, " [input.vxobj | --text <file>] [options]\n" ++
        "Options:\n" ++
        "  -o <path>              Output path (required in raw link mode)\n" ++
        "  --mode <mode>          Link mode: native (default)\n" ++
        "  --target <triple>      Target triple (e.g. x86_64-unknown-linux-gnu, aarch64-apple-darwin)\n" ++
        "  --dump                 Dump VXOBJ v4 section info\n" ++
        "  -h, --help             Show this help message\n" ++
        "\n" ++
        "Raw link mode (no .o file):\n" ++
        "  --text <file>          Raw machine code for .text\n" ++
        "  --rodata <file>        Raw bytes for .rodata\n" ++
        "  --data <file>          Raw bytes for .data\n" ++
        "  --bss-size <n>         Uninitialized data size\n" ++
        "  --entry-offset <n>     Entry point offset within .text\n" ++
        "\n" ++
        "Supported platforms:\n" ++
        "  - Linux / Windows / macOS / BSD / Embedded\n" ++
        "  - x86_64 / ARM32 / ARM64 / RV32 / RV64\n") catch {};
}

fn logErr(msg: []const u8) void {
    _ = platform_io.write(platform_io.STDERR_FILENO, msg) catch {};
}

fn dumpSectionStats(allocator: std.mem.Allocator, input_file: []const u8) !void {
    const vxobj = @import("vxobj");

    const data = try platform_io.readFileAlloc(allocator, input_file, 1024 * 1024 * 1024);
    defer allocator.free(data);

    var container = try vxobj.VxObjV4Container.parse(allocator, data);
    defer container.deinit(allocator);

    var buf: [4096]u8 = undefined;
    var len: usize = 0;

    const header = "VXOBJ v4 container:\n";
    @memcpy(buf[len .. len + header.len], header);
    len += header.len;

    const target_label = "  Target: ";
    @memcpy(buf[len .. len + target_label.len], target_label);
    len += target_label.len;
    @memcpy(buf[len .. len + container.header.target_triple.len], container.header.target_triple);
    len += container.header.target_triple.len;
    buf[len] = '\n';
    len += 1;

    const sections_label = "  Sections:\n";
    @memcpy(buf[len .. len + sections_label.len], sections_label);
    len += sections_label.len;

    for (container.header.sections) |section| {
        const line = try std.fmt.bufPrint(buf[len..], "    {s:<12} {d} bytes\n", .{ section.name, section.size });
        len += line.len;
    }

    _ = platform_io.write(platform_io.STDOUT_FILENO, buf[0..len]) catch {};
}
