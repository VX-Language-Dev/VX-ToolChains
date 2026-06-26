// ==================== 编译器字节码格式 ====================

use crate::{OpCode, Instruction as VmInstruction};

#[derive(Debug, Clone)]
pub enum BytecodeArg {
    None,
    Int(i32),
    String(String),
    ImportTuple(String, Option<String>, Option<String>),
}

impl BytecodeArg {
    #[allow(dead_code)]
    pub fn to_vm_args(&self) -> (Option<i32>, Option<String>) {
        match self {
            BytecodeArg::None => (None, None),
            BytecodeArg::Int(v) => (Some(*v), None),
            BytecodeArg::String(s) => (None, Some(s.clone())),
            BytecodeArg::ImportTuple(a, b, c) => {
                let s = format!(
                    "{},{},{}",
                    b.as_deref().unwrap_or(""),
                    c.as_deref().unwrap_or(""),
                    a,
                );
                (None, Some(s))
            }
        }
    }
}

#[derive(Debug, Clone)]
pub struct Instruction {
    pub op: OpCode,
    pub arg: BytecodeArg,
}

impl Instruction {
    #[allow(dead_code)]
    pub fn to_vm_instruction(&self) -> VmInstruction {
        let (iarg, sarg) = self.arg.to_vm_args();
        let mut inst = VmInstruction::new(self.op);
        inst.iarg = iarg;
        inst.sarg = sarg.map(|s| s.into_boxed_str());
        inst
    }
}

#[derive(Debug, Clone)]
pub struct BytecodeFunction {
    pub name: String,
    pub instructions: Vec<Instruction>,
    pub num_params: usize,
    pub has_return: bool,
    pub param_names: Vec<String>,
}

#[derive(Debug, Clone)]
#[allow(dead_code)]
pub enum ConstantValue {
    Nil,
    Bool(bool),
    Int(i64),
    Float(f64),
    String(String),
}

pub struct CompiledModule {
    pub functions: Vec<BytecodeFunction>,
    pub constants: Vec<ConstantValue>,
    pub structs: Vec<(String, Vec<String>)>,
    pub classes: Vec<(String, Vec<String>)>,
    pub type_ir_data: Vec<u8>,
    pub target_triple: String,
    /// 外部依赖信息，用于静态链接时的动态库链接
    pub external_deps: Vec<String>,
}