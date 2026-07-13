const std = @import("std");
const Allocator = std.mem.Allocator;

pub const LogLevel = enum {
    trace,
    debug,
    info,
    warn,
    err,

    pub fn asString(self: LogLevel) []const u8 {
        return switch (self) {
            .trace => "TRACE",
            .debug => "DEBUG",
            .info => " INFO",
            .warn => " WARN",
            .err => "ERROR",
        };
    }

    pub fn colorCode(self: LogLevel) []const u8 {
        return switch (self) {
            .trace => "\x1b[90m",
            .debug => "\x1b[36m",
            .info => "\x1b[32m",
            .warn => "\x1b[33m",
            .err => "\x1b[31m",
        };
    }

    pub fn resetCode() []const u8 {
        return "\x1b[0m";
    }
};

pub const Logger = struct {
    allocator: Allocator,
    level: LogLevel,
    use_color: bool,
    writer: std.Io.Writer,

    const Self = @This();

    pub fn init(allocator: Allocator, writer: std.Io.Writer, level: LogLevel) Self {
        return Self{
            .allocator = allocator,
            .level = level,
            .use_color = true,
            .writer = writer,
        };
    }

    pub fn noColor(self: *Self) *Self {
        self.use_color = false;
        return self;
    }

    pub fn setLevel(self: *Self, level: LogLevel) void {
        self.level = level;
    }

    pub fn log(self: *const Self, comptime level: LogLevel, comptime format: []const u8, args: anytype) void {
        if (@intFromEnum(level) < @intFromEnum(self.level)) return;

        const prefix = if (self.use_color)
            level.colorCode()
        else
            "";

        const suffix = if (self.use_color)
            LogLevel.resetCode()
        else
            "";

        self.writer.print("{s}[{s}]{s} " ++ format ++ "\n", .{
            prefix,
            level.asString(),
            suffix,
        } ++ args) catch {};
    }

    pub fn trace(self: *const Self, comptime format: []const u8, args: anytype) void {
        self.log(.trace, format, args);
    }

    pub fn debug(self: *const Self, comptime format: []const u8, args: anytype) void {
        self.log(.debug, format, args);
    }

    pub fn info(self: *const Self, comptime format: []const u8, args: anytype) void {
        self.log(.info, format, args);
    }

    pub fn warn(self: *const Self, comptime format: []const u8, args: anytype) void {
        self.log(.warn, format, args);
    }

    pub fn err(self: *const Self, comptime format: []const u8, args: anytype) void {
        self.log(.err, format, args);
    }

    pub fn scope(self: *const Self, comptime scope_name: []const u8) ScopedLogger {
        return ScopedLogger{
            .logger = self,
            .scope_name = scope_name,
        };
    }
};

pub const ScopedLogger = struct {
    logger: *const Logger,
    scope_name: []const u8,

    const Self = @This();

    pub fn log(self: *const Self, comptime level: LogLevel, comptime format: []const u8, args: anytype) void {
        if (@intFromEnum(level) < @intFromEnum(self.logger.level)) return;

        const prefix = if (self.logger.use_color)
            level.colorCode()
        else
            "";

        const suffix = if (self.logger.use_color)
            LogLevel.resetCode()
        else
            "";

        self.logger.writer.print("{s}[{s}][{s}]{s} " ++ format ++ "\n", .{
            prefix,
            level.asString(),
            self.scope_name,
            suffix,
        } ++ args) catch {};
    }

    pub fn trace(self: *const Self, comptime format: []const u8, args: anytype) void {
        self.log(.trace, format, args);
    }

    pub fn debug(self: *const Self, comptime format: []const u8, args: anytype) void {
        self.log(.debug, format, args);
    }

    pub fn info(self: *const Self, comptime format: []const u8, args: anytype) void {
        self.log(.info, format, args);
    }

    pub fn warn(self: *const Self, comptime format: []const u8, args: anytype) void {
        self.log(.warn, format, args);
    }

    pub fn err(self: *const Self, comptime format: []const u8, args: anytype) void {
        self.log(.err, format, args);
    }
};

pub fn logLevelFromEnv() LogLevel {
    const level_str = std.process.getEnvVarOwned(std.testing.allocator, "VX_LOG_LEVEL") catch
        return .info;
    defer std.testing.allocator.free(level_str);

    if (std.mem.eql(u8, level_str, "trace")) return .trace;
    if (std.mem.eql(u8, level_str, "debug")) return .debug;
    if (std.mem.eql(u8, level_str, "info")) return .info;
    if (std.mem.eql(u8, level_str, "warn")) return .warn;
    if (std.mem.eql(u8, level_str, "error")) return .err;
    return .info;
}

test "log level asString" {
    try std.testing.expectEqualStrings("TRACE", LogLevel.trace.asString());
    try std.testing.expectEqualStrings("ERROR", LogLevel.err.asString());
}

test "log level ordering" {
    try std.testing.expect(@intFromEnum(LogLevel.trace) < @intFromEnum(LogLevel.debug));
    try std.testing.expect(@intFromEnum(LogLevel.debug) < @intFromEnum(LogLevel.info));
    try std.testing.expect(@intFromEnum(LogLevel.info) < @intFromEnum(LogLevel.warn));
    try std.testing.expect(@intFromEnum(LogLevel.warn) < @intFromEnum(LogLevel.err));
}
