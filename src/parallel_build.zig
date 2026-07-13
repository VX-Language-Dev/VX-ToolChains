const std = @import("std");
const Allocator = std.mem.Allocator;
const Logger = @import("logger.zig").Logger;

pub const BuildTask = struct {
    id: usize,
    source: []const u8,
    output: []const u8,
    opt_level: u8,
    status: enum { pending, running, success, failed },
    error_msg: ?[]const u8 = null,
    duration_us: u64 = 0,

    const Self = @This();

    pub fn init(id: usize, source: []const u8, output: []const u8, opt_level: u8) Self {
        return Self{
            .id = id,
            .source = source,
            .output = output,
            .opt_level = opt_level,
            .status = .pending,
        };
    }
};

pub const BuildResult = struct {
    total: usize,
    succeeded: usize,
    failed: usize,
    skipped: usize,
    total_duration_us: u64,

    pub fn summary(self: *const BuildResult) []const u8 {
        if (self.failed > 0) return "FAILED";
        if (self.succeeded == 0) return "SKIPPED";
        return "OK";
    }
};

pub const BuildScheduler = struct {
    allocator: Allocator,
    tasks: std.ArrayList(BuildTask),
    max_parallel: usize,
    logger: ?*const Logger,

    const Self = @This();

    pub fn init(allocator: Allocator, max_parallel: usize) Self {
        return Self{
            .allocator = allocator,
            .tasks = std.ArrayList(BuildTask).init(allocator),
            .max_parallel = max_parallel,
            .logger = null,
        };
    }

    pub fn deinit(self: *Self) void {
        self.tasks.deinit();
    }

    pub fn setLogger(self: *Self, logger: *const Logger) void {
        self.logger = logger;
    }

    pub fn addTask(self: *Self, source: []const u8, output: []const u8, opt_level: u8) !usize {
        const id = self.tasks.items.len;
        try self.tasks.append(BuildTask.init(id, source, output, opt_level));
        return id;
    }

    pub fn execute(self: *Self, compileFn: *const fn (*BuildTask) anyerror!void) BuildResult {
        var result = BuildResult{
            .total = self.tasks.items.len,
            .succeeded = 0,
            .failed = 0,
            .skipped = 0,
            .total_duration_us = 0,
        };

        if (self.tasks.items.len == 0) return result;

        if (self.logger) |log| {
            log.info("开始并行构建: {} 个任务, 最大并行度: {}", .{ self.tasks.items.len, self.max_parallel });
        }

        var running: usize = 0;
        var next_idx: usize = 0;
        var completed: usize = 0;

        while (completed < self.tasks.items.len) {
            while (running < self.max_parallel and next_idx < self.tasks.items.len) {
                self.tasks.items[next_idx].status = .running;
                running += 1;
                next_idx += 1;
            }

            var i: usize = 0;
            while (i < next_idx) : (i += 1) {
                const task = &self.tasks.items[i];
                if (task.status != .running) continue;

                const start = std.time.microTimestamp();
                compileFn(task) catch |err| {
                    task.status = .failed;
                    task.error_msg = @errorName(err);
                    if (self.logger) |log| {
                        log.err("任务 {} 失败: {s} -> {s} ({s})", .{ task.id, task.source, task.output, @errorName(err) });
                    }
                    continue;
                };
                task.status = .success;
                task.duration_us = @intCast(std.time.microTimestamp() - start);
                result.total_duration_us += task.duration_us;

                if (self.logger) |log| {
                    log.debug("任务 {} 完成: {s} ({}us)", .{ task.id, task.source, task.duration_us });
                }
            }

            running = 0;
            completed = next_idx;
            for (self.tasks.items[0..next_idx]) |task| {
                switch (task.status) {
                    .success => result.succeeded += 1,
                    .failed => result.failed += 1,
                    .pending => result.skipped += 1,
                    .running => {},
                }
            }
        }

        if (self.logger) |log| {
            log.info("构建完成: {} 成功, {} 失败, {} 跳过 (总耗时 {}us)", .{
                result.succeeded,
                result.failed,
                result.skipped,
                result.total_duration_us,
            });
        }

        return result;
    }

    pub fn executeSequential(self: *Self, compileFn: *const fn (*BuildTask) anyerror!void) BuildResult {
        var result = BuildResult{
            .total = self.tasks.items.len,
            .succeeded = 0,
            .failed = 0,
            .skipped = 0,
            .total_duration_us = 0,
        };

        for (self.tasks.items) |*task| {
            task.status = .running;
            const start = std.time.microTimestamp();

            compileFn(task) catch |err| {
                task.status = .failed;
                task.error_msg = @errorName(err);
                result.failed += 1;
                if (self.logger) |log| {
                    log.err("任务 {} 失败: {s} ({s})", .{ task.id, task.source, @errorName(err) });
                }
                continue;
            };

            task.status = .success;
            task.duration_us = @intCast(std.time.microTimestamp() - start);
            result.succeeded += 1;
            result.total_duration_us += task.duration_us;
        }

        return result;
    }
};

test "build scheduler add tasks" {
    var scheduler = BuildScheduler.init(std.testing.allocator, 4);
    defer scheduler.deinit();

    _ = try scheduler.addTask("a.vx", "a.vxobj", 1);
    _ = try scheduler.addTask("b.vx", "b.vxobj", 5);

    try std.testing.expectEqual(@as(usize, 2), scheduler.tasks.items.len);
    try std.testing.expectEqual(@as(u8, 1), scheduler.tasks.items[0].opt_level);
    try std.testing.expectEqual(@as(u8, 5), scheduler.tasks.items[1].opt_level);
}

test "build result summary" {
    var r1 = BuildResult{ .total = 3, .succeeded = 3, .failed = 0, .skipped = 0, .total_duration_us = 100 };
    try std.testing.expectEqualStrings("OK", r1.summary());

    var r2 = BuildResult{ .total = 3, .succeeded = 2, .failed = 1, .skipped = 0, .total_duration_us = 100 };
    try std.testing.expectEqualStrings("FAILED", r2.summary());

    var r3 = BuildResult{ .total = 3, .succeeded = 0, .failed = 0, .skipped = 3, .total_duration_us = 0 };
    try std.testing.expectEqualStrings("SKIPPED", r3.summary());
}

test "sequential execution" {
    var scheduler = BuildScheduler.init(std.testing.allocator, 1);
    defer scheduler.deinit();

    _ = try scheduler.addTask("a.vx", "a.vxobj", 1);

    const result = scheduler.executeSequential(struct {
        fn compile(task: *BuildTask) anyerror!void {
            task.status = .success;
        }
    }.compile);

    try std.testing.expectEqual(@as(usize, 1), result.succeeded);
    try std.testing.expectEqual(@as(usize, 0), result.failed);
}
