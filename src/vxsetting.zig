const std = @import("std");
const Allocator = std.mem.Allocator;

pub const VxSettings = struct {
    source_dir: []const u8,
    bin: ?BuildTarget,
    vxlib: ?BuildTarget,
    lib: ?BuildTarget,
    modules: []ModuleDecl,
    vxset: VxsetConfig,
    libraries: std.StringHashMap([]const u8),
    compiler: ?VxCompilerSettings,
    linker: ?VxLinkerSettings,
    memory: ?VxMemorySettings,

    const Self = @This();

    pub fn init(allocator: Allocator, source_dir: []const u8) Self {
        return Self{
            .source_dir = source_dir,
            .bin = null,
            .vxlib = null,
            .lib = null,
            .modules = &[_]ModuleDecl{},
            .vxset = .{},
            .libraries = std.StringHashMap([]const u8).init(allocator),
            .compiler = null,
            .linker = null,
            .memory = null,
        };
    }

    pub fn deinit(self: *Self) void {
        self.libraries.deinit();
    }

    pub fn isMultiFileProject(self: *const Self) bool {
        return self.bin != null or self.vxlib != null or self.lib != null or self.modules.len > 0;
    }

    pub fn libraryPath(self: *const Self, name: []const u8) ?[]const u8 {
        return self.libraries.get(name);
    }

    pub fn fromFile(allocator: Allocator, path: []const u8) !Self {
        const cwd = std.fs.cwd();
        const content = try cwd.readFileAlloc(allocator, path, .unlimited);
        defer allocator.free(content);

        const abs = try std.fs.cwd().realpathAlloc(allocator, path);
        defer allocator.free(abs);

        const source_dir = if (std.mem.lastIndexOfScalar(u8, abs, '/')) |idx|
            abs[0..idx]
        else
            ".";

        return parseFromContent(allocator, content, source_dir);
    }

    pub fn parseFromContent(allocator: Allocator, content: []const u8, source_dir: []const u8) !Self {
        var settings = Self.init(allocator, source_dir);

        var lines = std.mem.splitSequence(u8, content, "\n");
        var current_section: ?[]const u8 = null;

        while (lines.next()) |raw_line| {
            const line = std.mem.trim(u8, raw_line, " \t\r");
            if (line.len == 0 or line[0] == '#') continue;

            if (line[0] == '[') {
                if (std.mem.indexOfScalar(u8, line, ']')) |end| {
                    current_section = line[1..end];
                    if (std.mem.eql(u8, current_section.?, "bin")) {
                        settings.bin = BuildTarget{ .optimization = 1 };
                    } else if (std.mem.eql(u8, current_section.?, "vxlib")) {
                        settings.vxlib = BuildTarget{ .optimization = 1 };
                    } else if (std.mem.eql(u8, current_section.?, "lib")) {
                        settings.lib = BuildTarget{ .optimization = 1 };
                    } else if (std.mem.eql(u8, current_section.?, "vxset")) {
                        current_section = "vxset";
                    } else if (std.mem.eql(u8, current_section.?, "libraries")) {
                        current_section = "libraries";
                    }
                }
                continue;
            }

            if (std.mem.indexOfScalar(u8, line, '=')) |eq_pos| {
                const key = std.mem.trim(u8, line[0..eq_pos], " \t");
                const val = std.mem.trim(u8, line[eq_pos + 1 ..], " \t");
                const val_unquoted = std.mem.trim(u8, val, "\"");

                if (current_section) |section| {
                    if (std.mem.eql(u8, section, "bin")) {
                        if (settings.bin) |*bt| {
                            try parseBuildTargetField(bt, key, val_unquoted, allocator);
                        }
                    } else if (std.mem.eql(u8, section, "vxlib")) {
                        if (settings.vxlib) |*bt| {
                            try parseBuildTargetField(bt, key, val_unquoted, allocator);
                        }
                    } else if (std.mem.eql(u8, section, "lib")) {
                        if (settings.lib) |*bt| {
                            try parseBuildTargetField(bt, key, val_unquoted, allocator);
                        }
                    } else if (std.mem.eql(u8, section, "vxset")) {
                        if (std.mem.eql(u8, key, "deadcode")) {
                            settings.vxset.deadcode = parseBool(val_unquoted);
                        } else if (std.mem.eql(u8, key, "cache")) {
                            settings.vxset.cache = parseBool(val_unquoted);
                        } else if (std.mem.eql(u8, key, "shell")) {
                            settings.vxset.shell = val_unquoted;
                        }
                    } else if (std.mem.eql(u8, section, "libraries")) {
                        try settings.libraries.put(key, val_unquoted);
                    }
                }
            }
        }

        return settings;
    }
};

fn parseBuildTargetField(bt: *BuildTarget, key: []const u8, val: []const u8, allocator: Allocator) !void {
    if (std.mem.eql(u8, key, "source")) {
        var sources = std.ArrayList([]const u8).init(allocator);
        var iter = std.mem.splitSequence(u8, val, ",");
        while (iter.next()) |s| {
            const trimmed = std.mem.trim(u8, s, " \"");
            if (trimmed.len > 0) {
                try sources.append(trimmed);
            }
        }
        bt.sources = try sources.toOwnedSlice();
    } else if (std.mem.eql(u8, key, "version")) {
        bt.version = val;
    } else if (std.mem.eql(u8, key, "output")) {
        bt.output = val;
    } else if (std.mem.eql(u8, key, "o") or std.mem.eql(u8, key, "optimization")) {
        bt.optimization = std.fmt.parseInt(u8, val, 10) catch 1;
        if (bt.optimization < 1) bt.optimization = 1;
        if (bt.optimization > 10) bt.optimization = 10;
    }
}

fn parseBool(val: []const u8) bool {
    if (std.mem.eql(u8, val, "true") or std.mem.eql(u8, val, "1")) return true;
    return false;
}

pub const BuildTarget = struct {
    sources: []const []const u8 = &[_][]const u8{},
    version: []const u8 = "0.0.1",
    output: ?[]const u8 = null,
    optimization: u8 = 1,

    pub fn optGroup(self: *const BuildTarget) []const u8 {
        if (self.optimization <= 4) return "Debug";
        if (self.optimization <= 7) return "Release";
        return "Super";
    }
};

pub const ModuleDecl = struct {
    info: []const u8,
    name: []const u8,
    sources: []const []const u8,
};

pub const VxsetConfig = struct {
    deadcode: bool = true,
    cache: bool = true,
    shell: ?[]const u8 = null,
};

pub const VxCompilerSettings = struct {
    target: ?[]const u8 = null,
    stdlib_path: ?[]const u8 = null,
};

pub const VxLinkerSettings = struct {
    linker: enum { builtin, lld } = .builtin,
    embed_vxobj: bool = false,
};

pub const VxMemorySettings = struct {
    stack_size: u64 = 8 * 1024 * 1024,
    heap_size: u64 = 64 * 1024 * 1024,
};

test "parse basic vxsetting" {
    const content =
        \\[bin]
        \\source = "main.vx, util.vx"
        \\version = "1.0.0"
        \\output = "dist/myapp"
        \\o = 5
        \\
        \\[vxset]
        \\deadcode = false
        \\cache = true
        \\
        \\[libraries]
        \\mymod = "package/mymod"
    ;

    var settings = try VxSettings.parseFromContent(std.testing.allocator, content, "/tmp");
    defer settings.deinit();

    try std.testing.expect(settings.bin != null);
    try std.testing.expectEqual(@as(u8, 5), settings.bin.?.optimization);
    try std.testing.expectEqualStrings("Release", settings.bin.?.optGroup());
    try std.testing.expect(!settings.vxset.deadcode);
    try std.testing.expect(settings.vxset.cache);
    try std.testing.expectEqualStrings("package/mymod", settings.libraries.get("mymod").?);
}

test "is multi file project" {
    var s1 = VxSettings.init(std.testing.allocator, "/tmp");
    defer s1.deinit();
    try std.testing.expect(!s1.isMultiFileProject());

    s1.bin = BuildTarget{};
    try std.testing.expect(s1.isMultiFileProject());
}

test "build target opt group" {
    var bt1 = BuildTarget{ .optimization = 1 };
    try std.testing.expectEqualStrings("Debug", bt1.optGroup());

    var bt2 = BuildTarget{ .optimization = 5 };
    try std.testing.expectEqualStrings("Release", bt2.optGroup());

    var bt3 = BuildTarget{ .optimization = 9 };
    try std.testing.expectEqualStrings("Super", bt3.optGroup());
}

test "memory settings defaults" {
    const mem = VxMemorySettings{};
    try std.testing.expectEqual(@as(u64, 8 * 1024 * 1024), mem.stack_size);
    try std.testing.expectEqual(@as(u64, 64 * 1024 * 1024), mem.heap_size);
}

test "linker settings defaults" {
    const lnk = VxLinkerSettings{};
    try std.testing.expect(lnk.linker == .builtin);
    try std.testing.expect(!lnk.embed_vxobj);
}
