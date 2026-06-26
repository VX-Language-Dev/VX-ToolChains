// ==================== TypeIR 栈模拟器 ====================
// 从 compiler_core 拆分而来，不修改任何业务逻辑。
// 在生成 TypeIR 时追踪字节码栈，将栈位置映射为正确的 VarId。

use std::collections::HashMap;

use crate::type_ir::{TypedInstruction, StructLayoutId, FuncId};
use crate::compiler_bytecode::{BytecodeArg, Instruction, ConstantValue};
use crate::OpCode;

/// TypeIR 栈模拟器：在生成 TypeIR 时追踪字节码栈，
/// 将栈位置映射为正确的 VarId（TypeIR 中的指令索引）。
pub(crate) struct TypeIRSimulator {
    body: Vec<TypedInstruction>,
    slot_to_var: HashMap<u32, u32>,
    stack: Vec<u32>,
    /// 跟踪字符串常量 VarId → 函数名，用于解析 Call 的 callee
    const_strings: HashMap<u32, String>,
    /// 函数名 → TypeIR FuncId
    func_name_to_id: HashMap<String, FuncId>,
}

impl TypeIRSimulator {
    pub(crate) fn new() -> Self {
        Self {
            body: Vec::new(),
            slot_to_var: HashMap::new(),
            stack: Vec::new(),
            const_strings: HashMap::new(),
            func_name_to_id: HashMap::new(),
        }
   }

    pub(crate) fn with_function_map(func_name_to_id: HashMap<String, FuncId>) -> Self {
        Self {
            body: Vec::new(),
            slot_to_var: HashMap::new(),
            stack: Vec::new(),
            const_strings: HashMap::new(),
            func_name_to_id,
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

    pub(crate) fn pop_val(&mut self) -> u32 {
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

    pub(crate) fn translate_inst(&mut self, inst: &Instruction, constants: &[ConstantValue]) {
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
                    Some(ConstantValue::String(s)) => {
                        let vid = self.body.len() as u32;
                        self.const_strings.insert(vid, s.clone());
                        ConstString(s.clone())
                    }
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
                let callee_vid = self.pop_val();
                // 根据 callee 字符串常量解析函数 ID
                let callee_id = self.const_strings.get(&callee_vid)
                    .and_then(|name| self.func_name_to_id.get(name))
                    .copied()
                    .unwrap_or(0);
                let vid = self.emit(Call(callee_id, args));
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

    pub(crate) fn into_body(self) -> Vec<TypedInstruction> {
        self.body
    }
}