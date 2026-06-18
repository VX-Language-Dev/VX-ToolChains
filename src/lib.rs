// ==================== VX VM Library 入口 ====================
// 各子模块定义见对应文件

mod opcode;
mod value;
mod instruction;
mod vm;
mod vm_dispatch;
mod vm_exec;
mod memory_safety;

pub mod bytecode;
pub mod type_ir;

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

// Re-export public API
pub use opcode::OpCode;
pub use value::Value;
pub use instruction::{Instruction, Function, Module, CallFrame};
pub use vm::{VM, DebugAction, StepMode};
pub use memory_safety::AllocRecord;
pub use vxsetting::VxSettings;
pub use builder::{VxBuilder, BuildError};
pub use cache::BuildCache;