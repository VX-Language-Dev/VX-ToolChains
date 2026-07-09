// ==================== VX VM Library 入口 ====================
//
// 已迁移到 Zig (src-zig/) 的模块不再在 Rust 端声明。
// Zig 对应模块: opcode, bytecode, type_ir, compiler_bytecode,
//   compiler_core, compiler_expr, compiler_stmt, compiler_module,
//   compiler_typeir, compiler_monomorph, token, parser, compiler_ownership,
//   macros, aot_backend, decompiler, delinker

// 项目配置 (尚未迁移)
pub mod vxsetting;

// 构建器 + 增量缓存 (尚未迁移)
pub mod builder;
pub mod cache;

// 链接器相关 (尚未迁移)
pub mod target_profile;
pub mod lld_linker;

// Re-export public API
pub use vxsetting::VxSettings;
pub use builder::{VxBuilder, BuildError};
pub use cache::BuildCache;
