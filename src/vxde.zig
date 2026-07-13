const std = @import("std");
const vx_vm = @import("lib.zig");

const VxObjV4Container = vx_vm.bytecode.VxObjV4Container;
const TypeModule = vx_vm.type_ir.TypeModule;
const TypeFunction = vx_vm.type_ir.TypeFunction;
const TypedInstruction = vx_vm.type_ir.TypedInstruction;
const Type = vx_vm.type_ir.Type;
const VarId = vx_vm.type_ir.VarId;
const FuncId = vx_vm.type_ir.FuncId;
const Linkage = vx_vm.type_ir.Linkage;
const ExternalDependency = vx_vm.bytecode.ExternalDependency;
const deserializeExternalDeps = vx_vm.bytecode.deserializeExternalDeps;
const deserializeTypeModule = vx_vm.type_ir.deserializeTypeModule;

pub fn main(init: std.process.Init) !void {
    const allocator = init.gpa;
    const io = init.io;

    // 收集参数
    var arg_iter = std.process.Args.Iterator.init(init.minimal.args);
    var args: std.ArrayList([]const u8) = .empty;
    while (arg_iter.next()) |arg| {
        try args.append(allocator, arg);
    }

    const prog = if (args.items.len > 0) args.items[0] else "vxde";

    if (args.items.len < 2) {
        printUsage(prog);
        std.process.exit(1);
    }

    var input: []const u8 = "";
    var output: ?[]const u8 = null;
    var info_only = false;
    var dump_ir = false;

    var i: usize = 1;
    while (i < args.items.len) : (i += 1) {
        if (std.mem.eql(u8, args.items[i], "-o")) {
            i += 1;
            if (i >= args.items.len) {
                std.debug.print("[Error] Missing argument for -o\n", .{});
                std.process.exit(1);
            }
            output = args.items[i];
        } else if (std.mem.eql(u8, args.items[i], "--info")) {
            info_only = true;
        } else if (std.mem.eql(u8, args.items[i], "--ir")) {
            dump_ir = true;
        } else if (std.mem.eql(u8, args.items[i], "--help") or std.mem.eql(u8, args.items[i], "-h")) {
            printUsage(prog);
            std.process.exit(0);
        } else if (std.mem.startsWith(u8, args.items[i], "-")) {
            std.debug.print("[Error] Unknown argument: {s}\n", .{args.items[i]});
            std.process.exit(1);
        } else {
            input = args.items[i];
        }
    }

    if (input.len == 0) {
        std.debug.print("[Error] No input file specified\n", .{});
        std.process.exit(1);
    }

    // 读取 .vxobj 文件
    const cwd = std.Io.Dir.cwd();
    const file_data = cwd.readFileAlloc(io, input, allocator, .unlimited) catch |err| {
        std.debug.print("[Error] Failed to read file '{s}': {}\n", .{ input, err });
        std.process.exit(1);
    };

    // 解析 VXOBJ v4 容器
    var container = VxObjV4Container.parse(allocator, file_data) catch |err| {
        std.debug.print("[Error] Failed to parse VXOBJ: {}\n", .{err});
        std.process.exit(1);
    };
    defer container.deinit();

    // --info 模式: 仅打印容器信息
    if (info_only) {
        var stderr_buf: [64]u8 = undefined;
        const stderr = std.debug.lockStderr(&stderr_buf);
        defer std.debug.unlockStderr();
        try printContainerInfo(&container, &stderr.file_writer.interface, allocator);
        return;
    }

    // 提取 TypeIR 数据
    const type_ir_data = container.getSection("TypeIR") orelse {
        std.debug.print("[Error] No TypeIR section found in VXOBJ\n", .{});
        std.process.exit(1);
    };

    // 反序列化 TypeIR 为 TypeModule
    var module = deserializeTypeModule(type_ir_data, allocator) catch |err| {
        std.debug.print("[Error] Failed to deserialize TypeIR: {}\n", .{err});
        std.process.exit(1);
    };
    defer module.deinit();

    // --ir 模式: dump TypeIR 结构信息
    if (dump_ir) {
        var stderr_buf: [64]u8 = undefined;
        const stderr = std.debug.lockStderr(&stderr_buf);
        defer std.debug.unlockStderr();
        try dumpTypeIR(&module, &stderr.file_writer.interface);
        return;
    }

    // 反编译为 VX 源代码
    var output_alloc: std.Io.Writer.Allocating = .init(allocator);

    var decompiler = Decompiler{
        .module = &module,
        .output = &output_alloc.writer,
        .target_triple = container.header.target_triple,
    };

    decompiler.decompile() catch |err| {
        std.debug.print("[Error] Decompilation failed: {}\n", .{err});
        std.process.exit(1);
    };

    // 写入输出文件
    const out_path = if (output) |o|
        o
    else if (std.mem.lastIndexOfScalar(u8, input, '.')) |dot|
        try std.fmt.allocPrint(allocator, "{s}.decompiled.vx", .{input[0..dot]})
    else
        try std.fmt.allocPrint(allocator, "{s}.decompiled.vx", .{input});

    const out_data = output_alloc.written();
    cwd.writeFile(io, .{ .sub_path = out_path, .data = out_data }) catch |err| {
        std.debug.print("[Error] Failed to write '{s}': {}\n", .{ out_path, err });
        std.process.exit(1);
    };

    std.debug.print("Decompiled to {s}\n", .{out_path});
}

fn printUsage(prog: []const u8) void {
    std.debug.print("VX Decompiler v1.0 (Zig) - VXOBJ v4 TypeIR -> VX Source\n", .{});
    std.debug.print("Usage: {s} <input.vxobj> [options]\n", .{prog});
    std.debug.print("Options:\n", .{});
    std.debug.print("  -o <path>       Output VX source path (default: input.decompiled.vx)\n", .{});
    std.debug.print("  --info          Show VXOBJ container info only\n", .{});
    std.debug.print("  --ir            Dump TypeIR structure (for debugging)\n", .{});
    std.debug.print("  -h, --help      Show this help message\n", .{});
}

fn printContainerInfo(container: *const VxObjV4Container, writer: *std.Io.Writer, allocator: std.mem.Allocator) !void {
    try writer.print("VXOBJ v{} Container\n", .{container.header.version});
    try writer.print("  Target: {s}\n", .{container.header.target_triple});
    try writer.print("  Sections: {}\n", .{container.header.sections.items.len});
    for (container.header.sections.items) |sec| {
        const data = container.getSection(sec.name) orelse "";
        try writer.print("    - {s}: {} bytes\n", .{ sec.name, data.len });
    }
    if (container.hasExternalDeps()) {
        try writer.print("  External dependencies:\n", .{});
        const deps_data = container.getSection("ExternalDeps");
        if (deps_data) |data| {
            var deps = deserializeExternalDeps(data, allocator);
            defer {
                for (deps.items) |*dep| {
                    dep.deinit(allocator);
                }
                deps.deinit(allocator);
            }
            for (deps.items) |dep| {
                if (dep.path) |p| {
                    try writer.print("    - {s} ({s})\n", .{ dep.name, p });
                } else {
                    try writer.print("    - {s}\n", .{dep.name});
                }
            }
        }
    }
}

fn dumpTypeIR(module: *const TypeModule, writer: *std.Io.Writer) !void {
    try writer.print("=== TypeIR Module ===\n", .{});
    try writer.print("Functions: {}\n", .{module.functions.items.len});
    try writer.print("Struct layouts: {}\n", .{module.struct_layouts.items.len});
    try writer.print("Linkage entries: {}\n", .{module.linkage.count()});

    for (module.functions.items) |func| {
        const linkage_val = module.linkage.get(func.id);
        const is_ext = if (linkage_val) |l| switch (l) {
            .External => true,
            .Internal => false,
        } else false;
        try writer.print("\n--- func {s} (id={}, params={}, vars={}){s} ---\n", .{
            func.name,
            func.id,
            func.param_count,
            func.var_count,
            if (is_ext) " [external]" else "",
        });
        for (func.params.items, 0..) |param, pi| {
            try writer.print("  param[{}]: {s} : {s}\n", .{ pi, param.name, typeName(&param.param_type) });
        }
        if (func.has_return) {
            try writer.print("  return: {s}\n", .{typeName(&func.return_type)});
        }
        var lt_iter = func.local_types.iterator();
        while (lt_iter.next()) |entry| {
            try writer.print("  var[{}]: {s}\n", .{ entry.key_ptr.*, typeName(entry.value_ptr) });
        }
        for (func.body.items, 0..) |inst, ii| {
            try writer.print("  [{:0>4}] ", .{ii});
            try printInstruction(writer, &inst);
            try writer.print("\n", .{});
        }
    }

    for (module.struct_layouts.items, 0..) |sl, si| {
        try writer.print("\n--- struct {s} (layout {}) ---\n", .{ sl.name, si });
        for (sl.fields, 0..) |field, fi| {
            try writer.print("  field[{}]: {s} : {s}\n", .{ fi, field.name, typeName(&field.field_type) });
        }
    }
}

// ==================== 反编译器核心 ====================

const Decompiler = struct {
    module: *const TypeModule,
    output: *std.Io.Writer,
    target_triple: []const u8,

    fn decompile(self: *Decompiler) !void {
        const writer = self.output;

        // 文件头注释
        try writer.print("// Decompiled from VXOBJ v4 TypeIR\n", .{});
        try writer.print("// Target: {s}\n\n", .{self.target_triple});

        // 外部函数声明 (linkage == External)
        var has_externs = false;
        for (self.module.functions.items) |*func| {
            if (self.module.linkage.get(func.id)) |linkage| {
                switch (linkage) {
                    .External => |name| {
                        try self.writeFuncSignature(func, true);
                        try writer.print(" // extern: {s}\n", .{name});
                        has_externs = true;
                    },
                    .Internal => {},
                }
            }
        }
        if (has_externs) {
            try writer.print("\n", .{});
        }

        // 结构体定义
        for (self.module.struct_layouts.items) |sl| {
            try writer.print("struct {s} {{\n", .{sl.name});
            for (sl.fields) |field| {
                try writer.print("    {s}: {s},\n", .{ field.name, typeName(&field.field_type) });
            }
            try writer.print("}}\n\n", .{});
        }

        // 函数定义 (跳过外部函数)
        for (self.module.functions.items) |*func| {
            const is_ext = if (self.module.linkage.get(func.id)) |linkage|
                switch (linkage) {
                    .External => true,
                    .Internal => false,
                }
            else
                false;
            if (is_ext) continue;

            try self.decompileFunc(func);
            try writer.print("\n", .{});
        }
    }

    fn writeFuncSignature(self: *Decompiler, func: *const TypeFunction, is_extern: bool) !void {
        const writer = self.output;

        if (is_extern) {
            try writer.print("extern ", .{});
        }
        try writer.print("func {s}(", .{func.name});

        for (func.params.items, 0..) |param, pi| {
            if (pi > 0) try writer.print(", ", .{});
            try writer.print("{s}: {s}", .{ param.name, typeName(&param.param_type) });
        }

        try writer.print(")", .{});

        if (func.has_return) {
            try writer.print(": {s}", .{typeName(&func.return_type)});
        }
    }

    fn decompileFunc(self: *Decompiler, func: *const TypeFunction) !void {
        const writer = self.output;

        try self.writeFuncSignature(func, false);
        try writer.print(" {{\n", .{});

        // 局部变量声明 (跳过参数 slot)
        var printed_var_header = false;
        for (0..func.var_count) |vid| {
            if (vid < func.param_count) continue;
            if (func.local_types.get(@as(VarId, @intCast(vid)))) |ty| {
                if (!printed_var_header) {
                    try writer.print("    // local variables\n", .{});
                    printed_var_header = true;
                }
                try writer.print("    let v{}: {s};\n", .{ vid, typeName(&ty) });
            }
        }
        if (printed_var_header) {
            try writer.print("\n", .{});
        }

        // 指令反编译
        for (func.body.items, 0..) |inst, idx| {
            try writer.print("    ", .{});
            try self.decompileInstruction(&inst, idx);
            try writer.print("\n", .{});
        }

        try writer.print("}}\n", .{});
    }

    fn decompileInstruction(self: *Decompiler, inst: *const TypedInstruction, idx: usize) !void {
        const writer = self.output;

        switch (inst.*) {
            .ConstInt => |v| {
                try writer.print("v{} = {}", .{ idx, v });
            },
            .ConstFloat => |v| {
                try writer.print("v{} = {d}", .{ idx, v });
            },
            .ConstBool => |v| {
                try writer.print("v{} = {}", .{ idx, v });
            },
            .ConstString => |v| {
                try writer.print("v{} = \"{s}\"", .{ idx, v });
            },
            .ConstNil => {
                try writer.print("v{} = nil", .{idx});
            },
            .LoadVar => |v| {
                try writer.print("v{} = v{}", .{ idx, v });
            },
            .StoreVar => |v| {
                try writer.print("v{} = v{}", .{ v, idx });
            },
            .I32Add => |pair| try self.writeBinOp(idx, "+", pair.a, pair.b),
            .I32Sub => |pair| try self.writeBinOp(idx, "-", pair.a, pair.b),
            .I32Mul => |pair| try self.writeBinOp(idx, "*", pair.a, pair.b),
            .I32Div => |pair| try self.writeBinOp(idx, "/", pair.a, pair.b),
            .I32Mod => |pair| try self.writeBinOp(idx, "%", pair.a, pair.b),
            .F64Add => |pair| try self.writeBinOp(idx, "+", pair.a, pair.b),
            .F64Sub => |pair| try self.writeBinOp(idx, "-", pair.a, pair.b),
            .F64Mul => |pair| try self.writeBinOp(idx, "*", pair.a, pair.b),
            .F64Div => |pair| try self.writeBinOp(idx, "/", pair.a, pair.b),
            .I32Eq => |pair| try self.writeBinOp(idx, "==", pair.a, pair.b),
            .I32Ne => |pair| try self.writeBinOp(idx, "!=", pair.a, pair.b),
            .I32Lt => |pair| try self.writeBinOp(idx, "<", pair.a, pair.b),
            .I32Gt => |pair| try self.writeBinOp(idx, ">", pair.a, pair.b),
            .I32Le => |pair| try self.writeBinOp(idx, "<=", pair.a, pair.b),
            .I32Ge => |pair| try self.writeBinOp(idx, ">=", pair.a, pair.b),
            .F64Eq => |pair| try self.writeBinOp(idx, "==", pair.a, pair.b),
            .F64Ne => |pair| try self.writeBinOp(idx, "!=", pair.a, pair.b),
            .F64Lt => |pair| try self.writeBinOp(idx, "<", pair.a, pair.b),
            .F64Gt => |pair| try self.writeBinOp(idx, ">", pair.a, pair.b),
            .F64Le => |pair| try self.writeBinOp(idx, "<=", pair.a, pair.b),
            .F64Ge => |pair| try self.writeBinOp(idx, ">=", pair.a, pair.b),
            .I32Neg => |v| {
                try writer.print("v{} = -v{}", .{ idx, v });
            },
            .F64Neg => |v| {
                try writer.print("v{} = -v{}", .{ idx, v });
            },
            .BoolNot => |v| {
                try writer.print("v{} = not v{}", .{ idx, v });
            },
            .I32And => |pair| try self.writeBinOp(idx, "and", pair.a, pair.b),
            .I32Or => |pair| try self.writeBinOp(idx, "or", pair.a, pair.b),
            .Jump => |target| {
                try writer.print("goto [{}]", .{target});
            },
            .JumpIfFalse => |pair| {
                try writer.print("if not v{}: goto [{}]", .{ pair.cond, pair.target });
            },
            .JumpIfTrue => |pair| {
                try writer.print("if v{}: goto [{}]", .{ pair.cond, pair.target });
            },
            .Call => |c| {
                const callee = if (c.ext_name) |name| name else self.module.function_map.get(c.func_id) orelse "unknown";
                try writer.print("v{} = {s}(", .{ idx, callee });
                for (c.args, 0..) |arg, ai| {
                    if (ai > 0) try writer.print(", ", .{});
                    try writer.print("v{}", .{arg});
                }
                try writer.print(")", .{});
            },
            .CallIndirect => |c| {
                try writer.print("v{} = (*v{})(", .{ idx, c.ptr });
                for (c.args, 0..) |arg, ai| {
                    if (ai > 0) try writer.print(", ", .{});
                    try writer.print("v{}", .{arg});
                }
                try writer.print(")", .{});
            },
            .Return => |v| {
                if (v) |val| {
                    try writer.print("return v{}", .{val});
                } else {
                    try writer.print("return", .{});
                }
            },
            .MakeStruct => |m| {
                try writer.print("v{} = struct_s{}(", .{ idx, m.layout.id });
                for (m.args, 0..) |arg, ai| {
                    if (ai > 0) try writer.print(", ", .{});
                    try writer.print("v{}", .{arg});
                }
                try writer.print(")", .{});
            },
            .GetField => |gf| {
                try writer.print("v{} = v{}.field{}", .{ idx, gf.obj, gf.idx });
            },
            .SetField => |sf| {
                try writer.print("v{}.field{} = v{}", .{ sf.obj, sf.idx, sf.val });
            },
            .MakeArray => |ma| {
                try writer.print("v{} = [", .{idx});
                for (ma.args, 0..) |arg, ai| {
                    if (ai > 0) try writer.print(", ", .{});
                    try writer.print("v{}", .{arg});
                }
                try writer.print("]", .{});
            },
            .IndexGet => |ig| {
                try writer.print("v{} = v{}[v{}]", .{ idx, ig.arr, ig.idx });
            },
            .IndexSet => |is_| {
                try writer.print("v{}[v{}] = v{}", .{ is_.arr, is_.idx, is_.val });
            },
            .MakeMap => |pairs| {
                try writer.print("v{} = {{", .{idx});
                for (pairs, 0..) |pair, pi| {
                    if (pi > 0) try writer.print(", ", .{});
                    try writer.print("v{}: v{}", .{ pair.key, pair.value });
                }
                try writer.print("}}", .{});
            },
            .Alloc => |t| {
                try writer.print("v{} = alloc({s})", .{ idx, typeName(&t) });
            },
            .Free => |v| {
                try writer.print("free v{}", .{v});
            },
            .OwnershipMove => |v| {
                try writer.print("move v{}", .{v});
            },
            .Borrow => |v| {
                try writer.print("borrow v{}", .{v});
            },
            .Deref => |v| {
                try writer.print("v{} = *v{}", .{ idx, v });
            },
            .AliveCheck => |v| {
                try writer.print("alive_check v{}", .{v});
            },
            .Dup => {
                try writer.print("dup", .{});
            },
            .Pop => {
                try writer.print("pop", .{});
            },
        }
    }

    fn writeBinOp(self: *Decompiler, idx: usize, op: []const u8, a: VarId, b: VarId) !void {
        const writer = self.output;
        try writer.print("v{} = v{} {s} v{}", .{ idx, a, op, b });
    }
};

// ==================== 辅助函数 ====================

fn typeName(ty: *const Type) []const u8 {
    return switch (ty.*) {
        .Void => "void",
        .Int => "int",
        .Float => "float",
        .Bool => "bool",
        .String => "string",
        .Struct => |s| s.name,
        .Array => "array",
        .Map => "map",
        .Func => "func",
        .Pointer => "ptr",
        .Generic => |g| g.name,
        .Unknown => "unknown",
    };
}

fn printInstruction(writer: anytype, inst: *const TypedInstruction) !void {
    switch (inst.*) {
        .ConstInt => |v| try writer.print("ConstInt({})", .{v}),
        .ConstFloat => |v| try writer.print("ConstFloat({d})", .{v}),
        .ConstBool => |v| try writer.print("ConstBool({})", .{v}),
        .ConstString => |v| try writer.print("ConstString(\"{s}\")", .{v}),
        .ConstNil => try writer.print("ConstNil", .{}),
        .LoadVar => |v| try writer.print("LoadVar({})", .{v}),
        .StoreVar => |v| try writer.print("StoreVar({})", .{v}),
        .I32Add => |p| try writer.print("I32Add(v{}, v{})", .{ p.a, p.b }),
        .I32Sub => |p| try writer.print("I32Sub(v{}, v{})", .{ p.a, p.b }),
        .I32Mul => |p| try writer.print("I32Mul(v{}, v{})", .{ p.a, p.b }),
        .I32Div => |p| try writer.print("I32Div(v{}, v{})", .{ p.a, p.b }),
        .I32Mod => |p| try writer.print("I32Mod(v{}, v{})", .{ p.a, p.b }),
        .F64Add => |p| try writer.print("F64Add(v{}, v{})", .{ p.a, p.b }),
        .F64Sub => |p| try writer.print("F64Sub(v{}, v{})", .{ p.a, p.b }),
        .F64Mul => |p| try writer.print("F64Mul(v{}, v{})", .{ p.a, p.b }),
        .F64Div => |p| try writer.print("F64Div(v{}, v{})", .{ p.a, p.b }),
        .I32Eq => |p| try writer.print("I32Eq(v{}, v{})", .{ p.a, p.b }),
        .I32Ne => |p| try writer.print("I32Ne(v{}, v{})", .{ p.a, p.b }),
        .I32Lt => |p| try writer.print("I32Lt(v{}, v{})", .{ p.a, p.b }),
        .I32Gt => |p| try writer.print("I32Gt(v{}, v{})", .{ p.a, p.b }),
        .I32Le => |p| try writer.print("I32Le(v{}, v{})", .{ p.a, p.b }),
        .I32Ge => |p| try writer.print("I32Ge(v{}, v{})", .{ p.a, p.b }),
        .F64Eq => |p| try writer.print("F64Eq(v{}, v{})", .{ p.a, p.b }),
        .F64Ne => |p| try writer.print("F64Ne(v{}, v{})", .{ p.a, p.b }),
        .F64Lt => |p| try writer.print("F64Lt(v{}, v{})", .{ p.a, p.b }),
        .F64Gt => |p| try writer.print("F64Gt(v{}, v{})", .{ p.a, p.b }),
        .F64Le => |p| try writer.print("F64Le(v{}, v{})", .{ p.a, p.b }),
        .F64Ge => |p| try writer.print("F64Ge(v{}, v{})", .{ p.a, p.b }),
        .I32Neg => |v| try writer.print("I32Neg(v{})", .{v}),
        .F64Neg => |v| try writer.print("F64Neg(v{})", .{v}),
        .BoolNot => |v| try writer.print("BoolNot(v{})", .{v}),
        .I32And => |p| try writer.print("I32And(v{}, v{})", .{ p.a, p.b }),
        .I32Or => |p| try writer.print("I32Or(v{}, v{})", .{ p.a, p.b }),
        .Jump => |t| try writer.print("Jump({})", .{t}),
        .JumpIfFalse => |p| try writer.print("JumpIfFalse(v{}, {})", .{ p.cond, p.target }),
        .JumpIfTrue => |p| try writer.print("JumpIfTrue(v{}, {})", .{ p.cond, p.target }),
        .Call => |c| {
            try writer.print("Call(f{}", .{c.func_id});
            for (c.args) |arg| {
                try writer.print(", v{}", .{arg});
            }
            if (c.ext_name) |name| {
                try writer.print(" ;{s}", .{name});
            }
            try writer.print(")", .{});
        },
        .CallIndirect => |c| {
            try writer.print("CallIndirect(v{}", .{c.ptr});
            for (c.args) |arg| {
                try writer.print(", v{}", .{arg});
            }
            try writer.print(")", .{});
        },
        .Return => |v| {
            if (v) |val| {
                try writer.print("Return(v{})", .{val});
            } else {
                try writer.print("Return", .{});
            }
        },
        .MakeStruct => |m| {
            try writer.print("MakeStruct(s{}", .{m.layout.id});
            for (m.args) |arg| {
                try writer.print(", v{}", .{arg});
            }
            try writer.print(")", .{});
        },
        .GetField => |gf| try writer.print("GetField(v{}, {})", .{ gf.obj, gf.idx }),
        .SetField => |sf| try writer.print("SetField(v{}, {}, v{})", .{ sf.obj, sf.idx, sf.val }),
        .MakeArray => |ma| {
            try writer.print("MakeArray(v{}", .{ma.base});
            for (ma.args) |arg| {
                try writer.print(", v{}", .{arg});
            }
            try writer.print(")", .{});
        },
        .IndexGet => |ig| try writer.print("IndexGet(v{}, v{})", .{ ig.arr, ig.idx }),
        .IndexSet => |is_| try writer.print("IndexSet(v{}, v{}, v{})", .{ is_.arr, is_.idx, is_.val }),
        .MakeMap => |pairs| {
            try writer.print("MakeMap(", .{});
            for (pairs, 0..) |pair, pi| {
                if (pi > 0) try writer.print(", ", .{});
                try writer.print("v{}:v{}", .{ pair.key, pair.value });
            }
            try writer.print(")", .{});
        },
        .Alloc => |t| try writer.print("Alloc({s})", .{typeName(&t)}),
        .Free => |v| try writer.print("Free(v{})", .{v}),
        .OwnershipMove => |v| try writer.print("OwnershipMove(v{})", .{v}),
        .Borrow => |v| try writer.print("Borrow(v{})", .{v}),
        .Deref => |v| try writer.print("Deref(v{})", .{v}),
        .AliveCheck => |v| try writer.print("AliveCheck(v{})", .{v}),
        .Dup => try writer.print("Dup", .{}),
        .Pop => try writer.print("Pop", .{}),
    }
}
