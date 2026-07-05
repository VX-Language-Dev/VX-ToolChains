const std = @import("std");
const platform_io = @import("platform_io");

pub const MachoError = error{
    InvalidArchitecture,
    IoError,
};

const MH_MAGIC_64: u32 = 0xfeedfacf;
const MH_CIGAM_64: u32 = 0xcffaceed;

const MH_MAGIC: u32 = 0xfeedface;
const MH_CIGAM: u32 = 0xcefaedfe;

const CPU_TYPE_X86_64: u32 = 0x01000007;
const CPU_TYPE_ARM64: u32 = 0x0100000c;
const CPU_TYPE_ARM: u32 = 0x0000000c;

const CPU_SUBTYPE_ARM64_ALL: u32 = 0x00000000;
const CPU_SUBTYPE_X86_64_ALL: u32 = 0x00000003;
const CPU_SUBTYPE_ARM_V7: u32 = 0x00000009;

const MH_EXECUTE: u32 = 0x00000002;

const LC_SEGMENT_64: u32 = 0x19;
const LC_SEGMENT: u32 = 0x01;
const LC_SYMTAB: u32 = 0x02;
const LC_UNIXTHREAD: u32 = 0x05;

const S_ATTR_PURE_INSTRUCTIONS: u32 = 0x80000000;
const S_ATTR_SOME_INSTRUCTIONS: u32 = 0x00000040;

pub fn linkMacho(allocator: std.mem.Allocator, text: []const u8, rodata: []const u8, data: []const u8, bss_size: u64, entry_offset: u64, output_path: []const u8, arch: []const u8) !void {
    const is_64bit = is64BitArch(arch);
    const cpu_type = getCpuType(arch) catch return MachoError.InvalidArchitecture;
    const cpu_subtype = getCpuSubtype(arch) catch return MachoError.InvalidArchitecture;

    if (is_64bit) {
        try linkMacho64(allocator, text, rodata, data, bss_size, entry_offset, output_path, cpu_type, cpu_subtype);
    } else {
        try linkMacho32(allocator, text, rodata, data, bss_size, entry_offset, output_path, cpu_type, cpu_subtype);
    }
}

fn linkMacho64(allocator: std.mem.Allocator, text: []const u8, rodata: []const u8, data: []const u8, bss_size: u64, entry_offset: u64, output_path: []const u8, cpu_type: u32, cpu_subtype: u32) !void {
    _ = allocator;
    const page_align = 0x1000;

    const text_size = alignUp(text.len, 16);
    const rodata_size = alignUp(rodata.len, 16);
    const data_size = alignUp(data.len, 16);

    const header_size: u64 = 32;
    const load_commands_size: u64 = 200 + 72 + 72;

    const text_offset = alignUp(header_size + load_commands_size, page_align);
    const rodata_offset = text_offset + text_size;
    const data_offset = rodata_offset + rodata_size;

    const text_vaddr: u64 = 0x100000000;
    const rodata_vaddr = text_vaddr + text_size;
    const data_vaddr = alignUp(rodata_vaddr + rodata_size, page_align);

    const fd = try platform_io.createFile(output_path);
    defer platform_io.close(fd);

    var file_writer = platform_io.FileWriter.init(fd);
    const writer = &file_writer;

    try writeU32BE(writer, MH_MAGIC_64);
    try writeU32LE(writer, cpu_type);
    try writeU32LE(writer, cpu_subtype);
    try writeU32LE(writer, MH_EXECUTE);
    try writeU32LE(writer, @intCast(header_size + load_commands_size));
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0x00200000);
    try writeU32LE(writer, 0);

    try writeLoadCommandSegment64(writer, "__TEXT", text_vaddr, text_size + rodata_size, text_offset, text_size + rodata_size, 7, 0, 16, 0);
    try writeLoadCommandSegment64(writer, "__DATA_CONST", rodata_vaddr, rodata_size, rodata_offset, rodata_size, 5, 0, 16, 0);
    try writeLoadCommandSegment64(writer, "__DATA", data_vaddr, data_size + bss_size, data_offset, data_size, 7, 0, 16, 0);

    try writeLoadCommandUnixThread64(writer, text_vaddr + entry_offset);

    try writeLoadCommandSymtab(writer, 0, 0);

    try padTo(writer, text_offset);
    try writer.writeAll(text);
    try padTo(writer, rodata_offset);
    try writer.writeAll(rodata);
    try padTo(writer, data_offset);
    try writer.writeAll(data);
}

fn linkMacho32(allocator: std.mem.Allocator, text: []const u8, rodata: []const u8, data: []const u8, bss_size: u64, entry_offset: u64, output_path: []const u8, cpu_type: u32, cpu_subtype: u32) !void {
    _ = allocator;
    const page_align = 0x1000;

    const text_size: u32 = @intCast(alignUp(text.len, 16));
    const rodata_size: u32 = @intCast(alignUp(rodata.len, 16));
    const data_size: u32 = @intCast(alignUp(data.len, 16));

    const header_size: u32 = 28;
    const load_commands_size: u32 = 152 + 56 + 56;

    const text_offset: u32 = @intCast(alignUp(header_size + load_commands_size, page_align));
    const rodata_offset: u32 = text_offset + text_size;
    const data_offset: u32 = rodata_offset + rodata_size;

    const text_vaddr: u32 = 0x1000;
    const rodata_vaddr: u32 = text_vaddr + text_size;
    const data_vaddr: u32 = @intCast(alignUp(rodata_vaddr + rodata_size, page_align));

    const fd = try platform_io.createFile(output_path);
    defer platform_io.close(fd);

    var file_writer = platform_io.FileWriter.init(fd);
    const writer = &file_writer;

    try writeU32BE(writer, MH_MAGIC);
    try writeU32LE(writer, cpu_type);
    try writeU32LE(writer, cpu_subtype);
    try writeU32LE(writer, MH_EXECUTE);
    try writeU32LE(writer, header_size + load_commands_size);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0x00200000);

    try writeLoadCommandSegment32(writer, "__TEXT", text_vaddr, text_size + rodata_size, text_offset, text_size + rodata_size, 7, 0, 16);
    try writeLoadCommandSegment32(writer, "__DATA_CONST", rodata_vaddr, rodata_size, rodata_offset, rodata_size, 5, 0, 16);
    try writeLoadCommandSegment32(writer, "__DATA", data_vaddr, @intCast(data_size + bss_size), data_offset, data_size, 7, 0, 16);

    try writeLoadCommandUnixThread32(writer, @intCast(text_vaddr + entry_offset));

    try writeLoadCommandSymtab32(writer, 0, 0);

    try padTo(writer, text_offset);
    try writer.writeAll(text);
    try padTo(writer, rodata_offset);
    try writer.writeAll(rodata);
    try padTo(writer, data_offset);
    try writer.writeAll(data);
}

fn writeLoadCommandSegment64(writer: anytype, name: []const u8, vmaddr: u64, vmsize: u64, fileoff: u64, filesize: u64, maxprot: u32, initprot: u32, nsects: u32, flags: u32) !void {
    try writeU32LE(writer, LC_SEGMENT_64);
    try writeU32LE(writer, 72);

    var name_buf: [16]u8 = [_]u8{0} ** 16;
    @memcpy(name_buf[0..@min(name.len, 16)], name);
    try writer.writeAll(&name_buf);

    try writeU64LE(writer, vmaddr);
    try writeU64LE(writer, vmsize);
    try writeU64LE(writer, fileoff);
    try writeU64LE(writer, filesize);
    try writeU32LE(writer, maxprot);
    try writeU32LE(writer, initprot);
    try writeU32LE(writer, nsects);
    try writeU32LE(writer, flags);
}

fn writeLoadCommandSegment32(writer: anytype, name: []const u8, vmaddr: u32, vmsize: u32, fileoff: u32, filesize: u32, maxprot: u32, initprot: u32, nsects: u32) !void {
    try writeU32LE(writer, LC_SEGMENT);
    try writeU32LE(writer, 56);

    var name_buf: [16]u8 = [_]u8{0} ** 16;
    @memcpy(name_buf[0..@min(name.len, 16)], name);
    try writer.writeAll(&name_buf);

    try writeU32LE(writer, vmaddr);
    try writeU32LE(writer, vmsize);
    try writeU32LE(writer, fileoff);
    try writeU32LE(writer, filesize);
    try writeU32LE(writer, maxprot);
    try writeU32LE(writer, initprot);
    try writeU32LE(writer, nsects);
    try writeU32LE(writer, 0);
}

fn writeLoadCommandUnixThread64(writer: anytype, entry: u64) !void {
    try writeU32LE(writer, LC_UNIXTHREAD);
    try writeU32LE(writer, 80);

    try writeU32LE(writer, 1);
    try writeU32LE(writer, 1);

    var state: [64]u8 = [_]u8{0} ** 64;
    @memcpy(state[48..56], &std.mem.toBytes(entry));
    try writer.writeAll(&state);
}

fn writeLoadCommandUnixThread32(writer: anytype, entry: u32) !void {
    try writeU32LE(writer, LC_UNIXTHREAD);
    try writeU32LE(writer, 68);

    try writeU32LE(writer, 1);
    try writeU32LE(writer, 1);

    var state: [52]u8 = [_]u8{0} ** 52;
    @memcpy(state[44..48], &std.mem.toBytes(entry));
    try writer.writeAll(&state);
}

fn writeLoadCommandSymtab(writer: anytype, symoff: u64, nsyms: u64) !void {
    try writeU32LE(writer, LC_SYMTAB);
    try writeU32LE(writer, 24);
    try writeU64LE(writer, symoff);
    try writeU64LE(writer, nsyms);
    try writeU64LE(writer, 0);
}

fn writeLoadCommandSymtab32(writer: anytype, symoff: u32, nsyms: u32) !void {
    try writeU32LE(writer, LC_SYMTAB);
    try writeU32LE(writer, 20);
    try writeU32LE(writer, symoff);
    try writeU32LE(writer, nsyms);
    try writeU32LE(writer, 0);
}

fn is64BitArch(arch: []const u8) bool {
    return std.mem.eql(u8, arch, "x86_64") or
        std.mem.eql(u8, arch, "aarch64") or
        std.mem.eql(u8, arch, "arm64") or
        std.mem.eql(u8, arch, "riscv64") or
        std.mem.eql(u8, arch, "rv64");
}

fn getCpuType(arch: []const u8) !u32 {
    if (std.mem.eql(u8, arch, "x86_64") or std.mem.eql(u8, arch, "amd64")) {
        return CPU_TYPE_X86_64;
    } else if (std.mem.eql(u8, arch, "aarch64") or std.mem.eql(u8, arch, "arm64")) {
        return CPU_TYPE_ARM64;
    } else if (std.mem.eql(u8, arch, "arm") or std.mem.eql(u8, arch, "armv7") or std.mem.eql(u8, arch, "arm32")) {
        return CPU_TYPE_ARM;
    }
    return MachoError.InvalidArchitecture;
}

fn getCpuSubtype(arch: []const u8) !u32 {
    if (std.mem.eql(u8, arch, "x86_64") or std.mem.eql(u8, arch, "amd64")) {
        return CPU_SUBTYPE_X86_64_ALL;
    } else if (std.mem.eql(u8, arch, "aarch64") or std.mem.eql(u8, arch, "arm64")) {
        return CPU_SUBTYPE_ARM64_ALL;
    } else if (std.mem.eql(u8, arch, "arm") or std.mem.eql(u8, arch, "armv7") or std.mem.eql(u8, arch, "arm32")) {
        return CPU_SUBTYPE_ARM_V7;
    }
    return MachoError.InvalidArchitecture;
}

fn alignUp(value: usize, alignment: usize) usize {
    return (value + alignment - 1) & ~(alignment - 1);
}

fn padTo(writer: anytype, offset: u64) !void {
    const current = writer.getPos();
    if (current < offset) {
        const padding = offset - current;
        var zeros: [1024]u8 = [_]u8{0} ** 1024;
        var remaining = padding;
        while (remaining > 0) {
            const to_write = @min(remaining, zeros.len);
            try writer.writeAll(zeros[0..to_write]);
            remaining -= to_write;
        }
    }
}

fn writeU16LE(writer: anytype, value: u16) !void {
    try writer.writeAll(&std.mem.toBytes(value));
}

fn writeU32LE(writer: anytype, value: u32) !void {
    try writer.writeAll(&std.mem.toBytes(value));
}

fn writeU32BE(writer: anytype, value: u32) !void {
    var bytes: [4]u8 = undefined;
    std.mem.writeInt(u32, &bytes, value, .big);
    try writer.writeAll(&bytes);
}

fn writeU64LE(writer: anytype, value: u64) !void {
    try writer.writeAll(&std.mem.toBytes(value));
}
