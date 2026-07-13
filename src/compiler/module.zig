const std = @import("std");
const OpCode = @import("../opcode.zig").OpCode;
const Instruction = @import("../compiler_bytecode.zig").Instruction;
const BytecodeArg = @import("../compiler_bytecode.zig").BytecodeArg;
const BytecodeFunction = @import("../compiler_bytecode.zig").BytecodeFunction;
const ConstantValue = @import("../compiler_bytecode.zig").ConstantValue;
const CompiledModule = @import("../compiler_bytecode.zig").CompiledModule;
const StructEntry = @import("../compiler_bytecode.zig").StructEntry;
const ClassEntry = @import("../compiler_bytecode.zig").ClassEntry;
const Expr = @import("../parser/ast.zig").Expr;
const Stmt = Expr;
const Type = @import("../type_ir.zig").Type;
const TypeModule = @import("../type_ir.zig").TypeModule;
const TypeFunction = @import("../type_ir.zig").TypeFunction;
const FuncId = @import("../type_ir.zig").FuncId;
const Linkage = @import("../type_ir.zig").Linkage;
const serializeTypeModule = @import("../type_ir.zig").serializeTypeModule;
const Compiler = @import("core.zig").Compiler;
const KnownType = @import("core.zig").KnownType;
const TypeIRSimulator = @import("typeir.zig").TypeIRSimulator;
const compileStmt = @import("stmt.zig").compileStmt;
const monomorphizeAst = @import("monomorph.zig").monomorphizeAst;

/// 编译整个 AST 模块，返回 CompiledModule。
pub fn compile(self: *Compiler, ast: std.ArrayList(*const Stmt)) !CompiledModule {
    self.constants.clearRetainingCapacity();
    self.instructions.clearRetainingCapacity();
    self.functions.clearRetainingCapacity();
    self.loop_stack.clearRetainingCapacity();
    self.for_counter = 0;
    self.enum_defs.clearRetainingCapacity();

    // 单态化：展开泛型声明为具体类型专门化版本
    var stmts: std.ArrayList(*Expr) = .empty;
    for (ast.items) |s| {
        const mutable: *Expr = @constCast(s);
        stmts.append(self.allocator, mutable) catch @panic("OOM");
    }
    var mono_ast = monomorphizeAst(stmts, self.allocator);
    defer mono_ast.deinit(self.allocator);

    var structs: std.ArrayList(StructEntry) = .empty;
    var classes: std.ArrayList(ClassEntry) = .empty;

    for (mono_ast.items) |s| {
        switch (s.*) {
            .StructDecl => |sd| {
                var fields: std.ArrayList([]const u8) = .empty;
                for (sd.fields.items) |*f| {
                    fields.append(self.allocator, f.field_type) catch @panic("OOM");
                }
                structs.append(self.allocator, .{ .name = self.allocator.dupe(u8, sd.name) catch @panic("OOM"), .fields = fields }) catch @panic("OOM");
                const save = self.instructions;
                self.instructions = .empty;
                _ = self.emit(OpCode.MakeStruct, BytecodeArg{ .String = self.allocator.dupe(u8, sd.name) catch @panic("OOM") });
                for (sd.fields.items) |*f| {
                    _ = self.emit(OpCode.Dup, .None);
                    _ = self.emit(OpCode.LoadVar, BytecodeArg{ .String = self.allocator.dupe(u8, f.name) catch @panic("OOM") });
                    _ = self.emit(OpCode.PropertySet, BytecodeArg{ .String = self.allocator.dupe(u8, f.name) catch @panic("OOM") });
                    _ = self.emit(OpCode.Pop, .None);
                }
                _ = self.emit(OpCode.Return, .None);
                var func_instructions: std.ArrayList(Instruction) = .empty;
                std.mem.swap(std.ArrayList(Instruction), &func_instructions, &self.instructions);
                const bf = BytecodeFunction{
                    .name = self.allocator.dupe(u8, sd.name) catch @panic("OOM"),
                    .instructions = func_instructions,
                    .num_params = sd.fields.items.len,
                    .has_return = true,
                    .param_names = blk: {
                        var pnames: std.ArrayList([]const u8) = .empty;
                        for (sd.fields.items) |*f| pnames.append(self.allocator, self.allocator.dupe(u8, f.name) catch @panic("OOM")) catch @panic("OOM");
                        break :blk pnames;
                    },
                    .param_types = .empty,
                };
                self.functions.append(self.allocator, bf) catch @panic("OOM");
                self.instructions = save;
                const name_const = self.addConst(ConstantValue{ .String = self.allocator.dupe(u8, sd.name) catch @panic("OOM") });
                _ = self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(name_const)) });
                _ = self.emit(OpCode.StoreVar, BytecodeArg{ .String = self.allocator.dupe(u8, sd.name) catch @panic("OOM") });
            },
            .ClassDecl => |cd| {
                var fields: std.ArrayList([]const u8) = .empty;
                for (cd.fields.items) |*f| fields.append(self.allocator, f.field_type) catch @panic("OOM");
                classes.append(self.allocator, .{ .name = self.allocator.dupe(u8, cd.name) catch @panic("OOM"), .fields = fields }) catch @panic("OOM");
                const save = self.instructions;
                self.instructions = .empty;
                _ = self.emit(OpCode.MakeClass, BytecodeArg{ .String = self.allocator.dupe(u8, cd.name) catch @panic("OOM") });
                for (cd.fields.items) |*f| {
                    _ = self.emit(OpCode.Dup, .None);
                    _ = self.emit(OpCode.LoadVar, BytecodeArg{ .String = self.allocator.dupe(u8, f.name) catch @panic("OOM") });
                    _ = self.emit(OpCode.PropertySet, BytecodeArg{ .String = self.allocator.dupe(u8, f.name) catch @panic("OOM") });
                    _ = self.emit(OpCode.Pop, .None);
                }
                _ = self.emit(OpCode.Return, .None);
                var func_instructions: std.ArrayList(Instruction) = .empty;
                std.mem.swap(std.ArrayList(Instruction), &func_instructions, &self.instructions);
                self.functions.append(self.allocator, BytecodeFunction{
                    .name = self.allocator.dupe(u8, cd.name) catch @panic("OOM"),
                    .instructions = func_instructions,
                    .num_params = cd.fields.items.len,
                    .has_return = true,
                    .param_names = blk: {
                        var pnames: std.ArrayList([]const u8) = .empty;
                        for (cd.fields.items) |*f| pnames.append(self.allocator, self.allocator.dupe(u8, f.name) catch @panic("OOM")) catch @panic("OOM");
                        break :blk pnames;
                    },
                    .param_types = .empty,
                }) catch @panic("OOM");
                self.instructions = save;
                // 编译方法
                for (cd.methods.items) |m| {
                    if (m.* == .FuncDecl) {
                        const m_fn = &m.FuncDecl;
                        const msave = self.instructions;
                        self.instructions = .empty;
                        var save_var_types = try self.var_types.clone();
                        defer save_var_types.deinit();
                        self.var_types.clearRetainingCapacity();
                        for (m_fn.params.items) |*p| {
                            const known_type = Compiler.typeNameToKnownType(p.param_type);
                            self.var_types.put(p.name, known_type) catch @panic("OOM");
                        }
                        for (m_fn.body.items) |x| try compileStmt(self, x);
                        if (!hasReturnStmt(m_fn.body)) {
                            _ = self.emit(OpCode.LoadNil, .None);
                            _ = self.emit(OpCode.Return, .None);
                        }
                        const method_name = std.fmt.allocPrint(self.allocator, "{s}_{s}", .{ cd.name, m_fn.name }) catch @panic("OOM");
                        var method_instructions: std.ArrayList(Instruction) = .empty;
                        std.mem.swap(std.ArrayList(Instruction), &method_instructions, &self.instructions);
                        self.functions.append(self.allocator, BytecodeFunction{
                            .name = self.allocator.dupe(u8, method_name) catch @panic("OOM"),
                            .instructions = method_instructions,
                            .num_params = m_fn.params.items.len,
                            .has_return = true,
                            .param_names = blk: {
                                var pnames: std.ArrayList([]const u8) = .empty;
                                for (m_fn.params.items) |*p| pnames.append(self.allocator, self.allocator.dupe(u8, p.name) catch @panic("OOM")) catch @panic("OOM");
                                break :blk pnames;
                            },
                            .param_types = .empty,
                        }) catch @panic("OOM");
                        self.instructions = msave;
                        const mname_const = self.addConst(ConstantValue{ .String = self.allocator.dupe(u8, method_name) catch @panic("OOM") });
                        _ = self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(mname_const)) });
                        _ = self.emit(OpCode.StoreVar, BytecodeArg{ .String = self.allocator.dupe(u8, method_name) catch @panic("OOM") });
                    }
                }
            },
            .EnumDecl => |ed| {
                self.enum_defs.put(self.allocator.dupe(u8, ed.name) catch @panic("OOM"), ed.variants) catch @panic("OOM");
            },
            .UnionDecl => {},
            .ImportStmt => |imp| {
                _ = self.emit(OpCode.Import, BytecodeArg{ .ImportTuple = .{
                    .a = self.allocator.dupe(u8, imp.path) catch @panic("OOM"),
                    .b = if (imp.alias) |a| self.allocator.dupe(u8, a) catch @panic("OOM") else null,
                    .c = null,
                } });
            },
            .ExternDecl => |ed| {
                if (!containsExternalDep(self.external_deps, ed.name)) {
                    self.external_deps.append(self.allocator, self.allocator.dupe(u8, ed.name) catch @panic("OOM")) catch @panic("OOM");
                }
            },
            .FuncDecl => |fd| {
                const save = self.instructions;
                self.instructions = .empty;
                var save_var_types = try self.var_types.clone();
                defer save_var_types.deinit();
                var save_var_slots = try self.var_slots.clone();
                defer save_var_slots.deinit();
                const save_next_slot = self.next_slot;
                self.var_types.clearRetainingCapacity();
                self.var_slots.clearRetainingCapacity();
                self.next_slot = 0;
                for (fd.params.items) |*p| {
                    const known_type = Compiler.typeNameToKnownType(p.param_type);
                    self.var_types.put(p.name, known_type) catch @panic("OOM");
                    _ = self.allocateSlot(p.name);
                }
                for (fd.body.items) |x| try compileStmt(self, x);
                if (!hasReturnStmt(fd.body)) {
                    _ = self.emit(OpCode.LoadNil, .None);
                    _ = self.emit(OpCode.Return, .None);
                }
                var func_instructions: std.ArrayList(Instruction) = .empty;
                std.mem.swap(std.ArrayList(Instruction), &func_instructions, &self.instructions);
                self.functions.append(self.allocator, BytecodeFunction{
                    .name = self.allocator.dupe(u8, fd.name) catch @panic("OOM"),
                    .instructions = func_instructions,
                    .num_params = fd.params.items.len,
                    .has_return = true,
                    .param_names = blk: {
                        var pnames: std.ArrayList([]const u8) = .empty;
                        for (fd.params.items) |*p| pnames.append(self.allocator, self.allocator.dupe(u8, p.name) catch @panic("OOM")) catch @panic("OOM");
                        break :blk pnames;
                    },
                    .param_types = .empty,
                }) catch @panic("OOM");
                self.instructions = save;
                self.var_types = save_var_types;
                self.var_slots = save_var_slots;
                self.next_slot = save_next_slot;
                const fname_const = self.addConst(ConstantValue{ .String = self.allocator.dupe(u8, fd.name) catch @panic("OOM") });
                _ = self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(fname_const)) });
                const fname_slot = self.allocateSlot(fd.name);
                _ = self.emit(OpCode.StoreVar, BytecodeArg{ .Int = @as(i32, @intCast(fname_slot)) });
            },
            // 其他语句（VarDecl, ExprStmt, Assign, IfStmt 等）
            else => {
                try compileStmt(self, @constCast(s));
            },
        }
    }

    if (self.instructions.items.len > 0) {
        var has_main = false;
        for (self.functions.items) |*f| {
            if (std.mem.eql(u8, f.name, "main")) {
                has_main = true;
                break;
            }
        }
        if (has_main) {
            const main_const = self.addConst(ConstantValue{ .String = "main" });
            _ = self.emit(OpCode.LoadConst, BytecodeArg{ .Int = @as(i32, @intCast(main_const)) });
            _ = self.emit(OpCode.Call, BytecodeArg{ .Int = 0 });
            _ = self.emit(OpCode.Pop, .None);
        }
        _ = self.emit(OpCode.LoadNil, .None);
        _ = self.emit(OpCode.Return, .None);
        var main_instructions: std.ArrayList(Instruction) = .empty;
        std.mem.swap(std.ArrayList(Instruction), &main_instructions, &self.instructions);
        self.functions.insert(self.allocator, 0, BytecodeFunction{
            .name = self.allocator.dupe(u8, "__main__") catch @panic("OOM"),
            .instructions = main_instructions,
            .num_params = 0,
            .has_return = false,
            .param_names = .empty,
            .param_types = .empty,
        }) catch @panic("OOM");
    }

    const type_ir_data = generateTypeIr(self, self.functions.items);

    const result_functions = self.functions;
    self.functions = .empty;
    const result_constants = self.constants;
    self.constants = .empty;
    const result_external_deps = self.external_deps;
    self.external_deps = .empty;
    return CompiledModule{
        .functions = result_functions,
        .constants = result_constants,
        .structs = structs,
        .classes = classes,
        .type_ir_data = type_ir_data,
        .target_triple = "",
        .external_deps = result_external_deps,
    };
}

fn generateTypeIr(self: *Compiler, functions: []const BytecodeFunction) []const u8 {
    var type_mod = TypeModule.init(self.allocator);
    var func_name_to_id = std.StringHashMap(FuncId).init(self.allocator);
    for (functions, 0..) |*f, i| {
        func_name_to_id.put(f.name, @as(FuncId, @intCast(i))) catch @panic("OOM");
    }

    for (self.external_deps.items, 0..) |dep, i| {
        const ext_func_id = @as(FuncId, @intCast(functions.len + i));
        type_mod.linkage.put(ext_func_id, Linkage{ .External = self.allocator.dupe(u8, dep) catch @panic("OOM") }) catch @panic("OOM");
    }

    for (functions, 0..) |*func, i| {
        var tf = TypeFunction.init(func.name, @as(FuncId, @intCast(i)), self.allocator);
        tf.param_count = @as(u32, @intCast(func.num_params));
        tf.has_return = func.has_return;
        for (func.param_names.items) |pname| {
            const ptype = self.getVarType(pname);
            tf.params.append(self.allocator, .{
                .name = self.allocator.dupe(u8, pname) catch @panic("OOM"),
                .param_type = knownToType(self, ptype),
            }) catch @panic("OOM");
        }
        var sim = TypeIRSimulator.init(self.allocator, func_name_to_id, self.extern_qualified_names);
        for (func.instructions.items) |*inst| {
            sim.translateInst(inst, self.constants.items);
        }
        const actual_var_count = sim.varCount();
        var slot_iter = sim.slotTypes().iterator();
        while (slot_iter.next()) |entry| {
            tf.local_types.put(entry.key_ptr.*, entry.value_ptr.*) catch @panic("OOM");
        }
        tf.body = sim.intoBody();
        tf.var_count = actual_var_count;
        type_mod.functions.append(self.allocator, tf) catch @panic("OOM");
        type_mod.function_map.put(@as(FuncId, @intCast(i)), self.allocator.dupe(u8, func.name) catch @panic("OOM")) catch @panic("OOM");
    }

    for (functions, 0..) |*f, i| {
        if (std.mem.eql(u8, f.name, "__main__")) {
            type_mod.entry_point = @as(FuncId, @intCast(i));
        }
    }

    return serializeTypeModule(&type_mod, self.allocator);
}

fn knownToType(compiler: *Compiler, kt: KnownType) Type {
    return switch (kt) {
        .Int => Type.Int,
        .Float => Type.Float,
        .Bool => Type.Bool,
        .String => Type.String,
        .Array => blk: {
            const inner = compiler.allocator.create(Type) catch @panic("OOM");
            inner.* = .Unknown;
            break :blk Type{ .Array = inner };
        },
        .Map => blk: {
            const k = compiler.allocator.create(Type) catch @panic("OOM");
            k.* = .Unknown;
            const v = compiler.allocator.create(Type) catch @panic("OOM");
            v.* = .Unknown;
            break :blk Type{ .Map = .{ .key = k, .value = v } };
        },
        .Instance => Type.Unknown,
        .Pointer => blk: {
            const inner = compiler.allocator.create(Type) catch @panic("OOM");
            inner.* = .Unknown;
            break :blk Type{ .Pointer = inner };
        },
        .Nil => Type.Unknown,
        .Unknown => Type.Unknown,
    };
}

fn containsExternalDep(deps: std.ArrayList([]const u8), name: []const u8) bool {
    for (deps.items) |dep| {
        if (std.mem.eql(u8, dep, name)) return true;
    }
    return false;
}

fn hasReturnStmt(body: std.ArrayList(*Expr)) bool {
    for (body.items) |stmt| {
        if (stmt.* == .ReturnStmt) return true;
    }
    return false;
}
