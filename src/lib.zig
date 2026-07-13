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
pub const compiler_optimizer = @import("compiler/optimize.zig");

// 优化器测试
comptime {
    _ = @import("compiler/optimize_test.zig");
}

// 解析器模块
pub const parser = @import("parser/mod.zig");

// 配置模块 (完整实现，替代 TODO 占位符)
pub const vxsetting = @import("vxsetting.zig");

// 已移植的模块
pub const delinker = @import("delinker.zig");
pub const target_profile = @import("target_profile.zig");

// 新增 Zig 原生模块
pub const linker = @import("linker.zig");
pub const logger = @import("logger.zig");
pub const cache = @import("cache.zig");
pub const parallel_build = @import("parallel_build.zig");

// 已翻译的构建器和LSP模块
pub const builder = @import("builder.zig");
pub const lsp_state = @import("lsp/state.zig");
pub const lsp_diagnostics = @import("lsp/diagnostics.zig");
pub const lsp_completion = @import("lsp/completion.zig");
pub const lsp_hover = @import("lsp/hover.zig");
pub const lsp_goto = @import("lsp/goto.zig");
pub const lsp_symbols = @import("lsp/symbols.zig");
pub const lsp_inlay_hints = @import("lsp/inlay_hints.zig");
pub const lsp_backend = @import("lsp/backend.zig");

// 尚未翻译的模块导出为空桩声明
pub const aot_backend = @import("stubs.zig");
pub const decompiler = @import("stubs.zig");

// Re-export commonly used types
pub const OpCode = opcode.OpCode;
pub const VxSettings = vxsetting.VxSettings;
pub const Compiler = compiler_core.Compiler;
pub const Macro = macros.Macro;
pub const MacroRegistry = macros.MacroRegistry;
pub const Expr = parser.Expr;
pub const Token = token.Token;
pub const VXError = token.VXError;
pub const Logger = logger.Logger;
pub const LogLevel = logger.LogLevel;
pub const BuiltinLinker = linker.BuiltinLinker;
pub const LldLinker = linker.LldLinker;
pub const BuildCache = cache.BuildCache;
pub const BuildScheduler = parallel_build.BuildScheduler;
pub const VxBuilder = builder.VxBuilder;
pub const BuildError = builder.BuildError;
pub const BackendState = lsp_state.BackendState;
pub const DocumentState = lsp_state.DocumentState;
pub const Diagnostic = lsp_diagnostics.Diagnostic;
pub const CompletionItem = lsp_completion.CompletionItem;
pub const Hover = lsp_hover.Hover;
pub const GotoDefinitionResponse = lsp_goto.GotoDefinitionResponse;
pub const DocumentSymbol = lsp_symbols.DocumentSymbol;
pub const InlayHint = lsp_inlay_hints.InlayHint;
pub const VxLspBackend = lsp_backend.VxLspBackend;
