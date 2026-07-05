// ==================== 编译器字节码格式 ====================
// 注意：此处的字节码仅为编译器内部用于推导 TypeIR 的临时表示，
// 不再作为可持久化或执行的输出格式。

use crate::OpCode;
use crate::type_ir::Type;

#[derive(Debug, Clone)]
pub enum BytecodeArg {
    None,
    Int(i32),
    String(String),
    ImportTuple(String, Option<String>, Option<String>),
}

impl BytecodeArg {
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
    pub fn new(op: OpCode) -> Self {
        Self { op, arg: BytecodeArg::None }
    }

    pub fn with_arg(op: OpCode, arg: BytecodeArg) -> Self {
        Self { op, arg }
    }
}

#[derive(Debug, Clone)]
pub struct BytecodeFunction {
    pub name: String,
    pub instructions: Vec<Instruction>,
    pub num_params: usize,
    pub has_return: bool,
    pub param_names: Vec<String>,
    /// 参数类型（VX 静态类型，以 TypeIR Type 表示）
    pub param_types: Vec<Type>,
}

#[derive(Debug, Clone)]
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
