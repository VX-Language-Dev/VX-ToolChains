// ==================== VX VM Library 入口 ====================
// 各子模块定义见对应文件

mod opcode;
mod value;
mod instruction;
mod vm;
mod vm_exec;
mod memory_safety;

pub mod bytecode;

// VX Language Core: 词法分析、语法分析、所有权检查
// 这些模块由编译器、LSP 服务器等共享使用
pub mod token;
pub mod parser;
pub mod compiler_ownership;

// Re-export public API
pub use opcode::OpCode;
pub use value::Value;
pub use instruction::{Instruction, Function, Module, CallFrame};
pub use vm::VM;
pub use memory_safety::AllocRecord;
