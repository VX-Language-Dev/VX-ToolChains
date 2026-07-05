// ==================== 多架构代码生成框架 ====================
// 根据目标架构选择合适的后端并生成机器码

const std = @import("std");
const typeir = @import("typeir");
const codebuf = @import("codebuf");
const codegen_x86_64 = @import("codegen_x86_64");
const codegen_aarch64 = @import("codegen_aarch64");
const codegen_arm32 = @import("codegen_arm32");
const codegen_riscv = @import("codegen_riscv");

pub const Architecture = enum {
    X86_64,
    ARM64,
    ARM32,
    RV32,
    RV64,
};

// ==================== 泛型函数代码生成器 ====================

fn FunctionCodegen(comptime Backend: type) type {
    return struct {
        const Self = @This();

        cb: *codebuf.CodeBuffer,
        func: *const typeir.TypeFunction,
        module: *const typeir.TypeModule,
        allocator: std.mem.Allocator,
        var_slots: std.AutoHashMap(u32, codebuf.VarSlot),
        next_stack_offset: i32,
        pc_offsets: std.ArrayList(usize),
        jump_patches: std.ArrayList(codebuf.JumpPatch),
        frame_size: i32,

        pub fn init(cb: *codebuf.CodeBuffer, func: *const typeir.TypeFunction, module: *const typeir.TypeModule, allocator: std.mem.Allocator) Self {
            return .{
                .cb = cb,
                .func = func,
                .module = module,
                .allocator = allocator,
                .var_slots = std.AutoHashMap(u32, codebuf.VarSlot).init(allocator),
                .next_stack_offset = -@as(i32, @intCast(Backend.word_size)),
                .pc_offsets = .empty,
                .jump_patches = .empty,
                .frame_size = 0,
            };
        }

        pub fn deinit(self: *Self) void {
            self.var_slots.deinit();
            self.pc_offsets.deinit(self.allocator);
            self.jump_patches.deinit(self.allocator);
        }

        fn allocSlot(self: *Self, vid: u32) !codebuf.VarSlot {
            const gop = try self.var_slots.getOrPut(vid);
            if (!gop.found_existing) {
                gop.value_ptr.* = .{ .offset = self.next_stack_offset };
                self.next_stack_offset -= @as(i32, @intCast(Backend.word_size));
            }
            return gop.value_ptr.*;
        }

        fn ensureSlot(self: *Self, vid: u32) !codebuf.VarSlot {
            if (self.var_slots.get(vid)) |slot| return slot;
            return self.allocSlot(vid);
        }

        fn compile(self: *Self) !void {
            const ws = Backend.word_size;
            const estimated_locals = @max(self.func.var_count, 8);
            self.frame_size = (@as(i32, @intCast(estimated_locals)) * @as(i32, @intCast(ws)) + 192);
            self.frame_size = (self.frame_size + 15) & ~@as(i32, 15);

            try Backend.emitPrologue(self.cb, self.frame_size);

            const param_count = @min(self.func.param_count, Backend.ARG_REGS.len);
            var i: u32 = 0;
            while (i < param_count) : (i += 1) {
                const slot = try self.allocSlot(i);
                try Backend.emitStoreToSlot(self.cb, slot.offset, Backend.ARG_REGS[i]);
            }

            try self.pc_offsets.resize(self.allocator, self.func.body.len);

            for (self.func.body, 0..) |inst, pc| {
                self.pc_offsets.items[pc] = self.cb.len();
                try self.compileInstruction(inst, @intCast(pc));
            }

            for (self.jump_patches.items) |patch| {
                const target_idx = patch.target_pc;
                if (target_idx >= self.pc_offsets.items.len) return codebuf.CodegenError.InvalidJumpTarget;
                const target_offset = self.pc_offsets.items[target_idx];
                const source_end = patch.source_offset + 4;
                const rel: i32 = @intCast(@as(i64, @intCast(target_offset)) - @as(i64, @intCast(source_end)));
                self.cb.writeU32LE(patch.source_offset, @bitCast(rel));
            }
        }

        fn compileInstruction(self: *Self, inst: typeir.Instruction, pc: u32) codebuf.CodegenError!void {
            const cb = self.cb;
            switch (inst) {
                .const_int => |v| {
                    try Backend.emitLoadImm64(cb, Backend.RESULT, @bitCast(v));
                    try self.push(Backend.RESULT);
                },
                .const_bool => |v| {
                    try Backend.emitLoadImm64(cb, Backend.RESULT, if (v) 1 else 0);
                    try self.push(Backend.RESULT);
                },
                .const_float, .const_string, .const_nil => {
                    try Backend.emitXorReg(cb, Backend.RESULT);
                    try self.push(Backend.RESULT);
                },
                .load_var => |vid| {
                    const slot = try self.ensureSlot(vid);
                    try Backend.emitLoadFromSlot(cb, Backend.RESULT, slot.offset);
                    try self.push(Backend.RESULT);
                },
                .store_var => |vid| {
                    try self.pop(Backend.RESULT);
                    const slot = try self.ensureSlot(vid);
                    try Backend.emitStoreToSlot(cb, slot.offset, Backend.RESULT);
                },
                .i32_add => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitAdd(cb, Backend.RESULT, Backend.SCRATCH);
                    try self.push(Backend.RESULT);
                },
                .i32_sub => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitSub(cb, Backend.RESULT, Backend.SCRATCH);
                    try self.push(Backend.RESULT);
                },
                .i32_mul => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitMul(cb, Backend.RESULT, Backend.SCRATCH);
                    try self.push(Backend.RESULT);
                },
                .i32_div => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitDiv(cb, Backend.RESULT, Backend.SCRATCH);
                    try self.push(Backend.RESULT);
                },
                .i32_mod => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitMod(cb, Backend.RESULT, Backend.SCRATCH, Backend.SCRATCH2);
                    try self.push(Backend.SCRATCH2);
                },
                .i32_eq => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitCmp(cb, Backend.RESULT, Backend.SCRATCH);
                    try Backend.emitSetCond(cb, Backend.RESULT, .eq);
                    try self.push(Backend.RESULT);
                },
                .i32_ne => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitCmp(cb, Backend.RESULT, Backend.SCRATCH);
                    try Backend.emitSetCond(cb, Backend.RESULT, .ne);
                    try self.push(Backend.RESULT);
                },
                .i32_lt => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitCmp(cb, Backend.RESULT, Backend.SCRATCH);
                    try Backend.emitSetCond(cb, Backend.RESULT, .lt);
                    try self.push(Backend.RESULT);
                },
                .i32_gt => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitCmp(cb, Backend.RESULT, Backend.SCRATCH);
                    try Backend.emitSetCond(cb, Backend.RESULT, .gt);
                    try self.push(Backend.RESULT);
                },
                .i32_le => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitCmp(cb, Backend.RESULT, Backend.SCRATCH);
                    try Backend.emitSetCond(cb, Backend.RESULT, .le);
                    try self.push(Backend.RESULT);
                },
                .i32_ge => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitCmp(cb, Backend.RESULT, Backend.SCRATCH);
                    try Backend.emitSetCond(cb, Backend.RESULT, .ge);
                    try self.push(Backend.RESULT);
                },
                .i32_neg => {
                    try self.pop(Backend.RESULT);
                    try Backend.emitNeg(cb, Backend.RESULT);
                    try self.push(Backend.RESULT);
                },
                .bool_not => {
                    try self.pop(Backend.RESULT);
                    try Backend.emitTest(cb, Backend.RESULT);
                    try Backend.emitSetCond(cb, Backend.RESULT, .eq);
                    try self.push(Backend.RESULT);
                },
                .i32_and => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitAnd(cb, Backend.RESULT, Backend.SCRATCH);
                    try self.push(Backend.RESULT);
                },
                .i32_or => {
                    try self.pop(Backend.SCRATCH);
                    try self.pop(Backend.RESULT);
                    try Backend.emitOr(cb, Backend.RESULT, Backend.SCRATCH);
                    try self.push(Backend.RESULT);
                },
                .jump => |target| {
                    const pos = try Backend.emitJmpRel32(cb);
                    try self.jump_patches.append(self.allocator, .{ .source_offset = pos, .target_pc = target });
                },
                .jump_if_false => |pair| {
                    const cond = pair[0];
                    const target = pair[1];
                    const slot = try self.ensureSlot(cond);
                    try Backend.emitLoadFromSlot(cb, Backend.RESULT, slot.offset);
                    try Backend.emitTest(cb, Backend.RESULT);
                    const pos = try Backend.emitJccRel32(cb, .eq);
                    try self.jump_patches.append(self.allocator, .{ .source_offset = pos, .target_pc = target });
                },
                .jump_if_true => |pair| {
                    const cond = pair[0];
                    const target = pair[1];
                    const slot = try self.ensureSlot(cond);
                    try Backend.emitLoadFromSlot(cb, Backend.RESULT, slot.offset);
                    try Backend.emitTest(cb, Backend.RESULT);
                    const pos = try Backend.emitJccRel32(cb, .ne);
                    try self.jump_patches.append(self.allocator, .{ .source_offset = pos, .target_pc = target });
                },
                .call => |call_info| {
                    try self.emitCall(call_info);
                },
                .ret => |opt_vid| {
                    try Backend.emitEpilogueReturn(cb, self.frame_size, if (opt_vid) |v| @as(usize, v) else null);
                },
                .dup => {
                    try self.peek(Backend.RESULT);
                    try self.push(Backend.RESULT);
                },
                .pop => {
                    try self.pop(Backend.RESULT);
                },
                .unsupported => |tag| {
                    std.log.warn("unsupported instruction tag {d}", .{tag});
                    try Backend.emitXorReg(cb, Backend.RESULT);
                    try self.push(Backend.RESULT);
                },
                else => {
                    std.log.warn("unsupported instruction at pc {d}", .{pc});
                    try Backend.emitXorReg(cb, Backend.RESULT);
                    try self.push(Backend.RESULT);
                },
            }
        }

        fn push(self: *Self, reg: usize) !void {
            const slot = codebuf.VarSlot{ .offset = self.next_stack_offset };
            self.next_stack_offset -= @as(i32, @intCast(Backend.word_size));
            try Backend.emitStoreToSlot(self.cb, slot.offset, reg);
        }

        fn pop(self: *Self, reg: usize) !void {
            self.next_stack_offset += @as(i32, @intCast(Backend.word_size));
            const offset = self.next_stack_offset;
            try Backend.emitLoadFromSlot(self.cb, reg, offset);
        }

        fn peek(self: *Self, reg: usize) !void {
            const offset = self.next_stack_offset + @as(i32, @intCast(Backend.word_size));
            try Backend.emitLoadFromSlot(self.cb, reg, offset);
        }

        fn emitCall(self: *Self, call_info: typeir.CallInfo) !void {
            const cb = self.cb;
            var i: usize = 0;
            while (i < call_info.args.len and i < Backend.ARG_REGS.len) : (i += 1) {
                const slot = try self.ensureSlot(call_info.args[i]);
                try Backend.emitLoadFromSlot(cb, Backend.ARG_REGS[i], slot.offset);
            }

            const is_external = self.module.isExternal(call_info.func);
            if (is_external or call_info.func == std.math.maxInt(u32)) {
                const name = call_info.ext_name orelse "vx_unknown";
                const pos = try Backend.emitCallRel32(cb);
                try cb.addCallPatch(pos, name);
            } else {
                const target_func = self.module.getFunction(call_info.func) orelse return codebuf.CodegenError.UnknownFunction;
                const pos = try Backend.emitCallRel32(cb);
                try cb.addCallPatch(pos, target_func.name);
            }
            try self.push(Backend.RESULT);
        }
    };
}

// ==================== 顶层模块编译 ====================

fn ModuleCodegen(comptime Backend: type) type {
    return struct {
        const Self = @This();

        text: codebuf.CodeBuffer,
        function_offsets: std.StringHashMap(usize),
        external_stubs: std.StringHashMap(usize),
        allocator: std.mem.Allocator,
        entry_offset: u64,

        pub fn init(allocator: std.mem.Allocator) Self {
            return .{
                .text = codebuf.CodeBuffer.init(allocator),
                .function_offsets = std.StringHashMap(usize).init(allocator),
                .external_stubs = std.StringHashMap(usize).init(allocator),
                .allocator = allocator,
                .entry_offset = 0,
            };
        }

        pub fn deinit(self: *Self) void {
            self.text.deinit();
            self.function_offsets.deinit();
            self.external_stubs.deinit();
        }

        pub fn compile(self: *Self, module: typeir.TypeModule) !void {
            std.log.warn("ModuleCodegen.compile: {d} functions, {d} linkage entries", .{ module.functions.len, module.linkage.len });

            for (module.functions) |*func| {
                if (module.isExternal(func.id)) {
                    std.log.warn("  skip external func '{s}' (id={d})", .{ func.name, func.id });
                    continue;
                }
                const offset = self.text.len();
                try self.function_offsets.put(func.name, offset);
                std.log.warn("  compile func '{s}' (id={d}) at offset {d}, body.len={d}", .{ func.name, func.id, offset, func.body.len });

                var fg = FunctionCodegen(Backend).init(&self.text, func, &module, self.allocator);
                defer fg.deinit();
                try fg.compile();
                std.log.warn("    after compile: text.len={d}", .{self.text.len()});
            }

            for (module.linkage) |link| {
                if (link.kind != .external) continue;
                const name = link.name orelse continue;
                if (self.external_stubs.contains(name)) continue;
                const stub_offset = self.text.len();
                try self.external_stubs.put(name, stub_offset);
                std.log.warn("  generate stub for external '{s}' at offset {d}", .{ name, stub_offset });
                try Backend.emitBuiltin(&self.text, name);
            }

            self.entry_offset = @intCast(self.text.len());
            std.log.warn("  entry_offset before start: {d}", .{self.entry_offset});
            try self.generateStartEntry(module);
            std.log.warn("  total code len after start: {d}", .{self.text.len()});
            try self.patchCalls();
            std.log.warn("  final entry_offset: {d}", .{self.entry_offset});
        }

        pub fn getEntryOffset(self: *Self) u64 {
            return self.entry_offset;
        }

        fn generateStartEntry(self: *Self, module: typeir.TypeModule) !void {
            const cb = &self.text;
            const entry_name = if (module.getFunctionByName("__main__") != null)
                "__main__"
            else if (module.getFunctionByName("main") != null)
                "main"
            else
                return;

            // 调用 entry point 并退出
            try Backend.emitXorReg(cb, Backend.RESULT);
            try Backend.emitMovRegReg(cb, Backend.ARG_REGS[0], Backend.RESULT);
            const call_pos = try Backend.emitCallRel32(cb);
            try cb.addCallPatch(call_pos, entry_name);
            try Backend.emitMovRegReg(cb, Backend.RESULT, Backend.ARG_REGS[0]);
            // syscall exit(result)
            try Backend.emitBuiltin(cb, "exit");
        }

        fn patchCalls(self: *Self) !void {
            const cb = &self.text;
            for (cb.call_patches.items) |patch| {
                const target_offset = blk: {
                    if (self.function_offsets.get(patch.target_name)) |off| break :blk off;
                    if (self.external_stubs.get(patch.target_name)) |off| break :blk off;
                    break :blk cb.len();
                };
                const source_end = patch.source_offset + 4;
                const rel: i32 = @intCast(@as(i64, @intCast(target_offset)) - @as(i64, @intCast(source_end)));
                cb.writeU32LE(patch.source_offset, @bitCast(rel));
            }
        }

        pub fn toOwnedSlice(self: *Self) ![]u8 {
            return try self.text.toOwnedSlice();
        }
    };
}

// ==================== 公共入口：根据架构选择后端 ====================

pub const CompileResult = struct {
    code: []u8,
    entry_offset: u64,
};

pub fn compile(allocator: std.mem.Allocator, module: typeir.TypeModule, arch: Architecture) !CompileResult {
    switch (arch) {
        .X86_64 => {
            var cg = ModuleCodegen(codegen_x86_64.Backend).init(allocator);
            defer cg.deinit();
            try cg.compile(module);
            return .{ .code = try cg.toOwnedSlice(), .entry_offset = cg.getEntryOffset() };
        },
        .ARM64 => {
            var cg = ModuleCodegen(codegen_aarch64.Backend).init(allocator);
            defer cg.deinit();
            try cg.compile(module);
            return .{ .code = try cg.toOwnedSlice(), .entry_offset = cg.getEntryOffset() };
        },
        .ARM32 => {
            var cg = ModuleCodegen(codegen_arm32.Backend).init(allocator);
            defer cg.deinit();
            try cg.compile(module);
            return .{ .code = try cg.toOwnedSlice(), .entry_offset = cg.getEntryOffset() };
        },
        .RV32 => {
            var cg = ModuleCodegen(codegen_riscv.rv32.Backend).init(allocator);
            defer cg.deinit();
            try cg.compile(module);
            return .{ .code = try cg.toOwnedSlice(), .entry_offset = cg.getEntryOffset() };
        },
        .RV64 => {
            var cg = ModuleCodegen(codegen_riscv.rv64.Backend).init(allocator);
            defer cg.deinit();
            try cg.compile(module);
            return .{ .code = try cg.toOwnedSlice(), .entry_offset = cg.getEntryOffset() };
        },
    }
}
