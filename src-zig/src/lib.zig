const std = @import("std");

pub const opcode = @import("opcode.zig");
pub const bytecode = @import("bytecode.zig");
pub const type_ir = @import("type_ir.zig");
pub const compiler_bytecode = @import("compiler_bytecode.zig");
pub const token = @import("token.zig");
pub const macros = @import("macros.zig");

// 编译器模块
pub const compiler_core = @import("compiler/core.zig");
pub const compiler_expr = @import("compiler/expr.zig");
pub const compiler_stmt = @import("compiler/stmt.zig");
pub const compiler_module = @import("compiler/module.zig");
pub const compiler_typeir = @import("compiler/typeir.zig");
pub const compiler_monomorph = @import("compiler/monomorph.zig");
pub const compiler_ownership = @import("compiler/ownership.zig");

// 解析器模块
pub const parser = @import("parser/mod.zig");

// 配置模块
pub const vxsetting = @import("vxsetting.zig");

// 已移植的模块
pub const delinker = @import("delinker.zig");
pub const target_profile = @import("target_profile.zig");

// 尚未翻译的模块导出为空桩声明
pub const aot_backend = @import("stubs.zig");
pub const decompiler = @import("stubs.zig");
pub const builder = @import("stubs.zig");
pub const cache = @import("stubs.zig");
pub const lld_linker = @import("stubs.zig");

// Re-export commonly used types
pub const OpCode = opcode.OpCode;
pub const VxSettings = vxsetting.VxSettings;
pub const Compiler = compiler_core.Compiler;
pub const Macro = macros.Macro;
pub const MacroRegistry = macros.MacroRegistry;
pub const Expr = parser.Expr;
pub const Token = token.Token;
pub const VXError = token.VXError;
