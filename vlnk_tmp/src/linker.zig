const std = @import("std");

const vxobj = @import("vxobj");
const elf_linker = @import("elf_linker");
const macho_linker = @import("macho_linker");
const pe_linker = @import("pe_linker");
const platform_io = @import("platform_io");
const typeir = @import("typeir");
const codegen = @import("codegen");

pub const LinkerError = error{
    InvalidPlatform,
    InvalidArchitecture,
    VxObjError,
    LinkError,
};

pub const Platform = enum {
    Linux,
    Windows,
    MacOS,
    BSD,
    Embedded,
};

pub const Architecture = enum {
    X86_64,
    ARM32,
    ARM64,
    RV32,
    RV64,
};

pub const Target = struct {
    platform: Platform,
    architecture: Architecture,
    triple: []const u8,

    pub fn parse(triple: []const u8) !Target {
        var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
        defer arena.deinit();
        const allocator = arena.allocator();

        const normalized = try std.ascii.allocLowerString(allocator, triple);

        const platform = detectPlatform(normalized);
        const architecture = try detectArchitecture(normalized);

        return Target{
            .platform = platform,
            .architecture = architecture,
            .triple = triple,
        };
    }

    pub fn defaultOutputExtension(self: Target) []const u8 {
        return switch (self.platform) {
            .Windows => "exe",
            .Linux, .BSD, .Embedded, .MacOS => "out",
        };
    }

    pub fn entrySymbol(self: Target) []const u8 {
        return switch (self.platform) {
            .Windows => "mainCRTStartup",
            .MacOS => "_main",
            .Linux, .BSD, .Embedded => "_start",
        };
    }
};

pub const LinkInput = struct {
    text: []const u8,
    rodata: []const u8 = &.{},
    data: []const u8 = &.{},
    bss_size: u64 = 0,
    entry_offset: u64 = 0,
};

pub fn linkVxObj(allocator: std.mem.Allocator, vxobj_path: []const u8, output_path: []const u8, explicit_target: ?Target) !void {
    const file_data = try platform_io.readFileAlloc(allocator, vxobj_path, 1024 * 1024 * 1024);
    defer allocator.free(file_data);

    var container = try vxobj.VxObjV4Container.parse(allocator, file_data);
    defer container.deinit(allocator);

    const target = if (explicit_target) |t| t else try Target.parse(container.header.target_triple);

    const type_ir_data = container.getSection(vxobj.SECTION_TYPE_IR) orelse return LinkerError.VxObjError;
    const external_deps_data = container.getSection(vxobj.SECTION_EXTERNAL_DEPS);

    var external_dep_names: std.ArrayList([]const u8) = .empty;
    defer {
        for (external_dep_names.items) |name| allocator.free(name);
        external_dep_names.deinit(allocator);
    }

    if (external_deps_data) |deps_data| {
        const deps = try vxobj.deserializeExternalDeps(allocator, deps_data);
        defer {
            for (deps) |*dep| dep.deinit(allocator);
            allocator.free(deps);
        }

        for (deps) |dep| {
            try external_dep_names.append(allocator, try allocator.dupe(u8, dep.name));
        }
    }

    const arch_str = architectureToString(target.architecture);

    // 解析 TypeIR 并生成机器码
    const module = try typeir.parse(allocator, type_ir_data);
    defer module.deinit(allocator);

    const codegen_arch: codegen.Architecture = switch (target.architecture) {
        .X86_64 => .X86_64,
        .ARM64 => .ARM64,
        .ARM32 => .ARM32,
        .RV32 => .RV32,
        .RV64 => .RV64,
    };
    const compile_result = try codegen.compile(allocator, module, codegen_arch);
    defer allocator.free(compile_result.code);

    const input = LinkInput{
        .text = compile_result.code,
        .rodata = &.{},
        .data = &.{},
        .bss_size = 0,
        .entry_offset = compile_result.entry_offset,
    };

    try linkDirect(allocator, input, output_path, target, arch_str);

    if (@import("builtin").os.tag == .linux or @import("builtin").os.tag == .macos or @import("builtin").os.tag.isBSD()) {
        try platform_io.chmod(output_path, 0o755);
    }
}

pub fn linkDirect(allocator: std.mem.Allocator, input: LinkInput, output_path: []const u8, target: Target, arch: []const u8) !void {
    switch (target.platform) {
        .Linux, .BSD, .Embedded => {
            const elf_platform: elf_linker.ElfPlatform = switch (target.platform) {
                .Linux => .Linux,
                .BSD => .BSD,
                .Embedded => .Embedded,
                else => unreachable,
            };
            try elf_linker.linkElf(allocator, input.text, input.rodata, input.data, input.bss_size, input.entry_offset, output_path, arch, elf_platform);
        },
        .MacOS => {
            try macho_linker.linkMacho(allocator, input.text, input.rodata, input.data, input.bss_size, input.entry_offset, output_path, arch);
        },
        .Windows => {
            try pe_linker.linkPe(allocator, input.text, input.rodata, input.data, input.bss_size, input.entry_offset, output_path, arch);
        },
    }
}

fn detectPlatform(triple: []const u8) Platform {
    if (std.mem.indexOf(u8, triple, "windows") != null or std.mem.indexOf(u8, triple, "win32") != null or std.mem.indexOf(u8, triple, "msvc") != null or std.mem.indexOf(u8, triple, "mingw") != null or std.mem.indexOf(u8, triple, "cygwin") != null) {
        return .Windows;
    }
    if (std.mem.indexOf(u8, triple, "macos") != null or std.mem.indexOf(u8, triple, "darwin") != null or std.mem.indexOf(u8, triple, "apple") != null) {
        return .MacOS;
    }
    if (std.mem.indexOf(u8, triple, "freebsd") != null or std.mem.indexOf(u8, triple, "openbsd") != null or std.mem.indexOf(u8, triple, "netbsd") != null or std.mem.indexOf(u8, triple, "dragonfly") != null) {
        return .BSD;
    }
    if (std.mem.indexOf(u8, triple, "none") != null or std.mem.indexOf(u8, triple, "elf") != null or std.mem.indexOf(u8, triple, "eabi") != null) {
        return .Embedded;
    }
    return .Linux;
}

fn detectArchitecture(triple: []const u8) !Architecture {
    if (std.mem.indexOf(u8, triple, "x86_64") != null or std.mem.indexOf(u8, triple, "amd64") != null or std.mem.indexOf(u8, triple, "x64") != null) {
        return .X86_64;
    }
    if (std.mem.indexOf(u8, triple, "aarch64") != null or std.mem.indexOf(u8, triple, "arm64") != null) {
        return .ARM64;
    }
    if (std.mem.indexOf(u8, triple, "armv7") != null or std.mem.indexOf(u8, triple, "armv6") != null or std.mem.indexOf(u8, triple, "armeabi") != null or (std.mem.startsWith(u8, triple, "arm") and std.mem.indexOf(u8, triple, "aarch64") == null)) {
        return .ARM32;
    }
    if (std.mem.indexOf(u8, triple, "riscv64") != null or std.mem.indexOf(u8, triple, "rv64") != null) {
        return .RV64;
    }
    if (std.mem.indexOf(u8, triple, "riscv32") != null or std.mem.indexOf(u8, triple, "rv32") != null) {
        return .RV32;
    }
    return LinkerError.InvalidArchitecture;
}

pub fn architectureToString(arch: Architecture) []const u8 {
    return switch (arch) {
        .X86_64 => "x86_64",
        .ARM32 => "armv7",
        .ARM64 => "aarch64",
        .RV32 => "riscv32",
        .RV64 => "riscv64",
    };
}

pub fn hostTarget() Target {
    const builtin = @import("builtin");

    const platform = switch (builtin.os.tag) {
        .windows => Platform.Windows,
        .macos => Platform.MacOS,
        .freebsd, .openbsd, .netbsd, .dragonfly => Platform.BSD,
        .linux => Platform.Linux,
        else => Platform.Embedded,
    };

    const architecture: Architecture = switch (builtin.cpu.arch) {
        .x86_64 => .X86_64,
        .aarch64 => .ARM64,
        .arm => .ARM32,
        .riscv32 => .RV32,
        .riscv64 => .RV64,
        else => .X86_64,
    };

    return Target{
        .platform = platform,
        .architecture = architecture,
        .triple = "native",
    };
}
