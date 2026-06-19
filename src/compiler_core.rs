// ==================== 编译器核心 ====================

use std::collections::HashMap;
use std::fs;
use std::io;

use crate::parser::{Expr, Stmt};
use crate::OpCode;
use crate::type_ir::{self, Type, TypeModule, TypeFunction, TypedInstruction, FuncId, StructLayoutId};
use crate::compiler_bytecode::{BytecodeArg, Instruction, BytecodeFunction, ConstantValue, CompiledModule};
use crate::bytecode;

pub struct LoopInfo {
    pub start: usize,
    pub break_jumps: Vec<usize>,
    pub continue_jumps: Vec<usize>,
}

/// TypeIR 栈模拟器：在生成 TypeIR 时追踪字节码栈，
/// 将栈位置映射为正确的 VarId（TypeIR 中的指令索引）。
struct TypeIRSimulator {
    body: Vec<TypedInstruction>,
    slot_to_var: HashMap<u32, u32>,
    stack: Vec<u32>,
}

impl TypeIRSimulator {
    fn new() -> Self {
        Self {
            body: Vec::new(),
            slot_to_var: HashMap::new(),
            stack: Vec::new(),
        }
    }

    /// 发射一条 TypeIR 指令，返回其在 body 中的索引（即 VarId）
    fn emit(&mut self, inst: TypedInstruction) -> u32 {
        self.body.push(inst);
        (self.body.len() - 1) as u32
    }

    fn push_val(&mut self, var_id: u32) {
        self.stack.push(var_id);
    }

    fn pop_val(&mut self) -> u32 {
        self.stack.pop().unwrap_or(0)
    }

    fn _peek_val(&self) -> Option<u32> {
        self.stack.last().copied()
    }

    fn get_slot_var(&mut self, slot: u32) -> u32 {
        let next = self.body.len() as u32;
        *self.slot_to_var.entry(slot).or_insert(next)
    }

    fn set_slot_var(&mut self, slot: u32, var_id: u32) {
        self.slot_to_var.insert(slot, var_id);
    }

    fn translate_inst(&mut self, inst: &Instruction, constants: &[ConstantValue]) {
        use TypedInstruction::*;
        match inst.op {
            OpCode::LoadConst => {
                let cv = match inst.arg {
                    BytecodeArg::Int(idx) => constants.get(idx as usize),
                    _ => None,
                };
                let typed = match cv {
                    Some(ConstantValue::Int(v)) => ConstInt(*v),
                    Some(ConstantValue::Float(v)) => ConstFloat(*v),
                    Some(ConstantValue::Bool(v)) => ConstBool(*v),
                    Some(ConstantValue::String(s)) => ConstString(s.clone()),
                    _ => ConstNil,
                };
                let vid = self.emit(typed);
                self.push_val(vid);
            }
            OpCode::LoadNil => {
                let vid = self.emit(ConstNil);
                self.push_val(vid);
            }
            OpCode::LoadTrue => {
                let vid = self.emit(ConstBool(true));
                self.push_val(vid);
            }
            OpCode::LoadFalse => {
                let vid = self.emit(ConstBool(false));
                self.push_val(vid);
            }
            OpCode::LoadVar => {
                let slot = match inst.arg { BytecodeArg::Int(s) => s as u32, _ => 0 };
                let vid = self.get_slot_var(slot);
                self.emit(LoadVar(vid));
                self.push_val(vid);
            }
            OpCode::StoreVar | OpCode::DefineVar => {
                let slot = match inst.arg { BytecodeArg::Int(s) => s as u32, _ => 0 };
                let vid = self.pop_val();
                self.set_slot_var(slot, vid);
                self.emit(StoreVar(vid));
            }
            OpCode::Dup => {
                if let Some(&v) = self.stack.last() {
                    self.emit(Dup);
                    self.push_val(v);
                } else {
                    self.emit(Dup);
                }
            }
            OpCode::Pop => {
                self.pop_val();
                self.emit(Pop);
            }
            OpCode::Jump => {
                let t = match inst.arg { BytecodeArg::Int(v) => v as u32, _ => 0 };
                self.emit(Jump(t));
            }
            OpCode::JumpIfFalse => {
                let vid = self.pop_val();
                let t = match inst.arg { BytecodeArg::Int(v) => v as u32, _ => 0 };
                self.emit(JumpIfFalse(vid, t));
            }
            OpCode::JumpIfTrue => {
                let vid = self.pop_val();
                let t = match inst.arg { BytecodeArg::Int(v) => v as u32, _ => 0 };
                self.emit(JumpIfTrue(vid, t));
            }
            OpCode::AddInt | OpCode::BinaryAdd => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Add(a, b));
                self.push_val(vid);
            }
            OpCode::SubInt | OpCode::BinarySub => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Sub(a, b));
                self.push_val(vid);
            }
            OpCode::MulInt | OpCode::BinaryMul => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Mul(a, b));
                self.push_val(vid);
            }
            OpCode::DivInt | OpCode::BinaryDiv => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Div(a, b));
                self.push_val(vid);
            }
            OpCode::ModInt | OpCode::BinaryMod => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Mod(a, b));
                self.push_val(vid);
            }
            OpCode::AddFloat => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(F64Add(a, b));
                self.push_val(vid);
            }
            OpCode::SubFloat => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(F64Sub(a, b));
                self.push_val(vid);
            }
            OpCode::MulFloat => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(F64Mul(a, b));
                self.push_val(vid);
            }
            OpCode::DivFloat => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(F64Div(a, b));
                self.push_val(vid);
            }
            OpCode::EqInt | OpCode::BinaryEq => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Eq(a, b));
                self.push_val(vid);
            }
            OpCode::BinaryNe => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Ne(a, b));
                self.push_val(vid);
            }
            OpCode::LtInt | OpCode::BinaryLt => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Lt(a, b));
                self.push_val(vid);
            }
            OpCode::GtInt | OpCode::BinaryGt => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Gt(a, b));
                self.push_val(vid);
            }
            OpCode::LeInt | OpCode::BinaryLe => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Le(a, b));
                self.push_val(vid);
            }
            OpCode::GeInt | OpCode::BinaryGe => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Ge(a, b));
                self.push_val(vid);
            }
            OpCode::EqFloat => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(F64Eq(a, b));
                self.push_val(vid);
            }
            OpCode::LtFloat => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(F64Lt(a, b));
                self.push_val(vid);
            }
            OpCode::GtFloat => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(F64Gt(a, b));
                self.push_val(vid);
            }
            OpCode::LeFloat => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(F64Le(a, b));
                self.push_val(vid);
            }
            OpCode::GeFloat => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(F64Ge(a, b));
                self.push_val(vid);
            }
            OpCode::NegInt => {
                let a = self.pop_val();
                let vid = self.emit(I32Neg(a));
                self.push_val(vid);
            }
            OpCode::NegFloat => {
                let a = self.pop_val();
                let vid = self.emit(F64Neg(a));
                self.push_val(vid);
            }
            OpCode::Not | OpCode::UnaryNot => {
                let a = self.pop_val();
                let vid = self.emit(BoolNot(a));
                self.push_val(vid);
            }
            OpCode::And => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Add(a, b)); // 占位
                self.push_val(vid);
            }
            OpCode::Or => {
                let b = self.pop_val();
                let a = self.pop_val();
                let vid = self.emit(I32Add(a, b)); // 占位
                self.push_val(vid);
            }
            OpCode::MakeArray => {
                let count = match inst.arg { BytecodeArg::Int(n) => n as usize, _ => 0 };
                let mut elems = Vec::with_capacity(count);
                for _ in 0..count { elems.push(self.pop_val()); }
                let vid = self.emit(MakeArray(0, elems));
                self.push_val(vid);
            }
            OpCode::IndexGet => {
                let idx = self.pop_val();
                let obj = self.pop_val();
                let vid = self.emit(IndexGet(obj, idx));
                self.push_val(vid);
            }
            OpCode::IndexSet => {
                let val = self.pop_val();
                let idx = self.pop_val();
                let obj = self.pop_val();
                self.emit(IndexSet(obj, idx, val));
                self.push_val(obj);
            }
            OpCode::MakeMap => {
                let count = match inst.arg { BytecodeArg::Int(n) => n as usize, _ => 0 };
                for _ in 0..count * 2 { self.pop_val(); }
                let vid = self.emit(MakeMap(vec![]));
                self.push_val(vid);
            }
            OpCode::PropertyGet | OpCode::PointerMember => {
                let obj = self.pop_val();
                let vid = self.emit(GetField(obj, 0));
                self.push_val(vid);
            }
            OpCode::PropertySet => {
                let val = self.pop_val();
                let obj = self.pop_val();
                self.emit(SetField(obj, 0, val));
                self.push_val(obj);
            }
            OpCode::OwnershipMove => {
                if let Some(&v) = self.stack.last() {
                    self.emit(OwnershipMove(v));
                } else {
                    self.emit(OwnershipMove(0));
                }
            }
            OpCode::BorrowCheck => {
                if let Some(&v) = self.stack.last() {
                    self.emit(Borrow(v));
                } else {
                    self.emit(Borrow(0));
                }
            }
            OpCode::AliveCheck => {
                if let Some(&v) = self.stack.last() {
                    self.emit(AliveCheck(v));
                }
            }
            OpCode::Free => {
                let ptr = self.pop_val();
                self.emit(Free(ptr));
            }
            OpCode::Call => {
                let num_args = match inst.arg { BytecodeArg::Int(n) => n as usize, _ => 0 };
                let mut args = Vec::with_capacity(num_args);
                for _ in 0..num_args { args.push(self.pop_val()); }
                let _callee = self.pop_val();
                let vid = self.emit(Call(0, args));
                self.push_val(vid);
            }
            OpCode::Return => {
                let ret = self.pop_val();
                self.emit(Return(Some(ret)));
            }
            OpCode::AddressOf => {
                if let Some(&v) = self.stack.last() {
                    self.push_val(v);
                }
            }
            OpCode::New | OpCode::Newz | OpCode::MakeStruct | OpCode::MakeClass => {
                // 弹出参数后产生一个新实例 VarId
                let count = match inst.arg { BytecodeArg::Int(n) => n as usize, _ => 0 };
                for _ in 0..count { self.pop_val(); }
                // New/Newz 也会弹出类名字符串
                if matches!(inst.op, OpCode::New | OpCode::Newz) {
                    self.pop_val();
                }
                let vid = self.emit(MakeStruct(StructLayoutId(0), vec![]));
                self.push_val(vid);
            }
            // 系统调用：忽略 TypeIR 映射（无类型敏感信息）
            OpCode::SysArgv => {
                let vid = self.emit(MakeArray(0, vec![]));
                self.push_val(vid);
            }
            OpCode::System => {
                self.pop_val();
                let vid = self.emit(ConstInt(0));
                self.push_val(vid);
            }
            OpCode::FileRead => {
                self.pop_val();
                let vid = self.emit(ConstString(String::new()));
                self.push_val(vid);
            }
            OpCode::FileWrite => {
                self.pop_val();
                self.pop_val();
                let vid = self.emit(ConstBool(false));
                self.push_val(vid);
            }
            OpCode::FileExists => {
                self.pop_val();
                let vid = self.emit(ConstBool(false));
                self.push_val(vid);
            }
            // 忽略 OpCode（无栈效果或仅控制流）
            OpCode::ScopeDrop | OpCode::Halt | OpCode::Import | OpCode::BinaryPow => {}
            _ => {}
        }
    }
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
    settings: crate::VxSettings,
    constants: Vec<ConstantValue>,
    instructions: Vec<Instruction>,
    functions: Vec<BytecodeFunction>,
    loop_stack: Vec<LoopInfo>,
    for_counter: usize,
    var_types: HashMap<String, KnownType>,
    var_slots: HashMap<String, u32>,
    next_slot: u32,
    stack_types: Vec<KnownType>,
    /// 优化等级 (0-20), 由 CLI --opt-level 或 vxsetting.toml [vxset].o 传入
    ///
    /// 默认 0 以便保持向后兼容 (旧调用方未指定时退化为 0, 与历史行为一致);
    /// 高层 CLI 与 vpm 构建器会通过 `with_options` 显式传入实际等级。
    pub opt_level: u8,
    /// 死代码警告开关: 为 true 时编译器对未使用变量/函数发出警告
    pub warn_dead_code: bool,
    /// 死代码错误开关: 为 true 时死代码诊断升级为编译错误
    pub error_dead_code: bool,
}

impl Compiler {
    /// 默认构造: 优化等级 0, 不发出死代码诊断。
    /// 保持向后兼容 — 所有现有 `Compiler::new(settings)` 调用不受影响。
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

    /// 链式配置: 设置优化等级与死代码诊断策略。
    ///
    /// 供 vxcompiler CLI 与 vpm VxBuilder 在拿到 `--opt-level` / `--warn-dead-code` /
    /// `--error-dead-code` 后显式注入, 避免 `Compiler::new` 签名变动影响其他构造路径。
    pub fn with_options(
        mut self,
        opt_level: u8,
        warn_dead_code: bool,
        error_dead_code: bool,
    ) -> Self {
        self.opt_level = opt_level;
        self.warn_dead_code = warn_dead_code;
        self.error_dead_code = error_dead_code;
        self
    }

    fn allocate_slot(&mut self, name: &str) -> u32 {
        if let Some(&slot) = self.var_slots.get(name) {
            return slot;
        }
        let slot = self.next_slot;
        self.next_slot += 1;
        self.var_slots.insert(name.to_string(), slot);
        slot
    }

    fn push_stack_type(&mut self, t: KnownType) {
        self.stack_types.push(t);
    }

    fn pop_stack_type(&mut self) -> KnownType {
        self.stack_types.pop().unwrap_or(KnownType::Unknown)
    }



    fn set_var_type(&mut self, name: &str, t: KnownType) {
        self.var_types.insert(name.to_string(), t);
    }

    fn get_var_type(&self, name: &str) -> KnownType {
        self.var_types.get(name).copied().unwrap_or(KnownType::Unknown)
    }

    fn binary_op_specialized(&self, op: &str, left: KnownType, right: KnownType) -> Option<OpCode> {
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

    fn unary_op_specialized(&self, op: &str, operand: KnownType) -> Option<OpCode> {
        match (op, operand) {
            ("-", KnownType::Int) => Some(OpCode::NegInt),
            ("-", KnownType::Float) => Some(OpCode::NegFloat),
            ("!", KnownType::Bool) => Some(OpCode::Not),
            _ => None,
        }
    }
    fn add_const(&mut self, v: ConstantValue) -> usize {
        self.constants.push(v.clone());
        self.constants.len() - 1
    }
    fn emit(&mut self, op: OpCode, arg: BytecodeArg) -> usize {
        self.instructions.push(Instruction { op, arg });
        self.instructions.len() - 1
    }
    fn patch(&mut self, pos: usize, tgt: usize) {
        if let Some(inst) = self.instructions.get_mut(pos) {
            inst.arg = match &inst.arg {
                BytecodeArg::None => BytecodeArg::Int(tgt as i32),
                _ => BytecodeArg::Int(tgt as i32),
            };
        }
    }
    pub fn compile_expr(&mut self, e: &Expr) -> Result<(), String> {
        match e {
            Expr::IntLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::Int(*v)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                self.push_stack_type(KnownType::Int);
            }
            Expr::FloatLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::Float(*v)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                self.push_stack_type(KnownType::Float);
            }
            Expr::StringLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::String(v.clone())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                self.push_stack_type(KnownType::String);
            }
            Expr::BoolLiteral(v, _, _) => {
                if *v {
                    self.emit(OpCode::LoadTrue, BytecodeArg::None);
                } else {
                    self.emit(OpCode::LoadFalse, BytecodeArg::None);
                }
                self.push_stack_type(KnownType::Bool);
            }
            Expr::NilLiteral(_, _) => {
                self.emit(OpCode::LoadNil, BytecodeArg::None);
                self.push_stack_type(KnownType::Unknown);
            }
            Expr::Identifier(name, _, _) => match name.as_str() {
                "sys_argv" => {
                    self.emit(OpCode::SysArgv, BytecodeArg::None);
                    self.push_stack_type(KnownType::Unknown);
                }
                "os_system" => {
                    self.emit(OpCode::System, BytecodeArg::None);
                    self.push_stack_type(KnownType::Int);
                }
                "file_read" => {
                    self.emit(OpCode::FileRead, BytecodeArg::None);
                    self.push_stack_type(KnownType::String);
                }
                "file_write" => {
                    self.emit(OpCode::FileWrite, BytecodeArg::None);
                    self.push_stack_type(KnownType::Unknown);
                }
                "file_exists" => {
                    self.emit(OpCode::FileExists, BytecodeArg::None);
                    self.push_stack_type(KnownType::Bool);
                }
                _ => {
                    let var_type = self.get_var_type(name);
                    let slot = self.allocate_slot(name);
                    self.emit(OpCode::LoadVar, BytecodeArg::Int(slot as i32));
                    self.push_stack_type(var_type);
                }
            },
            Expr::BinaryOp(op, left, right, _, _) => {
                self.compile_expr(left)?;
                self.compile_expr(right)?;
                let right_type = self.pop_stack_type();
                let left_type = self.pop_stack_type();
                let oc = match self.binary_op_specialized(op, left_type, right_type) {
                    Some(oc) => oc,
                    None => match op.as_ref() {
                        "+" => OpCode::BinaryAdd,
                        "-" => OpCode::BinarySub,
                        "*" => OpCode::BinaryMul,
                        "/" => OpCode::BinaryDiv,
                        "%" => OpCode::BinaryMod,
                        "^" => OpCode::BinaryPow,
                        "==" => OpCode::BinaryEq,
                        "!=" => OpCode::BinaryNe,
                        "<" => OpCode::BinaryLt,
                        ">" => OpCode::BinaryGt,
                        "<=" => OpCode::BinaryLe,
                        ">=" => OpCode::BinaryGe,
                        "&&" => OpCode::BinaryAnd,
                        "||" => OpCode::BinaryOr,
                        _ => return Err(format!("VX Error: 未知的二元操作符: {}", op)),
                    },
                };
                self.emit(oc, BytecodeArg::None);
                let result_type = match (op.as_ref(), left_type, right_type) {
                    ("+", KnownType::Int, KnownType::Int) => KnownType::Int,
                    ("+", KnownType::Float, KnownType::Float) => KnownType::Float,
                    ("-", KnownType::Int, KnownType::Int) => KnownType::Int,
                    ("-", KnownType::Float, KnownType::Float) => KnownType::Float,
                    ("*", KnownType::Int, KnownType::Int) => KnownType::Int,
                    ("*", KnownType::Float, KnownType::Float) => KnownType::Float,
                    ("/", KnownType::Int, KnownType::Int) => KnownType::Int,
                    ("/", KnownType::Float, KnownType::Float) => KnownType::Float,
                    ("%", KnownType::Int, KnownType::Int) => KnownType::Int,
                    ("==" | "!=" | "<" | ">" | "<=" | ">=", KnownType::Int, KnownType::Int) => KnownType::Bool,
                    ("==" | "!=" | "<" | ">" | "<=" | ">=", KnownType::Float, KnownType::Float) => KnownType::Bool,
                    ("&&" | "||", KnownType::Bool, KnownType::Bool) => KnownType::Bool,
                    _ => KnownType::Unknown,
                };
                self.push_stack_type(result_type);
            }
            Expr::UnaryOp(op, operand, _, _) => {
                self.compile_expr(operand)?;
                let operand_type = self.pop_stack_type();
                let oc = self.unary_op_specialized(&**op, operand_type)
                    .unwrap_or_else(|| {
                        if &**op == "-" {
                            OpCode::UnaryNeg
                        } else {
                            OpCode::UnaryNot
                        }
                    });
                self.emit(oc, BytecodeArg::None);
                let result_type = match (&**op, operand_type) {
                    ("-", KnownType::Int) => KnownType::Int,
                    ("-", KnownType::Float) => KnownType::Float,
                    ("!", KnownType::Bool) => KnownType::Bool,
                    _ => KnownType::Unknown,
                };
                self.push_stack_type(result_type);
            }
            Expr::CallExpr(callee, args, _, _) => {
                // 内置函数特殊处理: os_system / file_read / file_write / file_exists
                // 这些标识符对应的 OpCode 期望参数已在栈上，因此需先编译参数再发射 OpCode
                if let Expr::Identifier(name, _, _) = callee.as_ref() {
                    let builtin_op = match name.as_str() {
                        "os_system" => Some(OpCode::System),
                        "file_read" => Some(OpCode::FileRead),
                        "file_write" => Some(OpCode::FileWrite),
                        "file_exists" => Some(OpCode::FileExists),
                        _ => None,
                    };
                    if let Some(op) = builtin_op {
                        // 先编译参数（将参数推入栈），再发射对应的 OpCode
                        // OpCode::System/FileRead/FileWrite 会从栈顶弹出参数
                        for a in args {
                            self.compile_expr(a)?;
                        }
                        self.emit(op, BytecodeArg::None);
                        return Ok(());
                    }
                }

                if let Expr::PropertyAccess(obj, prop, _, _) = callee.as_ref() {
                    self.compile_expr(obj)?;
                    let idx = self.add_const(ConstantValue::String(prop.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                    for a in args {
                        self.compile_expr(a)?;
                    }
                    self.emit(OpCode::Call, BytecodeArg::Int((1 + args.len()) as i32));
                } else {
                    self.compile_expr(callee)?;
                    for a in args {
                        self.compile_expr(a)?;
                    }
                    self.emit(OpCode::Call, BytecodeArg::Int(args.len() as i32));
                }
            }
            Expr::IndexAccess(obj, index, _, _) => {
                self.compile_expr(obj)?;
                self.compile_expr(index)?;
                self.emit(OpCode::IndexGet, BytecodeArg::None);
            }
            Expr::PropertyAccess(obj, prop, _, _) => {
                self.compile_expr(obj)?;
                self.emit(OpCode::PropertyGet, BytecodeArg::String(prop.clone()));
            }
            Expr::ArrayLiteral(elements, _, _) => {
                for x in elements {
                    self.compile_expr(x)?;
                }
                self.emit(OpCode::MakeArray, BytecodeArg::Int(elements.len() as i32));
            }
            Expr::MapLiteral(pairs, _, _) => {
                for (k, v) in pairs {
                    self.compile_expr(k)?;
                    self.compile_expr(v)?;
                }
                self.emit(OpCode::MakeMap, BytecodeArg::Int(pairs.len() as i32));
            }
            Expr::NewExpr(type_name, _, args, _, _) => {
                let idx = self.add_const(ConstantValue::String(type_name.clone())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                for a in args {
                    self.compile_expr(a)?;
                }
                self.emit(OpCode::Call, BytecodeArg::Int(args.len() as i32));
            }
            // NewzExpr 已裁减 → 由 NewExpr + zero:true 扩展或 std::mem::zeroed<T>() 替代
            // 编译器将 newz Foo(args) 展开为 new (Foo) { args..., zero: true } 或调用 zeroed 内建
            // 运行时不再存在 NewzExpr AST 变体, 此处 match 由穷尽性保证不可达
            Expr::MoveExpr(target, _, _) => {
                self.compile_expr(target)?;
                self.emit(OpCode::OwnershipMove, BytecodeArg::None);
            }
            Expr::AddressOf(operand, _, _) => {
                self.compile_expr(operand)?;
                self.emit(OpCode::BorrowCheck, BytecodeArg::None);
                self.emit(OpCode::AddressOf, BytecodeArg::None);
            }
            Expr::Deref(operand, _, _) => {
                self.compile_expr(operand)?;
                self.emit(OpCode::AliveCheck, BytecodeArg::None);
                self.emit(OpCode::Deref, BytecodeArg::None);
            }
            Expr::PointerMember(obj, member, _, _) => {
                self.compile_expr(obj)?;
                self.emit(OpCode::AliveCheck, BytecodeArg::None);
                self.emit(OpCode::PropertyGet, BytecodeArg::String(member.clone()));
            }
            // 表达式位置不应出现语句级节点: 解析器保证此处不可达。
            // 添加新变体时编译器会报非穷尽 match 错误，强制显式处理。
            Expr::StructDecl(..)
            | Expr::ClassDecl(..)
            | Expr::EnumDecl(..)
            | Expr::UnionDecl(..)
            | Expr::FuncDecl(..)
            | Expr::ImportStmt(..)
            | Expr::TypeExpr(..)
            | Expr::ExprStmt(..)
            | Expr::VarDecl(..)
            | Expr::Assign(..)
            | Expr::IfStmt(..)
            | Expr::WhileStmt(..)
            | Expr::ForStmt(..)
            | Expr::BreakStmt(..)
            | Expr::ContinueStmt(..)
            | Expr::ReturnStmt(..) => {}
            // 以下变体已从其 AST/解析层裁减:
            //   NewzExpr → mem::zeroed<T>() 标准库调用
            //   FreeStmt → mem::free(ptr) 标准库调用
            //   VectorLiteral → 数组字面量自动转为 std::Vec<T>
        }
        Ok(())
    }
    pub fn compile_assign(&mut self, target: &Expr, op: &str, value: &Expr) -> Result<(), String> {
        if op == "=" {
            match target {
                Expr::Identifier(name, _, _) => {
                    self.compile_expr(value)?;
                    let value_type = self.pop_stack_type();
                    self.set_var_type(name, value_type);
                    let slot = self.allocate_slot(name);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(slot as i32));
                }
                Expr::IndexAccess(obj, index, _, _) => {
                    self.compile_expr(value)?;
                    self.compile_expr(obj)?;
                    self.compile_expr(index)?;
                    self.emit(OpCode::IndexSet, BytecodeArg::None);
                }
                Expr::PropertyAccess(obj, prop, _, _) => {
                    self.compile_expr(value)?;
                    self.compile_expr(obj)?;
                    self.emit(OpCode::PropertySet, BytecodeArg::String(prop.clone()));
                    self.emit(OpCode::Pop, BytecodeArg::None);
                }
                _ => {}
            }
        } else {
            let bin_op = match op {
                "+=" => "+",
                "-=" => "-",
                "*=" => "*",
                "/=" => "/",
                "%=" => "%",
                "^=" => "^",
                _ => op,
            };
            match target {
                Expr::Identifier(name, _, _) => {
                    let slot = self.allocate_slot(name);
                    self.emit(OpCode::LoadVar, BytecodeArg::Int(slot as i32));
                    let var_type = self.get_var_type(name);
                    self.push_stack_type(var_type);
                    self.compile_expr(value)?;
                    let value_type = self.pop_stack_type();
                    let oc = match self.binary_op_specialized(bin_op, var_type, value_type) {
                        Some(oc) => oc,
                        None => match bin_op {
                            "+" => OpCode::BinaryAdd,
                            "-" => OpCode::BinarySub,
                            "*" => OpCode::BinaryMul,
                            "/" => OpCode::BinaryDiv,
                            "%" => OpCode::BinaryMod,
                            "^" => OpCode::BinaryPow,
                            _ => return Err(format!("VX Error: 未知的二元操作符: {}", bin_op)),
                        },
                    };
                    self.emit(oc, BytecodeArg::None);
                    let result_type = match (bin_op, var_type, value_type) {
                        ("+", KnownType::Int, KnownType::Int) => KnownType::Int,
                        ("+", KnownType::Float, KnownType::Float) => KnownType::Float,
                        ("-", KnownType::Int, KnownType::Int) => KnownType::Int,
                        ("-", KnownType::Float, KnownType::Float) => KnownType::Float,
                        ("*", KnownType::Int, KnownType::Int) => KnownType::Int,
                        ("*", KnownType::Float, KnownType::Float) => KnownType::Float,
                        ("/", KnownType::Int, KnownType::Int) => KnownType::Int,
                        ("/", KnownType::Float, KnownType::Float) => KnownType::Float,
                        ("%", KnownType::Int, KnownType::Int) => KnownType::Int,
                        _ => KnownType::Unknown,
                    };
                    self.set_var_type(name, result_type);
                    let slot2 = self.allocate_slot(name);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(slot2 as i32));
                }
                Expr::IndexAccess(obj, index, _, _) => {
                    self.compile_expr(obj)?;
                    self.compile_expr(index)?;
                    self.emit(OpCode::IndexGet, BytecodeArg::None);
                    self.compile_expr(value)?;
                    let oc = match bin_op {
                        "+" => OpCode::BinaryAdd,
                        "-" => OpCode::BinarySub,
                        "*" => OpCode::BinaryMul,
                        "/" => OpCode::BinaryDiv,
                        "%" => OpCode::BinaryMod,
                        "^" => OpCode::BinaryPow,
                        _ => return Err(format!("VX Error: 未知的二元操作符: {}", bin_op)),
                    };
                    self.emit(oc, BytecodeArg::None);
                    let tmp = format!("__asg_v_{}", self.instructions.len());
                    let tmp_slot = self.allocate_slot(&tmp);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(tmp_slot as i32));
                    self.compile_expr(obj)?;
                    self.compile_expr(index)?;
                    let tmp_slot2 = self.allocate_slot(&tmp);
                    self.emit(OpCode::LoadVar, BytecodeArg::Int(tmp_slot2 as i32));
                    self.emit(OpCode::IndexSet, BytecodeArg::None);
                }
                Expr::PropertyAccess(obj, prop, _, _) => {
                    self.compile_expr(obj)?;
                    self.emit(OpCode::PropertyGet, BytecodeArg::String(prop.clone()));
                    self.compile_expr(value)?;
                    let oc = match bin_op {
                        "+" => OpCode::BinaryAdd,
                        "-" => OpCode::BinarySub,
                        "*" => OpCode::BinaryMul,
                        "/" => OpCode::BinaryDiv,
                        "%" => OpCode::BinaryMod,
                        "^" => OpCode::BinaryPow,
                        _ => return Err(format!("VX Error: 未知的二元操作符: {}", bin_op)),
                    };
                    self.emit(oc, BytecodeArg::None);
                    let tmp = format!("__asg_v_{}", self.instructions.len());
                    let tmp_slot = self.allocate_slot(&tmp);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(tmp_slot as i32));
                    self.compile_expr(obj)?;
                    self.emit(OpCode::PropertySet, BytecodeArg::String(prop.clone()));
                    self.emit(OpCode::Pop, BytecodeArg::None);
                }
                _ => {}
            }
        }
        Ok(())
    }

    pub fn compile_stmt(&mut self, s: &Stmt) -> Result<(), String> {
        match s {
            Expr::ExprStmt(expr, _, _) => {
                if let Expr::Assign(ref target, ref op, ref value, _, _) = **expr {
                    self.compile_assign(target, op, value)?;
                } else {
                    self.compile_expr(expr)?;
                }
            }
            Expr::VarDecl(name, _, value, _, _, _) => {
                self.compile_expr(value)?;
                let value_type = self.pop_stack_type();
                self.set_var_type(name, value_type);
                let slot = self.allocate_slot(name);
                self.emit(OpCode::DefineVar, BytecodeArg::Int(slot as i32));
            }
            Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
                self.compile_expr(cond)?;
                let jump_to_elif = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                for x in body {
                    self.compile_stmt(x)?;
                }
                let mut exit_jumps: Vec<usize> = Vec::new();
                exit_jumps.push(self.emit(OpCode::Jump, BytecodeArg::None));
                self.patch(jump_to_elif, self.instructions.len());
                for (c, b) in elifs {
                    self.compile_expr(c)?;
                    let jump_to_next = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                    for x in b {
                        self.compile_stmt(x)?;
                    }
                    exit_jumps.push(self.emit(OpCode::Jump, BytecodeArg::None));
                    self.patch(jump_to_next, self.instructions.len());
                }
                if let Some(b) = else_body {
                    for x in b {
                        self.compile_stmt(x)?;
                    }
                }
                let end_pc = self.instructions.len();
                for j in exit_jumps {
                    self.patch(j, end_pc);
                }
            }
            Expr::WhileStmt(cond, body, _, _) => {
                let start = self.instructions.len();
                self.loop_stack.push(LoopInfo {
                    start,
                    break_jumps: Vec::new(),
                    continue_jumps: Vec::new(),
                });
                self.compile_expr(cond)?;
                let exit_j = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                for x in body {
                    self.compile_stmt(x)?;
                }
                self.emit(OpCode::Jump, BytecodeArg::None);
                let exit_pc = self.instructions.len();
                self.patch(exit_j, exit_pc);
                self.patch(self.instructions.len() - 1, start);
                let (break_jumps, continue_jumps) = {
                    let info = self.loop_stack.last().unwrap();
                    (info.break_jumps.clone(), info.continue_jumps.clone())
                };
                for bj in &break_jumps {
                    self.patch(*bj, exit_pc);
                }
                for cj in &continue_jumps {
                    self.patch(*cj, start);
                }
                self.loop_stack.pop();
            }
            Expr::ForStmt(var, iter, body, _, _) => {
                let for_id = self.for_counter;
                self.for_counter += 1;
                let src_var = format!("__for_{}_src", for_id);
                let idx_var = format!("__for_{}_idx", for_id);
                self.compile_expr(iter)?;
                let src_slot = self.allocate_slot(&src_var);
                self.emit(OpCode::DefineVar, BytecodeArg::Int(src_slot as i32));
                let const_0 = self.add_const(ConstantValue::Int(0)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_0));
                let idx_slot = self.allocate_slot(&idx_var);
                self.emit(OpCode::DefineVar, BytecodeArg::Int(idx_slot as i32));
                let start = self.instructions.len();
                self.loop_stack.push(LoopInfo {
                    start,
                    break_jumps: Vec::new(),
                    continue_jumps: Vec::new(),
                });
                let idx_slot2 = self.allocate_slot(&idx_var);
                self.emit(OpCode::LoadVar, BytecodeArg::Int(idx_slot2 as i32));
                let src_slot2 = self.allocate_slot(&src_var);
                self.emit(OpCode::LoadVar, BytecodeArg::Int(src_slot2 as i32));
                let const_len = self.add_const(ConstantValue::String("len".into())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_len));
                self.emit(OpCode::Call, BytecodeArg::Int(1));
                self.emit(OpCode::BinaryLt, BytecodeArg::None);
                let exit_j = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                let src_slot3 = self.allocate_slot(&src_var);
                self.emit(OpCode::LoadVar, BytecodeArg::Int(src_slot3 as i32));
                let idx_slot3 = self.allocate_slot(&idx_var);
                self.emit(OpCode::LoadVar, BytecodeArg::Int(idx_slot3 as i32));
                self.emit(OpCode::IndexGet, BytecodeArg::None);
                let var_slot = self.allocate_slot(var);
                self.emit(OpCode::DefineVar, BytecodeArg::Int(var_slot as i32));
                for x in body {
                    self.compile_stmt(x)?;
                }
                let cont_pc = self.instructions.len();
                self.loop_stack.last_mut().unwrap().start = cont_pc;
                let idx_slot4 = self.allocate_slot(&idx_var);
                self.emit(OpCode::LoadVar, BytecodeArg::Int(idx_slot4 as i32));
                let const_1 = self.add_const(ConstantValue::Int(1)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_1));
                self.emit(OpCode::BinaryAdd, BytecodeArg::None);
                let idx_slot5 = self.allocate_slot(&idx_var);
                self.emit(OpCode::StoreVar, BytecodeArg::Int(idx_slot5 as i32));
                self.emit(OpCode::Jump, BytecodeArg::None);
                let exit_pc = self.instructions.len();
                self.patch(exit_j, exit_pc);
                self.patch(self.instructions.len() - 1, start);
                let (break_jumps, continue_jumps) = {
                    let info = self.loop_stack.last().unwrap();
                    (info.break_jumps.clone(), info.continue_jumps.clone())
                };
                for bj in &break_jumps {
                    self.patch(*bj, exit_pc);
                }
                for cj in &continue_jumps {
                    self.patch(*cj, cont_pc);
                }
                self.loop_stack.pop();
            }
            Expr::BreakStmt(line, col) => {
                if self.loop_stack.is_empty() {
                    return Err(format!("VX Error [line {}, col {}]: break outside loop", line, col));
                }
                let bj = self.emit(OpCode::Jump, BytecodeArg::None);
                self.loop_stack.last_mut().unwrap().break_jumps.push(bj);
            }
            Expr::ContinueStmt(line, col) => {
                if self.loop_stack.is_empty() {
                    return Err(format!("VX Error [line {}, col {}]: continue outside loop", line, col));
                }
                let cj = self.emit(OpCode::Jump, BytecodeArg::None);
                self.loop_stack
                    .last_mut()
                    .unwrap()
                    .continue_jumps
                    .push(cj);
            }
            Expr::ReturnStmt(val, _, _) => {
                if let Some(v) = val {
                    self.compile_expr(v)?;
                } else {
                    self.emit(OpCode::LoadNil, BytecodeArg::None);
                }
                self.emit(OpCode::Return, BytecodeArg::None);
            }
            // FreeStmt 已裁减 → mem::free(ptr) 标准库函数调用, 由 CallExpr 分支处理
            // 不可达: parse_statement 不会产生其他 Expr 变体作为顶层语句
            _ => {}
        }
        Ok(())
    }
    pub fn compile(&mut self, ast: &[Stmt]) -> Result<CompiledModule, String> {
        self.constants.clear();
        self.instructions.clear();
        self.functions.clear();
        self.loop_stack.clear();
        self.for_counter = 0;
        let mut structs = Vec::new();
        let mut classes = Vec::new();

        for s in ast {
            match s {
                Expr::StructDecl(name, fields, _, _, _) => {
                    structs.push((name.clone(), fields.iter().map(|f| f.1.clone()).collect()));
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    self.emit(OpCode::MakeStruct, BytecodeArg::String(name.clone()));
                    for (_, fname) in fields {
                        self.emit(OpCode::Dup, BytecodeArg::None);
                        self.emit(OpCode::LoadVar, BytecodeArg::String(fname.clone()));
                        self.emit(OpCode::PropertySet, BytecodeArg::String(fname.clone()));
                        self.emit(OpCode::Pop, BytecodeArg::None);
                    }
                    self.emit(OpCode::Return, BytecodeArg::None);
                    self.functions.push(BytecodeFunction {
                        name: name.clone(),
                        instructions: std::mem::replace(&mut self.instructions, save),
                        num_params: fields.len(),
                        has_return: true,
                        param_names: fields.iter().map(|f| f.1.clone()).collect(),
                    });
                    let name_const = self.add_const(ConstantValue::String(name.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(name_const));
                    self.emit(OpCode::StoreVar, BytecodeArg::String(name.clone()));
                }
                Expr::ClassDecl(name, fields, methods, _, _, _, _) => {
                    classes.push((name.clone(), fields.iter().map(|f| f.1.clone()).collect()));
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    self.emit(OpCode::MakeClass, BytecodeArg::String(name.clone()));
                    for f in fields {
                        self.emit(OpCode::Dup, BytecodeArg::None);
                        self.emit(OpCode::LoadVar, BytecodeArg::String(f.1.clone()));
                        self.emit(OpCode::PropertySet, BytecodeArg::String(f.1.clone()));
                        self.emit(OpCode::Pop, BytecodeArg::None);
                    }
                    self.emit(OpCode::Return, BytecodeArg::None);
                    self.functions.push(BytecodeFunction {
                        name: name.clone(),
                        instructions: std::mem::replace(&mut self.instructions, save),
                        num_params: fields.len(),
                        has_return: true,
                        param_names: fields.iter().map(|f| f.1.clone()).collect(),
                    });
                    for m in methods {
                        if let Expr::FuncDecl(mname, params, _, mbody, _, _) = m.as_ref() {
                            let msave = std::mem::replace(&mut self.instructions, Vec::new());
                            let save_var_types = self.var_types.clone();
                            self.var_types.clear();
                            for (pname, ptype) in params {
                                let known_type = match ptype.as_str() {
                                    "int" => KnownType::Int,
                                    "float" => KnownType::Float,
                                    "bool" => KnownType::Bool,
                                    "string" => KnownType::String,
                                    _ => KnownType::Unknown,
                                };
                                self.var_types.insert(pname.clone(), known_type);
                            }
                            for x in mbody {
                                self.compile_stmt(x)?;
                            }
                            self.var_types = save_var_types;
                            if !mbody
                                .iter()
                                .any(|x| matches!(&**x, Expr::ReturnStmt(_, _, _)))
                            {
                                self.emit(OpCode::LoadNil, BytecodeArg::None);
                                self.emit(OpCode::Return, BytecodeArg::None);
                            }
                            let method_name = format!("{}_{}", name, mname);
                            self.functions.push(BytecodeFunction {
                                name: method_name,
                                instructions: std::mem::replace(&mut self.instructions, msave),
                                num_params: params.len(),
                                has_return: true,
                                param_names: params.iter().map(|p| p.0.clone()).collect(),
                            });
                            let mname_const = self
                                .add_const(ConstantValue::String(format!("{}_{}", name, mname)))
                                as i32;
                            self.emit(OpCode::LoadConst, BytecodeArg::Int(mname_const));
                            self.emit(
                                OpCode::StoreVar,
                                BytecodeArg::String(format!("{}_{}", name, mname)),
                            );
                        }
                    }
                }
                Expr::EnumDecl(_, _, _, _) => {}
                Expr::UnionDecl(_, _, _, _) => {}
                Expr::ImportStmt(name, alias, dirs, _, _) => {
                    let lib_path = self.settings.library_path(name);
                    // dirs 现在为 Vec<String> 多路径列表, 合并为逗号分隔的 Option<String>
                    let combined_dirs = if dirs.is_empty() {
                        lib_path.clone()
                    } else {
                        let prefix = lib_path.clone().unwrap_or_default();
                        let paths: Vec<String> = dirs.iter()
                            .map(|d| format!("{}/{}", prefix, d))
                            .collect();
                        Some(paths.join(","))
                    };
                    self.emit(
                        OpCode::Import,
                        BytecodeArg::ImportTuple(name.clone(), alias.clone(), combined_dirs),
                    );
                }
                Expr::FuncDecl(fname, params, _, body, _, _) => {
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    let save_var_types = self.var_types.clone();
                    let save_var_slots = self.var_slots.clone();
                    let save_next_slot = self.next_slot;
                    self.var_types.clear();
                    self.var_slots.clear();
                    self.next_slot = 0;
                    for (pname, ptype) in params {
                        let known_type = match ptype.as_str() {
                            "int" => KnownType::Int,
                            "float" => KnownType::Float,
                            "bool" => KnownType::Bool,
                            "string" => KnownType::String,
                            _ => KnownType::Unknown,
                        };
                        self.var_types.insert(pname.clone(), known_type);
                        self.allocate_slot(pname);
                    }
                    for x in body {
                        self.compile_stmt(x)?;
                    }
                    self.var_types = save_var_types;
                    self.var_slots = save_var_slots;
                    self.next_slot = save_next_slot;
                    if !body
                        .iter()
                        .any(|x| matches!(&**x, Expr::ReturnStmt(_, _, _)))
                    {
                        self.emit(OpCode::LoadNil, BytecodeArg::None);
                        self.emit(OpCode::Return, BytecodeArg::None);
                    }
                    self.functions.push(BytecodeFunction {
                        name: fname.clone(),
                        instructions: std::mem::replace(&mut self.instructions, save),
                        num_params: params.len(),
                        has_return: true,
                        param_names: params.iter().map(|p| p.0.clone()).collect(),
                    });
                    let fname_const = self.add_const(ConstantValue::String(fname.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(fname_const));
                    let fname_slot = self.allocate_slot(fname);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(fname_slot as i32));
                }
                _ => {
                    self.compile_stmt(s)?;
                }
            }
        }
        if !self.instructions.is_empty() {
            self.emit(OpCode::LoadNil, BytecodeArg::None);
            self.emit(OpCode::Return, BytecodeArg::None);
            self.functions.insert(
                0,
                BytecodeFunction {
                    name: "__main__".into(),
                    instructions: std::mem::replace(&mut self.instructions, Vec::new()),
                    num_params: 0,
                    has_return: false,
                    param_names: Vec::new(),
                },
            );
        }
        // Generate TypeIR from the compiled bytecode (before replacing self.functions)
        let type_ir_data = self.generate_type_ir(&self.functions);
        Ok(CompiledModule {
            functions: std::mem::replace(&mut self.functions, Vec::new()),
            constants: std::mem::replace(&mut self.constants, Vec::new()),
            structs,
            classes,
            type_ir_data,
            target_triple: String::new(),
        })
    }

    fn generate_type_ir(&self, functions: &[BytecodeFunction]) -> Vec<u8> {
        let mut type_mod = TypeModule::new();
        for (i, func) in functions.iter().enumerate() {
            let mut tf = TypeFunction::new(&func.name, i as FuncId);
            tf.param_count = func.num_params as u32;
            tf.has_return = func.has_return;
            for param_name in &func.param_names {
                let ptype = self.get_var_type(param_name);
                tf.params.push((param_name.clone(), self.known_to_type(ptype)));
            }
            let mut sim = TypeIRSimulator::new();
            for inst in &func.instructions {
                sim.translate_inst(inst, &self.constants);
            }
            tf.body = sim.body;
            tf.var_count = tf.body.len() as u32;
            type_mod.functions.push(tf);
            type_mod.function_map.insert(i as FuncId, func.name.clone());
        }
        if let Some(main_idx) = functions.iter().position(|f| f.name == "__main__") {
            type_mod.entry_point = Some(main_idx as FuncId);
        }
        type_ir::serialize_type_module(&type_mod)
    }

    fn known_to_type(&self, kt: KnownType) -> Type {
        match kt {
            KnownType::Int => Type::Int,
            KnownType::Float => Type::Float,
            KnownType::Bool => Type::Bool,
            KnownType::String => Type::String,
            KnownType::Unknown => Type::Unknown,
        }
    }

    pub fn save(&self, der: &CompiledModule, path: &str) -> io::Result<()> {
        use std::io::BufWriter;

        let mut f = BufWriter::new(fs::File::create(path)?);

        let constants: Vec<bytecode::SerializedConstant> = der
            .constants
            .iter()
            .map(|c| match c {
                ConstantValue::Nil => bytecode::SerializedConstant::nil(),
                ConstantValue::Bool(b) => bytecode::SerializedConstant::bool(*b),
                ConstantValue::Int(v) => bytecode::SerializedConstant::int(*v),
                ConstantValue::Float(v) => bytecode::SerializedConstant::float(*v),
                ConstantValue::String(s) => bytecode::SerializedConstant::string(s),
            })
            .collect();

        let mut struct_map = HashMap::new();
        for (n, f) in &der.structs {
            struct_map.insert(n.clone(), f.clone());
        }
        for (n, f) in &der.classes {
            struct_map.insert(n.clone(), f.clone());
        }

        let mut func_data: Vec<(
            &str,
            u32,
            bool,
            Vec<String>,
            Vec<(u8, u8, Option<i32>, Option<String>)>,
        )> = Vec::with_capacity(der.functions.len());

        let mut temp_strings = Vec::new();
        for fn_ in &der.functions {
            let mut insts: Vec<(u8, u8, Option<i32>, Option<String>)> =
                Vec::with_capacity(fn_.instructions.len());
            for inst in &fn_.instructions {
                let (arg_type, iarg, sarg) = match &inst.arg {
                    BytecodeArg::None => (0, None, None),
                    BytecodeArg::Int(v) => (1, Some(*v), None),
                    BytecodeArg::String(s) => (2, None, Some(s.clone())),
                    BytecodeArg::ImportTuple(a, b, c) => {
                        let s = format!(
                            "{},{},{}",
                            b.as_deref().unwrap_or(""),
                            c.as_deref().unwrap_or(""),
                            a,
                        );
                        temp_strings.push(s.clone());
                        (2, None, Some(s))
                    }
                };
                insts.push((inst.op as u8, arg_type, iarg, sarg));
            }
            func_data.push((
                fn_.name.as_str(),
                fn_.num_params as u32,
                fn_.has_return,
                fn_.param_names.clone(),
                insts,
            ));
        }

        let func_refs: Vec<(
            &str,
            u32,
            bool,
            &[String],
            &[(u8, u8, Option<i32>, Option<String>)],
        )> = func_data
            .iter()
            .map(|(name, np, hr, pn, insts)| {
                (*name, *np, *hr, pn.as_slice(), insts.as_slice())
            })
            .collect();

        // Write v3 format if TypeIR is present
        if !der.type_ir_data.is_empty() {
            let mut bytecode_buf = Vec::new();
            bytecode::write_vxobj(&mut bytecode_buf, &constants, &func_refs, &struct_map)?;
            let target = if der.target_triple.is_empty() {
                "x86_64-unknown-linux-gnu"
            } else {
                &der.target_triple
            };
            bytecode::write_vxobj_v3(
                &mut f, target,
                &der.type_ir_data, &bytecode_buf,
                &[], &[], &[],
            )
        } else {
            bytecode::write_vxobj(&mut f, &constants, &func_refs, &struct_map)
        }
    }
}