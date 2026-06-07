// ==================== Instruction ====================

use std::collections::HashMap;
use crate::opcode::OpCode;
use crate::value::Value;

#[derive(Clone, Debug)]
pub struct Instruction {
    pub op: OpCode,
    pub iarg: Option<i32>,
    pub sarg: Option<String>,
}

impl Instruction {
    pub fn new(op: OpCode) -> Self {
        Self {
            op,
            iarg: None,
            sarg: None,
        }
    }
    pub fn with_iarg(op: OpCode, arg: i32) -> Self {
        Self {
            op,
            iarg: Some(arg),
            sarg: None,
        }
    }
    pub fn with_sarg(op: OpCode, arg: String) -> Self {
        Self {
            op,
            iarg: None,
            sarg: Some(arg),
        }
    }
}

// ==================== Function & Module ====================

#[derive(Clone, Debug)]
pub struct Function {
    pub name: String,
    pub instructions: Vec<Instruction>,
    pub constants: Vec<Value>,
    pub num_params: u32,
    pub has_return: bool,
    pub param_names: Vec<String>,
}

#[derive(Clone, Debug)]
pub struct Module {
    pub constants: Vec<Value>,
    pub functions: Vec<Function>,
    pub function_map: HashMap<String, usize>,
    pub struct_defs: HashMap<String, Vec<String>>,
}

// ==================== CallFrame ====================

#[derive(Clone, Debug)]
pub struct CallFrame {
    pub fn_idx: usize,
    pub pc: usize,
    pub stack_base: usize,
    pub tos_base: usize,
    pub locals: HashMap<String, Value>,
    pub owned_allocs: Vec<u64>,
}
