const std = @import("std");
const Allocator = std.mem.Allocator;
const fs = std.fs;
const process = std.process;

const VxSettings = @import("vxsetting.zig").VxSettings;
const BuildCache = @import("cache.zig").BuildCache;
const TargetProfile = @import("target_profile.zig").TargetProfile;

pub const BuildError = error{
    Config,
    ToolNotFound,
    CompileFailed,
    LinkFailed,
    NoEntryPoint,
    InvalidSource,
    IoError,
    ObjectFileWriteFailed,
};

const InstalledPackage = struct {
    name: []u8,
    path: []u8,
    version: []u8,
    language: []u8,
};

pub const VxBuilder = struct {
    allocator: Allocator,
    settings: VxSettings,
    single_entry: ?[]u8,
    force_rebuild: bool,
    no_cache: bool,
    warn_dead_code: ?bool,
    error_dead_code: ?bool,

    pub fn init(allocator: Allocator, settings: VxSettings) VxBuilder {
        return VxBuilder{
            .allocator = allocator,
            .settings = settings,
            .single_entry = null,
            .force_rebuild = false,
            .no_cache = false,
            .warn_dead_code = null,
            .error_dead_code = null,
        };
    }

    pub fn withSingleEntry(self: *VxBuilder, entry: ?[]u8) *VxBuilder {
        self.single_entry = entry;
        return self;
    }

    pub fn withForceRebuild(self: *VxBuilder, force: bool) *VxBuilder {
        self.force_rebuild = force;
        return self;
    }

    pub fn withNoCache(self: *VxBuilder, no_cache: bool) *VxBuilder {
        self.no_cache = no_cache;
        return self;
    }

    pub fn withWarnDeadCode(self: *VxBuilder, warn: ?bool) *VxBuilder {
        self.warn_dead_code = warn;
        return self;
    }

    pub fn withErrorDeadCode(self: *VxBuilder, err: ?bool) *VxBuilder {
        self.error_dead_code = err;
        return self;
    }

    fn effectiveWarnDeadCode(self: *const VxBuilder) bool {
        if (self.warn_dead_code) |warn| {
            return warn;
        }
        return !self.settings.vxset.deadcode;
    }

    fn effectiveErrorDeadCodeFor(self: *const VxBuilder, opt_level: u8) bool {
        if (self.error_dead_code) |err| {
            return err;
        }
        return opt_level >= 8;
    }

    pub fn build(self: *const VxBuilder) BuildError!void {
        if (self.settings.isMultiFileProject()) {
            try self.buildMultiFile();
        } else {
            try self.buildSingleFile();
        }
    }

    fn buildSingleFile(self: *const VxBuilder) BuildError!void {
        const entry = try self.resolveSingleEntry();
        _ = try self.runVxcompiler(&entry, null, 1);
    }

    fn resolveSingleEntry(self: *const VxBuilder) BuildError![]u8 {
        if (self.single_entry) |entry| {
            const p = self.resolveSourcePath(entry);
            if (!fs.cwd().openDir(p, .{})) |_| {
                return BuildError.InvalidSource;
            } else |_| {
                return entry;
            }
        }

        var vx_files = std.ArrayList([]const u8).init(self.allocator);
        defer vx_files.deinit();

        var dir = fs.cwd().openDir(self.settings.source_dir, .{ .iterate = true }) catch return BuildError.NoEntryPoint;
        defer dir.close();

        var iter = dir.iterate();
        while (try iter.next()) |entry| {
            if (std.mem.endsWith(u8, entry.name, ".vx")) {
                var full_path = std.ArrayList(u8).init(self.allocator);
                full_path.appendSlice(self.settings.source_dir) catch {};
                full_path.append('/') catch {};
                full_path.appendSlice(entry.name) catch {};
                try vx_files.append(full_path.items);
            }
        }

        switch (vx_files.items.len) {
            0 => return BuildError.NoEntryPoint,
            1 => return vx_files.items[0],
            else => {
                const stderr = std.io.getStdErr().writer();
                stderr.print("[VXBUILD 警告] 发现多个 .vx 文件, 请通过 `vpm build <entry.vx>` 指定入口:\n", .{}) catch {};
                for (vx_files.items) |f| {
                    stderr.print("  - {s}\n", .{f}) catch {};
                }
                return BuildError.NoEntryPoint;
            },
        }
    }

    fn buildMultiFile(self: *const VxBuilder) BuildError!void {
        var module_libs = std.StringHashMap([]u8).init(self.allocator);
        defer module_libs.deinit();
        try self.resolveModuleDependencies(&module_libs);

        _ = self.settings.vxset.cache and !self.no_cache;
        _ = self.settings.cacheFilePath();

        // ... (省略部分缓存逻辑，与Rust版本类似)

        // 构建各目标
        if (self.settings.bin) |bin| {
            _ = bin.optimization;
            // 编译逻辑...
        }

        // 链接目标
        if (self.settings.bin != null) {
            const bin = self.settings.bin.?;
            const entry = self.resolveSourcePath(bin.sources[0]);
            const obj = changeExtension(entry, "vxobj");
            const output = self.resolveTargetOutput(bin, .Bin);
            try self.runVxlinker(&obj, &output);
        }
    }

    fn resolveSourcePath(self: *const VxBuilder, src: []const u8) []u8 {
        if (std.fs.path.isAbsolute(src)) {
            return @constCast(src);
        }
        var result = std.ArrayList(u8).init(self.allocator);
        result.appendSlice(self.settings.source_dir) catch return src;
        result.append('/') catch return src;
        result.appendSlice(src) catch return src;
        return result.toOwnedSlice() catch src;
    }

    fn runVxcompiler(
        self: *const VxBuilder,
        source: []const u8,
        extra_libs_env: ?[]const u8,
        opt_level: u8,
    ) BuildError![]u8 {
        const tool = "vxcompiler";
        var output_obj = std.ArrayList(u8).init(self.allocator);
        output_obj.appendSlice(source) catch return BuildError.IoError;
        const ext_idx = std.mem.lastIndexOf(u8, output_obj.items, '.') orelse output_obj.items.len;
        output_obj.shrinkRetainingCapacity(ext_idx);
        output_obj.appendSlice(".vxobj") catch return BuildError.IoError;

        const warn_dc = self.effectiveWarnDeadCode();
        const err_dc = self.effectiveErrorDeadCodeFor(opt_level);

        var argv = std.ArrayList([]const u8).init(self.allocator);
        defer argv.deinit();

        argv.append(tool) catch return BuildError.IoError;
        argv.append(source) catch return BuildError.IoError;
        argv.append("-o") catch return BuildError.IoError;
        argv.append(output_obj.items) catch return BuildError.IoError;

        // 添加优化等级参数
        var opt_str: [2]u8 = undefined;
        _ = std.fmt.bufPrint(&opt_str, "{}", .{opt_level}) catch unreachable;
        argv.append("--opt-level") catch return BuildError.IoError;
        argv.append(&opt_str) catch return BuildError.IoError;

        if (warn_dc) {
            argv.append("--warn-dead-code") catch return BuildError.IoError;
            if (err_dc) {
                argv.append("--error-dead-code") catch return BuildError.IoError;
            }
        }

        var env_map = std.process.EnvMap.init(self.allocator);
        defer env_map.deinit();

        // 设置环境变量
        var opt_env: [2]u8 = undefined;
        _ = std.fmt.bufPrint(&opt_env, "{}", .{opt_level}) catch unreachable;
        env_map.put("VX_OPT_LEVEL", &opt_env) catch {};

        env_map.put("VX_WARN_DEAD_CODE", if (warn_dc) "1" else "0") catch {};
        env_map.put("VX_ERROR_DEAD_CODE", if (err_dc) "1" else "0") catch {};

        if (extra_libs_env) |env| {
            env_map.put("VX_EXTRA_LIBS", env) catch {};
        }

        const result = process.run(self.allocator, argv.items, .{
            .env_map = &env_map,
            .cwd = null,
            .stdin = std.process.StdIn.Ignore,
            .stdout = std.process.StdOut.Ignore,
            .stderr = std.process.StdErr.Pipe,
        }) catch |err| switch (err) {
            error.FileNotFound => return BuildError.ToolNotFound,
            else => return BuildError.IoError,
        };

        if (result.term != .Exited or result.term.Exited != 0) {
            return BuildError.CompileFailed;
        }

        return output_obj.toOwnedSlice() catch return BuildError.IoError;
    }

    fn runVxlinker(self: *const VxBuilder, obj_path: []const u8, output_path: []const u8) BuildError!void {
        const tool = "vxlinker";

        var argv = std.ArrayList([]const u8).init(self.allocator);
        defer argv.deinit();

        argv.append(tool) catch return BuildError.IoError;
        argv.append(obj_path) catch return BuildError.IoError;
        argv.append("-o") catch return BuildError.IoError;
        argv.append(output_path) catch return BuildError.IoError;

        const result = process.run(self.allocator, argv.items, .{
            .cwd = null,
            .stdin = std.process.StdIn.Ignore,
            .stdout = std.process.StdOut.Ignore,
            .stderr = std.process.StdErr.Pipe,
        }) catch |err| switch (err) {
            error.FileNotFound => return BuildError.ToolNotFound,
            else => return BuildError.IoError,
        };

        if (result.term != .Exited or result.term.Exited != 0) {
            return BuildError.LinkFailed;
        }
    }

    fn resolveModuleDependencies(self: *const VxBuilder, libs: *std.StringHashMap([]u8)) BuildError!void {
        _ = libs;
        if (self.settings.modules.len == 0) return;

        const vxmod_path = std.fmt.allocPrint(self.allocator, "{s}/vxmod.toml", .{self.settings.source_dir}) catch return BuildError.IoError;

        fs.cwd().openFile(vxmod_path, .{}) catch {
            const stderr = std.io.getStdErr().writer();
            stderr.print("[VXBUILD 警告] 未找到 vxmod.toml, 无法解析模块依赖\n", .{}) catch {};
            return;
        };
        // 解析vxmod.toml...
    }

    fn encodeLibsEnv(self: *const VxBuilder, libs: *std.StringHashMap([]u8)) []u8 {
        var result = std.ArrayList(u8).init(self.allocator);
        var it = libs.iterator();
        var first = true;
        while (it.next()) |entry| {
            if (!first) {
                result.append(';') catch break;
            }
            first = false;
            result.appendSlice(entry.key_ptr.*) catch break;
            result.append('=') catch break;
            result.appendSlice(entry.value_ptr.*) catch break;
        }
        return result.toOwnedSlice() catch "";
    }
};

fn changeExtension(path: []const u8, new_ext: []const u8) []u8 {
    const dot_idx = std.mem.lastIndexOf(u8, path, '.') orelse path.len;
    var result = std.ArrayList(u8).init(std.heap.page_allocator);
    result.appendSlice(path[0..dot_idx]) catch return path;
    result.append('.') catch return path;
    result.appendSlice(new_ext) catch return path;
    return result.toOwnedSlice() catch path;
}

pub fn parseVxmod(allocator: Allocator, content: []const u8) ![]InstalledPackage {
    var packages = std.ArrayList(InstalledPackage).init(allocator);
    var current: ?InstalledPackage = null;

    var lines = std.mem.split(u8, content, "\n");
    while (lines.next()) |line| {
        var trimmed = std.mem.trim(u8, line, " \t\r");
        if (trimmed.len == 0 or trimmed[0] == '#') continue;

        if (trimmed[0] == '[' and trimmed[trimmed.len - 1] == ']') {
            if (current) |pkg| {
                try packages.append(pkg);
            }
            const name = trimmed[1 .. trimmed.len - 1];
            current = InstalledPackage{
                .name = name,
                .path = "",
                .version = "",
                .language = "",
            };
            continue;
        }

        if (std.mem.indexOf(u8, trimmed, "=")) |eq_pos| {
            const key = std.mem.trim(u8, trimmed[0..eq_pos], " \t");
            const val = std.mem.trim(u8, trimmed[eq_pos + 1 ..], " \t\"");

            if (current) |*pkg| {
                if (std.mem.eql(u8, key, "path")) {
                    pkg.path = val;
                } else if (std.mem.eql(u8, key, "version")) {
                    pkg.version = val;
                } else if (std.mem.eql(u8, key, "language")) {
                    pkg.language = val;
                }
            }
        }
    }

    if (current) |pkg| {
        try packages.append(pkg);
    }

    return packages.toOwnedSlice();
}
