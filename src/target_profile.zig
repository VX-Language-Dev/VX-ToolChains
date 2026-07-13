const std = @import("std");

/// LLD 链接器风格
pub const LldFlavor = enum {
    Gnu,
    Darwin,
    Coff,
};

/// 输出文件格式
pub const OutputFormat = enum {
    Elf,
    MachO,
    Pe,
};

/// 目标平台配置（简化版，无需外部 target-lexicon crate）
pub const TargetProfile = struct {
    /// 目标三元组字符串（如 "x86_64-linux-gnu"）
    triple: []const u8,
    lld_flavor: LldFlavor,
    output_format: OutputFormat,
    entry_symbol: []const u8,
    default_output_extension: []const u8,
    static_link_flags: []const []const u8,
    lib_prefix: []const u8,
    lib_extension: []const u8,

    /// 构建为宿主平台
    pub fn host(allocator: std.mem.Allocator) TargetProfile {
        // Zig 内置的内置检测
        const builtin = @import("builtin");
        const triple = std.fmt.allocPrint(allocator, "{s}-{s}-{s}", .{
            @tagName(builtin.target.cpu.arch),
            @tagName(builtin.target.os.tag),
            @tagName(builtin.target.abi),
        }) catch @panic("OOM");
        return fromTriple(triple);
    }

    /// 从三元组字符串构建
    pub fn fromTriple(triple: []const u8) TargetProfile {
        // 简单的 os 检测（仅判断 tripe 中是否包含特定关键字）
        const is_macos = std.mem.indexOf(u8, triple, "darwin") != null or
            std.mem.indexOf(u8, triple, "macos") != null or
            std.mem.indexOf(u8, triple, "apple") != null;
        const is_windows = std.mem.indexOf(u8, triple, "windows") != null or
            std.mem.indexOf(u8, triple, "win32") != null or
            std.mem.indexOf(u8, triple, "coff") != null;
        const is_linux = std.mem.indexOf(u8, triple, "linux") != null;

        if (is_macos) {
            return TargetProfile{
                .triple = triple,
                .lld_flavor = .Darwin,
                .output_format = .MachO,
                .entry_symbol = "_main",
                .default_output_extension = "out",
                .static_link_flags = &[_][]const u8{ "-static", "-no_pie" },
                .lib_prefix = "lib",
                .lib_extension = ".dylib",
            };
        } else if (is_windows) {
            return TargetProfile{
                .triple = triple,
                .lld_flavor = .Coff,
                .output_format = .Pe,
                .entry_symbol = "mainCRTStartup",
                .default_output_extension = "exe",
                .static_link_flags = &[_][]const u8{ "/NODEFAULTLIB", "/SUBSYSTEM:CONSOLE" },
                .lib_prefix = "",
                .lib_extension = ".dll",
            };
        } else {
            // 默认 ELF (Linux 及其他)
            _ = is_linux;
            return TargetProfile{
                .triple = triple,
                .lld_flavor = .Gnu,
                .output_format = .Elf,
                .entry_symbol = "_start",
                .default_output_extension = "out",
                .static_link_flags = &[_][]const u8{"-static"},
                .lib_prefix = "lib",
                .lib_extension = ".so",
            };
        }
    }

    /// LLD 二进制名称
    pub fn lldBinaryName(self: *const TargetProfile) []const u8 {
        return switch (self.lld_flavor) {
            .Gnu => "ld.lld",
            .Darwin => "ld64.lld",
            .Coff => "lld-link",
        };
    }
};
