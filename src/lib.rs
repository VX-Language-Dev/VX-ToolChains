// ==================== VX VM Library 入口 ====================
// 各子模块定义见对应文件

mod opcode;

// 编译器模块：供 vxcompiler CLI、LSP、构建器、集成测试共享使用
pub mod bytecode;
pub mod type_ir;
pub mod compiler_bytecode;
pub mod compiler_typeir;
pub mod compiler_core;
pub mod compiler_expr;
pub mod compiler_stmt;
pub mod compiler_module;

// AOT 后端: Feature-gated Cranelift 原生代码生成
// 启用: cargo build --features aot
#[cfg(feature = "aot")]
pub mod aot_backend;

// VX Language Core: 词法分析、语法分析、所有权检查、项目配置、构建器
// 这些模块由编译器、LSP 服务器等共享使用
pub mod token;
pub mod parser;
pub mod compiler_ownership;
pub mod vxsetting;
pub mod builder;
pub mod cache;

// 宏系统：编译时宏展开支持
pub mod macros;

// 反编译器：TypeIR → VX 源码
pub mod decompiler;

// 反链接器：从可执行文件提取 VXOBJ v4 数据
pub mod delinker;

// 链接器模块
pub mod target_profile;
pub mod lld_linker;

// Re-export public API
pub use opcode::OpCode;
pub use vxsetting::VxSettings;
pub use builder::{VxBuilder, BuildError};
pub use cache::BuildCache;
pub use compiler_bytecode::{BytecodeArg, Instruction as CompilerInstruction, BytecodeFunction, ConstantValue, CompiledModule};
pub use compiler_core::Compiler;
pub use macros::{Macro, MacroRegistry};
