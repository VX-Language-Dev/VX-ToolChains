// ==================== 编译器核心 ====================
// 编译器数据结构、构造函数和核心辅助方法。
// 表达式/语句/模块编译分别拆分至 compiler_expr / compiler_stmt / compiler_module。

use std::collections::HashMap;

use crate::OpCode;
use crate::compiler_bytecode::{BytecodeArg, Instruction, BytecodeFunction, ConstantValue};

pub type BytecodeInstructionTuple = (u8, u8, Option<i32>, Option<String>);
pub type VxobjFunctionData<'a> = (&'a str, u32, bool, Vec<String>, Vec<BytecodeInstructionTuple>);
pub type VxobjFunctionRef<'a> = (&'a str, u32, bool, &'a [String], &'a [BytecodeInstructionTuple]);

pub struct LoopInfo {
    pub start: usize,
    pub break_jumps: Vec<usize>,
    pub continue_jumps: Vec<usize>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum KnownType {
    Int,
    Float,
    Bool,
    String,
    Unknown,
}

pub struct Compiler {
    pub(crate) settings: crate::VxSettings,
    pub(crate) constants: Vec<ConstantValue>,
    pub(crate) instructions: Vec<Instruction>,
    pub(crate) functions: Vec<BytecodeFunction>,
    pub(crate) loop_stack: Vec<LoopInfo>,
    pub(crate) for_counter: usize,
    pub(crate) var_types: HashMap<String, KnownType>,
    pub(crate) var_slots: HashMap<String, u32>,
    pub(crate) next_slot: u32,
    pub(crate) stack_types: Vec<KnownType>,
    pub opt_level: u8,
    pub warn_dead_code: bool,
    pub error_dead_code: bool,
}

impl Compiler {
    pub fn new(settings: crate::VxSettings) -> Self {
        Self {
            settings,
            constants: Vec::new(),
            instructions: Vec::new(),
            functions: Vec::new(),
            loop_stack: Vec::new(),
            for_counter: 0,
            var_types: HashMap::new(),
            var_slots: HashMap::new(),
            next_slot: 0,
            stack_types: Vec::new(),
            opt_level: 0,
            warn_dead_code: false,
            error_dead_code: false,
        }
    }

    pub fn with_options(mut self, opt_level: u8, warn_dead_code: bool, error_dead_code: bool) -> Self {
        self.opt_level = opt_level;
        self.warn_dead_code = warn_dead_code;
        self.error_dead_code = error_dead_code;
        self
    }

    pub(crate) fn allocate_slot(&mut self, name: &str) -> u32 {
        if let Some(&slot) = self.var_slots.get(name) { return slot; }
        let slot = self.next_slot;
        self.next_slot += 1;
        self.var_slots.insert(name.to_string(), slot);
        slot
    }

    pub(crate) fn push_stack_type(&mut self, t: KnownType) { self.stack_types.push(t); }
    pub(crate) fn pop_stack_type(&mut self) -> KnownType { self.stack_types.pop().unwrap_or(KnownType::Unknown) }
    pub(crate) fn set_var_type(&mut self, name: &str, t: KnownType) { self.var_types.insert(name.to_string(), t); }
    pub(crate) fn get_var_type(&self, name: &str) -> KnownType { self.var_types.get(name).copied().unwrap_or(KnownType::Unknown) }

    pub(crate) fn binary_op_specialized(&self, op: &str, left: KnownType, right: KnownType) -> Option<OpCode> {
        match (op, left, right) {
            ("+", KnownType::Int, KnownType::Int) => Some(OpCode::AddInt),
            ("+", KnownType::Float, KnownType::Float) => Some(OpCode::AddFloat),
            ("-", KnownType::Int, KnownType::Int) => Some(OpCode::SubInt),
            ("-", KnownType::Float, KnownType::Float) => Some(OpCode::SubFloat),
            ("*", KnownType::Int, KnownType::Int) => Some(OpCode::MulInt),
            ("*", KnownType::Float, KnownType::Float) => Some(OpCode::MulFloat),
            ("/", KnownType::Int, KnownType::Int) => Some(OpCode::DivInt),
            ("/", KnownType::Float, KnownType::Float) => Some(OpCode::DivFloat),
            ("%", KnownType::Int, KnownType::Int) => Some(OpCode::ModInt),
            ("==", KnownType::Int, KnownType::Int) => Some(OpCode::EqInt),
            ("==", KnownType::Float, KnownType::Float) => Some(OpCode::EqFloat),
            ("<", KnownType::Int, KnownType::Int) => Some(OpCode::LtInt),
            ("<", KnownType::Float, KnownType::Float) => Some(OpCode::LtFloat),
            (">", KnownType::Int, KnownType::Int) => Some(OpCode::GtInt),
            (">", KnownType::Float, KnownType::Float) => Some(OpCode::GtFloat),
            ("<=", KnownType::Int, KnownType::Int) => Some(OpCode::LeInt),
            ("<=", KnownType::Float, KnownType::Float) => Some(OpCode::LeFloat),
            (">=", KnownType::Int, KnownType::Int) => Some(OpCode::GeInt),
            (">=", KnownType::Float, KnownType::Float) => Some(OpCode::GeFloat),
            ("&&", KnownType::Bool, KnownType::Bool) => Some(OpCode::And),
            ("||", KnownType::Bool, KnownType::Bool) => Some(OpCode::Or),
            _ => None,
        }
    }

    pub(crate) fn unary_op_specialized(&self, op: &str, operand: KnownType) -> Option<OpCode> {
        match (op, operand) {
            ("-", KnownType::Int) => Some(OpCode::NegInt),
            ("-", KnownType::Float) => Some(OpCode::NegFloat),
            ("!", KnownType::Bool) => Some(OpCode::Not),
            _ => None,
        }
    }

    pub(crate) fn add_const(&mut self, v: ConstantValue) -> usize { self.constants.push(v.clone()); self.constants.len() - 1 }
    pub(crate) fn emit(&mut self, op: OpCode, arg: BytecodeArg) -> usize { self.instructions.push(Instruction { op, arg }); self.instructions.len() - 1 }
    pub(crate) fn patch(&mut self, pos: usize, tgt: usize) {
        if let Some(inst) = self.instructions.get_mut(pos) {
            inst.arg = match &inst.arg {
                BytecodeArg::None => BytecodeArg::Int(tgt as i32),
                _ => BytecodeArg::Int(tgt as i32),
            };
        }
    }
}
