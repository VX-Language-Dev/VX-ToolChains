const std = @import("std");
const platform_io = @import("platform_io");

pub const PeError = error{
    InvalidArchitecture,
    IoError,
};

const IMAGE_DOS_SIGNATURE: u16 = 0x5A4D;
const IMAGE_NT_SIGNATURE: u32 = 0x00004550;

const IMAGE_FILE_MACHINE_I386: u16 = 0x014C;
const IMAGE_FILE_MACHINE_AMD64: u16 = 0x8664;
const IMAGE_FILE_MACHINE_ARM: u16 = 0x01C0;
const IMAGE_FILE_MACHINE_ARM64: u16 = 0xAA64;

const IMAGE_FILE_EXECUTABLE_IMAGE: u16 = 0x0002;

const IMAGE_SCN_CNT_CODE: u32 = 0x00000020;
const IMAGE_SCN_CNT_INITIALIZED_DATA: u32 = 0x00000040;
const IMAGE_SCN_CNT_UNINITIALIZED_DATA: u32 = 0x00000080;
const IMAGE_SCN_MEM_EXECUTE: u32 = 0x20000000;
const IMAGE_SCN_MEM_READ: u32 = 0x40000000;
const IMAGE_SCN_MEM_WRITE: u32 = 0x80000000;

const IMAGE_SUBSYSTEM_WINDOWS_CUI: u16 = 3;

const IMAGE_DLL_CHARACTERISTICS_NX_COMPAT: u16 = 0x0100;

pub fn linkPe(allocator: std.mem.Allocator, text: []const u8, rodata: []const u8, data: []const u8, bss_size: u64, entry_offset: u64, output_path: []const u8, arch: []const u8) !void {
    const is_64bit = is64BitArch(arch);
    const machine = getMachine(arch) catch return PeError.InvalidArchitecture;

    if (is_64bit) {
        try linkPe64(allocator, text, rodata, data, bss_size, entry_offset, output_path, machine);
    } else {
        try linkPe32(allocator, text, rodata, data, bss_size, entry_offset, output_path, machine);
    }
}

fn linkPe64(allocator: std.mem.Allocator, text: []const u8, rodata: []const u8, data: []const u8, bss_size: u64, entry_offset: u64, output_path: []const u8, machine: u16) !void {
    _ = allocator;
    const page_align = 0x1000;

    const text_size = alignUp(text.len, 16);
    const rodata_size = alignUp(rodata.len, 16);
    const data_size = alignUp(data.len, 16);

    const dos_header_size: u32 = 64;
    const nt_headers_size: u32 = 24 + 96;
    const section_headers_size: u32 = 4 * 40;

    const text_offset = alignUp(dos_header_size + nt_headers_size + section_headers_size, page_align);
    const rodata_offset = text_offset + text_size;
    const data_offset = rodata_offset + rodata_size;

    const text_vaddr: u64 = 0x1000;
    const rodata_vaddr = text_vaddr + text_size;
    const data_vaddr = alignUp(rodata_vaddr + rodata_size, page_align);
    const bss_vaddr = data_vaddr + data_size;

    const file_alignment: u32 = 0x200;
    const section_alignment: u32 = 0x1000;

    const image_size = bss_vaddr + bss_size;

    const fd = try platform_io.createFile(output_path);
    defer platform_io.close(fd);

    var file_writer = platform_io.FileWriter.init(fd);
    const writer = &file_writer;

    try writeDosHeader(writer);

    try writeU32LE(writer, IMAGE_NT_SIGNATURE);

    try writeU16LE(writer, machine);
    try writeU16LE(writer, 4);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);
    try writeU16LE(writer, 0);
    try writeU16LE(writer, IMAGE_FILE_EXECUTABLE_IMAGE);

    try writeU16LE(writer, IMAGE_SUBSYSTEM_WINDOWS_CUI);
    try writeU16LE(writer, IMAGE_DLL_CHARACTERISTICS_NX_COMPAT);

    try writeU64LE(writer, 0);

    try writeU64LE(writer, text_vaddr + entry_offset);

    try writeU64LE(writer, dos_header_size + nt_headers_size);
    try writeU64LE(writer, 0);

    try writeU64LE(writer, 0);

    try writeU32LE(writer, nt_headers_size);
    try writeU32LE(writer, file_alignment);
    try writeU32LE(writer, section_alignment);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);

    try writeU32LE(writer, 0);
    try writeU32LE(writer, @intCast(image_size));

    try writeU32LE(writer, @intCast(text_vaddr));
    try writeU32LE(writer, 0);

    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);

    try writeU32LE(writer, 0x00000040);
    try writeU32LE(writer, 0x00000003);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);

    try writeSectionHeader64(writer, ".text", text_size, text_vaddr, text_offset, file_alignment, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ);
    try writeSectionHeader64(writer, ".rdata", rodata_size, rodata_vaddr, rodata_offset, file_alignment, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ);
    try writeSectionHeader64(writer, ".data", data_size, data_vaddr, data_offset, file_alignment, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);
    try writeSectionHeader64(writer, ".bss", bss_size, bss_vaddr, 0, file_alignment, IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);

    try padTo(writer, text_offset);
    try writer.writeAll(text);
    try padTo(writer, rodata_offset);
    try writer.writeAll(rodata);
    try padTo(writer, data_offset);
    try writer.writeAll(data);
}

fn linkPe32(allocator: std.mem.Allocator, text: []const u8, rodata: []const u8, data: []const u8, bss_size: u64, entry_offset: u64, output_path: []const u8, machine: u16) !void {
    _ = allocator;
    const page_align = 0x1000;

    const text_size: u32 = @intCast(alignUp(text.len, 16));
    const rodata_size: u32 = @intCast(alignUp(rodata.len, 16));
    const data_size: u32 = @intCast(alignUp(data.len, 16));

    const dos_header_size: u32 = 64;
    const nt_headers_size: u32 = 24 + 80;
    const section_headers_size: u32 = 4 * 40;

    const text_offset: u32 = @intCast(alignUp(dos_header_size + nt_headers_size + section_headers_size, page_align));
    const rodata_offset: u32 = text_offset + text_size;
    const data_offset: u32 = rodata_offset + rodata_size;

    const text_vaddr: u32 = 0x1000;
    const rodata_vaddr: u32 = text_vaddr + text_size;
    const data_vaddr: u32 = @intCast(alignUp(rodata_vaddr + rodata_size, page_align));
    const bss_vaddr: u32 = data_vaddr + data_size;

    const file_alignment: u32 = 0x200;
    const section_alignment: u32 = 0x1000;

    const image_size: u32 = @intCast(bss_vaddr + bss_size);

    const fd = try platform_io.createFile(output_path);
    defer platform_io.close(fd);

    var file_writer = platform_io.FileWriter.init(fd);
    const writer = &file_writer;

    try writeDosHeader(writer);

    try writeU32LE(writer, IMAGE_NT_SIGNATURE);

    try writeU16LE(writer, machine);
    try writeU16LE(writer, 4);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);
    try writeU16LE(writer, 0);
    try writeU16LE(writer, IMAGE_FILE_EXECUTABLE_IMAGE);

    try writeU16LE(writer, IMAGE_SUBSYSTEM_WINDOWS_CUI);
    try writeU16LE(writer, IMAGE_DLL_CHARACTERISTICS_NX_COMPAT);

    try writeU32LE(writer, 0);

    try writeU32LE(writer, @intCast(text_vaddr + entry_offset));

    try writeU32LE(writer, dos_header_size + nt_headers_size);
    try writeU32LE(writer, 0);

    try writeU32LE(writer, 0);

    try writeU32LE(writer, nt_headers_size);
    try writeU32LE(writer, file_alignment);
    try writeU32LE(writer, section_alignment);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);

    try writeU32LE(writer, 0);
    try writeU32LE(writer, image_size);

    try writeU32LE(writer, text_vaddr);
    try writeU32LE(writer, 0);

    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);

    try writeU32LE(writer, 0x00000040);
    try writeU32LE(writer, 0x00000003);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);

    try writeSectionHeader32(writer, ".text", text_size, text_vaddr, text_offset, file_alignment, IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ);
    try writeSectionHeader32(writer, ".rdata", rodata_size, rodata_vaddr, rodata_offset, file_alignment, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ);
    try writeSectionHeader32(writer, ".data", data_size, data_vaddr, data_offset, file_alignment, IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);
    try writeSectionHeader32(writer, ".bss", @intCast(bss_size), bss_vaddr, 0, file_alignment, IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE);

    try padTo(writer, text_offset);
    try writer.writeAll(text);
    try padTo(writer, rodata_offset);
    try writer.writeAll(rodata);
    try padTo(writer, data_offset);
    try writer.writeAll(data);
}

fn writeDosHeader(writer: anytype) !void {
    try writeU16LE(writer, IMAGE_DOS_SIGNATURE);

    var stub: [60]u8 = [_]u8{0} ** 60;
    stub[58] = 0x40;
    stub[59] = 0x00;
    try writer.writeAll(&stub);
}

fn writeSectionHeader64(writer: anytype, name: []const u8, virtual_size: u64, virtual_address: u64, raw_data_offset: u64, file_alignment: u32, characteristics: u32) !void {
    var name_buf: [8]u8 = [_]u8{0} ** 8;
    @memcpy(name_buf[0..@min(name.len, 8)], name);
    try writer.writeAll(&name_buf);

    try writeU32LE(writer, @intCast(virtual_size));
    try writeU32LE(writer, @intCast(virtual_address));

    const raw_size = if (raw_data_offset > 0) alignUp(@intCast(virtual_size), file_alignment) else 0;
    try writeU32LE(writer, @intCast(raw_size));
    try writeU32LE(writer, @intCast(raw_data_offset));

    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, characteristics);
}

fn writeSectionHeader32(writer: anytype, name: []const u8, virtual_size: u32, virtual_address: u32, raw_data_offset: u32, file_alignment: u32, characteristics: u32) !void {
    var name_buf: [8]u8 = [_]u8{0} ** 8;
    @memcpy(name_buf[0..@min(name.len, 8)], name);
    try writer.writeAll(&name_buf);

    try writeU32LE(writer, virtual_size);
    try writeU32LE(writer, virtual_address);

    const raw_size: u32 = if (raw_data_offset > 0) @intCast(alignUp(virtual_size, file_alignment)) else 0;
    try writeU32LE(writer, raw_size);
    try writeU32LE(writer, raw_data_offset);

    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, 0);
    try writeU32LE(writer, characteristics);
}

fn is64BitArch(arch: []const u8) bool {
    return std.mem.eql(u8, arch, "x86_64") or
        std.mem.eql(u8, arch, "aarch64") or
        std.mem.eql(u8, arch, "arm64") or
        std.mem.eql(u8, arch, "riscv64") or
        std.mem.eql(u8, arch, "rv64");
}

fn getMachine(arch: []const u8) !u16 {
    if (std.mem.eql(u8, arch, "x86_64") or std.mem.eql(u8, arch, "amd64")) {
        return IMAGE_FILE_MACHINE_AMD64;
    } else if (std.mem.eql(u8, arch, "aarch64") or std.mem.eql(u8, arch, "arm64")) {
        return IMAGE_FILE_MACHINE_ARM64;
    } else if (std.mem.eql(u8, arch, "arm") or std.mem.eql(u8, arch, "armv7") or std.mem.eql(u8, arch, "arm32")) {
        return IMAGE_FILE_MACHINE_ARM;
    } else if (std.mem.eql(u8, arch, "i386") or std.mem.eql(u8, arch, "x86")) {
        return IMAGE_FILE_MACHINE_I386;
    }
    return PeError.InvalidArchitecture;
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

fn writeU64LE(writer: anytype, value: u64) !void {
    try writer.writeAll(&std.mem.toBytes(value));
}
