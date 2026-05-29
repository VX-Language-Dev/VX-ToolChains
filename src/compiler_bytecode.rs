// ==================== 编译器字节码格式 ====================

#[derive(Debug, Clone)]
pub enum BytecodeArg {
    None,
    Int(i32),
    String(String),
    ImportTuple(String, Option<String>, Option<String>),
}

#[derive(Debug, Clone)]
pub struct Instruction {
    pub op: super::compiler_opcode::OpCode,
    pub arg: BytecodeArg,
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
}
