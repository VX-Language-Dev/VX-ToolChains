const std = @import("std");
const vx_vm = @import("lib.zig");

const Lexer = vx_vm.token.Lexer;
const Parser = vx_vm.parser.Parser;
const OwnershipChecker = vx_vm.compiler_ownership.OwnershipChecker;
const Compiler = vx_vm.Compiler;
const VxSettings = vx_vm.VxSettings;
const VxObjV4Container = vx_vm.bytecode.VxObjV4Container;

pub fn main(init: std.process.Init) !void {
    const allocator = init.gpa;
    const io = init.io;

    // 收集参数为 slice
    var arg_iter = std.process.Args.Iterator.init(init.minimal.args);
    var args: std.ArrayList([]const u8) = .empty;
    while (arg_iter.next()) |arg| {
        try args.append(allocator, arg);
    }

    const prog = if (args.items.len > 0) args.items[0] else "vxc";

    if (args.items.len < 2) {
        printUsage(prog);
        std.process.exit(1);
    }

    const input = args.items[1];
    var output: []const u8 = "";
    var target_triple: []const u8 = "";
    var opt_level: u8 = 1;
    var warn_dead_code = false;
    var error_dead_code = false;

    var i: usize = 2;
    while (i < args.items.len) : (i += 1) {
        if (std.mem.eql(u8, args.items[i], "-o")) {
            i += 1;
            if (i >= args.items.len) {
                std.debug.print("Missing output file for -o\n", .{});
                std.process.exit(1);
            }
            output = args.items[i];
        } else if (std.mem.eql(u8, args.items[i], "--target")) {
            i += 1;
            if (i >= args.items.len) {
                std.debug.print("Missing target triple for --target\n", .{});
                std.process.exit(1);
            }
            target_triple = args.items[i];
        } else if (std.mem.eql(u8, args.items[i], "--opt-level")) {
            i += 1;
            if (i >= args.items.len) {
                std.debug.print("Missing value for --opt-level\n", .{});
                std.process.exit(1);
            }
            const parsed = std.fmt.parseInt(u8, args.items[i], 10) catch 1;
            opt_level = if (parsed >= 1 and parsed <= 10) parsed else 1;
        } else if (std.mem.eql(u8, args.items[i], "--warn-dead-code")) {
            warn_dead_code = true;
        } else if (std.mem.eql(u8, args.items[i], "--error-dead-code")) {
            error_dead_code = true;
        } else {
            std.debug.print("Unknown argument: {s}\n", .{args.items[i]});
            std.process.exit(1);
        }
    }

    // 默认输出路径
    if (output.len == 0) {
        if (std.mem.lastIndexOfScalar(u8, input, '.')) |dot| {
            output = try std.mem.concat(allocator, u8, &[_][]const u8{ input[0..dot], ".vxobj" });
        } else {
            output = try std.mem.concat(allocator, u8, &[_][]const u8{ input, ".vxobj" });
        }
    }

    // 读取源文件
    const cwd = std.Io.Dir.cwd();
    const source = cwd.readFileAlloc(io, input, allocator, .unlimited) catch |err| {
        std.debug.print("Read failed: {}\n", .{err});
        std.process.exit(1);
    };

    // 1) 词法分析
    var lexer = Lexer.init(source, allocator);
    var tokens = lexer.tokenize() catch |err| {
        std.debug.print("Lexical analysis failed: {}\n", .{err});
        std.process.exit(1);
    };
    defer tokens.deinit(allocator);

    // 2) 语法分析
    var parser = Parser.init(tokens, source, allocator);
    var ast = parser.parse() catch |err| {
        if (parser.last_error) |e| {
            std.debug.print("{s}\n", .{e.msg});
        } else {
            std.debug.print("Parse failed: {}\n", .{err});
        }
        std.process.exit(1);
    };
    defer ast.deinit(allocator);

    // 3) 所有权检查
    var checker = OwnershipChecker.init(allocator, source);
    defer checker.deinit();
    // ast.items 是 []*Element, checkAst 需要 []*const Expr, 用 slice 重新解释
    const const_items: []*const vx_vm.Expr = @ptrCast(ast.items);
    checker.checkAst(const_items);

    for (checker.warnings.items) |warn| {
        std.debug.print("[warning] {s}\n", .{warn});
    }
    if (checker.errors.items.len > 0) {
        for (checker.errors.items) |err| {
            std.debug.print("[error] {s}\n", .{err});
        }
        std.debug.print("编译失败: 所有权检查发现 {} 个错误\n", .{checker.errors.items.len});
        std.process.exit(1);
    }

    // 4) 编译
    var settings = VxSettings.init(allocator, "");
    defer settings.deinit();
    var compiler = Compiler.init(allocator, settings);
    defer compiler.deinit();
    _ = compiler.withOptions(opt_level, warn_dead_code, error_dead_code);

    // 展开宏（由 Compiler.expandMacros 处理）
    var mono_ast: std.ArrayList(*const vx_vm.Expr) = .empty;
    for (ast.items) |e| {
        mono_ast.append(allocator, @as(*const vx_vm.Expr, @ptrCast(e))) catch @panic("OOM");
    }

    const compiled = vx_vm.compiler_module.compile(&compiler, mono_ast) catch |err| {
        std.debug.print("Compilation failed: {}\n", .{err});
        std.process.exit(1);
    };
    // compiled.module 中的 type_ir_data 等字段由编译核心管理

    // 4.5) 字节码优化
    const opt_stats = compiler.runOptimization();
    if (opt_stats.total() > 0) {
        std.debug.print("[Optimize] level={s}({}) folds={} dead={} prop={} branch={} unreachable={} passes={}\n", .{
            compiler.optGroup(),
            opt_level,
            opt_stats.folds,
            opt_stats.dead,
            opt_stats.prop,
            opt_stats.branch,
            opt_stats.unreach,
            opt_stats.passes,
        });
    }

    // 5) 序列化为 VXOBJ v4
    var container = VxObjV4Container.init(allocator, target_triple);
    defer container.deinit();
    try container.setSection(vx_vm.bytecode.SECTION_TYPE_IR, compiled.type_ir_data);

    // 标记外部依赖
    if (compiled.external_deps.items.len > 0) {
        container.setExternalDepsFlag(true);
    }

    // 6) 写入输出文件
    var buf: std.Io.Writer.Allocating = .init(allocator);
    try container.write(&buf.writer);
    const out_data = try buf.toOwnedSlice();
    defer allocator.free(out_data);

    cwd.writeFile(io, .{ .sub_path = output, .data = out_data }) catch |err| {
        std.debug.print("Write failed: {}\n", .{err});
        std.process.exit(1);
    };
}

fn printUsage(prog: []const u8) void {
    std.debug.print("VX Compiler v1.6 (Zig) - VX Source -> VXOBJ v4\n", .{});
    std.debug.print("Usage: {s} <input.vx> [-o output.vxobj] [--target triple]\n", .{prog});
    std.debug.print("Options:\n", .{});
    std.debug.print("  -o <path>         Output path (default: input with .vxobj)\n", .{});
    std.debug.print("  --target <triple> Target triple for cross-compilation\n", .{});
    std.debug.print("  --opt-level <n>   Optimization level: 1=none, 2-4=Debug, 5-7=Release, 8-10=Super\n", .{});
    std.debug.print("  --warn-dead-code  Warn on dead code\n", .{});
    std.debug.print("  --error-dead-code Error on dead code\n", .{});
}
