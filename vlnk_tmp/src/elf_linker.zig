const std = @import("std");
const platform_io = @import("platform_io");

pub const ElfError = error{
    InvalidArchitecture,
    TruncatedData,
    IoError,
};

pub const ElfPlatform = enum {
    Linux,
    BSD,
    Embedded,
};

const ELFMAG = [_]u8{ 0x7F, 'E', 'L', 'F' };

const EI_CLASS = enum(u8) {
    ELFCLASS32 = 1,
    ELFCLASS64 = 2,
};

const EI_DATA = enum(u8) {
    ELFDATA2LSB = 1,
    ELFDATA2MSB = 2,
};

const EI_VERSION = enum(u8) {
    EV_CURRENT = 1,
};

const ELFOSABI = enum(u8) {
    ELFOSABI_NONE = 0,
    ELFOSABI_LINUX = 3,
};

const ET = enum(u16) {
    ET_NONE = 0,
    ET_EXEC = 2,
    ET_DYN = 3,
};

const EM = enum(u16) {
    EM_NONE = 0,
    EM_386 = 3,
    EM_X86_64 = 62,
    EM_ARM = 40,
    EM_AARCH64 = 183,
    EM_RISCV = 243,
};

const PT = enum(u32) {
    PT_NULL = 0,
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
    PT_INTERP = 3,
    PT_NOTE = 4,
    PT_SHLIB = 5,
    PT_PHDR = 6,
};

const PF = struct {
    const R: u32 = 4;
    const W: u32 = 2;
    const X: u32 = 1;
};

const SHT = enum(u32) {
    SHT_NULL = 0,
    SHT_PROGBITS = 1,
    SHT_SYMTAB = 2,
    SHT_STRTAB = 3,
    SHT_RELA = 4,
    SHT_HASH = 5,
    SHT_DYNAMIC = 6,
    SHT_NOTE = 7,
    SHT_NOBITS = 8,
    SHT_REL = 9,
    SHT_SHLIB = 10,
    SHT_DYNSYM = 11,
};

const SHF = struct {
    const WRITE: u64 = 1;
    const ALLOC: u64 = 2;
    const EXECINSTR: u64 = 4;
};

pub fn linkElf(allocator: std.mem.Allocator, text: []const u8, rodata: []const u8, data: []const u8, bss_size: u64, entry_offset: u64, output_path: []const u8, arch: []const u8, platform: ElfPlatform) !void {
    const is_64bit = is64BitArch(arch);
    const em = getEM(arch) catch return ElfError.InvalidArchitecture;
    const osabi: ELFOSABI = switch (platform) {
        .Linux => .ELFOSABI_LINUX,
        .BSD, .Embedded => .ELFOSABI_NONE,
    };

    if (is_64bit) {
        try linkElf64(allocator, text, rodata, data, bss_size, entry_offset, output_path, em, osabi);
    } else {
        try linkElf32(allocator, text, rodata, data, bss_size, entry_offset, output_path, em, osabi);
    }
}

fn linkElf64(allocator: std.mem.Allocator, text: []const u8, rodata: []const u8, data: []const u8, bss_size: u64, entry_offset: u64, output_path: []const u8, em: EM, osabi: ELFOSABI) !void {
    _ = allocator;
    const page_align = 0x1000;

    const text_size = alignUp(text.len, 16);
    const rodata_size = alignUp(rodata.len, 16);
    const data_size = alignUp(data.len, 16);

    const phdr_size: u64 = 2 * 56;
    const shdr_size: u64 = 5 * 64;

    const ph_offset: u64 = 64;
    const sh_offset: u64 = ph_offset + phdr_size;

    const text_offset = alignUp(sh_offset + shdr_size, page_align);
    const rodata_offset = text_offset + text_size;
    const data_offset = rodata_offset + rodata_size;
    const bss_offset = data_offset + data_size;
    _ = bss_offset;

    const text_vaddr: u64 = 0x100000;
    const rodata_vaddr = text_vaddr + text_size;
    const data_vaddr = alignUp(rodata_vaddr + rodata_size, page_align);
    const bss_vaddr = data_vaddr + data_size;

    const fd = try platform_io.createFile(output_path);
    defer platform_io.close(fd);

    var file_writer = platform_io.FileWriter.init(fd);
    const writer = &file_writer;

    try writer.writeAll(&ELFMAG);
    try writer.writeByte(@intFromEnum(EI_CLASS.ELFCLASS64));
    try writer.writeByte(@intFromEnum(EI_DATA.ELFDATA2LSB));
    try writer.writeByte(@intFromEnum(EI_VERSION.EV_CURRENT));
    try writer.writeByte(@intFromEnum(osabi));
    try writer.writeByte(0);

    var ident_pad: [7]u8 = [_]u8{0} ** 7;
    try writer.writeAll(&ident_pad);

    try writeU16LE(writer, @intFromEnum(ET.ET_EXEC));
    try writeU16LE(writer, @intFromEnum(em));
    try writeU32LE(writer, 1);
    try writeU64LE(writer, text_vaddr + entry_offset);
    try writeU64LE(writer, ph_offset);
    try writeU64LE(writer, sh_offset);
    try writeU32LE(writer, 0);
    try writeU16LE(writer, 64);
    try writeU16LE(writer, 56);
    try writeU16LE(writer, 2);
    try writeU16LE(writer, 64);
    try writeU16LE(writer, 0);
    try writeU16LE(writer, 0);

    try writeU32LE(writer, @intFromEnum(PT.PT_LOAD));
    try writeU32LE(writer, PF.R | PF.X);
    try writeU64LE(writer, text_offset);
    try writeU64LE(writer, text_vaddr);
    try writeU64LE(writer, text_vaddr);
    try writeU64LE(writer, text_size + rodata_size);
    try writeU64LE(writer, text_size + rodata_size);
    try writeU64LE(writer, page_align);
    try writeU64LE(writer, page_align);

    try writeU32LE(writer, @intFromEnum(PT.PT_LOAD));
    try writeU32LE(writer, PF.R | PF.W);
    try writeU64LE(writer, data_offset);
    try writeU64LE(writer, data_vaddr);
    try writeU64LE(writer, data_vaddr);
    try writeU64LE(writer, data_size + bss_size);
    try writeU64LE(writer, data_size);
    try writeU64LE(writer, page_align);
    try writeU64LE(writer, page_align);

    const sh_offset_strtab = sh_offset + shdr_size;
    const strtab_data = "\x00.text\x00.rodata\x00.data\x00.bss\x00";

    try writeSectionHeader64(writer, 0, @intFromEnum(SHT.SHT_STRTAB), 0, 0, sh_offset_strtab, strtab_data.len, 0, 0, 1, 0);
    try writeSectionHeader64(writer, 1, @intFromEnum(SHT.SHT_PROGBITS), SHF.ALLOC | SHF.EXECINSTR, text_vaddr, text_offset, text.len, 0, 0, 16, 0);

    const rodata_sh_name: u64 = if (rodata.len > 0) 7 else 0;
    const rodata_sh_type: u64 = if (rodata.len > 0) @intFromEnum(SHT.SHT_PROGBITS) else @intFromEnum(SHT.SHT_NULL);
    const rodata_sh_flags: u64 = if (rodata.len > 0) SHF.ALLOC else 0;
    const rodata_sh_addr: u64 = if (rodata.len > 0) rodata_vaddr else 0;
    const rodata_sh_offset: u64 = if (rodata.len > 0) rodata_offset else 0;
    try writeSectionHeader64(writer, rodata_sh_name, rodata_sh_type, rodata_sh_flags, rodata_sh_addr, rodata_sh_offset, rodata.len, 0, 0, 16, 0);

    try writeSectionHeader64(writer, 15, @intFromEnum(SHT.SHT_PROGBITS), SHF.ALLOC | SHF.WRITE, data_vaddr, data_offset, data.len, 0, 0, 16, 0);
    try writeSectionHeader64(writer, 20, @intFromEnum(SHT.SHT_NOBITS), SHF.ALLOC | SHF.WRITE, bss_vaddr, 0, bss_size, 0, 0, 16, 0);

    try writer.writeAll(strtab_data);

    try padTo(writer, text_offset);
    try writer.writeAll(text);
    try padTo(writer, rodata_offset);
    try writer.writeAll(rodata);
    try padTo(writer, data_offset);
    try writer.writeAll(data);
}

fn linkElf32(allocator: std.mem.Allocator, text: []const u8, rodata: []const u8, data: []const u8, bss_size: u64, entry_offset: u64, output_path: []const u8, em: EM, osabi: ELFOSABI) !void {
    _ = allocator;
    const page_align = 0x1000;

    const text_size: u32 = @intCast(alignUp(text.len, 16));
    const rodata_size: u32 = @intCast(alignUp(rodata.len, 16));
    const data_size: u32 = @intCast(alignUp(data.len, 16));

    const phdr_size: u32 = 2 * 32;
    const shdr_size: u32 = 5 * 40;

    const ph_offset: u32 = 52;
    const sh_offset: u32 = ph_offset + phdr_size;

    const text_offset: u32 = @intCast(alignUp(sh_offset + shdr_size, page_align));
    const rodata_offset: u32 = text_offset + text_size;
    const data_offset: u32 = rodata_offset + rodata_size;

    const text_vaddr: u32 = 0x100000;
    const rodata_vaddr: u32 = text_vaddr + text_size;
    const data_vaddr: u32 = @intCast(alignUp(rodata_vaddr + rodata_size, page_align));
    const bss_vaddr: u32 = data_vaddr + data_size;

    const fd = try platform_io.createFile(output_path);
    defer platform_io.close(fd);

    var file_writer = platform_io.FileWriter.init(fd);
    const writer = &file_writer;

    try writer.writeAll(&ELFMAG);
    try writer.writeByte(@intFromEnum(EI_CLASS.ELFCLASS32));
    try writer.writeByte(@intFromEnum(EI_DATA.ELFDATA2LSB));
    try writer.writeByte(@intFromEnum(EI_VERSION.EV_CURRENT));
    try writer.writeByte(@intFromEnum(osabi));
    try writer.writeByte(0);

    var ident_pad: [7]u8 = [_]u8{0} ** 7;
    try writer.writeAll(&ident_pad);

    try writeU16LE(writer, @intFromEnum(ET.ET_EXEC));
    try writeU16LE(writer, @intFromEnum(em));
    try writeU32LE(writer, 1);
    try writeU32LE(writer, @intCast(text_vaddr + entry_offset));
    try writeU32LE(writer, ph_offset);
    try writeU32LE(writer, sh_offset);
    try writeU32LE(writer, 0);
    try writeU16LE(writer, 52);
    try writeU16LE(writer, 32);
    try writeU16LE(writer, 2);
    try writeU16LE(writer, 40);
    try writeU16LE(writer, 0);
    try writeU16LE(writer, 0);

    try writeU32LE(writer, @intFromEnum(PT.PT_LOAD));
    try writeU32LE(writer, text_offset);
    try writeU32LE(writer, text_vaddr);
    try writeU32LE(writer, text_vaddr);
    try writeU32LE(writer, @intCast(text_size + rodata_size));
    try writeU32LE(writer, @intCast(text_size + rodata_size));
    try writeU32LE(writer, PF.R | PF.X);
    try writeU32LE(writer, page_align);

    try writeU32LE(writer, @intFromEnum(PT.PT_LOAD));
    try writeU32LE(writer, data_offset);
    try writeU32LE(writer, data_vaddr);
    try writeU32LE(writer, data_vaddr);
    try writeU32LE(writer, @intCast(data_size + bss_size));
    try writeU32LE(writer, @intCast(data_size));
    try writeU32LE(writer, PF.R | PF.W);
    try writeU32LE(writer, page_align);

    const strtab_data = "\x00.text\x00.rodata\x00.data\x00.bss\x00";
    const sh_offset_strtab = sh_offset + shdr_size;

    try writeSectionHeader32(writer, 0, @intFromEnum(SHT.SHT_STRTAB), 0, 0, sh_offset_strtab, @intCast(strtab_data.len), 0, 0, 1, 0);
    try writeSectionHeader32(writer, 1, @intFromEnum(SHT.SHT_PROGBITS), SHF.ALLOC | SHF.EXECINSTR, text_vaddr, text_offset, @intCast(text.len), 0, 0, 16, 0);

    const rodata_sh_name: u32 = if (rodata.len > 0) 7 else 0;
    const rodata_sh_type: u32 = if (rodata.len > 0) @intFromEnum(SHT.SHT_PROGBITS) else @intFromEnum(SHT.SHT_NULL);
    const rodata_sh_flags: u32 = if (rodata.len > 0) SHF.ALLOC else 0;
    const rodata_sh_addr: u32 = if (rodata.len > 0) rodata_vaddr else 0;
    const rodata_sh_offset: u32 = if (rodata.len > 0) rodata_offset else 0;
    try writeSectionHeader32(writer, rodata_sh_name, rodata_sh_type, rodata_sh_flags, rodata_sh_addr, rodata_sh_offset, @intCast(rodata.len), 0, 0, 16, 0);

    try writeSectionHeader32(writer, 15, @intFromEnum(SHT.SHT_PROGBITS), SHF.ALLOC | SHF.WRITE, data_vaddr, data_offset, @intCast(data.len), 0, 0, 16, 0);
    try writeSectionHeader32(writer, 20, @intFromEnum(SHT.SHT_NOBITS), SHF.ALLOC | SHF.WRITE, bss_vaddr, 0, @intCast(bss_size), 0, 0, 16, 0);

    try writer.writeAll(strtab_data);

    try padTo(writer, text_offset);
    try writer.writeAll(text);
    try padTo(writer, rodata_offset);
    try writer.writeAll(rodata);
    try padTo(writer, data_offset);
    try writer.writeAll(data);
}

fn writeSectionHeader64(writer: anytype, name: u64, type_: u64, flags: u64, addr: u64, offset: u64, size: u64, link: u64, info: u64, addralign: u64, entsize: u64) !void {
    try writeU32LE(writer, @intCast(name));
    try writeU32LE(writer, @intCast(type_));
    try writeU64LE(writer, flags);
    try writeU64LE(writer, addr);
    try writeU64LE(writer, offset);
    try writeU64LE(writer, size);
    try writeU32LE(writer, @intCast(link));
    try writeU32LE(writer, @intCast(info));
    try writeU64LE(writer, addralign);
    try writeU64LE(writer, entsize);
}

fn writeSectionHeader32(writer: anytype, name: u32, type_: u32, flags: u32, addr: u32, offset: u32, size: u32, link: u32, info: u32, addralign: u32, entsize: u32) !void {
    try writeU32LE(writer, name);
    try writeU32LE(writer, type_);
    try writeU32LE(writer, flags);
    try writeU32LE(writer, addr);
    try writeU32LE(writer, offset);
    try writeU32LE(writer, size);
    try writeU32LE(writer, link);
    try writeU32LE(writer, info);
    try writeU32LE(writer, addralign);
    try writeU32LE(writer, entsize);
}

fn is64BitArch(arch: []const u8) bool {
    return std.mem.eql(u8, arch, "x86_64") or
        std.mem.eql(u8, arch, "aarch64") or
        std.mem.eql(u8, arch, "arm64") or
        std.mem.eql(u8, arch, "riscv64") or
        std.mem.eql(u8, arch, "rv64");
}

fn getEM(arch: []const u8) !EM {
    if (std.mem.eql(u8, arch, "x86_64") or std.mem.eql(u8, arch, "amd64")) {
        return EM.EM_X86_64;
    } else if (std.mem.eql(u8, arch, "aarch64") or std.mem.eql(u8, arch, "arm64")) {
        return EM.EM_AARCH64;
    } else if (std.mem.eql(u8, arch, "arm") or std.mem.eql(u8, arch, "armv7") or std.mem.eql(u8, arch, "arm32")) {
        return EM.EM_ARM;
    } else if (std.mem.eql(u8, arch, "riscv64") or std.mem.eql(u8, arch, "rv64")) {
        return EM.EM_RISCV;
    } else if (std.mem.eql(u8, arch, "riscv32") or std.mem.eql(u8, arch, "rv32")) {
        return EM.EM_RISCV;
    } else if (std.mem.eql(u8, arch, "i386") or std.mem.eql(u8, arch, "x86")) {
        return EM.EM_386;
    }
    return ElfError.InvalidArchitecture;
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
