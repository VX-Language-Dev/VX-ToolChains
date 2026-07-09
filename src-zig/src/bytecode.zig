const std = @import("std");
const Allocator = std.mem.Allocator;

// ==================== VXOBJ v4 Format ====================
//
// VXOBJ v4: 跨平台中间文件格式，不包含任何可执行文件特征。
// 编译器输出 VXOBJ v4，链接器解析后生成目标平台的原生可执行文件。
//
// 格式结构:
//   [Header]
//     5 bytes magic: "VXOBJ"
//     4 bytes version (u32 BE): 4
//     4 bytes flags (u32 BE): bit 0 = has_external_deps
//     4 bytes target_triple_len (u32 BE)
//     N bytes target_triple (UTF-8)
//   [Section Index Table]
//     4 bytes count (u32 BE)
//     For each section:
//       4 bytes name_len (u32 BE) + name bytes
//       4 bytes offset (u32 BE, from file start)
//       4 bytes size (u32 BE)
//   [Sections] (TypeIR, DebugInfo, SourceMap, ExternalDeps)

pub const MAGIC = "VXOBJ";
pub const VERSION_V4: u32 = 4;

// VXOBJ v4 Section names
pub const SECTION_TYPE_IR = "TypeIR";
pub const SECTION_DEBUG = "Debug";
pub const SECTION_SOURCE_MAP = "SourceMap";
pub const SECTION_EXTERNAL_DEPS = "ExternalDeps";

pub const VxObjV4SectionIndex = struct {
    name: []const u8,
    offset: u32,
    size: u32,
};

pub const VxObjV4Header = struct {
    version: u32,
    flags: u32,
    target_triple: []const u8,
    sections: std.ArrayList(VxObjV4SectionIndex),
};

pub const VxObjV4Container = struct {
    header: VxObjV4Header,
    section_data: std.StringHashMap([]u8),
    allocator: Allocator,

    pub fn init(allocator: Allocator, target_triple: []const u8) VxObjV4Container {
        return VxObjV4Container{
            .header = VxObjV4Header{
                .version = VERSION_V4,
                .flags = 0,
                .target_triple = allocator.dupe(u8, target_triple) catch @panic("OOM"),
                .sections = .empty,
            },
            .section_data = .empty,
            .allocator = allocator,
        };
    }

    pub fn deinit(self: *VxObjV4Container) void {
        // Free section data values (owned bytes)
        var data_iter = self.section_data.valueIterator();
        while (data_iter.next()) |val| {
            self.allocator.free(val.*);
        }
        // Deinit the HashMap; keys are owned by section entries and freed below
        self.section_data.deinit();

        // Free section names (these are also the HashMap keys)
        for (self.header.sections.items) |sec| {
            self.allocator.free(sec.name);
        }
        self.header.sections.deinit(self.allocator);

        self.allocator.free(self.header.target_triple);
    }

    pub fn setSection(self: *VxObjV4Container, name: []const u8, data: []const u8) !void {
        const owned_name = try self.allocator.dupe(u8, name);
        errdefer self.allocator.free(owned_name);
        const owned_data = try self.allocator.dupe(u8, data);
        errdefer self.allocator.free(owned_data);

        // Replace old entry if it exists (frees both old key and value)
        if (self.section_data.fetchPut(owned_name, owned_data)) |kv| {
            self.allocator.free(kv.key);
            self.allocator.free(kv.value);
        }

        try self.header.sections.append(self.allocator, VxObjV4SectionIndex{
            .name = owned_name,
            .offset = 0,
            .size = @as(u32, @intCast(data.len)),
        });
    }

    pub fn getSection(self: *const VxObjV4Container, name: []const u8) ?[]const u8 {
        return self.section_data.get(name);
    }

    pub fn hasExternalDeps(self: *const VxObjV4Container) bool {
        return (self.header.flags & 1) != 0;
    }

    pub fn setExternalDepsFlag(self: *VxObjV4Container, has_deps: bool) void {
        if (has_deps) {
            self.header.flags |= 1;
        } else {
            self.header.flags &= ~@as(u32, 1);
        }
    }

    pub fn write(self: *const VxObjV4Container, writer: *std.Io.Writer) !void {
        // Collect sections in the order defined by header.sections
        var sections: std.ArrayList(struct { name: []const u8, data: []const u8 }) = .empty;
        defer sections.deinit(self.allocator);

        for (self.header.sections.items) |sec| {
            if (self.section_data.get(sec.name)) |data| {
                try sections.append(self.allocator, .{ .name = sec.name, .data = data });
            }
        }

        // Calculate sizes
        const base_header_size: u32 = 5 + 4 + 4 + 4 + @as(u32, @intCast(self.header.target_triple.len));
        var section_index_size: u32 = 4; // count field
        for (self.header.sections.items) |sec| {
            const name_len: u32 = @as(u32, @intCast(sec.name.len));
            section_index_size += 4 + name_len + 4 + 4;
        }
        var cur_off = base_header_size + section_index_size;

        // Write header
        try writer.writeAll(MAGIC);
        try writeU32Be(writer, VERSION_V4);
        try writeU32Be(writer, self.header.flags);
        try writeString(writer, self.header.target_triple);

        // Write section index
        try writeU32Be(writer, @as(u32, @intCast(self.header.sections.items.len)));
        for (sections.items) |*sec| {
            try writeString(writer, sec.name);
            try writeU32Be(writer, cur_off);
            try writeU32Be(writer, @as(u32, @intCast(sec.data.len)));
            cur_off += @as(u32, @intCast(sec.data.len));
        }

        // Write section data
        for (sections.items) |sec| {
            try writer.writeAll(sec.data);
        }
    }

    pub fn parse(allocator: Allocator, data: []const u8) !VxObjV4Container {
        var reader = std.Io.Reader.fixed(data);

        // Read magic
        const magic = try reader.takeArray(5);
        if (!std.mem.eql(u8, magic, MAGIC)) {
            return error.InvalidMagic;
        }

        const version = try readU32Be(&reader);
        if (version != VERSION_V4) {
            return error.UnsupportedVersion;
        }

        const flags = try readU32Be(&reader);
        const target_triple = try readString(&reader, allocator);
        errdefer allocator.free(target_triple);

        const num_sections = try readU32Be(&reader);
        var sections: std.ArrayList(VxObjV4SectionIndex) = .empty;
        errdefer {
            for (sections.items) |s| allocator.free(s.name);
            sections.deinit(allocator);
        }

        var section_data = std.StringHashMap([]u8).init(allocator);
        errdefer {
            var iter = section_data.valueIterator();
            while (iter.next()) |val| allocator.free(val.*);
            section_data.deinit();
        }

        var i: u32 = 0;
        while (i < num_sections) : (i += 1) {
            const name = readString(&reader, allocator) catch |err| {
                // No per-iteration allocations yet; outer errdefers handle cleanup
                return err;
            };
            const offset = readU32Be(&reader) catch |err| {
                allocator.free(name);
                return err;
            };
            const size = readU32Be(&reader) catch |err| {
                allocator.free(name);
                return err;
            };

            const end = offset + size;
            if (end > data.len) {
                allocator.free(name);
                return error.SectionDataTruncated;
            }

            const section_slice = allocator.dupe(u8, data[offset..end]) catch @panic("OOM");
            section_data.put(name, section_slice) catch @panic("OOM");
            sections.append(allocator, VxObjV4SectionIndex{
                .name = name,
                .offset = offset,
                .size = size,
            }) catch @panic("OOM");
        }

        return VxObjV4Container{
            .header = VxObjV4Header{
                .version = version,
                .flags = flags,
                .target_triple = target_triple,
                .sections = sections,
            },
            .section_data = section_data,
            .allocator = allocator,
        };
    }
};

// ==================== External Dependencies ====================
//
// 简单格式: null-separated list of entries
// 每个 entry 格式: "name\0path\0is_optional\0"
// - name: 库名称
// - path: 库路径（可选，为空表示系统库）
// - is_optional: "1" 表示可选，"0" 表示必需

pub const ExternalDependency = struct {
    name: []const u8,
    path: ?[]const u8,
    is_optional: bool,

    pub fn deinit(self: *ExternalDependency, allocator: Allocator) void {
        allocator.free(self.name);
        if (self.path) |p| allocator.free(p);
    }

    pub fn init(allocator: Allocator, name: []const u8) ExternalDependency {
        return ExternalDependency{
            .name = allocator.dupe(u8, name) catch @panic("OOM"),
            .path = null,
            .is_optional = false,
        };
    }

    pub fn withPath(self: *ExternalDependency, allocator: Allocator, path: []const u8) void {
        if (self.path) |p| allocator.free(p);
        self.path = allocator.dupe(u8, path) catch @panic("OOM");
    }

    pub fn setOptional(self: *ExternalDependency, optional: bool) void {
        self.is_optional = optional;
    }

    pub fn toBytes(self: *const ExternalDependency, allocator: Allocator) []u8 {
        var result: std.ArrayList(u8) = .empty;
        result.appendSlice(allocator, self.name) catch @panic("OOM");
        result.append(allocator, 0) catch @panic("OOM");
        if (self.path) |p| {
            result.appendSlice(allocator, p) catch @panic("OOM");
        }
        result.append(allocator, 0) catch @panic("OOM");
        result.append(allocator, if (self.is_optional) @as(u8, '1') else '0') catch @panic("OOM");
        result.append(allocator, 0) catch @panic("OOM");
        return result.toOwnedSlice(allocator) catch @panic("OOM");
    }

    pub fn fromBytes(data: []const u8, allocator: Allocator) ?ExternalDependency {
        var parts: std.ArrayList([]const u8) = .empty;
        defer parts.deinit(allocator);

        var iter = std.mem.splitScalar(u8, data, @as(u8, 0));
        while (iter.next()) |part| {
            parts.append(allocator, part) catch @panic("OOM");
        }

        if (parts.items.len == 0) return null;

        const name = allocator.dupe(u8, parts.items[0]) catch @panic("OOM");
        const path = if (parts.items.len > 1 and parts.items[1].len > 0)
            allocator.dupe(u8, parts.items[1]) catch @panic("OOM")
        else
            null;
        const is_optional = parts.items.len > 2 and parts.items[2].len > 0 and parts.items[2][0] == '1';

        return ExternalDependency{ .name = name, .path = path, .is_optional = is_optional };
    }
};

pub fn serializeExternalDeps(deps: []const ExternalDependency, allocator: Allocator) []u8 {
    var result: std.ArrayList(u8) = .empty;
    for (deps) |*dep| {
        const bytes = dep.toBytes(allocator);
        result.appendSlice(allocator, bytes) catch @panic("OOM");
        allocator.free(bytes);
    }
    return result.toOwnedSlice(allocator) catch @panic("OOM");
}

pub fn deserializeExternalDeps(data: []const u8, allocator: Allocator) std.ArrayList(ExternalDependency) {
    var deps: std.ArrayList(ExternalDependency) = .empty;
    var pos: usize = 0;

    while (pos < data.len) {
        // name until null
        const name_end = std.mem.indexOfScalar(u8, data[pos..], 0) orelse break;
        const name = data[pos .. pos + name_end];
        pos += name_end + 1;
        if (pos >= data.len) break;

        // path until null
        const path_end = std.mem.indexOfScalar(u8, data[pos..], 0) orelse break;
        const path = data[pos .. pos + path_end];
        pos += path_end + 1;
        if (pos >= data.len) break;

        const is_optional = data[pos] == '1';
        pos += 1;

        deps.append(allocator, ExternalDependency{
            .name = allocator.dupe(u8, name) catch @panic("OOM"),
            .path = if (path.len > 0) allocator.dupe(u8, path) catch @panic("OOM") else null,
            .is_optional = is_optional,
        }) catch @panic("OOM");
    }

    return deps;
}

// ==================== VXOBJ v4 Writer ====================

pub fn writeVxobjV4(
    allocator: Allocator,
    writer: *std.Io.Writer,
    target_triple: []const u8,
    type_ir_data: []const u8,
    debug_data: []const u8,
    source_map_data: []const u8,
    external_deps: []const ExternalDependency,
) !void {
    var container = VxObjV4Container.init(allocator, target_triple);
    defer container.deinit();

    if (type_ir_data.len > 0) {
        try container.setSection(SECTION_TYPE_IR, type_ir_data);
    }
    if (debug_data.len > 0) {
        try container.setSection(SECTION_DEBUG, debug_data);
    }
    if (source_map_data.len > 0) {
        try container.setSection(SECTION_SOURCE_MAP, source_map_data);
    }
    if (external_deps.len > 0) {
        const deps_data = serializeExternalDeps(external_deps, allocator);
        defer allocator.free(deps_data);
        try container.setSection(SECTION_EXTERNAL_DEPS, deps_data);
        container.setExternalDepsFlag(true);
    }

    try container.write(writer);
}

// ==================== Section Size Stats ====================

pub fn dumpSectionStats(data: []const u8) void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    const allocator = gpa.allocator();

    var container = VxObjV4Container.parse(allocator, data) catch |err| {
        std.debug.print("Parse error: {}\n", .{err});
        return;
    };
    defer container.deinit();

    std.debug.print("VXOBJ v4 container:\n", .{});
    std.debug.print("  Target: {s}\n", .{container.header.target_triple});
    std.debug.print("  Sections:\n", .{});
    for (container.header.sections.items) |sec| {
        std.debug.print("    {s:12} {} bytes\n", .{ sec.name, sec.size });
    }
}

// ==================== Low-Level I/O ====================

fn writeU32Be(writer: *std.Io.Writer, v: u32) !void {
    var buf: [4]u8 = undefined;
    std.mem.writeInt(u32, &buf, v, .big);
    try writer.writeAll(&buf);
}

fn writeString(writer: *std.Io.Writer, s: []const u8) !void {
    try writeU32Be(writer, @as(u32, @intCast(s.len)));
    try writer.writeAll(s);
}

fn readU32Be(reader: *std.Io.Reader) !u32 {
    const bytes = try reader.takeArray(4);
    return std.mem.readInt(u32, bytes, .big);
}

fn readString(reader: *std.Io.Reader, allocator: Allocator) ![]const u8 {
    const len = try readU32Be(reader);
    const buf = try allocator.alloc(u8, len);
    errdefer allocator.free(buf);
    const bytes = try reader.take(len);
    @memcpy(buf, bytes);
    return buf;
}
