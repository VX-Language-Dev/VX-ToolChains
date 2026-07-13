const std = @import("std");
const Allocator = std.mem.Allocator;
const TargetProfile = @import("target_profile.zig").TargetProfile;

pub const LinkerError = error{
    LldNotFound,
    LinkFailed,
    IoError,
    ObjectFileWriteFailed,
    InvalidArchitecture,
    OutputNotCreated,
};

pub const LinkResult = struct {
    output_path: []const u8,
    output_size: usize,
};

pub const LinkerType = enum {
    builtin,
    lld,
};

pub fn Linker(comptime linker_type: LinkerType) type {
    return struct {
        allocator: Allocator,
        target: TargetProfile,

        const Self = @This();

        pub fn init(allocator: Allocator, target: TargetProfile) Self {
            return Self{
                .allocator = allocator,
                .target = target,
            };
        }

        pub fn link(
            self: *const Self,
            text: []const u8,
            rodata: []const u8,
            data: []const u8,
            bss_size: u64,
            entry_offset: u64,
            output_path: []const u8,
        ) LinkerError!LinkResult {
            switch (linker_type) {
                .builtin => return self.linkBuiltin(text, rodata, data, bss_size, entry_offset, output_path),
                .lld => return self.linkLld(text, output_path),
            }
        }

        fn linkBuiltin(
            self: *const Self,
            text: []const u8,
            rodata: []const u8,
            data: []const u8,
            bss_size: u64,
            entry_offset: u64,
            output_path: []const u8,
        ) LinkerError!LinkResult {
            _ = entry_offset;
            _ = bss_size;

            const cwd = std.fs.cwd();
            const file = cwd.createFile(output_path, .{ .truncate = true }) catch return LinkerError.ObjectFileWriteFailed;
            defer file.close();

            var writer = file.writer();

            switch (self.target.output_format) {
                .Elf => {
                    try writeElfHeader(&writer, text.len, rodata.len, data.len);
                },
                .MachO => {
                    try writeMachOHeader(&writer, text.len, rodata.len, data.len);
                },
                .Pe => {
                    try writePeHeader(&writer, text.len, rodata.len, data.len);
                },
            }

            try writer.writeAll(text);
            if (rodata.len > 0) try writer.writeAll(rodata);
            if (data.len > 0) try writer.writeAll(data);

            const stat = file.stat() catch return LinkerError.OutputNotCreated;
            return LinkResult{
                .output_path = output_path,
                .output_size = stat.size,
            };
        }

        fn linkLld(
            self: *const Self,
            obj_data: []const u8,
            output_path: []const u8,
        ) LinkerError!LinkResult {
            const tmp_dir = std.testing.tmpDir(.{});
            defer tmp_dir.cleanup();

            const obj_filename = "vx_lld_tmp.o";
            var tmp_file = tmp_dir.dir.createFile(obj_filename, .{ .truncate = true }) catch return LinkerError.ObjectFileWriteFailed;
            defer tmp_file.close();
            tmp_file.writeAll(obj_data) catch return LinkerError.ObjectFileWriteFailed;

            const lld_binary = self.target.lldBinaryName();
            var argv: std.ArrayList([]const u8) = .empty;
            defer argv.deinit(self.allocator);

            argv.append(self.allocator, lld_binary) catch return LinkerError.IoError;
            argv.append(self.allocator, "-o") catch return LinkerError.IoError;
            argv.append(self.allocator, output_path) catch return LinkerError.IoError;

            for (self.target.static_link_flags) |flag| {
                argv.append(self.allocator, flag) catch return LinkerError.IoError;
            }

            argv.append(self.allocator, obj_filename) catch return LinkerError.IoError;

            const result = std.process.run(self.allocator, argv.items) catch return LinkerError.LldNotFound;

            if (result.term != .Exited or result.term.Exited != 0) {
                return LinkerError.LinkFailed;
            }

            const cwd = std.fs.cwd();
            const stat = cwd.statFile(output_path) catch return LinkerError.OutputNotCreated;
            return LinkResult{
                .output_path = output_path,
                .output_size = stat.size,
            };
        }

        fn writeElfHeader(writer: anytype, text_len: usize, rodata_len: usize, data_len: usize) !void {
            _ = text_len;
            _ = rodata_len;
            _ = data_len;
            const e_ident = [16]u8{
                0x7f, 'E', 'L', 'F',
                2,    1,   1,   0,
                0,    0,   0,   0,
                0,    0,   0,   0,
            };
            try writer.writeAll(&e_ident);
            const e_type: u16 = 2;
            try writer.writeInt(u16, e_type, .little);
            const e_machine: u16 = 0x3E;
            try writer.writeInt(u16, e_machine, .little);
            const e_version: u32 = 1;
            try writer.writeInt(u32, e_version, .little);
            const e_entry: u64 = 0x401000;
            try writer.writeInt(u64, e_entry, .little);
            const e_phoff: u64 = 64;
            try writer.writeInt(u64, e_phoff, .little);
            const e_shoff: u64 = 0;
            try writer.writeInt(u64, e_shoff, .little);
            const e_flags: u32 = 0;
            try writer.writeInt(u32, e_flags, .little);
            const e_ehsize: u16 = 64;
            try writer.writeInt(u16, e_ehsize, .little);
            const e_phentsize: u16 = 56;
            try writer.writeInt(u16, e_phentsize, .little);
            const e_phnum: u16 = 1;
            try writer.writeInt(u16, e_phnum, .little);
            const e_shentsize: u16 = 64;
            try writer.writeInt(u16, e_shentsize, .little);
            const e_shnum: u16 = 0;
            try writer.writeInt(u16, e_shnum, .little);
            const e_shstrndx: u16 = 0;
            try writer.writeInt(u16, e_shstrndx, .little);
        }

        fn writeMachOHeader(writer: anytype, text_len: usize, rodata_len: usize, data_len: usize) !void {
            _ = text_len;
            _ = rodata_len;
            _ = data_len;
            const magic: u32 = 0xFEEDFACF;
            try writer.writeInt(u32, magic, .little);
            const cputype: u32 = 0x0100000C;
            try writer.writeInt(u32, cputype, .little);
            const cpusubtype: u32 = 0x00000003;
            try writer.writeInt(u32, cpusubtype, .little);
            const filetype: u32 = 0x00000002;
            try writer.writeInt(u32, filetype, .little);
            const ncmds: u32 = 0;
            try writer.writeInt(u32, ncmds, .little);
            const sizeofcmds: u32 = 0;
            try writer.writeInt(u32, sizeofcmds, .little);
            const flags: u32 = 0;
            try writer.writeInt(u32, flags, .little);
            const reserved: u32 = 0;
            try writer.writeInt(u32, reserved, .little);
        }

        fn writePeHeader(writer: anytype, text_len: usize, rodata_len: usize, data_len: usize) !void {
            _ = text_len;
            _ = rodata_len;
            _ = data_len;
            try writer.writeAll("MZ");
            var pe_stub: [58]u8 = undefined;
            @memset(&pe_stub, 0);
            try writer.writeAll(&pe_stub);
            const pe_offset: u32 = 0x80;
            try writer.writeInt(u32, pe_offset, .little);
            try writer.writeAll("PE\x00\x00");
            const machine: u16 = 0x8664;
            try writer.writeInt(u16, machine, .little);
            const number_of_sections: u16 = 1;
            try writer.writeInt(u16, number_of_sections, .little);
        }
    };
}

pub const BuiltinLinker = Linker(.builtin);
pub const LldLinker = Linker(.lld);

test "builtin linker init" {
    const target = TargetProfile.fromTriple("x86_64-linux-gnu");
    const linker = BuiltinLinker.init(std.testing.allocator, target);
    try std.testing.expectEqualStrings("x86_64-linux-gnu", linker.target.triple);
}

test "lld linker init" {
    const target = TargetProfile.fromTriple("aarch64-apple-darwin");
    const linker = LldLinker.init(std.testing.allocator, target);
    try std.testing.expect(linker.target.lld_flavor == .Darwin);
}

test "linker error coverage" {
    const target = TargetProfile.fromTriple("x86_64-linux-gnu");
    const linker = BuiltinLinker.init(std.testing.allocator, target);
    const result = linker.link("", "", "", 0, 0, "/nonexistent/path/output");
    try std.testing.expectError(LinkerError.ObjectFileWriteFailed, result);
}
