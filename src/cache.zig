const std = @import("std");
const Allocator = std.mem.Allocator;

pub const CacheEntry = struct {
    key: []const u8,
    sources: []const SourceInfo,
    obj_path: []const u8,
    output: []const u8,
    opt_level: u8,
    last_build: i64,
    is_valid: bool,

    pub const SourceInfo = struct {
        path: []const u8,
        fingerprint: []const u8,
        content_hash: []const u8,
    };
};

pub const CacheFile = struct {
    version: []const u8,
    vxsetting_hash: []const u8,
    vxmod_hash: ?[]const u8,
    toolchain_version: []const u8,
    targets: std.StringHashMap(CacheEntry),
};

pub const BuildCache = struct {
    allocator: Allocator,
    cache_dir: []const u8,
    file: CacheFile,
    enabled: bool,

    const Self = @This();
    const CACHE_VERSION = "2";
    const TOOLCHAIN_VERSION = "1.6.0";

    pub fn init(allocator: Allocator, cache_dir: []const u8) Self {
        return Self{
            .allocator = allocator,
            .cache_dir = cache_dir,
            .file = CacheFile{
                .version = CACHE_VERSION,
                .vxsetting_hash = "",
                .vxmod_hash = null,
                .toolchain_version = TOOLCHAIN_VERSION,
                .targets = std.StringHashMap(CacheEntry).init(allocator),
            },
            .enabled = true,
        };
    }

    pub fn deinit(self: *Self) void {
        self.file.targets.deinit();
    }

    pub fn disable(self: *Self) void {
        self.enabled = false;
    }

    pub fn isEnabled(self: *const Self) bool {
        return self.enabled;
    }

    pub fn setMeta(self: *Self, vxsetting_hash: []const u8, vxmod_hash: ?[]const u8, toolchain_version: []const u8) void {
        self.file.vxsetting_hash = vxsetting_hash;
        self.file.vxmod_hash = vxmod_hash;
        self.file.toolchain_version = toolchain_version;
    }

    pub fn isGloballyValid(self: *const Self, vxsetting_hash: []const u8, vxmod_hash: ?[]const u8, toolchain_version: []const u8) bool {
        if (!std.mem.eql(u8, self.file.vxsetting_hash, vxsetting_hash)) return false;
        if (vxmod_hash) |vh| {
            if (self.file.vxmod_hash) |stored| {
                if (!std.mem.eql(u8, stored, vh)) return false;
            } else return false;
        }
        if (!std.mem.eql(u8, self.file.toolchain_version, toolchain_version)) return false;
        return true;
    }

    pub fn invalidateAll(self: *Self) void {
        self.file.targets.clearAndFree();
        self.file.vxsetting_hash = "";
        self.file.vxmod_hash = null;
    }

    pub fn isTargetFresh(self: *const Self, key: []const u8, sources: []const []const u8, opt_level: u8) bool {
        const entry = self.file.targets.get(key) orelse return false;
        if (entry.opt_level != opt_level) return false;
        if (!entry.is_valid) return false;
        if (entry.sources.len != sources.len) return false;

        for (entry.sources, 0..) |src, i| {
            if (!std.mem.eql(u8, src.path, sources[i])) return false;

            const current_fp = fileFingerprint(sources[i]) orelse return false;
            if (!std.mem.eql(u8, src.fingerprint, current_fp)) {
                const current_hash = fileContentHash(sources[i]) orelse return false;
                if (!std.mem.eql(u8, src.content_hash, current_hash)) return false;
            }
        }

        return true;
    }

    pub fn updateEntry(self: *Self, key: []const u8, sources: []const []const u8, obj_path: []const u8, output: []const u8, opt_level: u8) !void {
        var source_infos = std.ArrayList(CacheEntry.SourceInfo).init(self.allocator);
        defer source_infos.deinit();

        for (sources) |src| {
            const fp = fileFingerprint(src) orelse "";
            const ch = fileContentHash(src) orelse "";
            try source_infos.append(.{
                .path = src,
                .fingerprint = fp,
                .content_hash = ch,
            });
        }

        try self.file.targets.put(key, CacheEntry{
            .key = key,
            .sources = try source_infos.toOwnedSlice(),
            .obj_path = obj_path,
            .output = output,
            .opt_level = opt_level,
            .last_build = std.time.timestamp(),
            .is_valid = true,
        });
    }

    pub fn loadFromDisk(self: *Self) !void {
        const cache_path = try std.fmt.allocPrint(self.allocator, "{s}/.vxbuild_cache", .{self.cache_dir});
        defer self.allocator.free(cache_path);

        const cwd = std.fs.cwd();
        const content = cwd.readFileAlloc(self.allocator, cache_path, .unlimited) catch return;
        defer self.allocator.free(content);

        const lines = std.mem.splitSequence(u8, content, "\n");
        const current_key: ?[]const u8 = null;
        _ = current_key;

        while (lines.next()) |raw_line| {
            const line = std.mem.trim(u8, raw_line, " \t\r");
            if (line.len == 0 or line[0] == '#') continue;

            if (std.mem.indexOfScalar(u8, line, '=')) |eq_pos| {
                const key = std.mem.trim(u8, line[0..eq_pos], " \t");
                const val = std.mem.trim(u8, line[eq_pos + 1 ..], " \t");

                if (std.mem.eql(u8, key, "vxsetting_hash")) {
                    self.file.vxsetting_hash = val;
                } else if (std.mem.eql(u8, key, "toolchain_version")) {
                    self.file.toolchain_version = val;
                }
            }
        }
    }

    pub fn saveToDisk(self: *const Self) !void {
        const cache_path = try std.fmt.allocPrint(self.allocator, "{s}/.vxbuild_cache", .{self.cache_dir});
        defer self.allocator.free(cache_path);

        const cwd = std.fs.cwd();
        const file = try cwd.createFile(cache_path, .{ .truncate = true });
        defer file.close();

        var writer = file.writer();
        try writer.print("# VX Build Cache v{s}\n", .{self.file.version});
        try writer.print("vxsetting_hash={s}\n", .{self.file.vxsetting_hash});
        try writer.print("toolchain_version={s}\n", .{self.file.toolchain_version});
    }

    fn fileFingerprint(path: []const u8) ?[]const u8 {
        const cwd = std.fs.cwd();
        const stat = cwd.statFile(path) catch return null;
        // 使用 mtime 作为快速指纹（纳秒精度）
        return std.fmt.allocPrint(std.heap.page_allocator, "{}", .{stat.mtime}) catch null;
    }

    fn fileContentHash(path: []const u8) ?[]const u8 {
        const cwd = std.fs.cwd();
        const content = cwd.readFileAlloc(std.heap.page_allocator, path, .unlimited) catch return null;
        defer std.heap.page_allocator.free(content);

        var hasher = std.hash.Fnv1a_64.init();
        hasher.update(content);
        const hash = hasher.final();
        // 返回十六进制字符串
        return std.fmt.allocPrint(std.heap.page_allocator, "{x}", .{hash}) catch null;
    }
};

test "build cache init" {
    var cache = BuildCache.init(std.testing.allocator, "/tmp");
    defer cache.deinit();
    try std.testing.expect(cache.isEnabled());
    try std.testing.expectEqualStrings("1.6.0", BuildCache.TOOLCHAIN_VERSION);
}

test "build cache disable" {
    var cache = BuildCache.init(std.testing.allocator, "/tmp");
    defer cache.deinit();
    cache.disable();
    try std.testing.expect(!cache.isEnabled());
}

test "build cache global validity" {
    var cache = BuildCache.init(std.testing.allocator, "/tmp");
    defer cache.deinit();

    cache.setMeta("abc123", null, "1.6.0");
    try std.testing.expect(cache.isGloballyValid("abc123", null, "1.6.0"));
    try std.testing.expect(!cache.isGloballyValid("different", null, "1.6.0"));
    try std.testing.expect(!cache.isGloballyValid("abc123", null, "2.0.0"));
}

test "build cache invalidate" {
    var cache = BuildCache.init(std.testing.allocator, "/tmp");
    defer cache.deinit();

    cache.setMeta("abc", null, "1.6.0");
    cache.invalidateAll();
    try std.testing.expectEqualStrings("", cache.file.vxsetting_hash);
}
