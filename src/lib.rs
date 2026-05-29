// ==================== VX VM Library 入口 ====================
// 各子模块定义见对应文件

mod opcode;
mod value;
mod instruction;
mod vm;
mod vm_exec;
mod memory_safety;

pub mod bytecode;

// Re-export public API
pub use opcode::OpCode;
pub use value::Value;
pub use instruction::{Instruction, Function, Module, CallFrame};
pub use vm::VM;
pub use memory_safety::AllocRecord;
