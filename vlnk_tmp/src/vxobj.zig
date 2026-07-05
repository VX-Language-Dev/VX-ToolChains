const std = @import("std");

pub const MAGIC = "VXOBJ";
pub const VERSION_V4: u32 = 4;

pub const SECTION_TYPE_IR = "TypeIR";
pub const SECTION_DEBUG = "Debug";
pub const SECTION_SOURCE_MAP = "SourceMap";
pub const SECTION_EXTERNAL_DEPS = "ExternalDeps";

pub const VxObjError = error{
    InvalidMagic,
    UnsupportedVersion,
    InvalidData,
    SectionNotFound,
    TruncatedData,
};

pub const VxObjV4SectionIndex = struct {
    name: []const u8,
    offset: u32,
    size: u32,
};

pub const VxObjV4Header = struct {
    version: u32,
    flags: u32,
    target_triple: []const u8,
    sections: []VxObjV4SectionIndex,
};

pub const VxObjV4Container = struct {
    header: VxObjV4Header,
    section_data: std.StringHashMap([]const u8),

    pub fn parse(allocator: std.mem.Allocator, data: []const u8) !VxObjV4Container {
        if (data.len < 5) return VxObjError.InvalidMagic;

        if (!std.mem.eql(u8, data[0..5], MAGIC)) return VxObjError.InvalidMagic;

        var offset: usize = 5;

        const version = readU32BE(data, &offset) catch return VxObjError.TruncatedData;
        if (version != VERSION_V4) return VxObjError.UnsupportedVersion;

        const flags = readU32BE(data, &offset) catch return VxObjError.TruncatedData;
        const target_triple = readString(allocator, data, &offset) catch return VxObjError.TruncatedData;

        const num_sections = readU32BE(data, &offset) catch return VxObjError.TruncatedData;

        const sections = try allocator.alloc(VxObjV4SectionIndex, num_sections);
        errdefer allocator.free(sections);

        var section_data = std.StringHashMap([]const u8).init(allocator);
        errdefer section_data.deinit();

        for (sections) |*section| {
            const name = readString(allocator, data, &offset) catch return VxObjError.TruncatedData;
            const sec_offset = readU32BE(data, &offset) catch return VxObjError.TruncatedData;
            const sec_size = readU32BE(data, &offset) catch return VxObjError.TruncatedData;

            const end = sec_offset + sec_size;
            if (end > data.len) return VxObjError.TruncatedData;

            const sec_data = try allocator.alloc(u8, sec_size);
            @memcpy(sec_data, data[sec_offset..end]);

            section.* = .{
                .name = name,
                .offset = sec_offset,
                .size = sec_size,
            };

            try section_data.put(name, sec_data);
        }

        return VxObjV4Container{
            .header = .{
                .version = version,
                .flags = flags,
                .target_triple = target_triple,
                .sections = sections,
            },
            .section_data = section_data,
        };
    }

    pub fn deinit(self: *VxObjV4Container, allocator: std.mem.Allocator) void {
        allocator.free(self.header.target_triple);
        for (self.header.sections) |section| {
            allocator.free(section.name);
        }
        allocator.free(self.header.sections);
        var iter = self.section_data.iterator();
        while (iter.next()) |entry| {
            allocator.free(entry.value_ptr.*);
        }
        self.section_data.deinit();
    }

    pub fn getSection(self: *VxObjV4Container, name: []const u8) ?[]const u8 {
        return self.section_data.get(name);
    }

    pub fn hasExternalDeps(self: *VxObjV4Container) bool {
        return (self.header.flags & 1) != 0;
    }
};

pub const ExternalDependency = struct {
    name: []const u8,
    path: ?[]const u8,
    is_optional: bool,

    pub fn fromBytes(allocator: std.mem.Allocator, data: []const u8) !ExternalDependency {
        var pos: usize = 0;

        const name_end = std.mem.indexOfScalar(u8, data[pos..], 0) orelse return VxObjError.InvalidData;
        const name = try allocator.dupe(u8, data[pos .. pos + name_end]);
        pos += name_end + 1;

        if (pos >= data.len) {
            return ExternalDependency{
                .name = name,
                .path = null,
                .is_optional = false,
            };
        }

        const path_end = std.mem.indexOfScalar(u8, data[pos..], 0) orelse return VxObjError.InvalidData;
        const path = if (path_end > 0) try allocator.dupe(u8, data[pos .. pos + path_end]) else null;
        pos += path_end + 1;

        const is_optional = if (pos < data.len) data[pos] == '1' else false;

        return ExternalDependency{
            .name = name,
            .path = path,
            .is_optional = is_optional,
        };
    }

    pub fn deinit(self: *ExternalDependency, allocator: std.mem.Allocator) void {
        allocator.free(self.name);
        if (self.path) |p| allocator.free(p);
    }
};

pub fn deserializeExternalDeps(allocator: std.mem.Allocator, data: []const u8) ![]ExternalDependency {
    var deps: std.ArrayList(ExternalDependency) = .empty;
    errdefer {
        for (deps.items) |*dep| dep.deinit(allocator);
        deps.deinit(allocator);
    }

    var pos: usize = 0;
    while (pos < data.len) {
        const name_end = std.mem.indexOfScalar(u8, data[pos..], 0) orelse break;
        if (name_end == 0) break;

        const entry_end = findEntryEnd(data, pos);
        if (entry_end == pos) break;

        const dep = try ExternalDependency.fromBytes(allocator, data[pos..entry_end]);
        try deps.append(allocator, dep);

        pos = entry_end;
    }

    return deps.toOwnedSlice(allocator);
}

fn findEntryEnd(data: []const u8, start: usize) usize {
    var pos = start;
    var null_count: usize = 0;
    while (pos < data.len) {
        if (data[pos] == 0) {
            null_count += 1;
            if (null_count >= 3) break;
        }
        pos += 1;
    }
    return if (pos < data.len) pos + 1 else pos;
}

fn readU32BE(data: []const u8, offset: *usize) !u32 {
    if (offset.* + 4 > data.len) return error.TruncatedData;
    const result = std.mem.readInt(u32, data[offset.* .. offset.* + 4][0..4], .big);
    offset.* += 4;
    return result;
}

fn readString(allocator: std.mem.Allocator, data: []const u8, offset: *usize) ![]const u8 {
    const len = try readU32BE(data, offset);
    if (offset.* + len > data.len) return error.TruncatedData;
    const str = try allocator.dupe(u8, data[offset.* .. offset.* + len]);
    offset.* += len;
    return str;
}

test "parse vxobj" {
    const allocator = std.testing.allocator;

    var buffer: [1024]u8 = undefined;
    @memset(&buffer, 0);
    var pos: usize = 0;

    const writeU32BE = struct {
        fn func(buf: []u8, p: *usize, value: u32) void {
            std.mem.writeInt(u32, buf[p.*..][0..4], value, .big);
            p.* += 4;
        }
    }.func;

    const writeBytes = struct {
        fn func(buf: []u8, p: *usize, bytes: []const u8) void {
            @memcpy(buf[p.* .. p.* + bytes.len], bytes);
            p.* += bytes.len;
        }
    }.func;

    writeBytes(&buffer, &pos, MAGIC);
    writeU32BE(&buffer, &pos, VERSION_V4);
    writeU32BE(&buffer, &pos, 0);

    const target_triple = "x86_64-unknown-linux-gnu";
    writeU32BE(&buffer, &pos, @intCast(target_triple.len));
    writeBytes(&buffer, &pos, target_triple);

    writeU32BE(&buffer, &pos, 1);

    const section_name = SECTION_TYPE_IR;
    writeU32BE(&buffer, &pos, @intCast(section_name.len));
    writeBytes(&buffer, &pos, section_name);

    const section_data = "test data";
    const section_offset: u32 = @intCast(pos + 8);
    writeU32BE(&buffer, &pos, section_offset);
    writeU32BE(&buffer, &pos, @intCast(section_data.len));

    writeBytes(&buffer, &pos, section_data);

    var container = try VxObjV4Container.parse(allocator, buffer[0..pos]);
    defer container.deinit(allocator);

    try std.testing.expectEqualStrings(target_triple, container.header.target_triple);
    try std.testing.expectEqual(@as(u32, 1), container.header.sections.len);
    try std.testing.expectEqualStrings(section_name, container.header.sections[0].name);
    try std.testing.expectEqualStrings(section_data, container.getSection(SECTION_TYPE_IR).?);
}
